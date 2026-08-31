#!/bin/bash
# smoke-check.sh — packaging / build sanity checks (no phone required).
# 패키징·빌드 건전성 검사. 실제 폰은 필요 없다.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
# Repo layout: scripts/smoke-check.sh → ../build/...
# Dist zip layout: smoke-check.sh sits next to the .app.
# 저장소에서는 scripts/ 아래, 배포 zip 에서는 .app 옆에 둔다.
if [ -d "$ROOT/macOS wired tethering.app" ]; then
    APP_BUNDLE="${APP_BUNDLE:-$ROOT/macOS wired tethering.app}"
    ENGINE="${ENGINE:-$APP_BUNDLE/Contents/MacOS/macos-wired-tethering-engine}"
else
    ROOT="$(cd "$ROOT/.." && pwd)"
    APP_BUNDLE="${APP_BUNDLE:-$ROOT/build/macOS wired tethering.app}"
    ENGINE="${ENGINE:-$ROOT/build/macos-wired-tethering-engine}"
fi
APP_EXEC="$APP_BUNDLE/Contents/MacOS/MacOSWiredTethering"
ENGINE_IN_APP="$APP_BUNDLE/Contents/MacOS/macos-wired-tethering-engine"
HELPER_IN_APP="$APP_BUNDLE/Contents/MacOS/macos-wired-tethering-helper"
HELPER_PLIST="$APP_BUNDLE/Contents/Library/LaunchDaemons/com.macoswiredtethering.helper.plist"
FAIL=0

ok() { printf "  [ok] %s\n" "$1"; }
bad() { printf "  [FAIL] %s\n" "$1"; FAIL=1; }

echo "macOS wired tethering smoke check"
echo "  app:    $APP_BUNDLE"
echo "  engine: $ENGINE"

if [ -x "$APP_EXEC" ]; then
    ok "GUI executable"
else
    bad "GUI executable missing: $APP_EXEC"
fi

if [ -x "$ENGINE_IN_APP" ]; then
    ok "bundled engine"
else
    bad "bundled engine missing: $ENGINE_IN_APP"
fi

if [ -x "$HELPER_IN_APP" ]; then
    ok "bundled helper"
else
    bad "bundled helper missing: $HELPER_IN_APP"
fi

if [ -f "$HELPER_PLIST" ]; then
    HLAB="$(/usr/libexec/PlistBuddy -c 'Print :Label' "$HELPER_PLIST" 2>/dev/null || true)"
    [ "$HLAB" = "com.macoswiredtethering.helper" ] && ok "helper plist $HLAB" || bad "helper plist label: $HLAB"
else
    bad "helper LaunchDaemon plist missing"
fi

if [ -x "$ENGINE" ]; then
    VER="$("$ENGINE" --version 2>/dev/null || true)"
    if echo "$VER" | grep -q "macOS wired tethering"; then
        ok "engine --version ($VER)"
    else
        bad "engine --version unexpected: $VER"
    fi
    if "$ENGINE" list >/dev/null 2>&1; then
        ok "engine list (USB scan)"
    else
        bad "engine list failed"
    fi
else
    bad "standalone engine missing: $ENGINE"
fi

PLIST="$APP_BUNDLE/Contents/Info.plist"
if [ -f "$PLIST" ]; then
    BID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST" 2>/dev/null || true)"
    MIN="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$PLIST" 2>/dev/null || true)"
    LSUI="$(/usr/libexec/PlistBuddy -c 'Print :LSUIElement' "$PLIST" 2>/dev/null || true)"
    [ "$BID" = "com.macoswiredtethering.app" ] && ok "bundle id $BID" || bad "bundle id: $BID"
    [ "$MIN" = "14.0" ] && ok "min OS $MIN" || bad "min OS: $MIN"
    [ "$LSUI" = "true" ] && ok "LSUIElement (menu-bar only)" || bad "LSUIElement: $LSUI"
    ENC="$(/usr/libexec/PlistBuddy -c 'Print :ITSAppUsesNonExemptEncryption' "$PLIST" 2>/dev/null || true)"
    [ "$ENC" = "false" ] && ok "export compliance ITSAppUsesNonExemptEncryption=false" || bad "ITSAppUsesNonExemptEncryption: $ENC"
    ADMIN="$(/usr/libexec/PlistBuddy -c 'Print :NSSystemAdministrationUsageDescription' "$PLIST" 2>/dev/null || true)"
    [ -n "$ADMIN" ] && ok "NSSystemAdministrationUsageDescription" || bad "missing NSSystemAdministrationUsageDescription"
else
    bad "Info.plist missing"
fi

PRIV="$APP_BUNDLE/Contents/Resources/PrivacyInfo.xcprivacy"
if [ -f "$PRIV" ]; then
    TRACK="$(/usr/libexec/PlistBuddy -c 'Print :NSPrivacyTracking' "$PRIV" 2>/dev/null || true)"
    [ "$TRACK" = "false" ] && ok "PrivacyInfo.xcprivacy (no tracking)" || bad "PrivacyInfo tracking: $TRACK"
else
    bad "PrivacyInfo.xcprivacy missing"
fi

if [ -f "$APP_BUNDLE/Contents/Resources/en.lproj/InfoPlist.strings" ] && \
   [ -f "$APP_BUNDLE/Contents/Resources/ko.lproj/InfoPlist.strings" ]; then
    ok "InfoPlist.strings en+ko"
else
    bad "localized InfoPlist.strings missing"
fi

if [ -f "$APP_BUNDLE/Contents/Resources/en.lproj/Localizable.strings" ] && \
   [ -f "$APP_BUNDLE/Contents/Resources/ko.lproj/Localizable.strings" ]; then
    ok "Localizable.strings en+ko"
else
    bad "Localizable.strings missing"
fi

if command -v codesign >/dev/null 2>&1 && [ -d "$APP_BUNDLE" ]; then
    if codesign --verify --deep --strict "$APP_BUNDLE" 2>/dev/null; then
        ok "codesign --verify"
    else
        echo "  [info] 앱이 아직 서명되지 않았습니다. make dist 가 ad-hoc 서명을 붙입니다."
    fi
fi

OSVER="$(sw_vers -productVersion 2>/dev/null || true)"
MAJOR="${OSVER%%.*}"
if [ -n "$MAJOR" ] && [ "$MAJOR" -ge 14 ]; then
    ok "host macOS $OSVER"
else
    echo "  [info] host macOS $OSVER (요구 사항은 14.0 이상)"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "smoke check FAILED"
    exit 1
fi
echo "smoke check PASSED"
