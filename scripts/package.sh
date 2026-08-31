#!/bin/bash
# package.sh — build an internal distribution folder, zip, SHA-256, optional pkg.
# 사내 배포 폴더·zip·SHA-256·선택적 pkg 를 만든다.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-1.0.0}"
BUILD_NUMBER="${BUILD_NUMBER:-1}"
APP_DISPLAY="macOS wired tethering"
APP_BUNDLE="${APP_BUNDLE:-$ROOT/build/$APP_DISPLAY.app}"
ENGINE="${ENGINE:-$ROOT/build/macos-wired-tethering-engine}"
DIST="$ROOT/dist"
STAGE_NAME="macos-wired-tethering-${VERSION}"
STAGE="$DIST/$STAGE_NAME"
ZIP_NAME="${STAGE_NAME}-macos.zip"
MAKE_PKG=0

if [ "${1:-}" = "--pkg" ]; then
    MAKE_PKG=1
fi

if [ ! -d "$APP_BUNDLE" ]; then
    echo "앱 번들이 없습니다: $APP_BUNDLE" >&2
    echo "먼저 make app 을 실행하세요." >&2
    exit 1
fi

mkdir -p "$DIST"
# Drop previous artifacts for this version so a leftover pkg cannot ship with an old engine.
# 같은 버전의 이전 산출물을 지워, 예전 엔진이 들어 있는 pkg 가 나가지 않게 한다.
rm -f "$DIST/$ZIP_NAME" "$DIST/${ZIP_NAME}.sha256" \
      "$DIST/${STAGE_NAME}.pkg" "$DIST/${STAGE_NAME}.pkg.sha256" \
      "$DIST/SHA256SUMS"

# Sign inside-out: helper, engine, then the app. Do not use --deep.
# 안쪽부터 서명한다. 헬퍼 → 엔진 → 앱. --deep 은 쓰지 않는다.
# Notarization (notarytool / stapler / productsign) is not invoked here.
# 공증(notarytool / stapler / productsign) 은 여기서 호출하지 않는다.
sign_app() {
    local identity="${CODESIGN_IDENTITY:-}"
    local extra=()
    if [ -z "$identity" ]; then
        echo "ad-hoc 서명 중 (CODESIGN_IDENTITY 미지정, Hardened Runtime 포함)"
        identity="-"
        extra=(--timestamp=none)
    else
        echo "Developer ID 서명 중: $identity"
        extra=(--timestamp)
    fi
    local macos="$APP_BUNDLE/Contents/MacOS"
    # Hardened Runtime is required for later notarization. notarytool is not called here.
    # 이후 공증을 위해 Hardened Runtime 을 붙인다. notarytool 은 여기서 호출하지 않는다.
    codesign --force --sign "$identity" --options runtime "${extra[@]}" \
        --entitlements "$ROOT/helper/helper.entitlements" \
        --identifier com.macoswiredtethering.helper \
        "$macos/macos-wired-tethering-helper"
    codesign --force --sign "$identity" --options runtime "${extra[@]}" \
        --entitlements "$ROOT/engine/engine.entitlements" \
        --identifier com.macoswiredtethering.engine \
        "$macos/macos-wired-tethering-engine"
    codesign --force --sign "$identity" --options runtime "${extra[@]}" \
        --entitlements "$ROOT/app/MacOSWiredTethering.entitlements" \
        --identifier com.macoswiredtethering.app \
        "$APP_BUNDLE"
    codesign --verify --deep --strict "$APP_BUNDLE"
    codesign -dv --verbose=2 "$APP_BUNDLE" 2>&1 | sed 's/^/  /'
    codesign -dv --verbose=2 "$macos/macos-wired-tethering-helper" 2>&1 | sed 's/^/  helper: /'
}

sign_app

rm -rf "$STAGE"
mkdir -p "$STAGE"

# ditto preserves resource forks and code signature.
# ditto 는 리소스 포크와 코드 서명을 유지한다.
ditto "$APP_BUNDLE" "$STAGE/$APP_DISPLAY.app"
cp "$ROOT/scripts/install.sh" "$STAGE/install.sh"
cp "$ROOT/scripts/uninstall.sh" "$STAGE/uninstall.sh"
cp "$ROOT/scripts/smoke-check.sh" "$STAGE/smoke-check.sh"
cp "$ROOT/README.md" "$STAGE/README.md"
cp "$ROOT/LICENSE" "$STAGE/LICENSE"
cp "$ROOT/PRIVACY.md" "$STAGE/PRIVACY.md"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh" "$STAGE/smoke-check.sh"

{
    echo "product=macOS wired tethering"
    echo "version=${VERSION}"
    echo "build=${BUILD_NUMBER}"
    echo "bundle_id=com.macoswiredtethering.app"
    echo "min_os=14.0"
    echo "arch=$(uname -m)"
    echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "engine_version=$("$ENGINE" --version 2>/dev/null || echo unknown)"
    echo "hardened_runtime=yes"
    echo "notarized=no"
    echo "codesign=${CODESIGN_IDENTITY:-adhoc}"
    echo "----- codesign -----"
    codesign -dv --verbose=2 "$STAGE/$APP_DISPLAY.app" 2>&1 || true
} > "$STAGE/BUILDINFO.txt"

echo "zip 생성 중: $DIST/$ZIP_NAME"
rm -f "$DIST/$ZIP_NAME"
# --norsrc --noextattr: unzip 이 AppleDouble(._*) 을 만들어 서명을 깨지 않게 한다.
# 코드 서명은 Mach-O 와 _CodeSignature 에 들어 있으므로 리소스 포크는 필요 없다.
ditto -c -k --norsrc --noextattr --keepParent "$STAGE" "$DIST/$ZIP_NAME"

(
    cd "$DIST"
    shasum -a 256 "$ZIP_NAME" > "${ZIP_NAME}.sha256"
    # SHA256SUMS is the usual IT checksum file name.
    # 사내 배포에서 흔히 쓰는 체크섬 파일 이름.
    shasum -a 256 "$ZIP_NAME" > SHA256SUMS
)

if [ "$MAKE_PKG" -eq 1 ]; then
    PKGROOT="$DIST/pkgroot"
    PKG="$DIST/${STAGE_NAME}.pkg"
    rm -rf "$PKGROOT"
    mkdir -p "$PKGROOT/Applications"
    ditto "$APP_BUNDLE" "$PKGROOT/Applications/$APP_DISPLAY.app"
    echo "pkg 생성 중: $PKG"
    pkgbuild \
        --root "$PKGROOT" \
        --identifier com.macoswiredtethering.app \
        --version "$VERSION" \
        --install-location / \
        "$PKG"
    (
        cd "$DIST"
        shasum -a 256 "$(basename "$PKG")" >> SHA256SUMS
        shasum -a 256 "$(basename "$PKG")" > "$(basename "$PKG").sha256"
    )
    rm -rf "$PKGROOT"
    echo "pkg: $PKG"
fi

echo
echo "배포 산출물:"
echo "  $STAGE/"
echo "  $DIST/$ZIP_NAME"
echo "  $DIST/${ZIP_NAME}.sha256"
cat "$DIST/${ZIP_NAME}.sha256"
echo
echo "설치: zip 을 푼 뒤 ./install.sh  (또는 make install)"
