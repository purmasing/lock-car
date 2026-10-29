#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#if __has_include("esp_netif_net_stack.h")
#include "esp_netif_net_stack.h"   // esp_netif_get_netif_impl() در IDF 5.x اینجاست
#endif
#include <string.h>
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include <TinyGPSPlus.h>
#include <PubSubClient.h>
// ===================== Pin Definitions (ESP32-S3) =====================
// GPIO26..32 : فلش/PSRAM  | GPIO33..37 : PSRAM اکتال (N8R8/N16R8)  | 19,20 : USB
// GPIO0,3,45,46 : strapping | 43,44 : UART0
#define OUTPUT_COUNT      5

#define PIN_DOOR_DRIVER   36
#define PIN_TRUNK         40
#define PIN_FUEL_PUMP     38
#define PIN_CAR_SWITCH    39
#define PIN_DOORS_INVERSE 37
#define PIN_BUZZER         1   // بازر/آژیر روی ESP32-S3 (از طریق ترانزیستور)
#define BUZZER_ON_LEVEL   HIGH // اگر بازر شما active-low است، این دو را جابه‌جا کنید
#define BUZZER_OFF_LEVEL  LOW
// هشدار: GPIO36/37 فقط روی بردهایی آزادند که PSRAM اکتال (R8) ندارند (مثلاً N8 / N16 / N4 / R2)

// ===================== SIM800 Module Pins =====================
#define SIM800_RX_PIN      18   // ESP32 RX  <- SIM800 TXD
#define SIM800_TX_PIN      17   // ESP32 TX  -> SIM800 RXD
#define SIM800_PWK_PIN      4   // Power Key (LOW pulse to power on)
#define SIM800_RI_PIN       5   // Ring Indicator input
#define SIM800_BAUD      9600

#define MAX_PHONE_NUMBERS   5
#define MAX_CALL_DURATION_MS 60000L

// جدید: حلقه‌ی SMS لوکیشن با فاصله‌ی قابل تنظیم بر حسب دقیقه (فرمان: Location <عدد 1..120>)
#define LOC_SMS_MIN_MINUTES          1
#define LOC_SMS_MAX_MINUTES          120
#define LOC_SMS_INTERVAL_DEFAULT_MS  60000UL   // «Location» بدون عدد = هر ۱ دقیقه
// جدید: تشخیص قطع آنتن GPS
#define GPS_ANTICUT_TIMEOUT_MS 60000UL   // 5 دقیقه
// جدید: OTA
#define OTA_HOSTNAME          "CarControl"
// ===================== GPS (NEO-6M) =====================
#define GPS_RX_PIN           7
#define GPS_TX_PIN           6
#define GPS_BAUD             9600
#define GPS_STALE_MS         120000UL   // 2 min without fix -> stale
#define GPS_FIX_TIMEOUT_MS   90000UL    // max wait for first fix

// ===================== MQTT over SIM800 =====================
#define MQTT_RETRY_MS        15000UL
#define MQTT_KEEPALIVE_S     30
#define MQTT_BUFFER_SIZE     600
#define MQTT_CMD_TTL_MS      60000UL
#define MQTT_CMD_ACK_TO_MS   15000UL
#define MQTT_CMD_RETRY_MAX   3
#define SIM800_TCP_RX_SIZE   600
// ===================== MQTT & Safe Zone =====================
// ===================== MQTT & Safe Zone =====================
#define MQTT_HOST                     "smartcarlock.ir"
#define SAFE_ZONE_CHECK_MS            5000UL
#define SAFE_ZONE_NO_FIX_TIMEOUT_MS   60000UL
#define SAFE_ZONE_CALL_GAP_MS         5000UL
#define SAFE_ZONE_CALL_TIMEOUT_MS     30000UL
#define SAFE_ZONE_CALL_POLL_MS        3000UL
// ===================== Pulse Timing =====================
#define DOOR_PULSE_MS   1000
#define TRUNK_PULSE_MS  1000
#define MAX_PULSES      5

// ===================== Default AP Settings =====================
#define DEFAULT_SSID      "CarControl"
#define DEFAULT_PASSWORD  "12345678"
#define DEFAULT_IP        "192.168.4.1"
#define HTTP_PORT         80
#define AP_CHANNEL        6
#define AP_MAX_CLIENTS    4

// ===================== Security Settings =====================
#define ACTIVATION_CODE       "1234"
#define MAX_ALLOWED_DEVICES   20

// ===================== Global Objects =====================
WebServer server(HTTP_PORT);
Preferences preferences;

String ap_ssid;
String ap_password;
IPAddress ap_ip;

bool outputState[OUTPUT_COUNT] = {};
bool desiredState[OUTPUT_COUNT] = {};

String customOutputNames[OUTPUT_COUNT];

// ===== احراز هویت با توکن (جایگزین لیست MAC) =====
// هر اپ پس از وارد کردن کد فعال‌سازی یک توکن تصادفی ۱۲۸ بیتی می‌گیرد و در هر درخواست می‌فرستد.
// MAC فقط به‌عنوان «آخرین دستگاه شناخته‌شده‌ی این توکن» ثبت می‌شود (برای سیستم امنیتی آنتن)؛
// اگر گوشی MAC تصادفی‌اش را عوض کند، با اولین درخواست بعدی خودکار به‌روز می‌شود.
struct Dev { String token; String mac; };
Dev devs[MAX_ALLOWED_DEVICES];
unsigned long devSeen[MAX_ALLOWED_DEVICES]   = {};   // آخرین درخواست معتبر (فقط RAM)
unsigned long devMacTry[MAX_ALLOWED_DEVICES] = {};
int devCount = 0;
#define AUTH_ACTIVE_MS   45000UL   // اپ در ۴۵ ثانیه‌ی اخیر درخواست معتبر داده = کاربر حاضر است
#define PAIR_MAX_FAILS   5
#define PAIR_LOCK_MS     60000UL

// نام خروجی‌ها فقط برای لاگ سریال (نام نمایشی را اپ می‌سازد)
static const char* const OUT_LOG_NAMES[OUTPUT_COUNT] = {"Doors", "Trunk", "Fuel Pump", "Car Switch", "Doors Inverse"};

int signalThreshold = 50;

bool weakAutoDisconnectEnabled = false;
bool weakDisconnectActive = false;
unsigned long lastWeakCheck = 0;

// Momentary pulse management
struct Pulse {
  bool active;
  int pin;
  unsigned long start;
  unsigned long duration;
  int resetOutputIndex;
};

Pulse pulses[MAX_PULSES];

// ---- انواع داده‌ی پیامک (باید قبل از اولین تابع تعریف شوند) ----
struct SimMsg {                 // پیامک خوانده‌شده از سیم‌کارت
  int idx;
  String st;
  String from;
  String date;
  String text;
};



// ===================== SIM800 Globals =====================
HardwareSerial sim800Serial(2);
// ===== Forward declarations for Sim800TcpClient =====
void sim800PumpRx();
extern String sim800APN;
extern String sim800RxBuf;
extern volatile bool g_sendOk;
extern volatile bool g_sendFail;
extern volatile bool g_tcpConnectOk;
extern volatile bool g_tcpConnectFail;
extern volatile bool g_tcpCloseOk;
// ===================== SMS Location Loop =====================
bool     locSmsActive = false;
String   locSmsTargetNumber = "";
unsigned long locSmsLastSend = 0;
int      locSmsSendCount = 0;              // شمارنده برای لاگ (حلقه تا فرمان Stop location ادامه دارد)
unsigned long locSmsIntervalMs = LOC_SMS_INTERVAL_DEFAULT_MS;   // فاصله‌ی ارسال لوکیشن (با فرمان Location <دقیقه> تنظیم می‌شود)

// ===================== Pending Status SMS =====================
// اگر Status هنگام تماس فعال درخواست شود، بعد از پایان تماس فقط یک‌بار ارسال می‌شود
bool     pendingStatusSms  = false;
String   pendingStatusFrom = "";

// ===================== Serial Command Console [PATCH-S] =====================
// lines typed in Serial Monitor are collected here and executed like SMS commands
String   serialCmdBuf = "";

// ===================== Pending Alarm SMS =====================
bool     pendingAlarmSms = false;
String   pendingAlarmSmsReason = "";

// ===================== GPS Anti-Cut =====================
bool     gpsAntiCutEnabled = false;
unsigned long gpsNoFixSince = 0;

// ===================== WiFi Presence =====================
bool     wifiUserWasConnected = false;
unsigned long lastWifiCheck = 0;
// ===================== GPS Safe Zone =====================
bool     safeZoneEnabled   = false;
double   safeZoneLat       = 0;
double   safeZoneLng       = 0;
uint32_t safeZoneRadiusM   = 100;
bool     safeZoneViolated  = false;
bool     safeZoneAlarmActive = false;
unsigned long safeZoneLastCheck = 0;
unsigned long safeZoneNoFixSince = 0;

int      safeZoneCallIdx   = 0;
unsigned long safeZoneCallStart = 0;
bool     safeZoneCallAnswered    = false;   // آیا تماس پاسخ داده شد؟
unsigned long safeZoneCallLastPoll = 0;      // زمان آخرین AT+CLCC

bool gpsSerialStarted = false;
// =====================================================

class Sim800TcpClient : public Client {
private:
  HardwareSerial* _s;
  uint8_t  _rxBuf[SIM800_TCP_RX_SIZE];
  volatile uint16_t _head = 0, _tail = 0;
  bool     _connected = false;

  void _drain() { while (_s->available()) sim800PumpRx(); }

  bool _waitLine(const char* tok, unsigned long to) {
    unsigned long st = millis();
    while (millis() - st < to) {
      sim800PumpRx();
      delay(5);
    }
    return true;  // best-effort; specific tokens checked via flags
  }

  bool _waitPrompt(unsigned long to) {
    unsigned long st = millis();
    while (millis() - st < to) {
      sim800PumpRx();
      if (sim800RxBuf.indexOf('>') >= 0) { sim800RxBuf = ""; return true; }
      delay(5);
    }
    return false;
  }

public:
  Sim800TcpClient(HardwareSerial* s) : _s(s) {}

  void pushData(uint8_t b) {
    uint16_t n = (_tail + 1) % SIM800_TCP_RX_SIZE;
    if (n != _head) { _rxBuf[_tail] = b; _tail = n; }
  }

  int connect(const char* host, uint16_t port) override {
    _connected = false;
    _head = _tail = 0;

    Serial.println("TCP: closing any prior socket");
    _s->print("AT+CIPCLOSE\r");
    _waitLine("CLOSE OK", 3000);
    _s->print("AT+CIPSHUT\r");
    _waitLine("SHUT OK", 20000);

    _s->print("AT+CIPMUX=0\r");
    _waitLine("OK", 3000);

    _s->print("AT+CGATT=1\r");
    _waitLine("OK", 10000);

    if (sim800APN.length() > 0) {
      _s->print("AT+CSTT=\"");
      _s->print(sim800APN);
      _s->print("\"\r");
      _waitLine("OK", 5000);
    }

    _s->print("AT+CIICR\r");
    _waitLine("OK", 60000);

    g_tcpConnectOk = false;
    g_tcpConnectFail = false;

    _s->print("AT+CIPSTART=\"TCP\",\"");
    _s->print(host);
    _s->print("\",");
    _s->print(port);
    _s->print('\r');

    unsigned long st = millis();
    while (millis() - st < 90000) {
      sim800PumpRx();
      if (g_tcpConnectOk)   { _connected = true;  Serial.println("TCP: connected"); return 1; }
      if (g_tcpConnectFail) { _connected = false; Serial.println("TCP: connect failed"); return 0; }
      delay(10);
    }
    Serial.println("TCP: connect timeout");
    return 0;
  }

  int connect(IPAddress ip, uint16_t port) override {
    return connect(ip.toString().c_str(), port);
  }

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!_connected) return 0;
    sim800RxBuf = "";
    g_sendOk = false;
    g_sendFail = false;

    _s->print("AT+CIPSEND=");
    _s->print(size);
    _s->print('\r');

    if (!_waitPrompt(10000)) {
      Serial.println("TCP: no prompt");
      return 0;
    }

    _s->write(buf, size);

    unsigned long st = millis();
    while (millis() - st < 30000) {
      sim800PumpRx();
      if (g_sendOk)   { g_sendOk = false;   return size; }
      if (g_sendFail) { g_sendFail = false; return 0; }
      delay(5);
    }
    Serial.println("TCP: send timeout");
    return 0;
  }

  int available() override {
    sim800PumpRx();
    return (_tail - _head + SIM800_TCP_RX_SIZE) % SIM800_TCP_RX_SIZE;
  }

  int read() override {
    sim800PumpRx();
    if (_head == _tail) return -1;
    uint8_t b = _rxBuf[_head];
    _head = (_head + 1) % SIM800_TCP_RX_SIZE;
    return b;
  }

  int read(uint8_t* buf, size_t n) override {
    size_t i = 0;
    while (i < n && available() > 0) buf[i++] = (uint8_t)read();
    return i;
  }

  int peek() override {
    sim800PumpRx();
    if (_head == _tail) return -1;
    return _rxBuf[_head];
  }

  void flush() override {}

  void stop() override {
    _s->print("AT+CIPCLOSE\r");
    _waitLine("CLOSE OK", 5000);
    _connected = false;
    _head = _tail = 0;
  }

  uint8_t connected() override {
    sim800PumpRx();
    return _connected ? 1 : 0;
  }

  operator bool() override { return _connected; }
};
Sim800TcpClient sim800TcpClient(&sim800Serial);
PubSubClient    mqttClient;

bool sim800Enabled = true;            // [CHANGE] همیشه فعال (چک‌باکس حذف شد)
bool sim800InternetEnabled = true;    // [CHANGE] همیشه فعال (چک‌باکس حذف شد)
bool sim800Ready   = false;
unsigned long sim800LastInit   = 0;
unsigned long sim800LastStatus = 0;
unsigned long sim800LastSignal = 0;

int gsmCSQ = 99;
int gsmPercent = 0;
bool gprsRegistered = false;
bool gprsAttached   = false;
String sim800Carrier = "";
String sim800APN     = "";
String sim800IP      = "";
// شمارنده‌ی داده‌ی برنامه؛ SIM800 در حالت SAPBR آمار کل ترافیک اپراتور را ارائه نمی‌کند.
uint64_t sim800TxDataBytes = 0;
uint64_t sim800RxDataBytes = 0;

String phoneNumbers[MAX_PHONE_NUMBERS];

// ===================== SMS commands / Security / Location =====================
#define SMS_CMD_COUNT    7      // Location, Lock, Unlock, Alarm_on, Alarm_off, Stop location, Status
#define SEC_GRACE_MS     300000UL   // 5 دقیقه
#define SEC_DEBOUNCE_MS  30000UL    // قطعی آنتن باید ۳۰ ثانیه ادامه داشته باشد

String smsCmd[SMS_CMD_COUNT];

bool carLocked = false;         // قفل فعال (پمپ و سوئیچ نمی‌توانند روشن شوند)
bool secLocked = false;         // این قفل توسط سیستم امنیتی اعمال شده
bool savedFuel = true, savedSwitch = true;
bool alarmManual = false;

bool securityEnabled = false;
bool securitySuppressed = false;
int  secState = 0;              // 0 عادی | 1 مهلت+بازر | 2 کاربر وصل شد | 3 قفل امنیتی
unsigned long secGraceStart = 0;
unsigned long gsmBadSince = 0;

int  locPhase = 0;              // 0 بیکار | 1 منتظر SAPBR | 2 منتظر CIPGSMLOC
unsigned long locStart = 0;
String locRequester = "";
bool locWanted = false;                 // مرحله‌ی بیرون‌آوردن اینترنت برای Location است؟
unsigned long bearerLastFail = 0;
unsigned long sim800LastBearerTry = 0;
unsigned long sim800LastStorageCheck = 0;
String locResult = "";
bool locDone = false;

bool ringDetected = false;
bool callActive = false;
unsigned long callStart = 0;
String activeCaller = "";
// ===================== GPS State =====================
TinyGPSPlus   gps;
HardwareSerial gpsSerial(1);
bool   gpsEnabled     = true;         // [CHANGE] همیشه فعال (چک‌باکس حذف شد)
int    gpsSite        = 0;              // 0=Google, 1=Neshan, 2=Balad
bool   gpsFix         = false;
double gpsLat         = 0;
double gpsLng         = 0;
unsigned long gpsLastFixMs = 0;
unsigned long gpsFirstSearchMs = 0;
int    gpsSatellites  = 0;
float  gpsHdop        = 0;

// ===================== MQTT State =====================
uint16_t mqttPort     = 1883;
String   mqttUser     = "";
String   mqttPass     = "";
String   deviceToken  = "";
bool     mqttEnabled  = false;
bool     mqttConnected= false;
unsigned long mqttLastTryMs     = 0;
unsigned long mqttLastPublishMs = 0;

// TCP flag helpers (set by sim800ProcessLine / sim800CleanAndQueue)
volatile bool g_sendOk        = false;
volatile bool g_sendFail      = false;
volatile bool g_tcpConnectOk  = false;
volatile bool g_tcpConnectFail= false;
volatile bool g_tcpCloseOk    = false;
volatile bool g_smsOk      = false;   // +CMGS: <n> دریافت شد
volatile bool g_smsFail    = false;   // +CMS ERROR دریافت شد
String sim800RxBuf = "";

const uint8_t outputPins[OUTPUT_COUNT] = {
    PIN_DOOR_DRIVER,
    PIN_TRUNK,
    PIN_FUEL_PUMP,
    PIN_CAR_SWITCH,
    PIN_DOORS_INVERSE
};

// ===================== Function Prototypes =====================
void loadAllSettings();
void initPrefsNamespaces();
void setupWiFiAP();
void setupWebServer();
// OTA
void setupOTA();
// SMS helpers
bool sendSms(const String& to, const String& text);
// WiFi reset
void checkWifiConnection();
void handleSerialCommands();   // [PATCH-S] serial command console
void resetAllSecurity();

void setOutput(int index, bool state);
void setDoors(bool lock);
void triggerTrunk();
void initPulses();
void startPulse(int pin, unsigned long duration, int resetOutputIndex = -1);
void cancelPulse(int pin);
void handlePulses();

int getClientRSSI();
int rssiToPercentage(int rssi);


IPAddress stringToIP(String ipStr);
bool ipIsUsable(IPAddress ip);

void loadDevices();
void saveDevices();
String getMacAddressString(const uint8_t* mac);
// کش IP→MAC برای کاهش فشار روی lwIP
struct MacCacheEntry {
  IPAddress ip;
  String    mac;
  unsigned long ts;
};
static MacCacheEntry macCache[AP_MAX_CLIENTS];
static int macCacheIdx = 0;
#define MAC_CACHE_TTL_MS 60000UL   // 60 ثانیه
String getMacByIP(IPAddress ip);

void handleWeakSignalProtection();
void saveWeakAutoSetting();
void loadCustomNames();
void saveCustomName(int idx);

// ---- JSON API (تعریف در انتهای فایل) ----
void apiPing();   void apiPair();    void apiUnpair();
void apiState();  void apiConfig();
void apiToggle(); void apiWeak();    void apiName();    void apiThreshold();
void apiApSet();  void apiSimSet();  void apiApn();
void apiGpsSet(); void apiMqttSet(); void apiNotFound();

void saveSignalThreshold();

// --- SIM800 ---
void loadSim800Config();
void saveSim800Config();
void setupSim800();
void powerOnSim800();
void updateSim800();
bool sim800Probe();
String sim800Exchange(const String& cmd, unsigned long timeout);
bool sim800SendAT(String cmd, String expected, unsigned long timeout);
bool sim800WaitFor(const char* token, unsigned long timeout, String* out);
void sim800PumpRx();
void sim800ProcessLine(const String& line);
String sim800CleanAndQueue(const String& raw);
void updateSim800Status();
void updateGsmSignal();
void detectAndSetAPN();
String apnForOperator(String op);
void handleSim800URC(String line);
void handleIncomingCall(String number);
void handleIncomingSMS(int index);
void processSMSCommand(String from, String body);
bool isPhoneAllowed(String number);
String normalizePhone(String number);
void startLocation(const String& who);
void startBearer(bool wantLoc);
void bearerFail(const String& msg);
void locFinish(const String& msg);
void sim800CheckStorage();
bool cmdMatch(int i, const String& upBody);
String secStateName();
String jsonEscape(const String& in);
bool asciiOnly(const String& s);

bool sim800ConfigureSms();
void sim800VerifySmsConfig();
void sim800DrainSim();
void sim800PollSms();
void sim800HandleMessage(const String& from, const String& date, const String& text, bool execute);
int  smsClassify(String body);
String smsCmdNormalized(int i);
String smsCollapseSpaces(const String& in);
String buildStatusText();
bool   sendSmsChunked(const String& to, const String& text);
void   sendStatusSms(const String& from);
void inboxLoad();
void inboxSave();

void applyLock(bool lockDoors, bool bySecurity);   // [CHANGE] پروتوتایپ با تعریف واقعی هماهنگ شد
void releaseLock(bool restore);
void unlockCar();
void saveLockState();
void loadLockState();
bool authorizedClientConnected();
bool gsmHealthy();
void updateSecurity();
void updateBuzzer();


// GPS
void setupGPS();
void updateGPS();
String gpsMapUrl();
String gpsStatusJson();
bool  gpsAvailable();
double haversineMeters(double lat1, double lng1, double lat2, double lng2);
void   updateSafeZone();
void   triggerSafeZoneAlarm(const String& reason);
void   mqttPublishAlarm(const String& reason);
void   updateSafeZoneCalls();

// MQTT
void loadMqttConfig();
void saveMqttConfig();
void setupMQTT();
void updateMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int len);
void mqttPublishStatus();
void mqttPublishAck(const String& cmdId, const String& status);
void mqttHandleCommand(const String& payload);

// ===================== MQTT =====================
void loadMqttConfig() {
  preferences.begin("sim800", true);
  // mqttHost حذف شد - هاردکد smartcarlock.ir
  mqttPort    = preferences.getUShort("mPort", 1883);
  mqttUser    = preferences.getString("mUser", "");
  mqttPass    = preferences.getString("mPass", "");
  deviceToken = preferences.getString("mToken", "");
  mqttEnabled = preferences.getBool("mEn", false);
  // [CHANGE] چک‌باکس Enable GPS حذف شد - GPS همیشه فعال است
  gpsEnabled  = true;
  gpsSite     = preferences.getInt("gSite", 0);
  safeZoneEnabled = preferences.getBool("szEn", false);
  safeZoneLat = preferences.getString("szLat", "0").toDouble();
  safeZoneLng = preferences.getString("szLng", "0").toDouble();
  safeZoneRadiusM = preferences.getUInt("szRad", 100);
  gpsAntiCutEnabled = preferences.getBool("gpsAC", false);
  preferences.end();
  if (mqttPort < 1 || mqttPort > 65535) mqttPort = 1883;
  if (gpsSite < 0 || gpsSite > 2) gpsSite = 0;
  if (safeZoneRadiusM < 1 || safeZoneRadiusM > 100000) safeZoneRadiusM = 100;
  if (safeZoneEnabled && safeZoneLat == 0 && safeZoneLng == 0) safeZoneEnabled = false;
}

void saveMqttConfig() {
  preferences.begin("sim800", false);
  // mqttHost حذف شد
  preferences.putUShort("mPort", mqttPort);
  preferences.putString("mUser", mqttUser);
  preferences.putString("mPass", mqttPass);
  preferences.putString("mToken", deviceToken);
  preferences.putBool("mEn", mqttEnabled);
  preferences.putBool("gEn", gpsEnabled);
  preferences.putInt("gSite", gpsSite);
  preferences.putBool("szEn", safeZoneEnabled);
  preferences.putString("szLat", String(safeZoneLat, 6));
  preferences.putString("szLng", String(safeZoneLng, 6));
  preferences.putUInt("szRad", safeZoneRadiusM);
  preferences.putBool("gpsAC", gpsAntiCutEnabled);
  preferences.end();
}

void mqttPublishStatus() {
  if (!mqttConnected) return;
  String base = "car/" + deviceToken;
  String j = "{";
  j += "\"ts\":"           + String(millis() / 1000) + ",";
  j += "\"ip\":"           + String(WiFi.softAPIP().toString().length() ? "\"" + WiFi.softAPIP().toString() + "\"" : "\"\"") + ",";
  j += "\"sim_ready\":"    + String(sim800Ready ? "true" : "false") + ",";
  j += "\"sim_csq\":"      + String(gsmCSQ) + ",";
  j += "\"sim_pct\":"      + String(gsmPercent) + ",";
  j += "\"gprs_reg\":"     + String(gprsRegistered ? "true" : "false") + ",";
  j += "\"gprs_att\":"     + String(gprsAttached ? "true" : "false") + ",";
  j += "\"carrier\":\""    + jsonEscape(sim800Carrier) + "\",";
  j += "\"car_ip\":\""     + jsonEscape(sim800IP) + "\",";
  j += "\"locked\":"       + String(carLocked ? "true" : "false") + ",";
  j += "\"sec_locked\":"   + String(secLocked ? "true" : "false") + ",";
  j += "\"security\":"     + String(securityEnabled ? "true" : "false") + ",";
  j += "\"alarm\":"        + String(alarmManual ? "true" : "false") + ",";
  j += "\"doors\":"        + String(outputState[0] ? "true" : "false") + ",";
  j += "\"fuel\":"         + String(outputState[2] ? "true" : "false") + ",";
  j += "\"switch\":"       + String(outputState[3] ? "true" : "false") + ",";
  j += "\"safe_zone_alarm\":"  + String(safeZoneAlarmActive ? "true" : "false") + ",";
  j += "\"safe_zone_violated\":"+ String(safeZoneViolated ? "true" : "false") + ",";
  j += "\"gps\":"              + gpsStatusJson();
  j += "}";
  mqttClient.publish((base + "/status").c_str(), j.c_str(), true);
  mqttLastPublishMs = millis();
}

void mqttPublishAck(const String& cmdId, const String& status) {
  if (!mqttConnected) return;
  String base = "car/" + deviceToken;
  String j = "{\"id\":\"" + jsonEscape(cmdId) + "\",\"status\":\"" + jsonEscape(status) + "\",\"ts\":" + String(millis() / 1000) + "}";
  mqttClient.publish((base + "/ack").c_str(), j.c_str(), false);
  Serial.println("MQTT ack: " + cmdId + " -> " + status);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t = String(topic);
  String msg = "";
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.println("MQTT in [" + t + "] " + msg);
  mqttHandleCommand(msg);
}

// command JSON: {"id":"xxx","cmd":"lock"|"unlock"|"location"|"sms"|"alarm_on"|"alarm_off"|"toggle","pin":0..4,"on":true|false,"to":"+98912...","text":"..."}
void mqttHandleCommand(const String& payload) {
  String id = "";
  int p = payload.indexOf("\"id\":");
  if (p >= 0) {
    int q1 = payload.indexOf('"', p + 5);
    int q2 = (q1 >= 0) ? payload.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) id = payload.substring(q1 + 1, q2);
  }
  String cmd = "";
  p = payload.indexOf("\"cmd\":");
  if (p >= 0) {
    int q1 = payload.indexOf('"', p + 6);
    int q2 = (q1 >= 0) ? payload.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) cmd = payload.substring(q1 + 1, q2);
  }
  cmd.toLowerCase();

    if (cmd == "lock") {
    cancelActiveCallAndLocation();
    applyLock(false, false);   // [CHANGE] فقط قطع سوئیچ و پمپ بنزین - درب‌ها و صندوق عقب دست نمی‌خورند
    mqttPublishAck(id, "ok");
    mqttPublishStatus();
    return;
  }
  if (cmd == "unlock") {
    cancelActiveCallAndLocation();
    unlockCar();
    mqttPublishAck(id, "ok");
    mqttPublishStatus();
    return;
  }
  if (cmd == "alarm_on") {
  alarmManual = true;
  updateBuzzer();
  pendingAlarmSms       = true;                        // ← جدید
  pendingAlarmSmsReason = "Manual alarm activated";    // ← جدید
  mqttPublishAck(id, "ok");
  mqttPublishStatus();
  return;
}
  if (cmd == "alarm_off") {
    cancelActiveCallAndLocation();
    alarmManual = false;
    safeZoneAlarmActive  = false;
    safeZoneCallIdx      = 0;
    safeZoneCallAnswered = false;
    pendingAlarmSms       = false;
    pendingAlarmSmsReason = "";
    locSmsActive         = false;
    locSmsTargetNumber   = "";
    updateBuzzer();
    mqttPublishAck(id, "ok");
    mqttPublishStatus();
    return;
  }
  if (cmd == "location") {
    if (gpsAvailable()) { mqttPublishAck(id, "ok"); mqttPublishStatus(); return; }
    if (!sim800InternetEnabled) { mqttPublishAck(id, "error_no_internet"); return; }
    startLocation("mqtt:" + id);
    mqttPublishAck(id, "pending");
    return;
  }
  if (cmd == "toggle") {
    int pin = 0;
    p = payload.indexOf("\"pin\":");
    if (p >= 0) pin = payload.substring(p + 6).toInt();
    bool on = payload.indexOf("\"on\":true") >= 0;
    if (pin >= 0 && pin < 5) {
      if (pin == 0) setDoors(on);
      else if (pin == 1) triggerTrunk();
      else { setOutput(pin, on); desiredState[pin] = on; }
      mqttPublishAck(id, "ok");
      mqttPublishStatus();
    } else mqttPublishAck(id, "error_bad_pin");
    return;
  }
  if (cmd == "sms") {
  String to = "";
  int p = payload.indexOf("\"to\":");
  if (p >= 0) {
    int q1 = payload.indexOf('"', p + 5);
    int q2 = (q1 >= 0) ? payload.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) to = payload.substring(q1 + 1, q2);
  }
  String text = "";
  p = payload.indexOf("\"text\":");
  if (p >= 0) {
    int q1 = payload.indexOf('"', p + 7);
    int q2 = (q1 >= 0) ? payload.lastIndexOf('"') : -1;
    if (q1 >= 0 && q2 > q1) text = payload.substring(q1 + 1, q2);
  }
  if (to.length() > 0 && text.length() > 0) {
    bool ok = sendSms(to, text);
    mqttPublishAck(id, ok ? "ok" : "error_sms");
  } else mqttPublishAck(id, "error_bad_args");
  return;
}
  mqttPublishAck(id, "error_unknown_cmd");
}

void setupMQTT() {
  if (!mqttEnabled || deviceToken.length() == 0) return;
  Serial.println("MQTT: configuring client for " MQTT_HOST ":" + String(mqttPort));
  mqttClient.setClient(sim800TcpClient);
  mqttClient.setServer(MQTT_HOST, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
  
  mqttConnected = false;
  mqttLastTryMs = 0;
}

void updateMQTT() {
  if (!mqttEnabled || deviceToken.length() == 0) {
    if (mqttConnected) { mqttClient.disconnect(); mqttConnected = false; }
    return;
  }
  if (!sim800Ready || gprsRegistered == false) return;

  // [PATCH 2] هنگام اجرای درخواست لوکیشن (LBS)، اتصال TCP جدید MQTT شروع نشود:
  // mqttClient.connect بلاک‌کننده است (تا ~190s) و حامل (bearer) اینترنت را می‌کُشد.
  if (!mqttConnected && locPhase != 0) return;

  if (mqttConnected && !mqttClient.connected()) {
    Serial.println("MQTT: lost connection");
    mqttConnected = false;
  }

  if (!mqttConnected && millis() - mqttLastTryMs > MQTT_RETRY_MS) {
    mqttLastTryMs = millis();
    Serial.println("MQTT: connecting...");
    if (mqttClient.connect(deviceToken.c_str(), mqttUser.length() ? mqttUser.c_str() : NULL,
                           mqttPass.length() ? mqttPass.c_str() : NULL)) {
      mqttConnected = true;
      String base = "car/" + deviceToken;
      mqttClient.subscribe((base + "/cmd").c_str());
      Serial.println("MQTT: connected, subscribed to " + base + "/cmd");
      mqttPublishStatus();
    } else {
      Serial.println("MQTT: connect failed, rc=" + String(mqttClient.state()));
    }
  }

  if (mqttConnected) {
    mqttClient.loop();
    if (millis() - mqttLastPublishMs > 15000UL) mqttPublishStatus();
  }
}

// Safe Zone prototypes (must be before setup/loop)
double haversineMeters(double lat1, double lng1, double lat2, double lng2);
void   updateSafeZone();
void   triggerSafeZoneAlarm(const String& reason);
void   mqttPublishAlarm(const String& reason);
void   updateSafeZoneCalls();
void   checkSafeZoneCallStatus();   // جدید

// ===================== Setup =====================
void setup() {
  // خروجی‌ها را قبل از هر چیز در حالت امن LOW قرار بده (جلوگیری از گلیچ)
  for (int i = 0; i < OUTPUT_COUNT; i++) {
    digitalWrite(outputPins[i], LOW);
    pinMode(outputPins[i], OUTPUT);
    digitalWrite(outputPins[i], LOW);
  }

  digitalWrite(PIN_BUZZER, BUZZER_OFF_LEVEL);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, BUZZER_OFF_LEVEL);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Car Controller (ESP32-S3) Starting ===");

  initPrefsNamespaces();
  initPulses();

  loadAllSettings();
  loadDevices();
  loadCustomNames();
  loadSim800Config();
  loadMqttConfig();
  loadLockState();

  for (int i = 0; i < OUTPUT_COUNT; i++) {
    if (i == 4) continue;
    if (i == 1) { desiredState[i] = false; continue; }
    desiredState[i] = outputState[i];
  }

  setupWiFiAP();
  setupWebServer();
  setupSim800();
  setupGPS();
  setupMQTT();
  setupOTA();                       // ← OTA
  Serial.print("AP IP: ");   Serial.println(WiFi.softAPIP());

  Serial.print("AP IP: ");   Serial.println(WiFi.softAPIP());
  Serial.print("SSID: ");    Serial.println(ap_ssid);
  Serial.print("Signal Threshold: "); Serial.println(String(signalThreshold) + "%");
  Serial.print("Weak Auto Disconnect: "); Serial.println(weakAutoDisconnectEnabled ? "ON" : "OFF");
  Serial.println("JSON API ready on /api/*");

  // [PATCH-S] راهنمای کنسول سریال
  Serial.println("Serial console ready: type a command + Enter (same words as SMS)");
  Serial.println("Commands: " + smsCmd[0] + " [1-120 min] | " + smsCmd[1] + " | " + smsCmd[2] + " | " +
                 smsCmd[3] + " | " + smsCmd[4] + " | " + smsCmd[5] + " | " + smsCmd[6] + " | ON/OFF [0-3]");
}

void loop() {
  ArduinoOTA.handle();              // ← OTA

  handleSerialCommands();           // [PATCH-S] خواندن فرمان‌های Serial Monitor

  server.handleClient();

  handlePulses();
  updateSim800();
  updateGPS();
  updateMQTT();
  updateSafeZone();
  updateSafeZoneCalls();
  updateSecurity();
  updateBuzzer();

  // ← بازه سیگنال: ۱۰ ثانیه اگر auto فعال، ۳۰ ثانیه اگر غیرفعال
  unsigned long wifiInterval = weakAutoDisconnectEnabled ? 10000UL : 30000UL;
  if (millis() - lastWeakCheck >= wifiInterval) {
    lastWeakCheck = millis();
    handleWeakSignalProtection();
  }

  // ← بررسی اتصال کاربر مجاز به AP
  if (millis() - lastWifiCheck >= 2000UL) {
    lastWifiCheck = millis();
    checkWifiConnection();
  }

  delay(1);
}

// ساخت namespace ها (در اولین بوت، begin(readOnly) روی namespace ناموجود خطا می‌دهد)
void initPrefsNamespaces() {
  const char* spaces[] = {"carctrl", "devices", "sim800"};
  for (int i = 0; i < 3; i++) {
    preferences.begin(spaces[i], false);
    preferences.end();
  }
}

// ===================== OTA =====================
void setupOTA() {
  MDNS.begin(OTA_HOSTNAME);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(ACTIVATION_CODE);
  ArduinoOTA.setPort(3232);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: end");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready: " OTA_HOSTNAME " / pass=" ACTIVATION_CODE);
}

// ===================== SMS send (non-blocking-safe) =====================
bool sendSms(const String& to, const String& text) {
  if (!sim800Ready || locPhase != 0) {
    Serial.println("SMS: skipped (ready=" + String(sim800Ready?"Y":"N") +
                   " locPhase=" + String(locPhase) + ") to=" + to);
    return false;
  }

  sim800RxBuf = "";
  g_smsOk     = false;
  g_smsFail   = false;

  sim800Serial.print("AT+CMGS=\"");
  sim800Serial.print(to);
  sim800Serial.print("\"\r");

  // ===== انتظار برای prompt > با تشخیص خطا =====
  unsigned long st = millis();
  bool promptFound = false;
  bool cmdRejected = false;
  while (millis() - st < 5000) {
    // ابتدا بدون پردازش newline، مستقیم از Serial بخوان (برای جلوگیری از merge با URC)
    while (sim800Serial.available()) {
      char c = (char)sim800Serial.read();
      if (c == '>') {
        promptFound = true;
        // پاک کردن باقی‌مانده‌ی این batch
        while (sim800Serial.available()) sim800Serial.read();
        break;
      }
      sim800RxBuf += c;
      if (sim800RxBuf.indexOf("ERROR") >= 0 || sim800RxBuf.indexOf("+CMS") >= 0) {
        Serial.println("SMS: AT+CMGS rejected -> " + sim800RxBuf);
        sim800RxBuf = "";
        cmdRejected = true;
        break;
      }
      if (sim800RxBuf.length() > 256) sim800RxBuf = "";
    }
    if (promptFound || cmdRejected) break;
    delay(10);
  }

  if (cmdRejected) return false;
  if (!promptFound) {
    Serial.println("SMS: no > prompt, to=" + to);
    return false;
  }

  // ===== ارسال متن + Ctrl+Z =====
  sim800Serial.print(text);
  sim800Serial.write(26);

  // ===== انتظار برای +CMGS یا +CMS ERROR =====
  st = millis();
  while (millis() - st < 15000) {   // ۱۵ ثانیه کافی است
    sim800PumpRx();
    if (g_smsOk)   { g_smsOk   = false; Serial.println("SMS OK to " + to); return true;  }
    if (g_smsFail) { g_smsFail = false; Serial.println("SMS FAIL to " + to + " (+CMS ERROR)"); return false; }
    delay(10);
  }
  Serial.println("SMS: timeout waiting for +CMGS, to=" + to + " len=" + String(text.length()));
  return false;
}

// ===================== WiFi presence & reset =====================
void resetAllSecurity() {
  bool hadAlarm = safeZoneAlarmActive || alarmManual || secLocked || carLocked;

  // ----- Safe Zone alarm -----
  safeZoneAlarmActive  = false;
  safeZoneCallIdx      = 0;
  safeZoneCallAnswered = false;
  safeZoneViolated     = false;
  safeZoneNoFixSince   = 0;
  gpsNoFixSince        = 0;

  // ----- Manual alarm -----
  alarmManual = false;

  // ----- Security runtime -----
  secState      = 0;
  gsmBadSince   = 0;
  secGraceStart = 0;
  secLocked     = false;

  // ===== مورد 4 و 6: آزادسازی کامل خودرو =====
  carLocked = false;
  setOutput(2, true);  desiredState[2] = true;   // پمپ بنزین ON
  setOutput(3, true);  desiredState[3] = true;   // سوئیچ خودرو ON
  if (outputState[0]) setDoors(false);           // فقط اگر درب‌ها واقعاً قفل بودند، پالس باز شدن

  // ----- Pending alarm SMS -----
  pendingAlarmSms       = false;
  pendingAlarmSmsReason = "";

  // ----- Location SMS loop -----
  locSmsActive       = false;
  locSmsTargetNumber = "";

  // ----- End active call -----
  if (callActive && locPhase == 0) {
    sim800SendAT("ATH", "OK", 1500);
    callActive   = false;
    activeCaller = "";
  }

  saveLockState();
  updateBuzzer();

  if (hadAlarm) Serial.println("WiFi: all alarms cleared, car fully unlocked (fuel+switch ON, doors open)");
}

void checkWifiConnection() {
  static unsigned long firstConnectedAt = 0;

  bool nowConnected = authorizedClientConnected();

  // [PATCH 6] قبلاً این خط در هر چرخه‌ی ۲ ثانیه‌ای اجرا می‌شد و firstConnectedAt
  // دائماً Refresh می‌شد؛ در نتیجه شرط ۳ ثانیه هیچ‌وقت true نمی‌شد و reset اجرا نمی‌شد.
  if (nowConnected && !wifiUserWasConnected && firstConnectedAt == 0) {
    firstConnectedAt = millis();       // شروع پنجره تثبیت (فقط یک‌بار)
  }

  // فقط اگر حداقل ۳ ثانیه پایدار وصل مانده باشد، reset کن
  if (nowConnected && !wifiUserWasConnected && firstConnectedAt != 0 &&
      millis() - firstConnectedAt >= 3000UL) {
    Serial.println("WiFi: authorized device connected (stable) -> reset alarms");
    resetAllSecurity();
    wifiUserWasConnected = true;
    firstConnectedAt = 0;
  }

  if (!nowConnected) {
    if (wifiUserWasConnected) Serial.println("WiFi: authorized device disconnected");
    wifiUserWasConnected = false;
    firstConnectedAt = 0;
  }
}
// ===================== GPS =====================
void setupGPS() {
  if (!gpsEnabled) return;
  if (gpsSerialStarted) { gpsFirstSearchMs = millis(); return; }
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  gpsSerialStarted = true;
  gpsFirstSearchMs = millis();
  Serial.println("GPS: serial started on pins RX=" + String(GPS_RX_PIN) + " TX=" + String(GPS_TX_PIN));
}

void updateGPS() {
  if (!gpsEnabled) return;
  while (gpsSerial.available() > 0) gps.encode(gpsSerial.read());

  if (gps.location.isValid() && gps.location.age() < 5000) {
    gpsLat = gps.location.lat();
    gpsLng = gps.location.lng();
    gpsLastFixMs = millis();
    gpsFix = true;
    gpsSatellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
    gpsHdop = gps.hdop.isValid() ? gps.hdop.hdop() : 0;
  } else if (gpsFix && millis() - gpsLastFixMs > GPS_STALE_MS) {
    gpsFix = false;
    Serial.println("GPS: fix stale");
  }
}

bool gpsAvailable() { return gpsEnabled && gpsFix; }
double haversineMeters(double lat1, double lng1, double lat2, double lng2);
void   updateSafeZone();
void   triggerSafeZoneAlarm(const String& reason);
void   mqttPublishAlarm(const String& reason);
void   updateSafeZoneCalls();

String gpsMapUrl() {
  if (!gpsFix) return "";
  String lat = String(gpsLat, 6);
  String lng = String(gpsLng, 6);
  if (gpsSite == 0) return "https://maps.google.com/?q=" + lat + "," + lng;
  if (gpsSite == 1) return "https://neshan.org/maps/@" + lat + "," + lng + ",16z";
  return "https://balad.ir/location?latitude=" + lat + "&longitude=" + lng;
}

String gpsStatusJson() {
  String j = "{";
  j += "\"enabled\":"       + String(gpsEnabled ? "true" : "false") + ",";
  j += "\"fix\":"           + String(gpsFix ? "true" : "false") + ",";
  j += "\"lat\":"           + String(gpsLat, 6) + ",";
  j += "\"lng\":"           + String(gpsLng, 6) + ",";
  j += "\"sats\":"          + String(gpsSatellites) + ",";
  j += "\"hdop\":"          + String(gpsHdop, 2) + ",";
  j += "\"site\":"          + String(gpsSite) + ",";
  j += "\"gpsAntiCut\":" + String(gpsAntiCutEnabled ? "true" : "false") + ",";
  j += "\"url\":\""         + jsonEscape(gpsMapUrl()) + "\",";
  j += "\"safeZoneEnabled\":"+ String(safeZoneEnabled ? "true" : "false") + ",";
  j += "\"safeZoneLat\":"   + String(safeZoneLat, 6) + ",";
  j += "\"safeZoneLng\":"   + String(safeZoneLng, 6) + ",";
  j += "\"safeZoneRadius\":"+ String(safeZoneRadiusM) + ",";
  j += "\"safeZoneViolated\":"+ String(safeZoneViolated ? "true" : "false") + ",";
  j += "\"safeZoneAlarm\":" + String(safeZoneAlarmActive ? "true" : "false");
  if (gpsEnabled && !gpsFix) {
    unsigned long elapsed = millis() - gpsFirstSearchMs;
    j += ",\"searching\":" + String(elapsed < GPS_FIX_TIMEOUT_MS ? "true" : "false");
  }
  j += "}";
  return j;
}

// ===================== Safe Zone =====================
double haversineMeters(double lat1, double lng1, double lat2, double lng2) {
  const double R = 6371000.0;
  double phi1 = lat1 * DEG_TO_RAD;
  double phi2 = lat2 * DEG_TO_RAD;
  double dphi = (lat2 - lat1) * DEG_TO_RAD;
  double dlng = (lng2 - lng1) * DEG_TO_RAD;
  double a = sin(dphi/2) * sin(dphi/2) +
             cos(phi1) * cos(phi2) * sin(dlng/2) * sin(dlng/2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

void updateSafeZone() {
  // ===================== GPS Anti-Cut (مستقل از Safe Zone) =====================
  static unsigned long lastAc = 0;
  if (gpsEnabled && gpsAntiCutEnabled && (millis() - lastAc > 5000UL)) {
    lastAc = millis();
    // «تازه» = همین الان fix معتبر داریم (نه ۲ دقیقه پس از قطع)
    bool freshFix = gps.location.isValid() && gps.location.age() < 5000;

    if (!freshFix) {
      if (gpsNoFixSince == 0) {
        gpsNoFixSince = millis();
        Serial.println("GPS-AC: no fresh fix - timer started");
      } else if (millis() - gpsNoFixSince > GPS_ANTICUT_TIMEOUT_MS && !safeZoneAlarmActive) {
        triggerSafeZoneAlarm("GPS antenna cut (no fix for " +
                             String(GPS_ANTICUT_TIMEOUT_MS / 1000UL) + "s)");
      }
    } else {
      if (gpsNoFixSince != 0) Serial.println("GPS-AC: fresh fix restored - timer reset");
      gpsNoFixSince = 0;
    }
  }

  // ===================== Safe Zone =====================
  // اگر GPS کلاً خاموش شد، آلارم را پاک کن
  if (!gpsEnabled) {
    if (safeZoneAlarmActive) {
      Serial.println("Safe zone: cleared (GPS disabled)");
      safeZoneAlarmActive  = false;
      safeZoneCallIdx      = 0;
      safeZoneCallAnswered = false;
    }
    safeZoneViolated   = false;
    safeZoneNoFixSince = 0;
    gpsNoFixSince      = 0;
    return;
  }

  // ←←← اصلاح حیاتی: اگر Safe Zone خاموش است، آلارم Anti-Cut را پاک نکن
  if (!safeZoneEnabled) {
    safeZoneViolated   = false;
    safeZoneNoFixSince = 0;
    return;
  }

  if (millis() - safeZoneLastCheck < SAFE_ZONE_CHECK_MS) return;
  safeZoneLastCheck = millis();

  if (!gpsFix) {
    if (safeZoneNoFixSince == 0) safeZoneNoFixSince = millis();
    else if (millis() - safeZoneNoFixSince > SAFE_ZONE_NO_FIX_TIMEOUT_MS && !safeZoneAlarmActive) {
      triggerSafeZoneAlarm("GPS signal lost for " + String(SAFE_ZONE_NO_FIX_TIMEOUT_MS / 1000) + "s");
    }
    return;
  }
  safeZoneNoFixSince = 0;

  if (safeZoneLat == 0 && safeZoneLng == 0) return;

  double dist = haversineMeters(gpsLat, gpsLng, safeZoneLat, safeZoneLng);
  if (dist > safeZoneRadiusM) {
    safeZoneViolated = true;
    if (!safeZoneAlarmActive) {
      triggerSafeZoneAlarm("Left safe zone (" + String((int)dist) + "m from center)");
    }
  } else {
    safeZoneViolated = false;
  }
}

void triggerSafeZoneAlarm(const String& reason) {
  if (safeZoneAlarmActive) return;
  safeZoneAlarmActive  = true;
  safeZoneCallIdx      = 0;
  safeZoneCallStart    = 0;
  safeZoneCallAnswered = false;
  safeZoneCallLastPoll = 0;

  Serial.println("!!! SAFE ZONE ALARM: " + reason);

  if (callActive) {
    sim800SendAT("ATH", "OK", 1500);
    callActive   = false;
    activeCaller = "";
  }

  applyLock(false, false);
  alarmManual = true;
  updateBuzzer();

  pendingAlarmSms       = true;
  pendingAlarmSmsReason = reason;

  mqttPublishAlarm(reason);
  mqttPublishStatus();
}

void mqttPublishAlarm(const String& reason) {
  if (!mqttConnected) { Serial.println("MQTT not connected - alarm only local"); return; }
  String base = "car/" + deviceToken;
  String j = "{";
  j += "\"type\":\"safe_zone\",";
  j += "\"reason\":\"" + jsonEscape(reason) + "\",";
  j += "\"lat\":"     + String(gpsLat, 6) + ",";
  j += "\"lng\":"     + String(gpsLng, 6) + ",";
  j += "\"sats\":"    + String(gpsSatellites) + ",";
  j += "\"locked\":"  + String(carLocked ? "true" : "false") + ",";
  j += "\"ts\":"      + String(millis() / 1000);
  j += "}";
  mqttClient.publish((base + "/alarm").c_str(), j.c_str(), true);
  Serial.println("MQTT alarm published: " + reason);
}

// =====================================================================
//    Safe Zone Calls (v2): 30s per number, AT+CLCC detection,
//    hang up previous call on alarm, stuck-detection.
// =====================================================================
void checkSafeZoneCallStatus() {
  if (locPhase != 0) return;
  if (!sim800Ready) return;

  String r = sim800Exchange("AT+CLCC", 1500);

  int idx = r.indexOf("+CLCC:");
  if (idx < 0) {
    if (callActive) {
      Serial.println("SAFE-ZONE: call ended (CLCC shows no active call)");
      callActive = false;
      activeCaller = "";
      safeZoneCallStart = millis();
    }
    return;
  }

  // +CLCC: <idx>,<dir>,<stat>,<mode>,<mpty>,"<number>",<type>
  // stat: 0=active(answer)  1=held  2=dialing  3=alerting  4=incoming  5=waiting
  int c1 = r.indexOf(',', idx);
  int c2 = (c1 > 0) ? r.indexOf(',', c1 + 1) : -1;
  int c3 = (c2 > 0) ? r.indexOf(',', c2 + 1) : -1;
  if (c2 > 0 && c3 > 0) {
    int stat = r.substring(c2 + 1, c3).toInt();
    if (stat == 0 && !safeZoneCallAnswered) {
      Serial.println("SAFE-ZONE: call ANSWERED by " + activeCaller +
                     " - holding until user hangs up");
      safeZoneCallAnswered = true;
      safeZoneCallIdx = MAX_PHONE_NUMBERS;   // no more numbers
    }
  }
}

void updateSafeZoneCalls() {  
  if (!safeZoneAlarmActive) return;
  if (!sim800Enabled) {
    static bool lg1 = false;
    if (!lg1) { lg1 = true; Serial.println("SAFE-ZONE: SIM800 DISABLED -> cannot call"); }
    return;
  }
  if (!sim800Ready) {
    static bool lg2 = false;
    if (!lg2) { lg2 = true; Serial.println("SAFE-ZONE: SIM800 NOT READY -> waiting"); }
    return;
  }
  if (locPhase != 0) return;   // don't interrupt LBS state machine

  unsigned long now = millis();

  // --- active call handling ---
  if (callActive) {
    if (safeZoneCallAnswered) return;   // answered - hold until user hangs up

    // stuck detection: callActive never resolved
    if (now - safeZoneCallStart > (SAFE_ZONE_CALL_TIMEOUT_MS + 60000UL)) {
      Serial.println("SAFE-ZONE: callActive stuck >90s -> forcing reset");
      sim800SendAT("ATH", "OK", 1500);
      callActive = false;
      activeCaller = "";
      safeZoneCallStart = now;
      return;
    }

    // poll call status every 3s
    if (now - safeZoneCallLastPoll >= SAFE_ZONE_CALL_POLL_MS) {
      safeZoneCallLastPoll = now;
      checkSafeZoneCallStatus();
    }
    if (safeZoneCallAnswered) return;
    if (!callActive) return;   // call ended between polls

    // 30s timeout -> hangup, next number
    if (now - callStart >= SAFE_ZONE_CALL_TIMEOUT_MS) {
      Serial.println("SAFE-ZONE: 30s no answer -> hanging up, next number");
      sim800SendAT("ATH", "OK", 2000);
      callActive = false;
      activeCaller = "";
      safeZoneCallStart = now;
    }
    return;
  }

  // --- all numbers tried -> wait for user ---
  if (safeZoneCallIdx >= MAX_PHONE_NUMBERS) return;

  // --- gap between calls ---
  if (safeZoneCallStart > 0 && now - safeZoneCallStart < SAFE_ZONE_CALL_GAP_MS) return;

  // --- skip empty numbers ---
  while (safeZoneCallIdx < MAX_PHONE_NUMBERS && phoneNumbers[safeZoneCallIdx].length() < 7) {
    safeZoneCallIdx++;
  }
  if (safeZoneCallIdx >= MAX_PHONE_NUMBERS) {
    static bool lg3 = false;
    if (!lg3) { lg3 = true; Serial.println("SAFE-ZONE: no valid phone numbers -> waiting for user (SMS/MQTT/WiFi)"); }
    return;
  }

  // --- dial ---
  // قبل از شماره‌گیری: اگر SMS آلارم هنوز ارسال نشده، حالا بفرست
if (pendingAlarmSms && sim800Ready && locPhase == 0) {
  pendingAlarmSms = false;
  String msg = "ALARM: " + pendingAlarmSmsReason;
  if (gpsAvailable()) msg += "\nLocation: " + gpsMapUrl();
  for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
    if (phoneNumbers[i].length() >= 7) sendSms(phoneNumbers[i], msg);
  }
  Serial.println("SAFE-ZONE: alarm SMS sent to all authorized numbers before calls");
}

// --- dial ---
String num = phoneNumbers[safeZoneCallIdx];
  Serial.println("SAFE-ZONE: dialing " + num +
                 " (" + String(safeZoneCallIdx + 1) + "/" + String(MAX_PHONE_NUMBERS) + ")");

  sim800Serial.print("ATD");
  sim800Serial.print(num);
  sim800Serial.print(";\r");
  sim800Serial.flush();

  callActive            = true;
  safeZoneCallAnswered  = false;
  safeZoneCallLastPoll  = now;
  callStart             = now;
  activeCaller          = num;
  safeZoneCallIdx++;
  safeZoneCallStart     = now;
}
// ===================== Pulse Management =====================
void initPulses() {
  for (int i = 0; i < MAX_PULSES; i++) {
    pulses[i].active = false;
    pulses[i].pin = -1;
    pulses[i].start = 0;
    pulses[i].duration = 0;
    pulses[i].resetOutputIndex = -1;
  }
}

void startPulse(int pin, unsigned long duration, int resetOutputIndex) {
  cancelPulse(pin);

  for (int i = 0; i < MAX_PULSES; i++) {
    if (!pulses[i].active) {
      pulses[i].active = true;
      pulses[i].pin = pin;
      pulses[i].start = millis();
      pulses[i].duration = duration;
      pulses[i].resetOutputIndex = resetOutputIndex;
      digitalWrite(pin, HIGH);
      return;
    }
  }
}

void cancelPulse(int pin) {
  for (int i = 0; i < MAX_PULSES; i++) {
    if (pulses[i].active && pulses[i].pin == pin) {
      digitalWrite(pin, LOW);
      pulses[i].active = false;
      pulses[i].pin = -1;
      pulses[i].duration = 0;
      pulses[i].resetOutputIndex = -1;
    }
  }
}

void handlePulses() {
  unsigned long now = millis();

  for (int i = 0; i < MAX_PULSES; i++) {
    if (pulses[i].active && (now - pulses[i].start >= pulses[i].duration)) {
      if (pulses[i].pin >= 0) digitalWrite(pulses[i].pin, LOW);

      if (pulses[i].resetOutputIndex >= 0 && pulses[i].resetOutputIndex < OUTPUT_COUNT) {
        outputState[pulses[i].resetOutputIndex] = false;
        desiredState[pulses[i].resetOutputIndex] = false;
      }

      pulses[i].active = false;
      pulses[i].pin = -1;
      pulses[i].duration = 0;
      pulses[i].resetOutputIndex = -1;
    }
  }
}

// قفل/باز کردن درها (مشترک بین وب و SMS)
void setDoors(bool lock) {
  outputState[0] = lock;
  desiredState[0] = lock;

  cancelPulse(PIN_DOOR_DRIVER);
  cancelPulse(PIN_DOORS_INVERSE);
  digitalWrite(PIN_DOOR_DRIVER, LOW);
  digitalWrite(PIN_DOORS_INVERSE, LOW);

  startPulse(lock ? PIN_DOOR_DRIVER : PIN_DOORS_INVERSE, DOOR_PULSE_MS, -1);
}

// پالس صندوق عقب (مشترک بین وب و SMS)
void triggerTrunk() {
  outputState[1] = true;
  desiredState[1] = false;
  startPulse(PIN_TRUNK, TRUNK_PULSE_MS, 1);
}

// ===================== Weak Signal Protection =====================
void saveWeakAutoSetting() {
  preferences.begin("carctrl", false);
  preferences.putBool("weakAuto", weakAutoDisconnectEnabled);
  preferences.end();
}


void handleWeakSignalProtection() {
  if (!weakAutoDisconnectEnabled) {
    weakDisconnectActive = false;
    return;
  }

  bool clientConnected = (WiFi.softAPgetStationNum() > 0);
  int percent = 0;

  if (clientConnected) {
    int rssi = getClientRSSI();
    percent = rssiToPercentage(rssi);
  }

  if (clientConnected && percent >= signalThreshold) {
    if (weakDisconnectActive) {
      Serial.println("Signal recovered - restoring relays");

      for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (i == 4) continue;
        if (i == 1) { desiredState[i] = false; continue; }
        if (outputState[i] != desiredState[i]) setOutput(i, desiredState[i]);
      }

      weakDisconnectActive = false;
    }
  } else {
    if (!weakDisconnectActive) {
      Serial.println("Weak/no signal - cutting active relays");

      for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (i == 4) continue;
        if (i == 1) { desiredState[i] = false; continue; }
        desiredState[i] = outputState[i];
      }

      for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (i == 4) continue;
        if (outputState[i]) setOutput(i, LOW);
      }

      weakDisconnectActive = true;
    } else {
      for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (i == 4) continue;
        if (outputState[i]) setOutput(i, LOW);
      }
    }
  }
}

// ===================== Device Authorization Functions =====================
String getMacAddressString(const uint8_t* mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

// یافتن MAC کلاینت از روی IP:
//  1) جدول ARP لایه lwIP (روی core 2.x و 3.x کار می‌کند و چند کلاینت را پشتیبانی می‌کند)
//  2) در صورت شکست، اگر فقط یک کلاینت وصل است، MAC همان
String getMacByIP(IPAddress ip) {
  // ← چک کش
  unsigned long now = millis();
  for (int i = 0; i < AP_MAX_CLIENTS; i++) {
    if (macCache[i].mac.length() > 0 && macCache[i].ip == ip &&
        now - macCache[i].ts < MAC_CACHE_TTL_MS) {
      return macCache[i].mac;
    }
  }
  esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apNetif) {
    struct netif* n = (struct netif*)esp_netif_get_netif_impl(apNetif);
    if (n) {
      ip4_addr_t a;
      IP4_ADDR(&a, ip[0], ip[1], ip[2], ip[3]);
      struct eth_addr* eth = NULL;
      const ip4_addr_t* ipRet = NULL;
#ifdef LOCK_TCPIP_CORE
      LOCK_TCPIP_CORE();
#endif
      s8_t r = etharp_find_addr(n, &a, &eth, &ipRet);
      String result = "";
      if (r >= 0 && eth) result = getMacAddressString(eth->addr);
#ifdef UNLOCK_TCPIP_CORE
      UNLOCK_TCPIP_CORE();
#endif
      if (result.length() > 0) {
    macCache[macCacheIdx].ip  = ip;
    macCache[macCacheIdx].mac = result;
    macCache[macCacheIdx].ts  = now;
    macCacheIdx = (macCacheIdx + 1) % AP_MAX_CLIENTS;
  }
  return result;
    }
  }

  wifi_sta_list_t wifi_sta_list;
  if (esp_wifi_ap_get_sta_list(&wifi_sta_list) == ESP_OK) {
    if (wifi_sta_list.num == 1) {
      return getMacAddressString(wifi_sta_list.sta[0].mac);
    }
  }

  return "";
}

// ===================== Output Name Functions =====================
void loadCustomNames() {
  preferences.begin("carctrl", true);
  for (int i = 0; i < OUTPUT_COUNT; i++) {
    String key = "name" + String(i);
    customOutputNames[i] = preferences.getString(key.c_str(), "");
  }
  preferences.end();
}

void saveCustomName(int idx) {
  if (idx < 0 || idx >= OUTPUT_COUNT) return;

  preferences.begin("carctrl", false);
  String key = "name" + String(idx);
  preferences.putString(key.c_str(), customOutputNames[idx]);
  preferences.end();
}

// ===================== Signal Threshold =====================
void saveSignalThreshold() {
  preferences.begin("carctrl", false);
  preferences.putInt("sigThresh", signalThreshold);
  preferences.end();
  Serial.println("Signal threshold saved: " + String(signalThreshold) + "%");
}

// ===================== RSSI Logic =====================
// RSSI کلاینتی که IP درخواست را دارد (در صورت نیافتن، اولین کلاینت)
int getClientRSSI() {
  wifi_sta_list_t wifi_sta_list;
  esp_err_t err = esp_wifi_ap_get_sta_list(&wifi_sta_list);
  if (err == ESP_OK && wifi_sta_list.num > 0) {
    int best = -127;
    for (int i = 0; i < wifi_sta_list.num; i++) {
      if (wifi_sta_list.sta[i].rssi > best) best = wifi_sta_list.sta[i].rssi;
    }
    return best;
  }
  return 0;
}

int rssiToPercentage(int rssi) {
  if (rssi == 0) return 0;
  int quality;
  if (rssi <= -100) quality = 0;
  else if (rssi >= -50) quality = 100;
  else quality = 2 * (rssi + 100);
  return constrain(quality, 0, 100);
}

// ===================== Settings Storage =====================
void loadAllSettings() {
  preferences.begin("carctrl", true);

  ap_ssid = preferences.getString("ssid", DEFAULT_SSID);
  ap_password = preferences.getString("pwd", DEFAULT_PASSWORD);

  String ipStr = preferences.getString("ip", DEFAULT_IP);
  ap_ip = stringToIP(ipStr);
  if (ap_ip == INADDR_NONE || !ipIsUsable(ap_ip)) ap_ip = stringToIP(DEFAULT_IP);


  signalThreshold = preferences.getInt("sigThresh", 50);
  if (signalThreshold < 0 || signalThreshold > 100) signalThreshold = 50;

  weakAutoDisconnectEnabled = preferences.getBool("weakAuto", false);


  preferences.end();

  // اعتبارسنجی SSID/Password (در غیر این صورت softAP بدون رمز بالا می‌آید یا شکست می‌خورد)
  if (ap_ssid.length() == 0 || ap_ssid.length() > 32) ap_ssid = DEFAULT_SSID;
  if (ap_password.length() < 8 || ap_password.length() > 63) ap_password = DEFAULT_PASSWORD;
}

// ===================== Wi-Fi Access Point =====================
void setupWiFiAP() {
  WiFi.persistent(false);          // جلوگیری از نوشتن بی‌مورد روی فلش
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);

  bool ok = WiFi.softAP(ap_ssid.c_str(), ap_password.c_str(), AP_CHANNEL, 0, AP_MAX_CLIENTS);
  if (!ok) {
    Serial.println("softAP FAILED - retrying with defaults");
    ok = WiFi.softAP(DEFAULT_SSID, DEFAULT_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  }
  delay(150);

  // IP و DHCP بعد از softAP تنظیم می‌شود (ترتیب مطمئن‌تر)
  for (int attempt = 0; attempt < 3; attempt++) {
    WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
    delay(100);
    if (WiFi.softAPIP() == ap_ip) break;
    Serial.println("softAPConfig retry...");
  }

  WiFi.setSleep(false);            // تاخیر کمتر و پایداری بیشتر وب‌سرور

  Serial.print("Access Point ");
  Serial.println(ok ? "started" : "FAILED");
  Serial.print("AP IP (actual): ");
  Serial.println(WiFi.softAPIP());
}


// ===================== Output Control =====================
void setOutput(int index, bool state) {
  if (index < 0 || index >= OUTPUT_COUNT) return;

  if (index == 0) {
    bool oldState = outputState[0];
    outputState[0] = state;

    cancelPulse(PIN_DOOR_DRIVER);
    cancelPulse(PIN_DOORS_INVERSE);
    digitalWrite(PIN_DOOR_DRIVER, LOW);
    digitalWrite(PIN_DOORS_INVERSE, LOW);

    if (oldState != state) {
      Serial.print(OUT_LOG_NAMES[0]);
      Serial.print(" ");
      Serial.println(state ? "ON" : "OFF");
    }
    return;
  }

  if (index == 1) {
    bool oldState = outputState[1];

    cancelPulse(PIN_TRUNK);
    digitalWrite(PIN_TRUNK, LOW);

    outputState[1] = false;
    desiredState[1] = false;

    if (oldState) {
      Serial.print(OUT_LOG_NAMES[1]);
      Serial.print(" ");
      Serial.println("OFF");
    }
    return;
  }

  if (index == 4) return;

  if ((index == 2 || index == 3) && state && carLocked) {
    Serial.println("Output blocked: car is LOCKED");
    return;
  }

  cancelPulse(outputPins[index]);

  bool oldState = outputState[index];
  outputState[index] = state;
  digitalWrite(outputPins[index], state ? HIGH : LOW);

  if (oldState != state) {
    Serial.print(OUT_LOG_NAMES[index]);
    Serial.print(" ");
    Serial.println(state ? "ON" : "OFF");
  }
}

// ===================== IP Address Parser =====================
IPAddress stringToIP(String ipStr) {
  ipStr.trim();
  int parts[4] = {0, 0, 0, 0};
  int part = 0;
  int count = 0;
  int digits = 0;
  bool valid = true;

  for (int i = 0; i <= (int)ipStr.length(); i++) {
    char c = ipStr[i];
    if (c == '.' || c == '\0') {
      if (digits == 0 || count >= 4) { valid = false; break; }
      parts[count++] = part;
      part = 0;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      part = part * 10 + (c - '0');
      digits++;
      if (part > 255) { valid = false; break; }
    } else {
      valid = false;
      break;
    }
  }

  if (valid && count == 4) return IPAddress(parts[0], parts[1], parts[2], parts[3]);
  return INADDR_NONE;
}

// IP قابل‌استفاده برای AP (نه 0.x ، نه loopback، نه multicast، نه شبکه/broadcast)
bool ipIsUsable(IPAddress ip) {
  if (ip[0] == 0 || ip[0] == 127 || ip[0] >= 224) return false;
  if (ip[3] == 0 || ip[3] == 255) return false;
  return true;
}

// ===================== Settings Menu Page =====================
// =====================================================================
//                              SIM800
// =====================================================================
#define URC_QUEUE_SIZE 8
String urcQueue[URC_QUEUE_SIZE];
uint8_t urcHead = 0, urcTail = 0;

bool sim800SerialStarted = false;
bool sim800PowerTried    = false;
bool sim800ForceInit     = false;
uint8_t sim800InitFails  = 0;
uint8_t sim800NoReply    = 0;

bool smsDrained = false;                 // پیامک‌های قدیمیِ قبل از بوت یک‌بار جمع شده‌اند؟
unsigned long sim800LastSmsPoll = 0;
unsigned long sim800LastSmsVerify = 0;

// صف فرمان‌هایی که باید بعد از پایان عملیات طولانی (اینترنت/لوکیشن) اجرا شوند
#define PEND_MAX 4
String pendFrom[PEND_MAX];
String pendBody[PEND_MAX];
int pendCount = 0;

const char* DEFAULT_CMDS[SMS_CMD_COUNT] = {"Location", "Lock", "Unlock", "Alarm_on", "Alarm_off", "Stop location", "Status"};
const char* CMD_KEYS[SMS_CMD_COUNT]     = {"cLoc", "cLock", "cUnl", "cAon", "cAof", "cStpL", "cStat"};

// ===================== Helpers =====================
String jsonEscape(const String& in) {
  String o;
  o.reserve(in.length() + 8);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

bool asciiOnly(const String& s) {
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 32 || c > 126) return false;
  }
  return true;
}

// استخراج رشته‌های داخل "..." به ترتیب
int collectQuoted(const String& line, String* arr, int maxN) {
  int cnt = 0, from = 0;
  while (cnt < maxN) {
    int a = line.indexOf('"', from);
    if (a < 0) break;
    int b = line.indexOf('"', a + 1);
    if (b < 0) break;
    arr[cnt++] = line.substring(a + 1, b);
    from = b + 1;
  }
  return cnt;
}

// ===================== Config Load/Save =====================
void loadSim800Config() {
  preferences.begin("sim800", true);
  // [CHANGE] چک‌باکس‌های Enable SIM800 / Enable GPRS حذف شدند - هر دو همیشه فعال‌اند
  sim800Enabled   = true;
  sim800InternetEnabled = true;
  sim800APN       = preferences.getString("apn", "");
  securityEnabled = preferences.getBool("secEn", false);
  for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
    String k = "ph" + String(i);
    phoneNumbers[i] = preferences.getString(k.c_str(), "");
  }
  for (int i = 0; i < SMS_CMD_COUNT; i++) smsCmd[i] = preferences.getString(CMD_KEYS[i], DEFAULT_CMDS[i]);
  preferences.end();
}

void saveSim800Config() {
  preferences.begin("sim800", false);
  preferences.putBool("enabled", sim800Enabled);
  preferences.putBool("netEn", sim800InternetEnabled);
  preferences.putString("apn", sim800APN);
  preferences.putBool("secEn", securityEnabled);
  for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
    String k = "ph" + String(i);
    preferences.putString(k.c_str(), phoneNumbers[i]);
  }
  for (int i = 0; i < SMS_CMD_COUNT; i++) preferences.putString(CMD_KEYS[i], smsCmd[i]);
  preferences.end();
}

// ===================== Lock state (persistent) =====================
void saveLockState() {
  preferences.begin("carctrl", false);
  preferences.putBool("locked", carLocked);
  preferences.putBool("secLk", secLocked);
  preferences.putBool("svFuel", savedFuel);
  preferences.putBool("svSw", savedSwitch);
  preferences.end();
}

void loadLockState() {
  preferences.begin("carctrl", true);
  carLocked   = preferences.getBool("locked", false);
  secLocked   = preferences.getBool("secLk", false);
  savedFuel   = preferences.getBool("svFuel", true);
  savedSwitch = preferences.getBool("svSw", true);
  preferences.end();
  if (carLocked && secLocked) secState = 3;
  if (carLocked) Serial.println("Car is LOCKED (restored from memory)");
}

// lockDoors: آیا درب‌ها هم قفل شوند؟
// bySecurity: آیا قفل امنیتیِ قطع آنتن است؟ (secLocked را ست می‌کند)
void applyLock(bool lockDoors, bool bySecurity) {
  if (!carLocked) {
    savedFuel   = outputState[2];
    savedSwitch = outputState[3];
  }
  carLocked = true;
  if (bySecurity) secLocked = true;    // ← مهم برای تغییر 6

  desiredState[2] = false;
  desiredState[3] = false;
  setOutput(2, false);
  setOutput(3, false);

  if (lockDoors) setDoors(true);

  saveLockState();
  Serial.println(String(bySecurity ? "LOCK (security)" : "LOCK (command)") +
                 String(lockDoors ? " + doors" : ""));
}

// فرمان Unlock: فقط پمپ بنزین و سوئیچ روشن می‌شوند
void unlockCar() {
  carLocked = false;
  secLocked = false;

  setOutput(2, true);  desiredState[2] = true;
  setOutput(3, true);  desiredState[3] = true;

  // ← مورد 2: setDoors(false) حذف شد — Unlock هیچ پالسی روی درب نمی‌زند
  // (چون آلارم درب‌ها را قفل نکرده، پس Unlock هم نباید به درب‌ها دست بزند)

  safeZoneAlarmActive  = false;
  safeZoneCallIdx      = 0;
  safeZoneCallAnswered = false;
  safeZoneViolated     = false;
  safeZoneNoFixSince   = 0;

  alarmManual = false;
  updateBuzzer();

  secState      = 0;
  secGraceStart = 0;
  gsmBadSince   = 0;
  securitySuppressed = true;   // ← مورد 8: تا برگشت GSM از قفل امنیتی جلوگیری کن

  if (callActive) {
    if (locPhase == 0) sim800SendAT("ATH", "OK", 1500);
    callActive   = false;
    activeCaller = "";
  }

  locSmsActive       = false;
  locSmsTargetNumber = "";

  saveLockState();
  Serial.println("UNLOCK: fuel+switch ON, all security reset");
}


void releaseLock(bool restore) {
  bool was = carLocked;
  carLocked = false;
  secLocked = false;
  if (was && restore) {
    if (savedFuel)   { setOutput(2, true); desiredState[2] = true; }
    if (savedSwitch) { setOutput(3, true); desiredState[3] = true; }
  }
  saveLockState();
  Serial.println("Lock released");
}

// ===================== Buzzer + Security =====================
bool authorizedClientConnected() {
  // ۱) اپ باز است و در چند ثانیه‌ی اخیر درخواست معتبر داده
  unsigned long now = millis();
  for (int i = 0; i < devCount; i++) {
    if (devSeen[i] != 0 && now - devSeen[i] < AUTH_ACTIVE_MS) return true;
  }
  // ۲) گوشیِ دارای توکن (با آخرین MAC شناخته‌شده) هنوز به وای‌فای دستگاه وصل است، حتی اگر اپ بسته باشد
  wifi_sta_list_t l;
  if (esp_wifi_ap_get_sta_list(&l) != ESP_OK) return false;
  for (int i = 0; i < l.num; i++) {
    String m = getMacAddressString(l.sta[i].mac);
    for (int j = 0; j < devCount; j++) {
      if (devs[j].mac.length() > 0 && devs[j].mac == m) return true;
    }
  }
  return false;
}

bool gsmHealthy() {
  return sim800Enabled && sim800Ready && gprsRegistered && gsmCSQ != 99;
}

void updateBuzzer() {
  static bool last = false;
  bool on = alarmManual || (secState == 1 && ((millis() / 500) % 2 == 0));
  if (on != last) {
    digitalWrite(PIN_BUZZER, on ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);
    last = on;
  }
}

// 0=عادی  1=مهلت (بازر)  2=کاربر وصل شد (تا برگشت آنتن)  3=قفل امنیتی
void updateSecurity() {
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  if (!securityEnabled) {
    if (secState == 3 && secLocked) releaseLock(true);
    secState    = 0;
    gsmBadSince = 0;
    return;
  }

  // ← جدید (مورد 8): پس از Unlock دستی، تا برگشت GSM امنیت غیرفعال بماند
  if (securitySuppressed) {
    if (gsmHealthy()) {
      securitySuppressed = false;
      Serial.println("SECURITY: GSM back - suppression lifted");
    } else {
      secState = 0;
      gsmBadSince = 0;
      return;
    }
  }

  bool ok   = gsmHealthy();
  bool user = authorizedClientConnected();

  switch (secState) {
    case 0:
      if (ok) {
        gsmBadSince = 0;
      } else {
        if (gsmBadSince == 0) gsmBadSince = millis();
        else if (millis() - gsmBadSince >= SEC_DEBOUNCE_MS) {
          if (user) {
            secState = 2;
          } else {
            secState = 1;
            secGraceStart = millis();
            applyLock(false, false);   // فقط قطع پمپ+سوئیچ، بدون قفل درب، secLocked ست نمی‌شود
            updateBuzzer();
            Serial.println("SECURITY: GSM lost - grace, fuel+switch CUT, buzzer blinking");
          }
        }
      }
      break;

    case 1:
      if (ok) {
        secState = 0;
        gsmBadSince = 0;
        if (secLocked) releaseLock(true);
        Serial.println("SECURITY: GSM back - grace cancelled");
      } else if (user) {
        secState = 2;
        if (secLocked) releaseLock(true);
        Serial.println("SECURITY: user connected - grace cancelled");
      } else if (millis() - secGraceStart >= SEC_GRACE_MS) {
        applyLock(true, true);   // doors locked + secLocked set
        secState = 3;
        Serial.println("SECURITY: grace expired - LOCKED (doors+fuel+switch)");
      }
      break;

    case 2:
      if (ok) { secState = 0; gsmBadSince = 0; }
      break;

    case 3:
      if (ok || user) {
        if (secLocked) releaseLock(true);
        secState = ok ? 0 : 2;
        gsmBadSince = 0;
        Serial.println("SECURITY: released");
      }
      break;
  }
}

// ===================== URC queue / line helpers =====================
void urcPush(const String& l) {
  uint8_t n = (urcTail + 1) % URC_QUEUE_SIZE;
  if (n == urcHead) return;
  urcQueue[urcTail] = l;
  urcTail = n;
}

bool urcPop(String& l) {
  if (urcHead == urcTail) return false;
  l = urcQueue[urcHead];
  urcQueue[urcHead] = "";
  urcHead = (urcHead + 1) % URC_QUEUE_SIZE;
  return true;
}

bool isUrcLine(const String& l) {
  return l == "RING" || l.startsWith("+CLIP:") || l.startsWith("+CMTI:") ||
         l == "NO CARRIER" || l == "BUSY" || l == "NO ANSWER" ||
         l.startsWith("+PDP: DEACT");
}

String sim800CleanAndQueue(const String& raw) {
  String out = "";
  int pos = 0, len = raw.length();
  while (pos < len) {
    int nl = raw.indexOf('\n', pos);
    String line;
    if (nl < 0) { line = raw.substring(pos); pos = len; }
    else        { line = raw.substring(pos, nl); pos = nl + 1; }
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith("+IPD,")) {
      int colon = line.indexOf(':');
      if (colon > 0) {
        int plen = line.substring(5, colon).toInt();
        String tail = line.substring(colon + 1);
        for (size_t i = 0; i < tail.length(); i++) sim800TcpClient.pushData((uint8_t)tail[i]);
      }
      continue;
    }
    if (line == "SEND OK" || line.startsWith("SEND OK"))   { g_sendOk = true; continue; }
    if (line == "SEND FAIL" || line.startsWith("SEND FAIL")) { g_sendFail = true; continue; }
    if (line == "CONNECT OK" || line == "ALREADY CONNECT") { g_tcpConnectOk = true; out += line + "\n"; continue; }
    if (line == "CONNECT FAIL") { g_tcpConnectFail = true; out += line + "\n"; continue; }
    if (line == "CLOSE OK" || line == "SHUT OK") { g_tcpCloseOk = true; out += line + "\n"; continue; }
    if (line.startsWith("+CMGS:"))      { g_smsOk   = true; continue; }
    if (line.startsWith("+CMS ERROR"))  { g_smsFail = true; continue; }
    if (isUrcLine(line)) urcPush(line);
    else out += line + "\n";
  }
  return out;
}

String locErrorText(int code) {
  switch (code) {
    case 404:  return "Location unavailable (not found by LBS server)";
    case 408:  return "Location unavailable (LBS server timeout)";
    case 601:  return "Location unavailable (network error)";
    case 602:  return "Location unavailable (no memory)";
    case 603:  return "Location unavailable (DNS error)";
    case 604:  return "Location unavailable (stack busy)";
    default:   return "Location unavailable (code " + String(code) + ")";
  }
}

void locFinish(const String& msg) {
  locResult = msg;
  locDone = true;
  locPhase = 0;
  locWanted = false;

  // If requested via MQTT, publish ack
  if (locRequester.startsWith("mqtt:")) {
    String id = locRequester.substring(5);
    mqttPublishAck(id, msg.indexOf("Location") >= 0 ? "ok" : "error_location");
    mqttPublishStatus();
  }
  // [PATCH 1] درخواست از طریق SMS: بلافاصله پاسخ بده (نه منتظر چرخه 10 ثانیه‌ای)
  else if (locRequester.startsWith("sms:")) {
    String to = (locSmsTargetNumber.length() > 0) ? locSmsTargetNumber
                                                  : locRequester.substring(4);
    // ارسال فوری؛ اگر الآن ممکن نبود (مثلاً تماس فعال)، حلقه‌ی ۱۰ ثانیه‌ای تحویلش می‌دهد
    if (sim800Ready && to.length() > 0 && !callActive) {
      if (sendSms(to, msg)) {
        locSmsLastSend = millis();   // جلوگیری از ارسال فوری دوباره در حلقه
        locResult = "";              // [PATCH 5] تحویل شد؛ در حلقه تکرار نشود
      }
    }
    locRequester = "";
  }
  // [PATCH-S] درخواست از طریق Serial Monitor: پاسخ روی همان Serial چاپ شود
  else if (locRequester.startsWith("serial:")) {
    Serial.println(">> " + msg);
    locRequester = "";
  }
}

void bearerFail(const String& msg) {
  Serial.print("SIM800: "); Serial.println(msg);
  bearerLastFail = millis();
  sim800IP = "";
  if (locWanted) locFinish(msg);
  else locPhase = 0;
  locWanted = false;
}

// خطوطی که فرمان همزمان ندارد: URC یا پاسخ مراحل اینترنت/مکان‌یابی
void sim800ProcessLine(const String& line) {
  if (isUrcLine(line)) { urcPush(line); return; }
  if (line.startsWith("AT")) return;
  if (line == "SEND OK"   || line.startsWith("SEND OK"))   { g_sendOk = true; return; }
  if (line == "SEND FAIL" || line.startsWith("SEND FAIL")) { g_sendFail = true; return; }
  if (line == "CONNECT OK" || line == "ALREADY CONNECT") { g_tcpConnectOk = true; Serial.println("TCP OK"); return; }
  if (line == "CONNECT FAIL") { g_tcpConnectFail = true; return; }
  if (line == "CLOSE OK" || line == "SHUT OK") { g_tcpCloseOk = true; return; }
  if (line.startsWith("+CMGS:"))      { g_smsOk   = true; return; }
  if (line.startsWith("+CMS ERROR"))  { g_smsFail = true; return; }

  if (locPhase == 1) {                       // پاسخ AT+SAPBR=1,1
    if (line == "OK") {
      sim800Serial.print("AT+SAPBR=2,1\r");
      locPhase = 3;
      locStart = millis();
    } else if (line.indexOf("ERROR") >= 0) {
      bearerFail("No internet (GPRS bearer failed)");
    }
  } else if (locPhase == 3) {                // پاسخ AT+SAPBR=2,1
    if (line.startsWith("+SAPBR:")) {
      int c1 = line.indexOf(',');
      int c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
      int st = (c1 >= 0) ? line.substring(c1 + 1, c2 > 0 ? c2 : line.length()).toInt() : 0;
      int q1 = line.indexOf('"');
      int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
      String ip = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
      if (st == 1 && ip.length() > 0 && ip != "0.0.0.0") {
        sim800IP = ip;
        Serial.print("SIM800: internet OK, IP = "); Serial.println(ip);
        if (locWanted) {
          sim800Serial.print("AT+CIPGSMLOC=1,1\r");
          locPhase = 2;
          locStart = millis();
        } else {
          locPhase = 0;
        }
      } else {
        bearerFail("No internet (no IP from operator)");
      }
    } else if (line.indexOf("ERROR") >= 0) {
      bearerFail("No internet");
    }
  } else if (locPhase == 2) {                // پاسخ AT+CIPGSMLOC
    if (line.startsWith("+CIPGSMLOC:")) {
      String v = line.substring(11);
      v.trim();
      int c1 = v.indexOf(',');
      int code = (c1 < 0) ? v.toInt() : v.substring(0, c1).toInt();
      if (code == 0 && c1 > 0) {
        int c2 = v.indexOf(',', c1 + 1);
        int c3 = v.indexOf(',', c2 + 1);
        String lon = v.substring(c1 + 1, c2);
        String lat = (c3 > 0) ? v.substring(c2 + 1, c3) : v.substring(c2 + 1);
        locFinish("Location: https://maps.google.com/?q=" + lat + "," + lon);
      } else {
        locFinish(locErrorText(code));
      }
    } else if (line.indexOf("ERROR") >= 0) {
      locFinish("Location failed");
    }
  }
}

void sim800PumpRx() {
  static int ipdRem = 0;
  while (sim800Serial.available()) {
    char c = (char)sim800Serial.read();

    if (ipdRem > 0) {
      sim800TcpClient.pushData((uint8_t)c);
      ipdRem--;
      continue;
    }

    if (c == '\n') {
      String line = sim800RxBuf;
      sim800RxBuf = "";
      line.trim();
      if (line.length() == 0) continue;

      if (line.startsWith("+IPD,")) {
        int colon = line.indexOf(':');
        if (colon > 0) {
          int plen = line.substring(5, colon).toInt();
          String tail = line.substring(colon + 1);
          for (size_t i = 0; i < tail.length(); i++) sim800TcpClient.pushData((uint8_t)tail[i]);
          ipdRem = plen - tail.length();
        }
        continue;
      }
      sim800ProcessLine(line);
    } else if (c != '\r') {
      sim800RxBuf += c;
      if (sim800RxBuf.length() > 512) sim800RxBuf = "";
    }
  }
}

String sim800Exchange(const String& cmd, unsigned long timeout) {
  sim800PumpRx();
  String resp = sim800RxBuf;
  sim800RxBuf = "";

  sim800Serial.print(cmd);
  sim800Serial.print('\r');

  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (sim800Serial.available()) resp += (char)sim800Serial.read();
    // قبل: if (resp.endsWith("OK\r\n") || resp.indexOf("ERROR") >= 0) break;
    if (resp.indexOf("OK\r\n") >= 0 || resp.indexOf("ERROR") >= 0) break;   // ← اصلاح
    delay(5);
  }
  return sim800CleanAndQueue(resp);
}

bool sim800SendAT(String cmd, String expected, unsigned long timeout) {
  String r = sim800Exchange(cmd, timeout);
  if (expected.length() == 0) return true;
  return r.indexOf(expected) >= 0;
}

bool sim800WaitFor(const char* token, unsigned long timeout, String* out) {
  String buf = "";
  unsigned long start = millis();
  bool found = false;
  while (millis() - start < timeout) {
    while (sim800Serial.available()) buf += (char)sim800Serial.read();
    if (buf.indexOf(token) >= 0) { found = true; break; }
    if (buf.indexOf("ERROR") >= 0) break;
    delay(5);
  }
  if (out) *out = buf;
  return found;
}

bool sim800Probe() {
  for (int i = 0; i < 5; i++) {
    if (sim800SendAT("AT", "OK", 500)) return true;
    delay(100);
  }
  return false;
}

// ===================== SMS parsing =====================

bool smsPduSeen = false;     // ماژول پیام را به صورت PDU (هگز) داده است

int pduHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool looksLikePdu(const String& s) {
  if (s.length() < 20 || (s.length() & 1)) return false;
  for (unsigned int i = 0; i < s.length(); i++) if (pduHex(s[i]) < 0) return false;
  return true;
}

char gsm7Char(int v) {
  if (v == 0x00) return '@';
  if (v == 0x02) return '$';
  if (v == 0x11) return '_';
  if (v == 0x0A || v == 0x0D) return (char)v;
  if (v >= 0x20 && v <= 0x7E) {
    if (v == 0x24 || v == 0x40 || (v >= 0x5B && v <= 0x60) || (v >= 0x7B && v <= 0x7E)) return '?';
    return (char)v;
  }
  return '?';
}

char gsm7Ext(int v) {
  switch (v) {
    case 0x14: return '^';  case 0x28: return '{';  case 0x29: return '}';
    case 0x2F: return '\\'; case 0x3C: return '[';  case 0x3D: return '~';
    case 0x3E: return ']';  case 0x40: return '|';
    default: return '?';
  }
}

// باز کردن septet های 7-بیتی GSM
String gsm7Unpack(const uint8_t* d, int avail, int septets, int skip) {
  String t = "";
  bool esc = false;
  for (int i = 0; i < septets; i++) {
    int bit = i * 7, bi = bit / 8, sh = bit % 8;
    if (bi >= avail) break;
    int v = d[bi] >> sh;
    if (sh > 1 && bi + 1 < avail) v |= d[bi + 1] << (8 - sh);
    v &= 0x7F;
    if (i < skip) continue;
    if (esc) { esc = false; t += gsm7Ext(v); }
    else if (v == 0x1B) esc = true;
    else t += gsm7Char(v);
  }
  return t;
}

void appendUtf8(String& t, uint16_t cp) {
  if (cp < 0x80) t += (char)cp;
  else if (cp < 0x800) { t += (char)(0xC0 | (cp >> 6)); t += (char)(0x80 | (cp & 0x3F)); }
  else { t += (char)(0xE0 | (cp >> 12)); t += (char)(0x80 | ((cp >> 6) & 0x3F)); t += (char)(0x80 | (cp & 0x3F)); }
}

// رمزگشایی PDU از نوع SMS-DELIVER
bool pduDecode(String pdu, String& from, String& date, String& text) {
  pdu.trim();
  pdu.toUpperCase();
  if (!looksLikePdu(pdu)) return false;
  int nb = pdu.length() / 2;
  if (nb > 200) return false;
  uint8_t b[200];
  for (int i = 0; i < nb; i++) b[i] = (uint8_t)(pduHex(pdu[2 * i]) * 16 + pduHex(pdu[2 * i + 1]));

  int p = 0;
  int smsc = b[p++];
  p += smsc;
  if (p + 3 >= nb) return false;
  int first = b[p++];
  if ((first & 0x03) != 0) return false;                 // فقط SMS-DELIVER
  bool udhi = (first & 0x40) != 0;
  int oaLen = b[p++];
  int oaType = b[p++];
  int oaBytes = (oaLen + 1) / 2;
  if (p + oaBytes + 10 > nb) return false;

  String num = "";
  if ((oaType & 0x70) == 0x50) {                         // فرستنده الفبایی
    num = gsm7Unpack(b + p, oaBytes, (oaLen * 4) / 7, 0);
  } else {
    for (int i = 0; i < oaBytes; i++) {
      int lo = b[p + i] & 0x0F, hi = b[p + i] >> 4;
      if ((int)num.length() < oaLen) num += (lo <= 9) ? (char)('0' + lo) : (lo == 0x0A ? '*' : '#');
      if ((int)num.length() < oaLen) num += (hi <= 9) ? (char)('0' + hi) : (hi == 0x0A ? '*' : '#');
    }
    if ((oaType & 0x70) == 0x10) num = "+" + num;
  }
  p += oaBytes;
  p++;                                                    // PID
  int dcs = b[p++];

  String d = "";
  for (int i = 0; i < 7; i++) { d += (char)('0' + (b[p + i] & 0x0F)); d += (char)('0' + (b[p + i] >> 4)); }
  date = d.substring(0, 2) + "/" + d.substring(2, 4) + "/" + d.substring(4, 6) + "," +
         d.substring(6, 8) + ":" + d.substring(8, 10) + ":" + d.substring(10, 12);
  p += 7;

  int udl = b[p++];
  if (p > nb) return false;

  String t = "";
  if ((dcs & 0x0C) == 0x08) {                             // UCS2
    for (int i = 0; i + 1 < udl && p + i + 1 < nb; i += 2) appendUtf8(t, (uint16_t)((b[p + i] << 8) | b[p + i + 1]));
  } else if ((dcs & 0x0C) == 0x04) {                      // 8-bit
    for (int i = 0; i < udl && p + i < nb; i++) t += (char)b[p + i];
  } else {                                                // 7-bit
    int skip = 0;
    if (udhi) { int udhl = b[p]; skip = ((udhl + 1) * 8 + 6) / 7; }
    t = gsm7Unpack(b + p, nb - p, udl, skip);
  }

  from = num;
  text = t;
  return true;
}

// اگر فرستنده خالی و متن شبیه PDU بود، رمزگشایی کن
bool tryDecodePdu(String& from, String& date, String& text) {
  if (from.length() > 0) return false;
  String t = text;
  t.trim();
  String f, d, x;
  if (!pduDecode(t, f, d, x)) return false;
  from = f; date = d; text = x;
  return true;
}


// خروجی AT+CMGL="ALL":  +CMGL: <idx>,"<st>","<from>","<alpha>","<date>"\n<text>
int simParseCmgl(const String& r, SimMsg* out, int maxN) {
  int n = -1;
  int pos = 0, len = r.length();
  while (pos < len) {
    int nl = r.indexOf('\n', pos);
    String line;
    if (nl < 0) { line = r.substring(pos); pos = len; }
    else        { line = r.substring(pos, nl); pos = nl + 1; }
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith("+CMGL:")) {
      n++;
      if (n >= maxN) { n = maxN - 1; break; }
      int comma = line.indexOf(',');
      out[n].idx = line.substring(6, comma > 0 ? comma : line.length()).toInt();
      String q[6];
      int c = collectQuoted(line, q, 6);
      out[n].st   = (c > 0) ? q[0] : "";
      out[n].from = (c > 1) ? q[1] : "";
      out[n].date = (c > 2) ? q[c - 1] : "";
      out[n].text = "";
    } else if (line == "OK") {
      // پایان
    } else if (n >= 0) {
      if (out[n].text.length() > 0) out[n].text += "\n";
      out[n].text += line;
    }
  }
  for (int i = 0; i <= n && i < maxN; i++) {
    if (tryDecodePdu(out[i].from, out[i].date, out[i].text)) smsPduSeen = true;
  }
  return n + 1;
}

// ===================== SMS config (CNMI/CPMS/CMGF) =====================
bool sim800ConfigureSms() {
  bool ok = true;
  ok &= sim800SendAT("AT+CMGF=1", "OK", 1000);          // حالت متنی (نه PDU)
  sim800SendAT("AT+CSCS=\"GSM\"", "OK", 1000);
  sim800SendAT("AT+CPMS=\"SM\",\"SM\",\"SM\"", "OK", 2500);
  ok &= sim800SendAT("AT+CSMP=17,167,0,0", "OK", 1000); // [PATCH 4] پارامترهای SMS (بسیاری از ماژول‌های SIM800L بدون این فرمان SMS نمی‌فرستند)
  ok &= sim800SendAT("AT+CNMI=2,1,0,0,0", "OK", 1000);   // اعلان +CMTI برای هر پیام جدید
  return ok;
}

void sim800VerifySmsConfig() {
  if (!sim800Ready || callActive || locPhase != 0) return;
  String m = sim800Exchange("AT+CMGF?", 1200);
  String r = sim800Exchange("AT+CNMI?", 1200);
  // بعض فریمورها پاسخ را با فاصله و بعضی بدون فاصله پس از ':' می‌دهند.
  m.replace(" ", "");
  r.replace(" ", "");
  bool textMode = (m.indexOf("+CMGF:1") >= 0);
  bool notifyMode = (r.indexOf("+CNMI:2,1") >= 0);
  if (!textMode || !notifyMode) {
    Serial.println("SIM800: SMS config mismatch (CMGF=" + String(textMode ? "OK" : "BAD") +
                   ", CNMI=" + String(notifyMode ? "OK" : "BAD") + ") - reconfiguring once");
    sim800ConfigureSms();
  }
}

// ===================== Init =====================
void powerOnSim800() {
  Serial.println("SIM800: power key pulse");
  pinMode(SIM800_PWK_PIN, OUTPUT);
  digitalWrite(SIM800_PWK_PIN, HIGH);
  delay(100);
  digitalWrite(SIM800_PWK_PIN, LOW);
  delay(1200);
  digitalWrite(SIM800_PWK_PIN, HIGH);
  delay(3000);
}

void sim800HandleMessage(const String& from, const String& text);   // (date و execute حذف)

// پیامک‌های موجود روی سیم‌کارت (قبل از بوت) را در حافظه‌ی ESP32 ذخیره و از سیم‌کارت پاک می‌کند (اجرا نمی‌شوند)
void sim800DrainSim() {
  // پاک‌سازی همه پیام‌های قدیمی روی سیم‌کارت (بدون خواندن، بدون ذخیره، بدون اجرا)
  sim800SendAT("AT+CMGD=1,4", "OK", 20000);
}

void setupSim800() {
  if (!sim800Enabled) return;

  sim800LastInit = millis();
  Serial.println("SIM800: initializing...");

  if (!sim800SerialStarted) {
    pinMode(SIM800_PWK_PIN, OUTPUT);
    digitalWrite(SIM800_PWK_PIN, HIGH);
    pinMode(SIM800_RI_PIN, INPUT_PULLUP);
    sim800Serial.setRxBufferSize(1024);
    sim800Serial.begin(SIM800_BAUD, SERIAL_8N1, SIM800_RX_PIN, SIM800_TX_PIN);
    sim800Serial.setTimeout(200);
    sim800SerialStarted = true;
    delay(1500);
  }

  bool alive = sim800Probe();

  if (!alive && (!sim800PowerTried || sim800InitFails >= 3)) {
    sim800PowerTried = true;
    sim800InitFails = 0;
    powerOnSim800();
    alive = sim800Probe();
  }

  if (!alive) {
    sim800InitFails++;
    Serial.println("SIM800: no response (check power 4V/2A, wiring, baud)");
    return;
  }

  sim800SendAT("ATE0",       "OK", 1000);
  sim800SendAT("AT+CMEE=1",  "OK", 1000);
  sim800SendAT("AT+CSCLK=0", "OK", 1000);
  sim800SendAT("AT+CMIC=0,15", "OK", 1000);   // gain میکروفن
  sim800SendAT("AT+CLIP=1",  "OK", 1000);

  // منتظر آماده شدن سیم‌کارت (تنظیمات پیامک قبل از آن ممکن است رد شود)
  bool simReady = false;
  for (int i = 0; i < 12 && !simReady; i++) {
    String pin = sim800Exchange("AT+CPIN?", 1500);
    simReady = (pin.indexOf("READY") >= 0);
    if (!simReady) delay(1000);
  }
  Serial.print("SIM800 SIM status: "); Serial.println(simReady ? "READY" : "NOT READY");

  // [PATCH C4] AT+CLIP=1 was originally sent BEFORE the SIM was READY and its result
  // was never checked. If it silently failed there is no +CLIP URC at all and the
  // firmware can never decide answer/reject. Re-apply and verify now that SIM is READY.
  if (simReady) {
    if (!sim800SendAT("AT+CLIP=1", "OK", 1000)) {
      Serial.println("SIM800: WARNING AT+CLIP=1 failed -> caller ID unavailable, incoming calls cannot be authorized");
    }
  }

  bool smsOk = false;
  for (int i = 0; i < 3 && !smsOk; i++) {
    smsOk = sim800ConfigureSms();
    if (!smsOk) delay(500);
  }
  String csca = sim800Exchange("AT+CSCA?", 1000);
  csca.replace("\n", " "); csca.trim();
  Serial.println("SIM800 SMSC: " + csca);
  Serial.println(smsOk ? "SIM800: SMS configured" : "SIM800: SMS config incomplete (will retry)");

  if (!smsDrained) {
    sim800DrainSim();
    smsDrained = true;
  }

  sim800Ready = true;
  sim800InitFails = 0;
  sim800NoReply = 0;
  locPhase = 0;
  Serial.println("SIM800: ready");
  updateSim800Status();
  updateGsmSignal();
}

// ===================== Main update =====================
void updateSim800() {
  if (!sim800Enabled) {
    if (sim800Ready) { sim800Ready = false; locPhase = 0; }
    return;
  }

  if (!sim800Ready) {
    if (sim800ForceInit || millis() - sim800LastInit > 30000UL) {
      sim800ForceInit = false;
      setupSim800();
    }
    return;
  }

  sim800PumpRx();

    if (locDone) {
    locDone = false;
    Serial.println("Location result: " + locResult);
    locRequester = "";
  }

  if (locPhase != 0) {
    unsigned long lim = (locPhase == 1) ? 90000UL : 60000UL;
    if (millis() - locStart > lim) {
      if (locPhase == 2) locFinish("Location timeout");
      else bearerFail("No internet (timeout)");
    }
  }

  bool busy = (locPhase != 0);

  // [PATCH C5] While LBS is busy, call state must stay healthy:
  //  - NO CARRIER / BUSY / NO ANSWER: clear callActive immediately (no AT command sent)
  //  - RING / +CLIP: answering now is NOT safe (ATA/ATH replies could be swallowed
  //    by the LBS phase state machine) -> let this one ring out and log it
  //  - all other URCs are kept for the normal cycle
  if (busy) {
    String keep[URC_QUEUE_SIZE]; int keepN = 0; String cl; int cg = 0;
    while (cg++ < 16 && urcPop(cl)) {
      if (cl == "NO CARRIER" || cl == "BUSY" || cl == "NO ANSWER") {
        handleSim800URC(cl);
      } else if (cl == "RING" || cl.startsWith("+CLIP:")) {
        Serial.println("SIM800: call offer during LBS -> cannot answer now, letting it ring out (missed)");
      } else if (keepN < URC_QUEUE_SIZE) {
        keep[keepN++] = cl;
      }
    }
    for (int i = 0; i < keepN; i++) urcPush(keep[i]);
  }

  if (!busy) {
    // اعلان‌های رسیده (پیامک/تماس)
    String l;
    int guard = 0;
    while (guard++ < 16 && locPhase == 0 && urcPop(l)) handleSim800URC(l);

    // فرمان‌های منتظر
    if (locPhase == 0 && pendCount > 0) {
      String f = pendFrom[0], b = pendBody[0];
      for (int i = 1; i < pendCount; i++) { pendFrom[i - 1] = pendFrom[i]; pendBody[i - 1] = pendBody[i]; }
      pendCount--;
      processSMSCommand(f, b);
    }
  }

  busy = (locPhase != 0);

    if (!busy) {
    // URC ها
    String l;
    int guard = 0;
    while (guard++ < 16 && locPhase == 0 && urcPop(l)) handleSim800URC(l);

    // دستورات منتظر
    if (locPhase == 0 && pendCount > 0) {
      String f = pendFrom[0], b = pendBody[0];
      for (int i = 1; i < pendCount; i++) { pendFrom[i-1] = pendFrom[i]; pendBody[i-1] = pendBody[i]; }
      pendCount--;
      processSMSCommand(f, b);
    }

    // ← تغییر 10: ارسال SMS آلارم به همه شماره‌های مجاز
    if (pendingAlarmSms && sim800Ready && locPhase == 0 && !callActive) {
      pendingAlarmSms = false;
      String msg = "ALARM: " + pendingAlarmSmsReason;
      String loc = gpsAvailable() ? gpsMapUrl() : "";
      if (loc.length() > 0) msg += "\nLocation: " + loc;
      for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
        if (phoneNumbers[i].length() >= 7) {
          sendSms(phoneNumbers[i], msg);
        }
      }
    }

    // ← جدید: Status که هنگام تماس/LBS درخواست شده بود، بعد از آزاد شدن فقط یک‌بار ارسال شود
    if (pendingStatusSms && sim800Ready && locPhase == 0 && !callActive) {
      pendingStatusSms = false;
      Serial.println("STATUS: channel free -> sending deferred status to " + pendingStatusFrom);
      sendStatusSms(pendingStatusFrom);
    }

    // ← تغییر 7: حلقه ارسال SMS لوکیشن با فاصله‌ی قابل تنظیم (فرمان: Location <عدد 1..120 دقیقه>)
    // [PATCH 5] نتیجه‌ی LBS فقط یک‌بار ارسال می‌شود (locResult بعد از ارسال پاک می‌شود)
    // حلقه تا رسیدن فرمان Stop location (یا Lock/Unlock/Alarm_off/اتصال کاربر مجاز به وای‌فای) ادامه دارد
    if (locSmsActive && sim800Ready && locPhase == 0 && !callActive) {
      if (locSmsLastSend == 0 || millis() - locSmsLastSend >= locSmsIntervalMs) {
        locSmsLastSend = millis();
        String msg;
        if (gpsAvailable()) {
          msg = "Location: " + gpsMapUrl();
        } else if (locResult.length() > 0) {
          msg = locResult;
        } else {
          msg = "Location: searching for GPS/LBS...";
        }
        locSmsSendCount++;
        Serial.println("LOCATION SMS #" + String(locSmsSendCount) + " (interval " + String(locSmsIntervalMs / 60000UL) + " min)");
        sendSms(locSmsTargetNumber, msg);
        if (!gpsAvailable()) {
          locResult = "";                  // [PATCH 5] یک‌بار مصرف: نتیجه‌ی کهنه دوباره نفرست
        }
      }
    }

  if (millis() - sim800LastStatus > 60000UL) { sim800LastStatus = millis(); updateSim800Status(); }   // 10s → 60s
if (sim800Ready && millis() - sim800LastSignal > 20000UL) { sim800LastSignal = millis(); updateGsmSignal(); }  // 7s → 20s
if (sim800Ready && locPhase == 0 && millis() - sim800LastSmsPoll > 60000UL) { sim800LastSmsPoll = millis(); sim800PollSms(); }  // 8s → 60s
if (sim800Ready && locPhase == 0 && millis() - sim800LastSmsVerify > 300000UL) { sim800LastSmsVerify = millis(); sim800VerifySmsConfig(); sim800CheckStorage(); }  // 2min → 5min

    if (sim800InternetEnabled && sim800Ready && locPhase == 0 && gprsRegistered && gprsAttached && sim800APN.length() > 0 &&
        sim800IP.length() == 0 &&
        (bearerLastFail == 0 || millis() - bearerLastFail > 60000UL) &&
        (sim800LastBearerTry == 0 || millis() - sim800LastBearerTry > 60000UL)) {
      startBearer(false);
    }
  }

    // فقط تماس‌های غیر safe-zone را با تایم‌اوت ببند.
// تماس Safe-Zone چه پاسخ‌داده‌شده چه در حال زنگ، توسط updateSafeZoneCalls مدیریت می‌شود.
if (callActive && !safeZoneAlarmActive && millis() - callStart > MAX_CALL_DURATION_MS) {
    if (locPhase == 0) sim800SendAT("ATH", "OK", 1500);
    callActive = false;
    activeCaller = "";
    Serial.println("SIM800: auto hung up");
}
}

// ===================== Internet (GPRS bearer) + Location (LBS) =====================
void startBearer(bool wantLoc) {
  if (!sim800InternetEnabled) {
    if (wantLoc && locRequester.length() > 0) locFinish("Internet disabled in SIM800 settings");
    return;
  }
  locWanted = wantLoc;
  locStart = millis();
  sim800LastBearerTry = millis();

  sim800Exchange("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", 1500);
  sim800Exchange("AT+SAPBR=3,1,\"APN\",\"" + sim800APN + "\"", 1500);

  String r = sim800Exchange("AT+SAPBR=2,1", 1500);
  int i = r.indexOf("+SAPBR:");
  if (i >= 0 && r.indexOf(",1,", i) > 0) {
    int q1 = r.indexOf('"', i);
    int q2 = (q1 >= 0) ? r.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) sim800IP = r.substring(q1 + 1, q2);
    if (wantLoc) {
      sim800Serial.print("AT+CIPGSMLOC=1,1\r");
      locPhase = 2;
    } else {
      locWanted = false;
      locPhase = 0;
    }
    return;
  }

  sim800Serial.print("AT+SAPBR=1,1\r");
  locPhase = 1;
}

void startLocation(const String& who) {
  // GPS first
  if (gpsAvailable()) {
    Serial.println("Location: GPS fix -> " + gpsMapUrl());
    locResult = "Location: " + gpsMapUrl();
    locDone = true;
    locRequester = who;
    return;
  }
  if (gpsEnabled && !gpsFix) {
    unsigned long elapsed = millis() - gpsFirstSearchMs;
    if (elapsed < GPS_FIX_TIMEOUT_MS) {
      Serial.println("Location: GPS searching, no fix yet");
      locResult = "Searching for satellite... (" + String(elapsed / 1000) + "s)";
      locDone = true;
      locRequester = who;
      return;
    }
  }
  // LBS fallback
  // [PATCH 3] پیام دلیل در locResult قرار می‌گیرد تا به درخواست‌کننده (SMS/Serial) اعلام شود
  if (!sim800InternetEnabled) {
    Serial.println("Location: internet disabled");
    locResult = "Location: internet is OFF in SIM800 settings";
    locDone = true;
    return;
  }
  if (locPhase != 0) {
    if (!locWanted && locRequester.length() == 0 && locPhase != 2) {
      locRequester = who; locWanted = true;
      Serial.println("Location: joined running internet setup");
    } else if (pendCount < PEND_MAX) {
      pendFrom[pendCount] = who; pendBody[pendCount] = smsCmd[0]; pendCount++;
    }
    return;
  }
  if (!sim800Ready || !gprsRegistered) {
    Serial.println("Location: no GSM");
    locResult = "Location: no GSM network";      // [PATCH 3]
    locDone = true;
    return;
  }
  if (sim800APN.length() == 0) detectAndSetAPN();
  if (sim800APN.length() == 0) {
    Serial.println("Location: APN not set");
    locResult = "Location: APN not set";          // [PATCH 3]
    locDone = true;
    return;
  }

  String g = sim800Exchange("AT+CGATT?", 1500);
  bool att = (g.indexOf("+CGATT: 1") >= 0 || g.indexOf("+CGATT:1") >= 0);
  if (!att) {
    sim800Exchange("AT+CGATT=1", 8000);
    g = sim800Exchange("AT+CGATT?", 1500);
    att = (g.indexOf("+CGATT: 1") >= 0 || g.indexOf("+CGATT:1") >= 0);
  }
  gprsAttached = att;
  if (!att) {
    Serial.println("Location: GPRS not attached");
    locResult = "Location: GPRS not attached";    // [PATCH 3]
    locDone = true;
    return;
  }

  locRequester = who;
  startBearer(true);
}

// ===================== URC handling =====================
void handleSim800URC(String line) {
  if (line == "RING") {
    ringDetected = true;
    Serial.println("SIM800: RING");   // [PATCH C2] make incoming rings visible in Serial Monitor
  } else if (line.startsWith("+CLIP:")) {
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) handleIncomingCall(line.substring(q1 + 1, q2));
  } else if (line.startsWith("+CMTI:")) {
    int comma = line.lastIndexOf(',');
    if (comma >= 0) {
      int idx = line.substring(comma + 1).toInt();
      Serial.println("SIM800: new SMS notification, index " + String(idx));
      if (idx > 0) handleIncomingSMS(idx);
    }
  } else if (line == "NO CARRIER" || line == "BUSY" || line == "NO ANSWER") {
    bool wasActive = callActive;
    callActive = false;
    activeCaller = "";
    ringDetected = false;
    // شروع تایمر فاصله تا شماره بعدی در updateSafeZoneCalls
    if (wasActive) safeZoneCallStart = millis();
    Serial.println("SAFE-ZONE: call ended by SIM800 (NO CARRIER/BUSY/NO ANSWER) -> next");
} else if (line.startsWith("+PDP: DEACT")) {
    sim800IP = "";
    sim800LastBearerTry = 0;
    bearerLastFail = 0;
  }
}

void handleIncomingCall(String caller) {
  if (callActive) return;

  // [PATCH C1] No usable caller ID received -> NEVER reject the call.
  // Rejecting (ATH) is what produces the "busy" tone the caller hears.
  // If the number is empty/garbled, just let the phone ring it out.
  String nrm = normalizePhone(caller);
  if (nrm.length() < 7) {
    Serial.println("SIM800: incoming call without usable caller ID (\"" + caller + "\") -> NOT rejecting, letting it ring");
    return;
  }

  // ← تغییر 1: در حالت آلارم تماس رد نمی‌شود
  // (فقط ثبت می‌شود، تصمیم پاسخ/رد بر اساس مجاز بودن شماره)
  if (safeZoneAlarmActive) {
    Serial.println("SAFE-ZONE: alarm active - incoming call from " + caller + " (will be evaluated)");
  }

  if (isPhoneAllowed(caller)) {
    Serial.print("SIM800: answering call from "); Serial.println(caller);
    delay(300);
    bool aok = sim800SendAT("ATA", "OK", 3000);                        // [PATCH C3] gate on real result
    if (!aok) { delay(400); aok = sim800SendAT("ATA", "OK", 3000); }   // [PATCH C3] one retry
    if (aok) {
      callActive   = true;
      callStart    = millis();
      activeCaller = caller;
    } else {
      // [PATCH C3] never fake the state: previously callActive was set unconditionally,
      // so a failed ATA blocked all further call handling for MAX_CALL_DURATION_MS (60s)
      Serial.println("SIM800: ATA failed twice -> not faking callActive, next rings may retry");
    }
  } else {
    // [PATCH C2] log the FULL reason so the cause is visible in Serial Monitor
    int storedCnt = 0;
    for (int i = 0; i < MAX_PHONE_NUMBERS; i++) if (phoneNumbers[i].length() >= 7) storedCnt++;
    Serial.print("SIM800: rejecting unauthorized call from "); Serial.println(caller);
    Serial.print("SIM800: normalized caller = "); Serial.println(nrm);
    if (storedCnt == 0) {
      Serial.println("SIM800: !!! whitelist is EMPTY -> EVERY caller is rejected (busy tone). Set phone numbers in web page /sim800 !!!");
    } else {
      Serial.print("SIM800: whitelist (" + String(storedCnt) + "):");
      for (int i = 0; i < MAX_PHONE_NUMBERS; i++)
        if (phoneNumbers[i].length() > 0)
          Serial.println("  [" + String(i) + "] \"" + phoneNumbers[i] + "\" -> \"" + normalizePhone(phoneNumbers[i]) + "\"");
    }
    delay(300);
    sim800SendAT("ATH", "OK", 1500);   // reject only after a POSITIVE mismatch, never on empty ID
  }
}

// ===================== Incoming SMS =====================
// +CMGR: "REC UNREAD","<sender>","","<time>"\n<body>\nOK
void handleIncomingSMS(int index) {
  String r = sim800Exchange("AT+CMGR=" + String(index), 3000);

  int h = r.indexOf("+CMGR:");
  if (h < 0) {
    // حتی اگر خوانده نشد، از سیم‌کارت حذف شود تا پر نشود
    Serial.println("SIM800: CMGR gave nothing for index " + String(index));
    sim800SendAT("AT+CMGD=" + String(index), "OK", 1500);
    return;
  }

  int nlHdr = r.indexOf('\n', h);
  String header = (nlHdr >= 0) ? r.substring(h, nlHdr) : r.substring(h);
  String q[6];
  int c = collectQuoted(header, q, 6);
  String sender = (c > 1) ? q[1] : "";
  String date   = (c > 2) ? q[c - 1] : "";

  String body = (nlHdr >= 0) ? r.substring(nlHdr + 1) : "";
  int okIdx = body.lastIndexOf("\nOK");
  if (okIdx >= 0) body = body.substring(0, okIdx);
  else if (body.endsWith("OK\n")) body = body.substring(0, body.length() - 3);
  body.trim();

  if (tryDecodePdu(sender, date, body)) smsPduSeen = true;
  if (smsPduSeen) {
    smsPduSeen = false;
    Serial.println("SIM800: module was in PDU mode - switching to text mode");
    sim800SendAT("AT+CMGF=1", "OK", 1000);
  }

  // حذف فوری از سیم‌کارت، سپس اجرای فرمان در صورت مجاز بودن
  sim800HandleMessage(sender, body);
  sim800SendAT("AT+CMGD=" + String(index), "OK", 1500);
}

// پیام را در ESP32 ذخیره می‌کند و (در صورت مجاز بودن فرستنده) اجرا می‌کند
int smsClassify(String body);

void cancelActiveCallAndLocation() {
  if (callActive) {
    if (locPhase == 0) sim800SendAT("ATH", "OK", 1500);
    callActive   = false;
    activeCaller = "";
  }
  if (locPhase != 0) {
    locPhase  = 0;
    locDone   = false;
    locWanted = false;
  }
}

void sim800HandleMessage(const String& from, const String& text) {
  bool allowed = isPhoneAllowed(from);
  int  cls     = smsClassify(text);

  Serial.println("SMS from [" + from + "] -> \"" + text +
                 "\" allowed=" + String(allowed) + " cls=" + String(cls));

  if (mqttConnected) {
    String base = "car/" + deviceToken;
    String j = "{\"from\":\"" + jsonEscape(from) + "\",\"text\":\"" + jsonEscape(text) +
               "\",\"allowed\":" + String(allowed ? "true" : "false") +
               ",\"ts\":" + String(millis() / 1000) + "}";
    mqttClient.publish((base + "/sms").c_str(), j.c_str(), true);
  }

  if (!allowed || cls < 0) return;

  // ← اولویت: Lock(1) / Unlock(2) / Alarm_off(4) / Stop location(5) بلافاصله اجرا شوند
  bool isPriority = (cls == 1 || cls == 2 || cls == 4 || cls == 5);
  if (isPriority) {
    if (cls == 5) {
      // Stop location: فقط درخواست لوکیشن در جریان لغو شود؛ تماس فعال قطع نشود
      if (locPhase != 0) { locPhase = 0; locWanted = false; locDone = false; locResult = ""; }
    } else {
      cancelActiveCallAndLocation();
    }
    processSMSCommand(from, text);
    return;
  }

  // غیر اولویت (Location / Alarm_on / ON..OFF): در صورت مشغولی صف شود
  if (locPhase != 0 || pendCount > 0) {
    if (pendCount < PEND_MAX) {
      pendFrom[pendCount] = from;
      pendBody[pendCount] = text;
      pendCount++;
    }
  } else {
    processSMSCommand(from, text);
  }
}

// مسیر پشتیبان: همه‌ی پیامک‌های روی سیم‌کارت را بخوان، ذخیره/پاک/اجرا کن
void sim800PollSms() {
   String r = sim800Exchange("AT+CMGL=\"REC UNREAD\"", 4000);
  if (r.indexOf("+CMGL:") < 0) return;

  SimMsg msgs[8];
  int n = simParseCmgl(r, msgs, 8);
  if (n <= 0) return;
  Serial.println("SIM800: poll found " + String(n) + " SMS on SIM");

  if (smsPduSeen) {
    smsPduSeen = false;
    Serial.println("SIM800: module was in PDU mode - switching to text mode");
    sim800SendAT("AT+CMGF=1", "OK", 1000);
  }

  // اول همه را از سیم‌کارت حذف کن، سپس فرمان‌های مجاز را اجرا کن
  for (int i = 0; i < n; i++) sim800HandleMessage(msgs[i].from, msgs[i].text);
  for (int i = 0; i < n; i++) sim800SendAT("AT+CMGD=" + String(msgs[i].idx), "OK", 1500);
  
}

// فشرده‌سازی فاصله‌های تکراری به یک فاصله؛ تا «stop  location» هم مثل «stop location» عمل کند
String smsCollapseSpaces(const String& in) {
  String o;
  o.reserve(in.length());
  bool prevSp = false;
  for (unsigned int j = 0; j < in.length(); j++) {
    char c = in[j];
    if (c == ' ' || c == '\t') { if (!prevSp) { o += ' '; prevSp = true; } }
    else { o += c; prevSp = false; }
  }
  return o;
}

// نسخه‌ی استانداردشده‌ی فرمان شماره‌ی i (بزرگ‌نویسه + فاصله‌های تکی)
String smsCmdNormalized(int i) {
  String k = smsCmd[i];
  k.trim();
  k.toUpperCase();
  return smsCollapseSpaces(k);
}

bool cmdMatch(int i, const String& upBody) {
  String k = smsCmdNormalized(i);
  if (k.length() == 0) return false;
  return k == smsCollapseSpaces(upBody);
}

// 0..6 فرمان‌های سفارشی | 10 فرمان قدیمی ON/OFF | -1 ناشناخته
int smsClassify(String body) {
  body.trim();
  body.toUpperCase();
  for (int i = 0; i < SMS_CMD_COUNT; i++) if (cmdMatch(i, body)) return i;

  // Location با عدد (فاصله‌ی ارسال بر حسب دقیقه): «location 15» → cls 0
  String k0 = smsCmdNormalized(0);
  String cb = smsCollapseSpaces(body);
  if (k0.length() > 0 && cb.startsWith(k0 + " ")) {
    String arg = cb.substring(k0.length() + 1);
    arg.trim();
    bool digits = (arg.length() > 0 && arg.length() <= 3);
    for (unsigned int j = 0; j < arg.length() && digits; j++) if (!isDigit(arg[j])) digits = false;
    if (digits) return 0;
  }

  if (body.startsWith("ON") || body.startsWith("OFF")) {
    bool on = body.startsWith("ON");
    String num = body.substring(on ? 2 : 3);
    num.trim();
    if (num.length() == 0) return 10;
    if (num.length() == 1 && num[0] >= '0' && num[0] <= '3') return 10;
  }
  return -1;
}

// ===================== Status Command (فرمان Status) =====================
// متن کامل وضعیت؛ لوکیشن فقط یک‌بار در همین متن می‌آید و هیچ حلقه‌ای شروع نمی‌شود
String buildStatusText() {
  String st = "STATUS:\n";

  // ---- آنتن GSM ----
  if (!sim800Enabled)    st += "GSM: disabled\n";
  else if (!sim800Ready) st += "GSM: module not ready\n";
  else {
    st += "GSM: " + String(gsmPercent) + "% (CSQ " + String(gsmCSQ) + ")";
    if (sim800Carrier.length() > 0) st += " " + sim800Carrier;
    st += "\n";
  }

  // ---- آنتن GPRS (اینترنت) ----
  if (!sim800InternetEnabled) st += "GPRS: internet OFF\n";
  else {
    st += "GPRS: ";
    st += gprsRegistered ? "registered" : "NO network";
    st += gprsAttached   ? ", attached" : ", NOT attached";
    if (sim800IP.length() > 0) st += ", IP " + sim800IP;
    st += "\n";
  }

  // ---- آنتن GPS ----
  if (!gpsEnabled)  st += "GPS: disabled\n";
  else if (gpsFix)  st += "GPS: FIX, " + String(gpsSatellites) + " sats, HDOP " + String(gpsHdop, 1) + "\n";
  else              st += "GPS: no fix\n";

  // ---- وضعیت قفل‌ها ----
  st += "Locks: car=" + String(carLocked ? "LOCKED" : "OPEN");
  if (secLocked)            st += ", security=LOCKED";
  else if (securityEnabled) st += ", security=ON";
  st += ", alarm="  + String(alarmManual ? "ON" : "OFF");
  st += ", doors="  + String(outputState[0] ? "LOCKED" : "OPEN");
  st += ", fuel="   + String(outputState[2] ? "ON" : "OFF");
  st += ", switch=" + String(outputState[3] ? "ON" : "OFF");
  st += "\n";

  // ---- وضعیت حلقه‌ی لوکیشن ----
  st += "LocLoop: ";
  if (locSmsActive) st += "ON every " + String(locSmsIntervalMs / 60000UL) + " min";
  else              st += "OFF";
  st += "\n";

  // ---- لوکیشن (فقط یک‌بار) ----
  if (gpsAvailable()) {
    st += "Location: " + gpsMapUrl() + "\n";
  } else if (locResult.length() > 0) {
    if (!locResult.startsWith("Location:")) st += "Location: ";
    st += locResult + "\n";
  } else {
    st += "Location: not available\n";
  }

  // ---- لیست شماره‌های مجاز ----
  st += "Allowed numbers:";
  int cnt = 0;
  for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
    if (phoneNumbers[i].length() >= 7) {
      cnt++;
      st += "\n" + String(cnt) + ") " + phoneNumbers[i];
    }
  }
  if (cnt == 0) st += " (none)";
  return st;
}

// ارسال متن چندخطی/بلند در تکه‌های مجاز (هر پیامک GSM حداکثر ۱۶۰ نویسه)
bool sendSmsChunked(const String& to, const String& text) {
  const unsigned int MAXCH = 150;   // حاشیه‌ی امن زیر ۱۶۰
  String rest = text;
  bool allOk = true;
  while (rest.length() > 0) {
    if (rest.length() <= MAXCH) {
      if (!sendSms(to, rest)) allOk = false;
      break;
    }
    int cut = -1;
    for (int i = (int)MAXCH; i >= (int)MAXCH / 2; i--) {
      if (i < (int)rest.length() && rest[i] == '\n') { cut = i; break; }
    }
    if (cut < 0) cut = (int)MAXCH;
    if (!sendSms(to, rest.substring(0, cut))) allOk = false;
    rest = (rest[cut] == '\n') ? rest.substring(cut + 1) : rest.substring(cut);
  }
  return allOk;
}

// ارسال وضعیت: برای سریال روی Serial چاپ می‌شود، برای شماره‌ی مجاز SMS می‌شود
void sendStatusSms(const String& from) {
  String st = buildStatusText();
  if (from == "SERIAL") {
    Serial.println("----- STATUS -----");
    Serial.println(st);
    return;
  }
  sendSmsChunked(from, st);
}

String secStateName() {
  if (!securityEnabled) return "OFF";
  switch (secState) {
    case 1: return "GRACE";
    case 2: return "USER-OK";
    case 3: return "LOCKED";
    default: return "NORMAL";
  }
}


// cls: 0=Location 1=Lock 2=Unlock 3=Alarm_on 4=Alarm_off 5=Stop location 6=Status 10=ON/OFF legacy
void processSMSCommand(String from, String body) {
  int cls = smsClassify(body);
  String up = body;
  up.trim();
  up.toUpperCase();
  Serial.println("CMD from " + from + ": \"" + up + "\" -> class " + String(cls));

  // Location [X]: شروع حلقه SMS لوکیشن با فاصله‌ی X دقیقه (1..120)؛ بدون عدد = ۱ دقیقه
  if (cls == 0) {
    int minutes = LOC_SMS_MIN_MINUTES;
    String k0 = smsCmdNormalized(0);
    String cb = smsCollapseSpaces(up);
    if (k0.length() > 0 && cb.startsWith(k0 + " ")) {
      String arg = cb.substring(k0.length() + 1);
      arg.trim();
      bool digits = (arg.length() > 0 && arg.length() <= 3);
      for (unsigned int j = 0; j < arg.length() && digits; j++) if (!isDigit(arg[j])) digits = false;
      if (digits) {
        long v = arg.toInt();
        if (v < LOC_SMS_MIN_MINUTES) v = LOC_SMS_MIN_MINUTES;
        if (v > LOC_SMS_MAX_MINUTES) v = LOC_SMS_MAX_MINUTES;
        minutes = (int)v;
      }
    }
    locSmsIntervalMs   = (unsigned long)minutes * 60000UL;
    locSmsActive       = true;
    locSmsTargetNumber = from;
    locSmsLastSend     = 0;   // ارسال فوری در اولین چرخه
    locSmsSendCount    = 0;
    Serial.println("LOCATION SMS loop started for " + from + " (every " + String(minutes) + " min)");
    startLocation("sms:" + from);
    if (mqttConnected) {
      String base = "car/" + deviceToken;
      String j = "{\"type\":\"location_start\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
      mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
    }
    return;
  }

  if (cls == 1) {   // Lock
    applyLock(false, false);     // [CHANGE] فقط قطع سوئیچ و پمپ بنزین - درب‌ها و صندوق عقب دست نمی‌خورند
    locSmsActive       = false;  // ← توقف حلقه لوکیشن
    locSmsTargetNumber = "";
    if (mqttConnected) {
      String base = "car/" + deviceToken;
      String j = "{\"type\":\"lock_cmd\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
      mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
    }
    Serial.println("LOCK executed (fuel+switch only, doors/trunk untouched)");
    return;
  }

  if (cls == 2) {   // Unlock
    unlockCar();                 // شامل لغو مهلت ۵ دقیقه (تغییر 8)
    locSmsActive       = false;
    locSmsTargetNumber = "";
    if (mqttConnected) {
      String base = "car/" + deviceToken;
      String j = "{\"type\":\"unlock_cmd\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
      mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
    }
    Serial.println("UNLOCK executed");
    return;
  }

  // Status (cls==3) حذف شد — تغییر 9

  if (cls == 3) {   // Alarm_on
  alarmManual = true;
  updateBuzzer();
  pendingAlarmSms       = true;                          // ← جدید
  pendingAlarmSmsReason = "Manual alarm activated";      // ← جدید
  if (mqttConnected) {
    String base = "car/" + deviceToken;
    String j = "{\"type\":\"alarm_on\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
    mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
  }
  Serial.println("ALARM ON");
  return;
}

  if (cls == 4) {   // Alarm_off
    alarmManual = false;
    safeZoneAlarmActive  = false;   // ← خاموش کردن آلارم Safe Zone
    safeZoneCallIdx      = 0;
    safeZoneCallAnswered = false;
    updateBuzzer();
    locSmsActive       = false;      // ← توقف حلقه لوکیشن
    locSmsTargetNumber = "";
    if (mqttConnected) {
      String base = "car/" + deviceToken;
      String j = "{\"type\":\"alarm_off\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
      mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
    }
    Serial.println("ALARM OFF");
    return;
  }

  if (cls == 5) {   // Stop location: توقف کامل حلقه‌ی ارسال لوکیشن
    bool wasActive = locSmsActive;
    locSmsActive       = false;
    locSmsTargetNumber = "";
    locSmsSendCount    = 0;
    if (mqttConnected) {
      String base = "car/" + deviceToken;
      String j = "{\"type\":\"location_stop\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
      mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
    }
    if (wasActive) Serial.println("LOCATION SMS loop STOPPED by command from " + from);
    else           Serial.println("Stop location: loop was not active");
    return;
  }

  if (cls == 6) {   // Status: گزارش یک‌باره‌ی وضعیت (لوکیشن فقط یک‌بار، بدون شروع حلقه)
    if (from == "SERIAL") {
      Serial.println(buildStatusText());
      return;
    }
    // اگر تماس فعال یا LBS در جریان است، بعد از آزاد شدن کانال فقط یک‌بار ارسال شود
    if (callActive || locPhase != 0) {
      pendingStatusSms  = true;
      pendingStatusFrom = from;
      Serial.println("STATUS: busy (call/LBS) -> deferred");
      return;
    }
    if (mqttConnected) {
      String base = "car/" + deviceToken;
      String j = "{\"type\":\"status_cmd\",\"from\":\"" + jsonEscape(from) + "\",\"ts\":" + String(millis()/1000) + "}";
      mqttClient.publish((base + "/event").c_str(), j.c_str(), false);
    }
    sendStatusSms(from);
    return;
  }

  // legacy ON/OFF
  if (cls == 10) {
    bool on = up.startsWith("ON");
    String num = up.substring(on ? 2 : 3);
    num.trim();
    int idx = (num.length() == 1) ? num.toInt() : 0;
    if (idx == 0) { setDoors(on); Serial.println(on ? "Doors ON" : "Doors OFF"); }
    else if (idx == 1) {
      if (on) { triggerTrunk(); Serial.println("Trunk pulse"); }
      else Serial.println("Trunk momentary; use ON1");
    }
    else if (on && carLocked) Serial.println("Output blocked: car locked");
    else { setOutput(idx, on); desiredState[idx] = on; Serial.println("Output " + String(idx) + (on ? " ON" : " OFF")); }
    return;
  }
  Serial.println("Unknown SMS command");
}

// ===================== [PATCH-S] Serial Monitor Command Console =====================
// علت سکوت Serial Monitor این بود که اسکچ اصلاً از Serial ورودی نمی‌خواند.
// حالا خطوط تایپ‌شده در Serial Monitor دقیقاً مثل پیامک طبقه‌بندی و اجرا می‌شوند:
//  - پاسخ Location روی همان Serial چاپ می‌شود (حلقه پیامکی روشن نمی‌شود)
//  - بقیه فرمان‌ها (Lock/Unlock/Alarm/ON/OFF) از همان processSMSCommand عبور می‌کنند
void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;                 // نادیده گرفتن CR (ویندوز)
    if (c != '\n') {                         // هنوز پایان خط نرسیده
      if (serialCmdBuf.length() < 120) serialCmdBuf += c;
      continue;
    }
    if (serialCmdBuf.length() == 0) continue; // خط خالی
    serialCmdBuf.trim();

    Serial.println("SERIAL CMD: \"" + serialCmdBuf + "\"");

    int cls = smsClassify(serialCmdBuf);
    if (cls == 0) {
      // Location از سریال: پاسخ فقط روی Serial، بدون ارسال SMS
      locSmsActive       = false;
      locSmsTargetNumber = "";
      locSmsSendCount    = 0;
      locResult          = "";
      if (locPhase != 0) {
        Serial.println("Location: busy (LBS in progress) - try again in a few seconds");
      } else {
        Serial.println("Location: requesting (GPS first, then LBS)...");
        startLocation("serial:");
      }
    } else {
      // بقیه فرمان‌ها همان مسیر پیامک (بدون بررسی شماره؛ دسترسی فیزیکی = قابل اعتماد)
      processSMSCommand("SERIAL", serialCmdBuf);
    }
    serialCmdBuf = "";
  }
}

// ===================== Status / Signal =====================
void sim800CheckStorage() {
  for (int pass = 0; pass < 2; pass++) {
    String r = sim800Exchange("AT+CPMS?", 1500);
    int i = r.indexOf("+CPMS:");
    if (i < 0) return;
    int c1 = r.indexOf(',', i);
    int c2 = (c1 >= 0) ? r.indexOf(',', c1 + 1) : -1;
    int c3 = (c2 >= 0) ? r.indexOf(',', c2 + 1) : -1;
    if (c1 < 0 || c2 < 0) return;
    int used  = r.substring(c1 + 1, c2).toInt();
    int total = r.substring(c2 + 1, c3 > 0 ? c3 : r.length()).toInt();
    if (total <= 0 || used < total - 2) return;
    Serial.println("SIM800: SIM SMS storage nearly full (" + String(used) + "/" + String(total) + ") - cleaning");
    sim800SendAT(pass == 0 ? "AT+CMGD=1,3" : "AT+CMGD=1,4", "OK", 5000);
  }
}

void updateSim800Status() {
  if (!sim800Ready) return;

  static unsigned long lastCops = 0;   // ← تعریف قبل از استفاده (رفع خطای ۱)
  String r;

  // ===== CREG: ثبت شبکه تلفنی =====
  r = sim800Exchange("AT+CREG?", 1500);
  int i = r.indexOf("+CREG:");
  if (i >= 0) {
    sim800NoReply = 0;                 // ← هر AT موفق شمارنده را صفر می‌کند
    int c = r.indexOf(',', i);
    int nl = r.indexOf('\n', i);
    if (c > 0 && (nl < 0 || c < nl)) {
      int st = r.substring(c + 1).toInt();
      gprsRegistered = (st == 1 || st == 5);
    }
  }

  // ===== COPS: نام اپراتور (فقط هر ۳۰ ثانیه — کند است) =====
  if (millis() - lastCops > 30000UL) {
    lastCops = millis();
    r = sim800Exchange("AT+COPS?", 2000);
    int q1 = r.indexOf('"');
    int q2 = (q1 >= 0) ? r.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) sim800Carrier = r.substring(q1 + 1, q2);
    else if (r.indexOf("+COPS:") >= 0) sim800Carrier = "";
  }

  // ===== CGREG: ثبت شبکه بسته‌ای (جایگزین CGATT) =====
  String gr = sim800Exchange("AT+CGREG?", 1000);
  int gi = gr.indexOf("+CGREG:");
  if (gi >= 0) {
    sim800NoReply = 0;
    int gc = gr.indexOf(',', gi);
    if (gc > 0) {
      int gst = gr.substring(gc + 1).toInt();
      gprsAttached = (gst == 1 || gst == 5);
      if (gprsAttached) gprsRegistered = true;
    }
  }

  if (!gprsRegistered || !gprsAttached) sim800IP = "";

  if (gprsRegistered && sim800APN.length() == 0 && sim800Carrier.length() > 0) detectAndSetAPN();
}   // ← این آکولاد ممکن است در نسخه‌ی شما جا افتاده باشد
void updateGsmSignal() {
  if (!sim800Ready) return;

  String r = sim800Exchange("AT+CSQ", 1000);
  int idx = r.indexOf("+CSQ:");
  if (idx >= 0) {
    sim800NoReply = 0;
    int comma = r.indexOf(',', idx);
    if (comma > 0) {
      gsmCSQ = r.substring(idx + 5, comma).toInt();
      gsmPercent = (gsmCSQ == 99 || gsmCSQ < 0) ? 0 : constrain(gsmCSQ * 100 / 31, 0, 100);
    }
  } else if (++sim800NoReply >= 10) {
    Serial.println("SIM800: module stopped responding - re-init");
    sim800Ready = false;
    sim800NoReply = 0;
    sim800LastInit = millis();
    locPhase = 0;
    sim800IP = "";
  }
}

void detectAndSetAPN() {
  String apn = apnForOperator(sim800Carrier);
  if (apn.length() > 0) {
    sim800APN = apn;
    saveSim800Config();
    sim800IP = "";
    Serial.print("SIM800: APN auto-set to "); Serial.println(apn);
  }
}

String apnForOperator(String op) {
  op.toUpperCase();
  if (op.indexOf("MCI") >= 0 || op.indexOf("TCI") >= 0 || op.indexOf("HAMRAH") >= 0 || op.indexOf("43211") >= 0) return "mcinet";
  if (op.indexOf("IRANCELL") >= 0 || op.indexOf("MTN") >= 0 || op.indexOf("43235") >= 0) return "mtnirancell";
  if (op.indexOf("RIGHTEL") >= 0 || op.indexOf("43220") >= 0) return "rightel";
  if (op.indexOf("TALIYA") >= 0) return "taliya";
  if (op.indexOf("SHATEL") >= 0) return "shatel";
  return "";
}

// ===================== Phone whitelist =====================
String normalizePhone(String n) {
  n.trim();
  n.replace(" ", ""); n.replace("-", "");
  n.replace("(", ""); n.replace(")", "");
  n.replace("+", "");
  if (n.startsWith("00")) n = n.substring(2);
  return n;
}

bool isPhoneAllowed(String num) {
  String n = normalizePhone(num);
  if (n.length() < 7) return false;
  for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
    if (phoneNumbers[i].length() == 0) continue;
    String a = normalizePhone(phoneNumbers[i]);
    if (a.length() < 7) continue;
    if (a == n) return true;
    String a2 = a, n2 = n;
    if (a2.startsWith("0")) a2 = a2.substring(1);
    if (n2.startsWith("0")) n2 = n2.substring(1);
    if (a2 == n2) return true;
    String a3 = a2, n3 = n2;
    if (a3.startsWith("98") && a3.length() > 10) a3 = a3.substring(2);
    if (n3.startsWith("98") && n3.length() > 10) n3 = n3.substring(2);
    if (a3 == n3 && a3.length() >= 9) return true;
  }
  return false;
}


// =====================================================================
//                    JSON API  (ارتباط با اپ اندروید/iOS)
//  همه‌ی مسیرها GET هستند و پاسخ JSON کوچک برمی‌گردانند.
//  به‌جز /api/ping و /api/pair، همه نیاز به توکن دارند:
//      هدر  X-Token: <token>     (یا پارامتر ?t=<token>)
// =====================================================================

// ---------- ذخیره‌ی دستگاه‌های دارای توکن ----------
void loadDevices() {
  preferences.begin("devices", true);
  devCount = preferences.getInt("tcount", 0);
  if (devCount > MAX_ALLOWED_DEVICES) devCount = MAX_ALLOWED_DEVICES;
  if (devCount < 0) devCount = 0;
  for (int i = 0; i < devCount; i++) {
    devs[i].token = preferences.getString(("tk" + String(i)).c_str(), "");
    devs[i].mac   = preferences.getString(("mc" + String(i)).c_str(), "");
    devs[i].mac.toUpperCase();
  }
  preferences.end();
  Serial.println("Paired devices loaded: " + String(devCount));
}

void saveDevices() {
  preferences.begin("devices", false);
  preferences.putInt("tcount", devCount);
  for (int i = 0; i < devCount; i++) {
    preferences.putString(("tk" + String(i)).c_str(), devs[i].token);
    preferences.putString(("mc" + String(i)).c_str(), devs[i].mac);
  }
  preferences.end();
}

static void removeDeviceAt(int idx) {
  for (int i = idx; i < devCount - 1; i++) {
    devs[i] = devs[i + 1];
    devSeen[i] = devSeen[i + 1];
    devMacTry[i] = devMacTry[i + 1];
  }
  if (devCount > 0) devCount--;
}

static int findDeviceByToken(const String& tk) {
  if (tk.length() != 32) return -1;
  for (int i = 0; i < devCount; i++) {
    if (devs[i].token == tk) return i;
  }
  return -1;
}

static String newToken() {
  char b[33];
  for (int i = 0; i < 4; i++) snprintf(b + i * 8, 9, "%08x", (unsigned)esp_random());
  return String(b);
}

// ---------- ابزارهای پاسخ ----------
static const char* jb(bool v) { return v ? "true" : "false"; }

static void apiCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "X-Token, Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Cache-Control", "no-store");
}

static void apiSend(int code, const String& json) {
  apiCors();
  server.send(code, "application/json", json);
}

static String reqToken() {
  String t = server.header("X-Token");
  if (t.length() == 0) t = server.arg("t");
  t.trim();
  return t;
}

// درخواست‌های preflight را جواب می‌دهد و توکن را بررسی می‌کند. false = پاسخ همین‌جا ارسال شد.
static bool apiGuard() {
  if (server.method() == HTTP_OPTIONS) {
    apiCors();
    server.send(204, "text/plain", "");
    return false;
  }
  int i = findDeviceByToken(reqToken());
  if (i < 0) {
    apiSend(401, "{\"ok\":false,\"err\":\"unauthorized\"}");
    return false;
  }
  unsigned long now = millis();
  devSeen[i] = now ? now : 1;

  // MAC فعلی گوشی را به توکن گره بزن (کم‌تکرار: ARP/lwIP گران است)
  unsigned long gap = devs[i].mac.length() ? 300000UL : 10000UL;
  if (devMacTry[i] == 0 || now - devMacTry[i] > gap) {
    devMacTry[i] = now ? now : 1;
    String m = getMacByIP(server.client().remoteIP());
    m.toUpperCase();
    if (m.length() > 0 && m != devs[i].mac) {
      devs[i].mac = m;
      saveDevices();
    }
  }
  return true;
}

static bool apiPreflight() {   // برای مسیرهای بدون توکن
  if (server.method() == HTTP_OPTIONS) {
    apiCors();
    server.send(204, "text/plain", "");
    return true;
  }
  return false;
}

// ---------- بدون توکن ----------
void apiPing() {
  if (apiPreflight()) return;
  bool auth = findDeviceByToken(reqToken()) >= 0;
  apiSend(200, String("{\"ok\":true,\"api\":2,\"auth\":") + jb(auth) + "}");
}

void apiPair() {
  if (apiPreflight()) return;
  static uint8_t fails = 0;
  static unsigned long lockUntil = 0;
  unsigned long now = millis();

  if (lockUntil != 0 && (long)(lockUntil - now) > 0) {
    apiSend(429, "{\"ok\":false,\"err\":\"locked\",\"wait\":" + String((lockUntil - now) / 1000UL + 1) + "}");
    return;
  }

  String code = server.arg("code");
  code.trim();
  if (code != ACTIVATION_CODE) {
    if (++fails >= PAIR_MAX_FAILS) {
      fails = 0;
      lockUntil = now + PAIR_LOCK_MS;
      if (lockUntil == 0) lockUntil = 1;
    }
    apiSend(403, "{\"ok\":false,\"err\":\"wrong_code\"}");
    return;
  }
  fails = 0;

  if (devCount >= MAX_ALLOWED_DEVICES) removeDeviceAt(0);   // قدیمی‌ترین حذف می‌شود
  String tk = newToken();
  devs[devCount].token = tk;
  devs[devCount].mac = "";
  devSeen[devCount] = 0;
  devMacTry[devCount] = 0;

  String m = getMacByIP(server.client().remoteIP());
  m.toUpperCase();
  devs[devCount].mac = m;
  devMacTry[devCount] = now ? now : 1;
  devCount++;
  saveDevices();

  apiSend(200, "{\"ok\":true,\"token\":\"" + tk + "\"}");
}

// ---------- با توکن ----------
void apiUnpair() {
  if (!apiGuard()) return;
  int i = findDeviceByToken(reqToken());
  if (i >= 0) { removeDeviceAt(i); saveDevices(); }
  apiSend(200, "{\"ok\":true}");
}

// یک درخواست به‌جای چهار درخواست جداگانه (/status + /rssi + /sim800status + /gpsstatus)
void apiState() {
  if (!apiGuard()) return;

  long left = 0;
  if (secState == 1) {
    unsigned long el = millis() - secGraceStart;
    left = (el >= SEC_GRACE_MS) ? 0 : (long)((SEC_GRACE_MS - el) / 1000UL);
  }
  int rssi = getClientRSSI();
  bool searching = gpsEnabled && !gpsFix && (millis() - gpsFirstSearchMs < GPS_FIX_TIMEOUT_MS);

  String j;
  j.reserve(700);
  j += "{\"o\":[";
  for (int i = 0; i < 4; i++) { j += outputState[i] ? '1' : '0'; if (i < 3) j += ','; }
  j += "],\"w\":";  j += jb(weakAutoDisconnectEnabled);
  j += ",\"rs\":";  j += String(rssi);
  j += ",\"wp\":";  j += String(rssiToPercentage(rssi));

  j += ",\"g\":{\"en\":"; j += jb(sim800Enabled);
  j += ",\"rdy\":";  j += jb(sim800Ready);
  j += ",\"pct\":";  j += String(gsmPercent);
  j += ",\"csq\":";  j += String(gsmCSQ);
  j += ",\"inet\":"; j += jb(sim800InternetEnabled);
  j += ",\"reg\":";  j += jb(gprsRegistered);
  j += ",\"att\":";  j += jb(gprsAttached);
  j += ",\"busy\":"; j += jb(locPhase != 0);
  j += ",\"call\":"; j += jb(callActive);
  j += ",\"rx\":";   j += String((double)sim800RxDataBytes / 1024.0, 2);
  j += ",\"tx\":";   j += String((double)sim800TxDataBytes / 1024.0, 2);
  j += ",\"car\":\""; j += jsonEscape(sim800Carrier);
  j += "\",\"ip\":\""; j += jsonEscape(sim800IP);
  j += "\"}";

  j += ",\"s\":{\"en\":"; j += jb(securityEnabled);
  j += ",\"st\":";   j += String(secState);
  j += ",\"left\":"; j += String(left);
  j += ",\"lock\":"; j += jb(carLocked);
  j += ",\"alm\":";  j += jb(alarmManual);
  j += "}";

  j += ",\"p\":{\"en\":"; j += jb(gpsEnabled);
  j += ",\"fix\":";  j += jb(gpsFix);
  j += ",\"srch\":"; j += jb(searching);
  j += ",\"sat\":";  j += String(gpsSatellites);
  j += ",\"lat\":";  j += String(gpsLat, 6);
  j += ",\"lng\":";  j += String(gpsLng, 6);
  j += ",\"sza\":";  j += jb(safeZoneAlarmActive);
  j += ",\"szv\":";  j += jb(safeZoneViolated);
  j += "}}";

  apiSend(200, j);
}

// تنظیمات (فقط هنگام باز کردن صفحه‌های تنظیمات خوانده می‌شود)
void apiConfig() {
  if (!apiGuard()) return;
  String j;
  j.reserve(1400);

  j += "{\"names\":[";
  for (int i = 0; i < OUTPUT_COUNT; i++) {
    j += "\"" + jsonEscape(customOutputNames[i]) + "\"";
    if (i < OUTPUT_COUNT - 1) j += ',';
  }
  j += "],\"ap\":{\"ssid\":\"" + jsonEscape(ap_ssid) + "\",\"pwd\":\"" + jsonEscape(ap_password) +
       "\",\"ip\":\"" + ap_ip.toString() + "\"}";
  j += ",\"thr\":" + String(signalThreshold);

  j += ",\"sim\":{\"apn\":\"" + jsonEscape(sim800APN) + "\",\"sec\":" + String(jb(securityEnabled)) + ",\"phones\":[";
  for (int i = 0; i < MAX_PHONE_NUMBERS; i++) {
    j += "\"" + jsonEscape(phoneNumbers[i]) + "\"";
    if (i < MAX_PHONE_NUMBERS - 1) j += ',';
  }
  j += "],\"cmds\":[";
  for (int i = 0; i < SMS_CMD_COUNT; i++) {
    j += "\"" + jsonEscape(smsCmd[i]) + "\"";
    if (i < SMS_CMD_COUNT - 1) j += ',';
  }
  j += "]}";

  j += ",\"gps\":{\"site\":" + String(gpsSite) + ",\"sz\":" + String(jb(safeZoneEnabled)) +
       ",\"lat\":" + String(safeZoneLat, 6) + ",\"lng\":" + String(safeZoneLng, 6) +
       ",\"rad\":" + String(safeZoneRadiusM) + ",\"anti\":" + String(jb(gpsAntiCutEnabled)) + "}";

  j += ",\"mqtt\":{\"en\":" + String(jb(mqttEnabled)) + ",\"host\":\"" MQTT_HOST "\",\"port\":" + String(mqttPort) +
       ",\"user\":\"" + jsonEscape(mqttUser) + "\",\"hasPass\":" + String(jb(mqttPass.length() > 0)) +
       ",\"token\":\"" + jsonEscape(deviceToken) + "\",\"on\":" + String(jb(mqttConnected)) + "}}";

  apiSend(200, j);
}

void apiToggle() {
  if (!apiGuard()) return;
  if (!server.hasArg("pin")) { apiSend(400, "{\"ok\":false}"); return; }

  int idx = server.arg("pin").toInt();
  if (idx == 4) idx = 0;
  if (idx < 0 || idx >= OUTPUT_COUNT) { apiSend(400, "{\"ok\":false}"); return; }

  if (idx == 0) {
    bool newLockState = !outputState[0];
    setDoors(newLockState);

    // اگر آلارم محدوده‌ی امن فعال بود و کاربر درب را باز کرد → کنترل را بدست می‌گیرد
    if (!newLockState && safeZoneAlarmActive) {
      safeZoneAlarmActive  = false;
      safeZoneCallIdx      = 0;
      safeZoneCallAnswered = false;
      alarmManual = false;
      if (callActive) {
        if (locPhase == 0) sim800SendAT("ATH", "OK", 1500);
        callActive = false;
        activeCaller = "";
      }
      updateBuzzer();
      Serial.println("Safe zone alarm CLEARED via app door unlock");
    }
    apiSend(200, "{\"ok\":true,\"pin\":0,\"state\":" + String(outputState[0] ? 1 : 0) + "}");
    return;
  }

  if (idx == 1) {
    triggerTrunk();
    apiSend(200, "{\"ok\":true,\"pin\":1,\"state\":1}");
    return;
  }

  bool newState = !outputState[idx];
  setOutput(idx, newState);
  desiredState[idx] = outputState[idx];
  apiSend(200, "{\"ok\":true,\"pin\":" + String(idx) + ",\"state\":" + String(outputState[idx] ? 1 : 0) + "}");
}

void apiWeak() {
  if (!apiGuard()) return;
  if (!server.hasArg("enabled")) { apiSend(400, "{\"ok\":false}"); return; }

  bool enabled = (server.arg("enabled") == "1" || server.arg("enabled") == "true");
  if (enabled != weakAutoDisconnectEnabled) {
    weakAutoDisconnectEnabled = enabled;
    saveWeakAutoSetting();
    weakDisconnectActive = false;
    if (weakAutoDisconnectEnabled) {
      for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (i == 4) continue;
        if (i == 1) { desiredState[i] = false; continue; }
        desiredState[i] = outputState[i];
      }
      Serial.println("Weak signal auto disconnect enabled.");
    } else {
      Serial.println("Weak signal auto disconnect disabled.");
    }
  }
  apiSend(200, String("{\"ok\":true,\"w\":") + jb(weakAutoDisconnectEnabled) + "}");
}

void apiName() {
  if (!apiGuard()) return;
  if (!server.hasArg("pin") || !server.hasArg("name")) { apiSend(400, "{\"ok\":false,\"err\":\"args\"}"); return; }

  int idx = server.arg("pin").toInt();
  String nm = server.arg("name");
  nm.trim();
  if (idx < 0 || idx >= OUTPUT_COUNT) { apiSend(400, "{\"ok\":false,\"err\":\"pin\"}"); return; }
  if (nm.length() == 0 || nm.length() > 60) { apiSend(400, "{\"ok\":false,\"err\":\"length\"}"); return; }   // UTF-8: ~30 نویسه فارسی

  customOutputNames[idx] = nm;
  saveCustomName(idx);
  apiSend(200, "{\"ok\":true,\"name\":\"" + jsonEscape(nm) + "\"}");
}

void apiThreshold() {
  if (!apiGuard()) return;
  if (!server.hasArg("value")) { apiSend(400, "{\"ok\":false}"); return; }
  int val = server.arg("value").toInt();
  if (val < 0 || val > 100) { apiSend(400, "{\"ok\":false,\"err\":\"range\"}"); return; }
  signalThreshold = val;
  saveSignalThreshold();
  apiSend(200, "{\"ok\":true,\"thr\":" + String(signalThreshold) + "}");
}

// تغییر SSID / رمز / IP نقطه‌ی دسترسی  → ذخیره و راه‌اندازی مجدد
void apiApSet() {
  if (!apiGuard()) return;

  String new_ssid = ap_ssid;
  String new_pwd  = ap_password;
  IPAddress new_ip = ap_ip;
  bool changed = false;

  if (server.hasArg("ssid")) {
    String s = server.arg("ssid"); s.trim();
    if (s.length() > 0 && s.length() <= 32 && s != ap_ssid) { new_ssid = s; changed = true; }
  }
  if (server.hasArg("password")) {
    String p = server.arg("password");
    if (p.length() >= 8 && p.length() <= 63 && p != ap_password) { new_pwd = p; changed = true; }
  }
  if (server.hasArg("ip")) {
    IPAddress ip = stringToIP(server.arg("ip"));
    if (ip != INADDR_NONE && ip != ap_ip && ipIsUsable(ip)) { new_ip = ip; changed = true; }
  }

  if (!changed) { apiSend(200, "{\"ok\":true,\"changed\":false}"); return; }

  preferences.begin("carctrl", false);
  preferences.putString("ssid", new_ssid);
  preferences.putString("pwd", new_pwd);
  preferences.putString("ip", new_ip.toString());
  preferences.end();

  apiSend(200, "{\"ok\":true,\"changed\":true,\"ip\":\"" + new_ip.toString() + "\"}");
  delay(800);
  ESP.restart();
}

void apiSimSet() {
  if (!apiGuard()) return;
  bool changed = false;

  sim800Enabled = true;            // همیشه فعال
  sim800InternetEnabled = true;

  if (server.hasArg("apn")) {
    String a = server.arg("apn"); a.trim();
    if (a.length() > 40) a = a.substring(0, 40);
    if (a != sim800APN) {
      sim800APN = a; changed = true;
      sim800IP = ""; sim800LastBearerTry = 0; bearerLastFail = 0;
    }
  }

  if (server.hasArg("sec")) {
    bool s = (server.arg("sec") == "1");
    if (s != securityEnabled) {
      securityEnabled = s; gsmBadSince = 0;
      if (!s && secState != 3) secState = 0;
      changed = true;
    }
  }

  String newCmd[SMS_CMD_COUNT];
  for (int k = 0; k < SMS_CMD_COUNT; k++) {
    newCmd[k] = smsCmd[k];
    String arg = "c" + String(k);
    if (server.hasArg(arg)) {
      String v = server.arg(arg); v.trim();
      if (v.length() > 0 && v.length() <= 30 && asciiOnly(v)) newCmd[k] = v;
    }
  }
  for (int a = 0; a < SMS_CMD_COUNT; a++) {
    for (int b = a + 1; b < SMS_CMD_COUNT; b++) {
      String x = newCmd[a], y = newCmd[b];
      x.toUpperCase(); y.toUpperCase();
      if (x == y) { apiSend(400, "{\"ok\":false,\"err\":\"duplicate\"}"); return; }
    }
  }
  for (int k = 0; k < SMS_CMD_COUNT; k++) {
    if (newCmd[k] != smsCmd[k]) { smsCmd[k] = newCmd[k]; changed = true; }
  }

  for (int k = 0; k < MAX_PHONE_NUMBERS; k++) {
    String arg = "ph" + String(k);
    if (server.hasArg(arg)) {
      String p = server.arg(arg); p.trim();
      if (p.length() > 20) p = p.substring(0, 20);
      if (p != phoneNumbers[k]) { phoneNumbers[k] = p; changed = true; }
    }
  }

  if (changed) {
    saveSim800Config();
    if (sim800Enabled && !sim800Ready) sim800ForceInit = true;
  }
  apiSend(200, "{\"ok\":true}");
}

void apiApn() {
  if (!apiGuard()) return;
  if (sim800Enabled && sim800Ready && locPhase == 0) updateSim800Status();
  detectAndSetAPN();
  bool ok = sim800APN.length() > 0;
  apiSend(200, String("{\"ok\":") + jb(ok) + ",\"apn\":\"" + jsonEscape(sim800APN) +
               "\",\"car\":\"" + jsonEscape(sim800Carrier) + "\"}");
}

void apiGpsSet() {
  if (!apiGuard()) return;
  bool changed = false;
  gpsEnabled = true;   // GPS همیشه فعال

  if (server.hasArg("site")) {
    int s = server.arg("site").toInt();
    if (s >= 0 && s <= 2 && s != gpsSite) { gpsSite = s; changed = true; }
  }
  if (server.hasArg("sz")) {
    bool en = (server.arg("sz") == "1");
    if (en != safeZoneEnabled) {
      safeZoneEnabled = en; changed = true;
      if (!en) {
        safeZoneAlarmActive = false;
        safeZoneCallIdx = 0;
        safeZoneNoFixSince = 0;
        safeZoneCallAnswered = false;
      }
    }
  }
  if (server.hasArg("lat")) {
    double v = server.arg("lat").toDouble();
    if (v >= -90 && v <= 90 && v != safeZoneLat) { safeZoneLat = v; changed = true; }
  }
  if (server.hasArg("lng")) {
    double v = server.arg("lng").toDouble();
    if (v >= -180 && v <= 180 && v != safeZoneLng) { safeZoneLng = v; changed = true; }
  }
  if (server.hasArg("rad")) {
    long v = server.arg("rad").toInt();
    if (v >= 1 && v <= 100000 && (uint32_t)v != safeZoneRadiusM) { safeZoneRadiusM = (uint32_t)v; changed = true; }
  }
  if (server.hasArg("anti")) {
    bool en = (server.arg("anti") == "1");
    if (en != gpsAntiCutEnabled) { gpsAntiCutEnabled = en; changed = true; }
  }
  if (changed) saveMqttConfig();

  bool warn = safeZoneEnabled && safeZoneLat == 0 && safeZoneLng == 0;
  apiSend(200, String("{\"ok\":true,\"warn\":") + jb(warn) + "}");
}

void apiMqttSet() {
  if (!apiGuard()) return;
  bool changed = false;

  if (server.hasArg("port")) {
    uint16_t p = server.arg("port").toInt();
    if (p > 0 && p != mqttPort) { mqttPort = p; changed = true; }
  }
  if (server.hasArg("user")) {
    String u = server.arg("user"); u.trim();
    if (u != mqttUser) { mqttUser = u; changed = true; }
  }
  if (server.hasArg("pass")) {            // فقط وقتی اپ بفرستد (کاربر تغییرش داده)
    String pw = server.arg("pass");
    if (pw != mqttPass) { mqttPass = pw; changed = true; }
  }
  if (server.hasArg("token")) {
    String tk = server.arg("token"); tk.trim();
    if (tk != deviceToken) { deviceToken = tk; changed = true; }
  }
  if (server.hasArg("enabled")) {
    bool en = (server.arg("enabled") == "1");
    if (en != mqttEnabled) { mqttEnabled = en; changed = true; }
  }
  if (changed) {
    saveMqttConfig();
    if (mqttConnected) { mqttClient.disconnect(); mqttConnected = false; }
    setupMQTT();
    Serial.println("MQTT config updated");
  }
  apiSend(200, "{\"ok\":true}");
}

void apiNotFound() {
  if (apiPreflight()) return;
  apiSend(404, "{\"ok\":false,\"err\":\"not_found\"}");
}

void setupWebServer() {
  static const char* hdrs[] = {"X-Token"};
  server.collectHeaders(hdrs, 1);

  server.on("/api/ping",      apiPing);
  server.on("/api/pair",      apiPair);
  server.on("/api/unpair",    apiUnpair);
  server.on("/api/state",     apiState);
  server.on("/api/config",    apiConfig);
  server.on("/api/toggle",    apiToggle);
  server.on("/api/weak",      apiWeak);
  server.on("/api/name",      apiName);
  server.on("/api/threshold", apiThreshold);
  server.on("/api/ap",        apiApSet);
  server.on("/api/sim",       apiSimSet);
  server.on("/api/apn",       apiApn);
  server.on("/api/gps",       apiGpsSet);
  server.on("/api/mqtt",      apiMqttSet);
  server.onNotFound(apiNotFound);
  server.begin();
  Serial.println("HTTP API server started");
}