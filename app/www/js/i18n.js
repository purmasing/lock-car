// ترجمه‌ها (فارسی / انگلیسی)
const D = {
  fa: {
    appTitle: 'کنترل خودرو', settings: 'تنظیمات', back: 'بازگشت', save: 'ذخیره', cancel: 'انصراف',
    on: 'روشن', off: 'خاموش', saving: 'در حال ذخیره...', saved: 'با موفقیت ذخیره شد', saveFailed: 'ذخیره ناموفق',
    out0: 'دربها', out1: 'صندوق عقب', out2: 'پمپ بنزین', out3: 'سوئیچ خودرو',
    signal: 'سیگنال', wifi: 'وای‌فای', gsm: 'GSM', gps: 'GPS', search: 'جستجو', alarm: 'آلارم',
    autoDisc: 'قطع و وصل خودکار رله‌ها با سیگنال ضعیف (کمتر از {n}٪)',
    editName: 'تغییر نام خروجی',
    modeLocal: 'مستقیم (وای‌فای)', modeRemote: 'از راه دور', modeOffline: 'قطع',
    connecting: 'در حال اتصال...', waitLive: 'آخرین وضعیت ذخیره‌شده؛ منتظر به‌روزرسانی...',
    // فعال‌سازی
    pairTitle: 'فعال‌سازی دستگاه',
    pairInfo: 'گوشی را به وای‌فای دستگاه خودرو وصل کنید و کد فعال‌سازی را وارد کنید. این کار فقط یک بار لازم است.',
    pairCode: 'کد فعال‌سازی', pairBtn: 'فعال‌سازی', pairWrong: 'کد فعال‌سازی اشتباه است.',
    pairLocked: 'تلاش‌های ناموفق زیاد بود. {n} ثانیه صبر کنید.', pairNoDevice: 'دستگاه پیدا نشد. به وای‌فای خودرو وصل شوید.',
    pairOk: 'دستگاه فعال شد', remoteOnly: 'فقط کنترل از راه دور (بدون وای‌فای)',
    // منو
    menuAp: 'تنظیمات نقطه دسترسی', menuSim: 'SIM800', menuGps: 'GPS و محدوده امن', menuMqtt: 'سرور MQTT (LBS VPS)', menuApp: 'تنظیمات برنامه',
    // AP
    apTitle: 'تنظیمات نقطه دسترسی', ssid: 'نام شبکه وای‌فای (SSID)', pwd: 'رمز عبور وای‌فای (حداقل ۸ نویسه)', ip: 'نشانی آی‌پی (مانند 192.168.4.1)',
    apBtn: 'ذخیره و راه‌اندازی مجدد', apWarn: 'تغییر تنظیمات باعث راه‌اندازی مجدد دستگاه می‌شود. باید دوباره به شبکه وای‌فای جدید متصل شوید.',
    apNoChange: 'هیچ تغییری اعمال نشد', apReboot: 'دستگاه در حال راه‌اندازی مجدد است. به وای‌فای جدید وصل شوید.',
    thrTitle: 'آستانه سیگنال',
    thrDesc: 'اگر محافظت خودکار سیگنال فعال باشد، خروجی‌ها در کمتر از این درصد خاموش می‌شوند و با بهبود سیگنال دوباره روشن می‌شوند.',
    thrLabel: 'آستانه سیگنال ضعیف (٪)', thrSave: 'ذخیره آستانه', thrInvalid: 'عددی بین ۰ تا ۱۰۰ وارد کنید.',
    // SIM800
    simTitle: 'تنظیمات SIM800', simGsm: 'سیگنال GSM', simNet: 'اینترنت', simCarrier: 'اپراتور', simIp: 'نشانی آی‌پی', simCall: 'تماس',
    simSec: 'امنیت', simLock: 'قفل خودرو', simAlarm: 'آلارم',
    apn: 'APN (خالی = تشخیص خودکار)', autoDetect: 'تشخیص خودکار APN',
    apnHint: 'برای دستور Location، APN باید تنظیم باشد. اپراتور شناسایی و APN خودکار تنظیم می‌شود.',
    phonesTitle: 'شماره‌های مجاز', phonesHint: 'فقط این شماره‌ها می‌توانند فرمان پیامکی بفرستند یا تماس بگیرند.',
    smsTitle: 'فرمان‌های پیامکی (SMS)',
    smsHint: 'متن پیامی که کاربر می‌فرستد. فقط حروف لاتین، بدون تفاوت حروف بزرگ/کوچک، حداکثر ۳۰ نویسه. «Location 15» یعنی ارسال لوکیشن هر ۱۵ دقیقه (۱ تا ۱۲۰).',
    secTitle: 'امنیت قطع آنتن سیم‌کارت',
    secLabel: 'با قطع آنتن سیم‌کارت: ۵ دقیقه بازر، سپس قفل و قطع سوئیچ و پمپ بنزین',
    secHint: 'در مهلت ۵ دقیقه اگر دستگاه مجاز به وای‌فای متصل شود یا آنتن برگردد، بازر قطع می‌شود و قفلی اعمال نمی‌شود.',
    dup: 'فرمان‌های پیامکی نباید تکراری باشند.',
    secNames: ['عادی', 'مهلت - بازر', 'کاربر متصل شد', 'قفل شده'], lockedS: 'قفل', free: 'آزاد', disabled: 'غیرفعال', idle: 'آماده',
    online: 'آنلاین', noNet: 'بدون شبکه', registered: 'ثبت‌شده', attachedNoIp: 'متصل (بدون IP)', init: 'در حال راه‌اندازی...',
    // GPS
    gpsTitle: 'تنظیمات GPS', gpsStatus: 'وضعیت', gpsSats: 'ماهواره‌ها', gpsLat: 'عرض جغرافیایی', gpsLng: 'طول جغرافیایی', gpsDist: 'فاصله تا مرکز',
    gpsFix: 'متصل (FIX)', gpsSearching: 'در جستجوی ماهواره...', gpsNoFix: 'بدون fix (آنتن را بررسی کنید)',
    antiCut: 'تشخیص قطع آنتن GPS', antiCutHint: 'در صورت قطع آنتن GPS بیش از ۵ دقیقه، آلارم فعال می‌شود.',
    mapSite: 'سایت نقشه', szTitle: 'محدوده امن', szEnable: 'فعال‌سازی محدوده امن', szLat: 'عرض جغرافیایی', szLng: 'طول جغرافیایی',
    szRadius: 'شعاع (متر)', useCurrent: 'استفاده از موقعیت فعلی', noFixYet: 'هنوز fix ندارید؛ منتظر ماهواره‌ها بمانید.',
    szHint: 'اگر خارج از محدوده امن یا قطع سیگنال GPS (بیش از ۶۰ ثانیه) رخ دهد، خودرو قفل و به همه شماره‌های مجاز تماس زده می‌شود و سرور مطلع می‌شود.',
    szAlarm: '⚠️ آلارم محدوده امن فعال است', szNoCoords: 'محدوده امن فعال است ولی مختصات تنظیم نشده؛ تا تنظیم نشود آلارم نمی‌دهد.',
    // MQTT
    mqttTitle: 'سرور MQTT', mqttEnable: 'فعال‌سازی MQTT', mqttHost: 'آدرس سرور', mqttPort: 'پورت سرور', mqttUser: 'نام کاربری (اختیاری)',
    mqttPass: 'رمز عبور (اختیاری)', mqttPassKeep: 'ذخیره شده؛ برای تغییر، رمز جدید بنویسید', mqttToken: 'توکن دستگاه (الزامی)',
    mqttTokenHint: 'توکن توسط ادمین سرور هنگام افزودن دستگاه ساخته می‌شود.', mqttConnected: 'متصل', mqttDisc: 'قطع', mqttOff: 'غیرفعال',
    // برنامه
    appTitle2: 'تنظیمات برنامه', language: 'زبان', connMode: 'روش اتصال', mAuto: 'خودکار', mLocal: 'فقط مستقیم', mRemote: 'فقط از راه دور',
    localHost: 'نشانی آی‌پی دستگاه', remoteTitle: 'کنترل از راه دور (MQTT)', wsUrl: 'نشانی WebSocket بروکر', wsHint: 'مثال: wss://smartcarlock.ir:8084/mqtt',
    brokerUser: 'نام کاربری بروکر', brokerPass: 'رمز بروکر', devToken: 'توکن دستگاه (همان توکن MQTT)',
    remoteSave: 'ذخیره و اتصال', unpair: 'خروج و حذف فعال‌سازی این گوشی', unpairConfirm: 'این گوشی از دستگاه حذف شود؟',
    pwaNote: 'در نسخه وب فقط کنترل از راه دور ممکن است (مرورگر اجازه‌ی اتصال مستقیم به دستگاه را نمی‌دهد).',
    // راه دور
    rLock: 'قفل (قطع سوئیچ)', rUnlock: 'آزاد کردن', rAlarmOn: 'آلارم روشن', rAlarmOff: 'آلارم خاموش', rLoc: 'لوکیشن',
    cmdOk: 'انجام شد', cmdPending: 'در حال انجام...', cmdFail: 'فرمان ناموفق بود', cmdTimeout: 'پاسخی از دستگاه نیامد',
    unauthorized: 'دسترسی منقضی شده؛ دوباره فعال‌سازی کنید.', netErr: 'ارتباط با دستگاه برقرار نشد',
    needRemote: 'ابتدا اطلاعات کنترل از راه دور را وارد کنید.', required: 'این فیلد الزامی است',
  },
  en: {
    appTitle: 'Car Control', settings: 'Settings', back: 'Back', save: 'Save', cancel: 'Cancel',
    on: 'ON', off: 'OFF', saving: 'Saving...', saved: 'Saved successfully', saveFailed: 'Save failed',
    out0: 'Doors', out1: 'Trunk', out2: 'Fuel Pump', out3: 'Car Switch',
    signal: 'Signal', wifi: 'Wi‑Fi', gsm: 'GSM', gps: 'GPS', search: 'search', alarm: 'ALARM',
    autoDisc: 'Auto relay control with weak signal (below {n}%)',
    editName: 'Edit Output Name',
    modeLocal: 'Direct (Wi‑Fi)', modeRemote: 'Remote', modeOffline: 'Offline',
    connecting: 'Connecting...', waitLive: 'Last saved status; waiting for update...',
    pairTitle: 'Device Activation',
    pairInfo: 'Connect your phone to the car\'s Wi‑Fi and enter the activation code. This is only needed once.',
    pairCode: 'Activation code', pairBtn: 'Activate', pairWrong: 'Activation code is incorrect.',
    pairLocked: 'Too many failed attempts. Wait {n} seconds.', pairNoDevice: 'Device not found. Connect to the car\'s Wi‑Fi.',
    pairOk: 'Device activated', remoteOnly: 'Remote control only (no Wi‑Fi)',
    menuAp: 'Access Point Settings', menuSim: 'SIM800', menuGps: 'GPS & Safe Zone', menuMqtt: 'MQTT Server (LBS VPS)', menuApp: 'App Settings',
    apTitle: 'Access Point Settings', ssid: 'Wi‑Fi Network Name (SSID)', pwd: 'Wi‑Fi Password (minimum 8 characters)', ip: 'IP Address (e.g. 192.168.4.1)',
    apBtn: 'Save and Reboot', apWarn: 'Changing settings reboots the device. You must reconnect to the new Wi‑Fi network.',
    apNoChange: 'No changes applied', apReboot: 'Device is rebooting. Connect to the new Wi‑Fi network.',
    thrTitle: 'Signal Threshold',
    thrDesc: 'If automatic signal protection is enabled, outputs turn off below this percentage and turn back on when signal recovers.',
    thrLabel: 'Weak signal threshold (%)', thrSave: 'Save Threshold', thrInvalid: 'Enter a number between 0 and 100.',
    simTitle: 'SIM800 Settings', simGsm: 'GSM Signal', simNet: 'Internet', simCarrier: 'Carrier', simIp: 'IP', simCall: 'Call',
    simSec: 'Security', simLock: 'Car lock', simAlarm: 'Alarm',
    apn: 'APN (empty = auto detect)', autoDetect: 'Auto Detect APN',
    apnHint: 'The Location command needs an APN. The carrier is detected and the APN is set automatically.',
    phonesTitle: 'Authorized Phone Numbers', phonesHint: 'Only these numbers can send SMS commands or make calls.',
    smsTitle: 'SMS Commands',
    smsHint: 'Text the user sends. Latin letters only, case-insensitive, max 30 characters. "Location 15" sends the location every 15 minutes (1–120).',
    secTitle: 'SIM antenna-loss security',
    secLabel: 'On SIM antenna loss: 5 min buzzer, then lock and cut ignition and fuel pump',
    secHint: 'During the 5 min grace period, if an authorized device joins the Wi‑Fi or the signal returns, the buzzer stops and no lock is applied.',
    dup: 'SMS commands must be unique.',
    secNames: ['Normal', 'GRACE - buzzer', 'User connected', 'LOCKED'], lockedS: 'LOCKED', free: 'free', disabled: 'disabled', idle: 'idle',
    online: 'Online', noNet: 'No network', registered: 'Registered', attachedNoIp: 'Attached (no IP yet)', init: 'initializing...',
    gpsTitle: 'GPS Settings', gpsStatus: 'Status', gpsSats: 'Satellites', gpsLat: 'Latitude', gpsLng: 'Longitude', gpsDist: 'Distance to center',
    gpsFix: 'FIX', gpsSearching: 'Searching for satellites...', gpsNoFix: 'No fix (check antenna)',
    antiCut: 'GPS Antenna Cut Detection', antiCutHint: 'If the GPS antenna is cut for more than 5 minutes, the alarm is activated.',
    mapSite: 'Map Site', szTitle: 'Safe Zone', szEnable: 'Enable Safe Zone', szLat: 'Latitude', szLng: 'Longitude',
    szRadius: 'Radius (meters)', useCurrent: 'Use Current Location', noFixYet: 'No GPS fix yet - wait for satellites.',
    szHint: 'If the car leaves the safe zone or GPS is lost (over 60 s), it is locked, all authorized numbers are called and the server is notified.',
    szAlarm: '⚠️ SAFE ZONE ALARM ACTIVE', szNoCoords: 'Safe zone is enabled but coordinates are not set - the alarm will not trigger until they are.',
    mqttTitle: 'MQTT Server', mqttEnable: 'Enable MQTT', mqttHost: 'Server Host', mqttPort: 'Server Port', mqttUser: 'Username (optional)',
    mqttPass: 'Password (optional)', mqttPassKeep: 'Saved; type a new one to change', mqttToken: 'Device Token (required)',
    mqttTokenHint: 'The token is created by the server admin when the device is added.', mqttConnected: 'CONNECTED', mqttDisc: 'disconnected', mqttOff: 'disabled',
    appTitle2: 'App Settings', language: 'Language', connMode: 'Connection mode', mAuto: 'Automatic', mLocal: 'Direct only', mRemote: 'Remote only',
    localHost: 'Device IP address', remoteTitle: 'Remote control (MQTT)', wsUrl: 'Broker WebSocket URL', wsHint: 'Example: wss://smartcarlock.ir:8084/mqtt',
    brokerUser: 'Broker username', brokerPass: 'Broker password', devToken: 'Device token (same as the MQTT token)',
    remoteSave: 'Save and connect', unpair: 'Sign out and remove this phone', unpairConfirm: 'Remove this phone from the device?',
    pwaNote: 'The web version supports remote control only (browsers cannot connect directly to the device).',
    rLock: 'Lock (cut ignition)', rUnlock: 'Unlock', rAlarmOn: 'Alarm ON', rAlarmOff: 'Alarm OFF', rLoc: 'Location',
    cmdOk: 'Done', cmdPending: 'In progress...', cmdFail: 'Command failed', cmdTimeout: 'No response from device',
    unauthorized: 'Access expired; please activate again.', netErr: 'Could not reach the device',
    needRemote: 'Enter the remote control details first.', required: 'This field is required',
  },
};

let lang = 'fa';
export const getLang = () => lang;
export function setLang(l) {
  lang = D[l] ? l : 'fa';
  document.documentElement.lang = lang;
  document.documentElement.dir = lang === 'fa' ? 'rtl' : 'ltr';
}
export function t(key, vars) {
  let s = (D[lang] && D[lang][key] !== undefined) ? D[lang][key] : (D.en[key] !== undefined ? D.en[key] : key);
  if (vars && typeof s === 'string') for (const k in vars) s = s.replace('{' + k + '}', vars[k]);
  return s;
}
