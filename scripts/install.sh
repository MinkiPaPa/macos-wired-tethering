#!/bin/bash
# install.sh — copy the app to /Applications and drop Gatekeeper quarantine.
# 앱을 /Applications 로 복사하고 Gatekeeper 격리 속성을 제거한다.
set -euo pipefail

APP_NAME="macOS wired tethering.app"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=""
OPEN_AFTER=0

usage() {
    cat <<'EOF'
사용법 / Usage:
  ./install.sh [--src PATH] [--open]

  --src PATH   설치할 .app 경로 (기본: 이 스크립트와 같은 폴더)
  --open       설치 후 앱을 실행합니다
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --src)
            SRC="${2:-}"
            shift 2
            ;;
        --open)
            OPEN_AFTER=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "알 수 없는 옵션: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [ -z "$SRC" ]; then
    SRC="${APP_SRC:-$HERE/$APP_NAME}"
fi

# Re-exec with sudo, keeping the same --src so env is not required.
# sudo 로 다시 실행할 때 --src 를 넘겨 환경 변수에 의존하지 않는다.
need_root() {
    if [ "$(id -u)" -eq 0 ]; then
        return 1
    fi
    if [ -w /Applications ]; then
        return 1
    fi
    return 0
}

if need_root; then
    echo "관리자 권한이 필요합니다. /Applications 에 설치합니다."
    exec sudo "$0" --src "$SRC" ${OPEN_AFTER:+--open}
fi

if [ ! -d "$SRC" ]; then
    echo "앱을 찾을 수 없습니다: $SRC" >&2
    echo "배포 zip 을 푼 폴더에서 실행하거나 --src 로 .app 경로를 지정하세요." >&2
    exit 1
fi

DEST="/Applications/$APP_NAME"

echo "설치 중: $SRC -> $DEST"

# SIGTERM first so the engine can restore routes; SIGKILL only after a wait.
# 먼저 SIGTERM 으로 경로를 복구하게 하고, 기다린 뒤에만 SIGKILL 한다.
term_then_kill() {
    local name="$1"
    local wait_s="${2:-8}"
    if ! pgrep -x "$name" >/dev/null 2>&1; then
        return 0
    fi
    killall -TERM "$name" >/dev/null 2>&1 || true
    local i=0
    while [ "$i" -lt "$wait_s" ]; do
        pgrep -x "$name" >/dev/null 2>&1 || return 0
        sleep 1
        i=$((i + 1))
    done
    killall -KILL "$name" >/dev/null 2>&1 || true
}

term_then_kill MacOSWiredTethering 2
term_then_kill macos-wired-tethering-engine 8
term_then_kill macos-wired-tethering-helper 2
launchctl bootout system/com.macoswiredtethering.helper >/dev/null 2>&1 || true

rm -rf "$DEST"
ditto "$SRC" "$DEST"

# Quarantine blocks unsigned internal builds opened from Downloads/AirDrop.
# 다운로드·AirDrop 으로 받은 미공증 빌드는 격리 속성 때문에 막힐 수 있다.
if command -v xattr >/dev/null 2>&1; then
    xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
fi

chmod -R a+rX "$DEST"
chmod +x "$DEST/Contents/MacOS/MacOSWiredTethering" \
         "$DEST/Contents/MacOS/macos-wired-tethering-engine" \
         "$DEST/Contents/MacOS/macos-wired-tethering-helper" 2>/dev/null || true

echo "설치 완료: $DEST"
echo "메뉴 막대에서 'macOS wired tethering' 아이콘을 여세요."

if [ "$OPEN_AFTER" -eq 1 ]; then
    open "$DEST"
fi
