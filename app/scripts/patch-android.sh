#!/usr/bin/env bash
# بعد از «npx cap add android» اجرا می‌شود: مجوزها، HTTP محلی و پلاگین اتصال به وای‌فای را اضافه می‌کند.
set -euo pipefail
cd "$(dirname "$0")/.."

APP_ID=$(python3 -c "import json;print(json.load(open('capacitor.config.json'))['appId'])")
PKG_DIR="android/app/src/main/java/$(echo "$APP_ID" | tr . /)"
MANIFEST="android/app/src/main/AndroidManifest.xml"

mkdir -p "$PKG_DIR"
cp native/android/WifiBindPlugin.java "$PKG_DIR/WifiBindPlugin.java"

cat > "$PKG_DIR/MainActivity.java" <<JAVA
package $APP_ID;

import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(WifiBindPlugin.class);
        super.onCreate(savedInstanceState);
    }
}
JAVA

python3 - "$MANIFEST" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
perms = ['android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE', 'android.permission.CHANGE_NETWORK_STATE']
for perm in perms:
    if perm not in s:
        s = s.replace('<application', '<uses-permission android:name="%s" />\n    <application' % perm, 1)
if 'usesCleartextTraffic' not in s:
    s = s.replace('<application', '<application android:usesCleartextTraffic="true"', 1)
open(p, 'w', encoding='utf-8').write(s)
print('manifest patched')
PY
echo "android patched ($APP_ID)"
