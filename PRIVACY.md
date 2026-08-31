# 개인정보 처리 방침 / Privacy

macOS wired tethering은 **개발사 서버로 개인정보를 보내지 않습니다.** 분석·광고·크래시 원격 수집이 없습니다.

This app does **not** send personal data to a developer server. There is no analytics, advertising, or remote crash reporting.

## 이 Mac에서만 다루는 정보 / On-device only

- **관리자 계정 이름과 암호:** 연결 시 인증 창에서만 사용합니다. 디스크에 쓰지 않으며, 엔진을 띄운 뒤 메모리에서 버립니다. 암호는 `ps` argv에 올라가지 않도록 AppleScript 표준입력으로만 전달합니다.
- **USB 장치 이름·VID/PID·시리얼:** 장치 목록과 로컬 로그에만 표시합니다.
- **IP·게이트웨이·DNS·송수신 바이트:** 메뉴와 로컬 로그에만 표시합니다. 테더링 트래픽은 휴대폰 이동통신망으로 나갑니다.
- **「기본 연결로 사용」 설정:** 이 Mac의 `UserDefaults`에만 저장합니다.

Administrator name and password are used only for the local auth prompt and are not written to disk. USB identifiers, IP addresses, and traffic counters stay on this Mac. The “Use as default route” preference is stored in UserDefaults only.

## 권한 / Permissions

- USB (IOKit): 안드로이드 테더링 가젯과 통신
- 관리자 권한: `utun` 주소, 기본 경로, `scutil` DNS, 선택적 Wi-Fi 전원
- Apple Events: 헬퍼가 없을 때 엔진을 root로 시작
- CoreWLAN: 테더링 중 끈 Wi-Fi를 이 사용자 세션에서 다시 켬

## 연락 / Contact

이 저장소의 이슈 트래커로 문의하세요. 별도 수집 서버가 없으므로 계정 삭제 요청은 해당되지 않습니다.

Use the project issue tracker. There is no cloud account to delete.
