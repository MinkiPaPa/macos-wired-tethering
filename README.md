# macOS wired tethering

**Android USB tethering for macOS.** No kernel extension, no DriverKit, no SIP off.

macOS용 **Android USB 테더링** 메뉴 막대 앱입니다. 폰을 USB로 연결하고 USB 테더링만 켜면, Mac이 모바일 데이터로 인터넷에 접속합니다.

[![macOS 14+](https://img.shields.io/badge/macOS-14%2B-000000)](#요구-사항)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0.0-informational)](#)

Developed by [Minkipapa](https://github.com/MinkiPaPa)

---

macOS에는 Microsoft RNDIS 드라이버가 없습니다. 그래서 안드로이드에서 USB 테더링을 켜도 **시스템 설정에 새 네트워크 장치가 나타나지 않습니다.** 이 앱은 사용자 공간에서 RNDIS를 처리하고, `utun` 터널로 IP를 macOS에 넘깁니다. Dock 아이콘과 메인 창은 없고, 메뉴 막대에서만 동작합니다.

```
Android phone (USB RNDIS)
        │
        ▼
  tethering engine (userspace)
        │
        ▼
     utun  →  macOS  (Safari, Chrome, default route / DNS)
```

## 주요 기능

- USB 케이블만으로 Android 테더링 (kext / SIP 해제 불필요)
- 메뉴 막대에서 장치 목록, 연결, 속도, 로그
- **기본 연결로 사용** — 테더링을 Wi-Fi보다 우선하고, 해제 시 이전 경로·DNS·Wi-Fi 복구
- 관리자 암호는 앱 자체 인증 창에서만 받으며 **저장하지 않음**
- Developer ID 빌드에서는 **권한 헬퍼**를 한 번 허용하면 이후 암호 없이 연결
- UI 언어: 시스템 언어가 한국어이면 한국어, 그 외는 영어. 저장 로그는 영어
- 개인 정보를 개발자 서버로 보내지 않음 ([PRIVACY.md](PRIVACY.md))

## 요구 사항

- macOS 14 Sonoma 이상 (Apple Silicon 또는 Intel)
- 데이터 전송이 되는 USB 케이블 (충전 전용 케이블은 불가)
- 연결 시 **관리자 암호** 한 번 (헬퍼가 허용된 Developer ID 빌드는 이후 생략)

소스에서 빌드할 때는 Xcode Command Line Tools (`xcode-select --install`)가 필요합니다.

## 설치

배포 zip을 받은 경우, Finder에서 앱만 더블클릭하지 말고 압축을 푼 뒤 `install.sh`를 실행하세요. AirDrop·메일로 받은 앱은 Gatekeeper가 막을 수 있고, 스크립트가 격리 속성을 제거합니다.

```bash
unzip macos-wired-tethering-1.0.0-macos.zip
cd macos-wired-tethering-1.0.0
./install.sh              # /Applications 로 복사
./install.sh --open       # 설치 후 실행
./uninstall.sh            # 제거
```

체크섬을 확인하려면:

```bash
shasum -a 256 -c macos-wired-tethering-1.0.0-macos.zip.sha256
```

개발 머신에서 직접 넣으려면 저장소에서 `make install` / `make uninstall`을 사용합니다.

## 사용 방법

1. 안드로이드 폰을 Mac에 USB로 연결합니다.
2. 폰에서 **USB 테더링**을 켭니다.
   - 설정 → 네트워크 및 인터넷 → 핫스팟 및 테더링 → USB 테더링
   - 삼성: 설정 → 연결 → 모바일 핫스팟 및 테더링 → USB 테더링
3. 메뉴 막대 아이콘을 열고, 장치가 **테더링 준비됨**인지 확인한 뒤 **연결**을 누릅니다.
4. 헬퍼가 켜져 있으면 바로 연결됩니다. 아니면 앱 인증 창에 관리자 암호를 입력합니다. 시스템 자물쇠 창은 뜨지 않습니다.
5. IPv4와 게이트웨이가 보이면 브라우저로 모바일 데이터를 사용할 수 있습니다.

**기본 연결로 사용**을 켜면 USB 테더링이 Wi-Fi보다 우선합니다. 끄면 시스템 기본 경로는 그대로 두고, 테더링 인터페이스만 올립니다.

연결 해제 후 Wi-Fi(또는 이전 기본 경로)가 돌아와야 합니다. 앱을 강제 종료했다면 다음 **다시 시도**에서 남은 경로를 복구합니다.

| 단계 | 정상 | 안 될 때 |
|------|------|----------|
| USB 연결, 테더링 OFF | 목록에 기기, **USB 테더링 필요** | 충전 전용 케이블, 잠금 화면에서 USB 미허용 |
| 테더링 ON | **테더링 준비됨** | 폰이 MTP만 노출. USB 구성을 다시 선택 |
| 연결 | IPv4·게이트웨이 표시, 웹 접속 | 다른 앱이 USB를 점유하면 열기 실패 |
| 해제 | 이전 경로·DNS·Wi-Fi 복구 | 강제 종료 후에는 다시 시도로 복구 |

## 빌드

```bash
cd "macOS wired tethering"
make            # 단위 테스트 + macOS wired tethering.app
make run        # 앱 실행
make smoke      # 폰 없이 번들 건전성 검사
make test-tsan  # ThreadSanitizer (C 단위 테스트)
```

| 경로 | 설명 |
|------|------|
| `build/macOS wired tethering.app` | 메뉴 막대 앱 (엔진·헬퍼 포함) |
| `build/macos-wired-tethering-engine` | CLI 엔진 |
| `build/macos-wired-tethering-helper` | 권한 헬퍼 |

### CLI

장치 검색은 root가 필요 없습니다.

```bash
./build/macos-wired-tethering-engine list
./build/macos-wired-tethering-engine list --json
./build/macos-wired-tethering-engine --version
sudo ./build/macos-wired-tethering-engine connect --default-route
sudo ./build/macos-wired-tethering-engine connect --vid 04e8 --pid 6860
```

종료는 `Ctrl+C`입니다. GUI에서 연결을 끊으면 엔진이 경로를 복구한 뒤 종료합니다.

## 배포

Mac App Store에는 올릴 수 없습니다. 샌드박스와 root `utun`/라우트/헬퍼가 양립하지 않습니다.

사내·직접 배포는 zip + `install.sh`가 기본입니다. Makefile은 Apple 공증(`notarytool`)을 호출하지 않습니다.

```bash
# 로컬·사내: ad-hoc 서명 zip
make dist

# 권장: Developer ID
make dist CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"
make dist-pkg CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"
```

`dist/`에 앱, `install.sh` / `uninstall.sh`, README, PRIVACY, LICENSE, SHA-256이 들어갑니다.

ad-hoc zip 제한:

- 반드시 `install.sh`로 설치하세요. Finder 더블클릭만으로는 Gatekeeper가 막을 수 있습니다.
- 권한 헬퍼는 등록되지 않는 경우가 많습니다. 그때는 연결마다 관리자 암호를 넣습니다.
- 바이너리는 **빌드한 Mac의 CPU**입니다. Apple Silicon zip은 Intel에서 실행되지 않습니다.
- 회사 밖 배포에는 Developer ID와 공증이 필요합니다.

외부 배포를 준비할 때: Developer ID로 `make dist`한 뒤, 계정에서 `notarytool submit` → `stapler staple`을 따로 수행하면 됩니다.

## 동작 원리

1. **USB** — IOKit으로 RNDIS 인터페이스를 찾습니다 (`0xE0/0x01/0x03`, Android CDC vendor, Miscellaneous RNDIS).
2. **RNDIS** — `INITIALIZE` → MAC QUERY → 패킷 필터 SET. 필터가 0이 아니어야 프레임이 흐릅니다.
3. **utun** — L3 점대점 터널. 시스템 설정에는 USB 이더넷 포트로 안 보일 수 있습니다.
4. **DHCP** — 엔진이 USB로 Discover/Request를 보내고, T1 Renew · T2 Rebind로 임대를 유지합니다.
5. **경로** — 「기본 연결로 사용」이면 스냅샷 후 기본 경로와 Global IPv4/DNS를 테더링으로 올리고 Wi-Fi를 끕니다. 해제·다음 시작 시 백업으로 되돌립니다.

GUI는 일반 권한입니다. utun과 라우트는 root 엔진이 담당하고, UNIX 소켓(`0600`)으로 상태를 주고받습니다.

## 제한 사항

- 첫 연결(또는 헬퍼 미등록)에는 관리자 암호가 필요합니다.
- USB 3 이론 속도까지는 나오지 않습니다. 사용자 공간 경로이며, USB 2.0 High Speed 폰에서는 대략 100–300 Mbps를 목표로 합니다.
- CDC-ECM/NCM 전용 가젯(RNDIS가 아닌 보드)은 지원하지 않습니다.
- DHCP NAK이거나 임대가 만료되면 Discover부터 다시 받으며, 그동안 데이터 경로가 잠시 멈출 수 있습니다.

## 프로젝트 구조

```
engine/     RNDIS, USB, utun, DHCP, 네트워크, CLI
app/        메뉴 막대 SwiftUI GUI
helper/     SMAppService 권한 헬퍼
scripts/    설치, 제거, 패키징, 스모크 검사
```

## 라이선스

[MIT](LICENSE) © 2026 macOS wired tethering contributors

Developed by [Minkipapa](https://github.com/MinkiPaPa)
