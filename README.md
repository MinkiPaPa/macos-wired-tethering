# macOS wired tethering

macOS용 **Android USB 테더링** 앱입니다. 커널 확장(kext), DriverKit, SIP 해제가 필요 없습니다.

macOS에는 Microsoft RNDIS 드라이버가 없어서, 안드로이드 폰에서 USB 테더링을 켜도 시스템 설정에 새 네트워크 인터페이스가 나타나지 않습니다. macOS wired tethering은 사용자 공간에서 RNDIS를 말하고, `utun`(L3 터널)으로 IP 패킷을 macOS 스택에 넘깁니다.

```
안드로이드 폰 (USB RNDIS)
        │  bulk IN/OUT
        ▼
 macOS wired tethering 엔진 (사용자 공간)
        │  Ethernet 언랩 / DHCP / ARP
        ▼
     utunN  (L3 점대점 터널)
        │
        ▼
 macOS 네트워크 스택 (기본 경로 / DNS / Safari · Chrome)
```

메뉴 막대 전용 앱입니다. Dock 아이콘과 메인 창은 없습니다.

`utun`은 시스템 설정의 네트워크에 USB 이더넷 포트로 보이지 않을 수 있습니다. 브라우저는 **기본 연결로 사용**이 켜져 있을 때 엔진이 올리는 기본 경로와 DNS를 따릅니다.

## 요구 사항

- macOS 14 Sonoma 이상 (Apple Silicon / Intel)
- Xcode Command Line Tools (`xcode-select --install`)
- 데이터 전송이 되는 USB 케이블 (충전 전용 케이블은 안 됩니다)
- USB·utun·라우트 변경을 위한 **관리자 암호** (연결 시 한 번). Developer ID 빌드는 메뉴의 **헬퍼**를 한 번 허용하면 이후 암호 없이 연결됩니다.

## 빌드

```bash
cd "macOS wired tethering"
make          # 단위 테스트 + "macOS wired tethering.app"
make helper   # SMAppService 권한 헬퍼만 빌드
make test-tsan  # ThreadSanitizer 로 C 단위 테스트 (MAC 경합 포함)
make run      # 앱 실행
make smoke    # 폰 없이 빌드·서명 전 건전성 검사
```

산출물:

| 경로 | 설명 |
|------|------|
| `build/macOS wired tethering.app` | 메뉴 막대 SwiftUI 앱 |
| `build/macos-wired-tethering-engine` | CLI 엔진 (앱 번들에도 동일 이름) |
| `build/macos-wired-tethering-helper` | SMAppService 권한 헬퍼 (번들 `Contents/MacOS`) |
| `build/test_rndis` | RNDIS 프레이밍 단위 테스트 |
| `build/test_dhcp` | DHCPv4 Discover 레이아웃 단위 테스트 |
| `build/test_mac_sync` | 게이트웨이 MAC 스냅샷/저장 경합 테스트 |
| `build/test_net_parse` | scutil·networksetup 텍스트 파서 테스트 |
| `build/test_parse` | 장치 JSON/표·엔진 상태 줄·소켓 경로·엔진 경로 해석 테스트 |
| `build/test_utun` | utun 읽기 절단·폐기 정책 테스트 |

개발 PC에 넣으려면 `make install` 을 실행합니다. 제거는 `make uninstall` 입니다.

## Mac App Store / 공증

이 앱은 **Mac App Store에 올릴 수 없습니다.** App Store 앱은 샌드박스가 필수이고, USB 테더링은 root로 `utun`·라우트·DNS를 바꿔야 하므로 가이드라인(샌드박스·권한 헬퍼)에 맞지 않습니다.

외부 배포 경로:

1. Developer ID Application 으로 `make dist CODESIGN_IDENTITY="…"` (Hardened Runtime·entitlements·PrivacyInfo 포함)
2. 나중에 Apple 공증: `notarytool submit` → `stapler staple` (이 Makefile은 호출하지 않습니다)
3. 공증된 zip/pkg를 배포

품질 기준(개인정보 매니페스트, 사용 목적 문구, 정보 패널, 접근성 라벨)은 App Store 심사용 앱과 같은 수준으로 맞춰 두었습니다. 공증만 계정 준비 후 수행하면 됩니다.

## 사내 배포

개발 머신에서 버전을 찍은 zip(및 선택적 pkg)을 만들고, IT가 `/Applications`에 설치하는 흐름입니다. Apple 공증(notarization)은 이 Makefile이 호출하지 않습니다.

Developer ID 계정이 없으면 `make dist` 만으로 **ad-hoc 서명 zip**이 만들어집니다. 공증은 호출하지 않습니다. 사내 파일 서버·USB로 넘기고, 받는 쪽에서 `./install.sh`로 격리 속성을 지우면 됩니다.

ad-hoc 빌드의 제한:

- Gatekeeper는 다운로드/메일/AirDrop 앱을 막을 수 있습니다. `install.sh`가 `com.apple.quarantine`을 제거합니다. Finder에서 앱을 더블클릭만 하지 말고 반드시 `install.sh`를 쓰세요.
- 권한 **헬퍼**는 등록되지 않는 경우가 많습니다. 연결 시 앱 인증 창에 관리자 암호를 넣으면 됩니다.
- 바이너리는 **빌드한 Mac의 CPU**입니다. Apple Silicon에서 만든 zip은 Intel Mac에서 실행되지 않습니다. Intel이 필요하면 Intel Mac에서 `make dist` 하세요.
- 다른 회사·외부 배포에는 Developer ID + 공증이 필요합니다. 사내만이면 ad-hoc으로 충분합니다.

### 패키지 만들기

```bash
# 권장: 사내 Developer ID 로 서명
make dist CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"
make dist-pkg CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"

# 로컬 확인만: ad-hoc 서명 + zip + SHA-256
make dist
```

산출물 (`dist/`):

| 경로 | 설명 |
|------|------|
| `macos-wired-tethering-1.0.0/` | 앱 + `install.sh` / `uninstall.sh` / `README.md` / `PRIVACY.md` / `LICENSE` / `BUILDINFO.txt` |
| `macos-wired-tethering-1.0.0-macos.zip` | 위 폴더를 압축한 배포본 |
| `macos-wired-tethering-1.0.0-macos.zip.sha256` | SHA-256 (한 줄) |
| `SHA256SUMS` | 같은 해시, IT 검수용 파일 이름 |
| `macos-wired-tethering-1.0.0.pkg` | `make dist-pkg` 일 때만. `/Applications` 설치 위치 |

zip을 받은 쪽에서는 압축을 푼 뒤:

```bash
shasum -a 256 -c macos-wired-tethering-1.0.0-macos.zip.sha256
# unzip 또는 ditto 모두 가능. Finder로 zip을 풀어 앱만 더블클릭하지 말 것.
unzip macos-wired-tethering-1.0.0-macos.zip
# 또는: ditto -x -k macos-wired-tethering-1.0.0-macos.zip .
cd macos-wired-tethering-1.0.0
./install.sh              # /Applications 로 복사, 격리 속성 제거
./install.sh --open       # 설치 후 실행
./uninstall.sh            # /Applications 에서 앱 제거
```

AirDrop·파일 서버·메일로 받은 앱은 `com.apple.quarantine` 때문에 Gatekeeper가 막을 수 있습니다. `install.sh`가 이 속성을 지웁니다. 그래도 막히면:

```bash
xattr -dr com.apple.quarantine "/Applications/macOS wired tethering.app"
open "/Applications/macOS wired tethering.app"
```

다른 부서로 넘기는 zip은 Developer ID로 서명하세요. ad-hoc 서명은 **그 Mac에서 빌드한 것처럼**만 동작합니다. 공증까지 하려면 Apple notary 계정과 `notarytool`이 필요하며, 이 Makefile은 호출하지 않습니다.

### 관리자 스모크 테스트 (배포 전)

빌드 PC:

```bash
make test
make smoke
make dist
```

설치 대상 Mac (폰 없이도 1–4, 폰이 있으면 5–8):

1. zip SHA-256이 `SHA256SUMS`와 같은지 확인한다.
2. `./install.sh` 후 `/Applications/macOS wired tethering.app`이 생긴다.
3. 메뉴 막대에 아이콘이 나타나고 Dock에는 없다 (`LSUIElement`).
4. 메뉴를 열어 장치 목록이 비어 있거나 USB 장치가 보이는지 확인한다. 앱이 바로 죽으면 안 된다.
5. 안드로이드에서 USB 테더링을 켜면 상태가 **테더링 준비됨**이 된다.
6. **연결** → 헬퍼가 켜져 있으면 암호 없이 연결된다. 아니면 앱 인증 창에 관리자 암호. 시스템 자물쇠 창은 뜨지 않아야 한다.
7. IPv4·게이트웨이가 표시되고 브라우저가 모바일 데이터로 열린다.
8. **연결 해제** 후 Wi-Fi(또는 이전 기본 경로)가 돌아온다.

### 배포 시 보안 메모

- 관리자 암호는 앱 인증 창에서만 받고, 디스크에 저장하지 않습니다.
- 암호 확인은 OpenDirectory `verifyPassword`를 쓰며, `dscl -authonly` argv에 암호를 올리지 않습니다.
- 엔진 기동용 AppleScript는 `osascript -` 표준입력으로 넘겨 `ps` argv에 암호가 보이지 않게 합니다. (프로세스 메모리에는 잠시 남습니다.)
- GUI는 일반 권한입니다. utun/라우트는 엔진이 root로 수행합니다.
- Developer ID 로 서명한 앱은 메뉴의 **헬퍼** 로 `SMAppService` 데몬을 등록할 수 있습니다. 시스템 설정에서 한 번 허용하면 이후 연결은 암호를 묻지 않습니다. **헬퍼 해제**로 내립니다.
- 헬퍼 XPC는 GUI 번들 ID와, Developer ID 빌드에서는 같은 Team ID(`anchor apple generic`)를 요구합니다. 헬퍼는 번들 안의 엔진만 실행하고, 엔진 서명이 헬퍼와 같은 팀인지 확인합니다. 소켓·로그는 임시 트리만 허용하며 임시 트리 밖을 가리키는 심볼릭 링크는 거절합니다.
- Developer ID + Hardened Runtime 서명은 `make dist`가 헬퍼·엔진·앱에 entitlements를 붙여 수행합니다. ad-hoc도 같은 Runtime 옵션을 써서 이후 공증 준비 상태를 유지합니다. `notarytool`은 호출하지 않습니다.
- `./install.sh`는 기존 GUI·엔진·헬퍼를 먼저 종료하고 `launchctl bootout`으로 시스템 헬퍼를 내립니다. `uninstall.sh`는 사용자 홈의 Wi-Fi 복구 마커도 지웁니다.
- ad-hoc 서명에서는 헬퍼 등록이 거절되는 경우가 많습니다. 그때는 기존처럼 앱 인증 창과 `osascript` 로 연결합니다.
- 상태 소켓은 사용자 임시 폴더에 `0600`입니다.
- Wi-Fi 복구 마커는 `~/Library/Application Support/macos-wired-tethering/` 에 둡니다. 이전 빌드의 `/tmp` 마커는 읽기만 하고 지웁니다.

## 사용 방법

1. 안드로이드 폰을 Mac에 USB로 연결합니다.
2. 폰에서 **USB 테더링**을 켭니다.
   - 설정 → 네트워크 및 인터넷 → 핫스팟 및 테더링 → USB 테더링
   - 삼성: 설정 → 연결 → 모바일 핫스팟 및 테더링 → USB 테더링
3. 메뉴 막대의 macOS wired tethering 아이콘을 열고, 장치가 **테더링 준비됨**인지 확인한 뒤 **연결**을 누릅니다.
4. **헬퍼**를 눌러 권한 데몬을 등록할 수 있습니다 (Developer ID 권장). 허용되면 암호 없이 연결됩니다. 아니면 인증 창에 관리자 암호를 입력합니다. 시스템 `osascript` 자물쇠 창은 뜨지 않습니다.
5. 사용자 공간 DHCP로 주소가 할당되면 인터넷을 사용할 수 있습니다.

**기본 연결로 사용**을 켜면 USB 테더링이 Wi-Fi보다 우선합니다. 연결 전에 기본 경로와 `scutil` Global IPv4/DNS를 스냅샷하고, 연결 해제 시 복구합니다. Wi-Fi와 동시에 쓰고 싶다면 끄면 됩니다 (이 경우 시스템 기본 경로는 바꾸지 않습니다).

### CLI

장치 검색은 root가 필요 없습니다.

```bash
./build/macos-wired-tethering-engine list
./build/macos-wired-tethering-engine list --json
./build/macos-wired-tethering-engine --version
sudo ./build/macos-wired-tethering-engine connect --default-route
sudo ./build/macos-wired-tethering-engine connect --vid 04e8 --pid 6860
```

종료는 `Ctrl+C` 입니다. GUI에서 연결을 끊으면 소켓으로 `stop`을 보내고 엔진 PID를 종료해, 경로·DNS가 복구된 뒤에 프로세스가 끝납니다.

## 동작 원리

1. **USB 탐색** — IOKit으로 `IOUSBHostDevice`를 순회하고, 인터페이스 클래스/서브클래스/프로토콜로 RNDIS를 식별합니다.
   - `0xE0/0x01/0x03` Wireless RNDIS
   - `0x02/0x02/0xFF` CDC ACM vendor (Android USB tethering)
   - `0xEF/0x04/0x01` Miscellaneous RNDIS
   - 데이터 인터페이스는 CDC Data `0x0A` (alternate setting 1에 벌크 엔드포인트가 있는 경우가 많음)
2. **RNDIS 핸드셰이크** — `INITIALIZE` → MAC `QUERY` (`OID_802_3_PERMANENT_ADDRESS`) → `SET OID_GEN_CURRENT_PACKET_FILTER`. 필터를 0이 아닌 값으로 써야 프레임이 흐릅니다.
3. **utun 터널** — `PF_SYSTEM` / `SYSPROTO_CONTROL` 로 utun을 엽니다. 엔진이 이더넷 헤더를 붙이거나 떼서 USB RNDIS와 IP 스택 사이를 잇습니다.
4. **DHCP** — 시스템 `ipconfig`가 아니라 엔진이 USB로 DHCPv4 Discover/Request를 보냅니다. ACK의 주소·게이트웨이·DNS를 utun에 점대점으로 적용하고, T1에서 Renew·T2에서 Rebind로 임대를 유지합니다.
5. **경로** — 「기본 연결로 사용」이면 스냅샷 후 기본 경로와 `State:/Network/Global/IPv4|DNS`를 테더링으로 올리고 Wi-Fi를 끕니다. 토글을 끄면 Wi-Fi와 시스템 기본 경로는 그대로 둡니다. 해제·크래시 다음 시작 시 백업 키로 되돌립니다.

연결에는 root가 필요합니다. utun 주소 설정과 라우팅 테이블/`scutil` 변경 때문입니다. GUI는 일반 권한으로 남고, 상태 UNIX 소켓(권한 `0600`)으로 엔진과 대화합니다. 연결을 누르면 macOS wired tethering 인증 패널에서 암호를 받은 뒤, `osascript` 표준입력으로 AppleScript를 넘겨 시스템 자물쇠 창 없이 엔진을 올립니다. 경로 인자는 AppleScript `quoted form of` 로 이스케이프합니다.

## 예상 동작 (테스트 피드백)

단위 테스트 `test_rndis`는 USB 없이 다음을 검증합니다.

- `INITIALIZE` 메시지 레이아웃 (버전 1.0, max transfer)
- 이더넷 프레임 wrap/unwrap 왕복
- 한 벌크 전송에 PACKET_MSG 두 개가 들어 있는 경우 모두 추출
- QUERY 완료의 InformationBufferOffset이 **바이트 8(RequestId)** 기준임
- 패킷 필터 SET 메시지

`test_dhcp`는 Discover/Request 레이아웃, T1 유니캐스트 Renew, T2 브로드캐스트 Rebind, 만료·NAK 복귀를 검증합니다.

`test_net_parse`는 scutil·networksetup 문구(영문/한글)와 Kind/Kind6 필드 경계를 검증합니다. `test_parse`는 장치 JSON/표 파서, 엔진 상태 줄, UNIX 소켓 경로, 번들/개발용 엔진 경로, AppleScript 이스케이프, 헬퍼 경로·Team ID 요구 문자열을 검증합니다. `test_utun`은 잘린 utun 읽기를 폐기하는 조건을 검증합니다.

실제 폰이 연결되면 기대 흐름은 다음과 같습니다.

| 단계 | 기대 결과 | 실패 시 |
|------|-----------|---------|
| USB 삽입, 테더링 OFF | 목록에 기기가 보이며 상태가 **USB 테더링 필요** | 충전 전용 케이블, 또는 잠금 화면에서 허용 안 함 |
| 테더링 ON | 상태가 **테더링 준비됨** (RNDIS 인터페이스 등장) | 폰이 MTP만 노출. USB 구성이 바뀌었는지 확인 |
| 연결 + 관리자 암호 | USB 오픈, RNDIS init, 사용자 공간 DHCP | SIP/커널 확장은 필요 없음. 다른 앱이 USB 인터페이스를 점유하면 `ACCESS` 오류 |
| 수 초 후 | IPv4 (삼성 테더링은 `10.x` 인 경우가 많음) 와 게이트웨이 표시 | DHCP 서버가 없는 일부 커스텀 ROM |
| 브라우저 | 모바일 데이터로 웹 접속 | **기본 연결로 사용**이 꺼져 있으면 시스템 기본 경로는 Wi-Fi 유지 |
| 연결 해제 | 이전 기본 경로·DNS 복구 | 엔진이 강제 종료되면 다음 연결 시작 시 백업을 먼저 복구 |

삼성 기기는 테더링 MAC을 무작위화하는 경우가 있어, 매 연결마다 MAC QUERY를 다시 수행합니다.

## 프로젝트 구조

```
engine/include    공통 헤더
engine/src        RNDIS, USB, utun, DHCP, 네트워크 설정, 엔진, CLI
engine/tests      RNDIS / DHCP 단위 테스트
app               메뉴 막대 SwiftUI GUI
scripts           설치·제거·패키징·스모크 검사
```

코드는 공통 함수를 먼저 두고 진입점(`main`, 엔진 시작)을 뒤에 배치했습니다. 주요 기능 블록에는 한국어/영어 주석이 있습니다.

## 제한 사항

- 첫 연결 시 관리자 암호가 필요합니다.
- USB 3 이론 대역폭까지는 나오지 않습니다. 사용자 공간 복사 경로이며 USB 2.0 High Speed 폰에서는 대략 100–300 Mbps 수준을 목표로 합니다. TX 큐(128)가 가득 차면 프레임을 버리고, 로그에 누적 횟수를 남깁니다.
- CDC-ECM/NCM 전용 가젯(RNDIS가 아닌 일부 임베디드 보드)은 이 버전에서 지원하지 않습니다.
- `utun`은 시스템 설정에 하드웨어 USB 이더넷으로 등록되지 않습니다.
- DHCP 임대는 T1(50%)에서 유니캐스트 Renew, T2(87.5%)에서 브로드캐스트 Rebind를 수행합니다. NAK이거나 임대가 만료되면 Discover부터 다시 받으며, 그때 데이터 경로가 잠시 멈출 수 있습니다.
- 기본 `make dist`는 Apple 공증을 하지 않습니다. 사내 zip은 `install.sh`가 격리 속성을 제거합니다. Developer ID 서명은 `CODESIGN_IDENTITY`로 붙일 수 있습니다.
