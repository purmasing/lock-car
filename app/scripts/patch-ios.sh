#!/usr/bin/env bash
# بعد از «npx cap add ios» روی macOS اجرا می‌شود: اجازه‌ی شبکه‌ی محلی و HTTP به دستگاه.
set -euo pipefail
cd "$(dirname "$0")/.."
PLIST="ios/App/App/Info.plist"
PB=/usr/libexec/PlistBuddy

$PB -c "Delete :NSLocalNetworkUsageDescription" "$PLIST" 2>/dev/null || true
$PB -c "Add :NSLocalNetworkUsageDescription string Car Control needs local network access to talk to your car controller over its Wi-Fi." "$PLIST"
$PB -c "Delete :NSAppTransportSecurity" "$PLIST" 2>/dev/null || true
$PB -c "Add :NSAppTransportSecurity dict" "$PLIST"
$PB -c "Add :NSAppTransportSecurity:NSAllowsLocalNetworking bool true" "$PLIST"
echo "ios patched"
