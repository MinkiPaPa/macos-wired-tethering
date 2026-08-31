# macOS wired tethering

**Android USB tethering for macOS.** No kernel extension, no DriverKit, no SIP off.

macOS용 **Android USB 테더링** 메뉴 막대 앱입니다. 폰을 USB로 연결하고 USB 테더링만 켜면, Mac이 모바일 데이터로 인터넷에 접속합니다.

[![macOS 14+](https://img.shields.io/badge/macOS-14%2B-000000)](#요구-사항)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0.0-informational)](#)

Developed by [Minkipapa](https://github.com/MinkiPaPa)

---

macOS에는 Microsoft RNDIS 드라이버가 없습니다. 안드로이드에서 USB 테더링을 켜도 **시스템 설정에 새 네트워크 장치가 나타나지 않습니다.** 이 앱은 사용자 공간에서 RNDIS를 처리하고, `utun` 터널로 IP를 macOS에 넘깁니다. Dock 아이콘과 메인 창은 없고, 메뉴 막대에서만 동작합니다.

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
- UI 언어: 시스템 언어가 한국어이면 한국어, 그 외는 영어
- 개인 정보를 개발자 서버로 보내지 않음 ([PRIVACY.md](PRIVACY.md))

## 요구 사항

- macOS 14 Sonoma 이상 (Apple Silicon 또는 Intel)
- 데이터 전송이 되는 USB 케이블 (충전 전용 케이블은 불가)
- 연결 시 **관리자 암호** 한 번 (헬퍼가 허용된 Developer ID 빌드는 이후 생략)

## 설치

배포 zip을 받은 경우, Finder에서 앱만 더블클릭하지 마세요. 압축을 푼 뒤 `install.sh`를 실행하세요. AirDrop·메일로 받은 앱은 Gatekeeper가 막을 수 있고, 스크립트가 격리 속성을 제거합니다.

```bash
unzip macos-wired-tethering-1.0.0-macos.zip
cd macos-wired-tethering-1.0.0
./install.sh              # /Applications 로 복사
./install.sh --open       # 설치 후 실행
./uninstall.sh            # 제거
```

자세한 순서는 zip 안의 `설치방법.txt` / `INSTALL.txt`를 보세요.

## 사용 방법

1. 안드로이드 폰을 Mac에 USB로 연결합니다.
2. 폰에서 **USB 테더링**을 켭니다.
   - 설정 → 네트워크 및 인터넷 → 핫스팟 및 테더링 → USB 테더링
   - 삼성: 설정 → 연결 → 모바일 핫스팟 및 테더링 → USB 테더링
3. 메뉴 막대 아이콘을 열고, 장치가 **테더링 준비됨**인지 확인한 뒤 **연결**을 누릅니다.
4. 헬퍼가 켜져 있으면 바로 연결됩니다. 아니면 앱 인증 창에 관리자 암호를 입력합니다.
5. IPv4와 게이트웨이가 보이면 브라우저로 모바일 데이터를 사용할 수 있습니다.

**기본 연결로 사용**을 켜면 USB 테더링이 Wi-Fi보다 우선합니다. 끄면 시스템 기본 경로는 그대로 두고, 테더링 인터페이스만 올립니다.

| 단계 | 정상 | 안 될 때 |
|------|------|----------|
| USB 연결, 테더링 OFF | 목록에 기기, **USB 테더링 필요** | 충전 전용 케이블, 잠금 화면에서 USB 미허용 |
| 테더링 ON | **테더링 준비됨** | 폰이 MTP만 노출. USB 구성을 다시 선택 |
| 연결 | IPv4·게이트웨이 표시, 웹 접속 | 다른 앱이 USB를 점유하면 열기 실패 |
| 해제 | 이전 경로·DNS·Wi-Fi 복구 | 강제 종료 후에는 다시 시도로 복구 |

## 제한 사항

- 첫 연결(또는 헬퍼 미등록)에는 관리자 암호가 필요합니다.
- USB 3 이론 속도까지는 나오지 않습니다. USB 2.0 High Speed 폰에서는 대략 100–300 Mbps를 목표로 합니다.
- CDC-ECM/NCM 전용 가젯(RNDIS가 아닌 보드)은 지원하지 않습니다.
- Mac App Store에는 올릴 수 없습니다. 샌드박스와 root `utun`/라우트/헬퍼가 양립하지 않습니다.

## 개발자

빌드, CLI, 배포, 동작 원리는 [DEVELOPMENT.md](DEVELOPMENT.md)를 보세요.

## 라이선스

[MIT](LICENSE) © 2026 macOS wired tethering contributors

Developed by [Minkipapa](https://github.com/MinkiPaPa)
