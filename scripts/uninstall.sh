#!/bin/bash
# uninstall.sh — remove the app and leftover Wi-Fi restore markers.
# 앱과 남은 Wi-Fi 복구 마커를 제거한다.
set -euo pipefail

APP_DEST="/Applications/macOS wired tethering.app"

need_root() {
    if [ "$(id -u)" -eq 0 ]; then
        return 1
    fi
    if [ -e "$APP_DEST" ] && [ ! -w "$APP_DEST" ] && [ ! -w /Applications ]; then
        return 0
    fi
    return 1
}

if need_root; then
    echo "관리자 권한이 필요합니다. 앱을 제거합니다."
    exec sudo "$0" "$@"
fi

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
# Best-effort: drop a leftover system daemon after a Developer ID install.
# Developer ID 설치가 남긴 시스템 데몬을 있으면 내린다.
launchctl bootout system/com.macoswiredtethering.helper >/dev/null 2>&1 || true

rm -rf "$APP_DEST"

# Markers only; sockets live in the user temp dir and expire on logout/reboot.
# 마커만 지운다. 소켓은 사용자 임시 폴더에 있고 로그아웃·재부팅 시 사라진다.
rm -f /tmp/macos-wired-tethering-wifi-restore
# Preferred marker lives under the console user's Application Support.
# 기본 마커는 콘솔 사용자 Application Support 아래에 있다.
USER_HOME="${HOME}"
if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
    SUDO_HOME="$(dscl . -read "/Users/${SUDO_USER}" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
    [ -n "$SUDO_HOME" ] && USER_HOME="$SUDO_HOME"
fi
rm -f "${USER_HOME}/Library/Application Support/macos-wired-tethering/wifi-restore"
rmdir "${USER_HOME}/Library/Application Support/macos-wired-tethering" 2>/dev/null || true

echo "제거 완료."
echo "참고: 연결 중 강제 종료했다면 다음 연결 시작 시 엔진이 scutil 백업 경로를 복구합니다."
echo "참고: 시스템 설정 → 일반 → 로그인 항목에 헬퍼가 남아 있으면 직접 제거하세요."
