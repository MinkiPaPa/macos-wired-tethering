# DEVELOPMENT — macOS wired tethering

사용자용 안내(설치·사용)는 [README.md](README.md)를 보세요. 이 문서는 소스 빌드, CLI, 배포, 엔진 동작, 프로젝트 구조를 설명합니다.

End-user install and usage live in [README.md](README.md). This file covers build, CLI, packaging, engine internals, and repo layout.

## 요구 사항

- macOS 14 Sonoma 이상
- Xcode Command Line Tools (`xcode-select --install`)
- 소스에서 설치·실행할 때는 관리자 암호가 필요할 수 있음

## 빠른 시작

```bash
cd "macOS wired tethering"
make            # 단위 테스트 + macOS wired tethering.app
make run        # 앱 실행
make smoke      # 폰 없이 번들 건전성 검사
make test-tsan  # ThreadSanitizer (C 단위 테스트)
make install    # /Applications 로 복사
make uninstall  # 제거
```

| 경로 | 설명 |
|------|------|
| `build/macOS wired tethering.app` | 메뉴 막대 앱 (엔진·헬퍼 포함) |
| `build/macos-wired-tethering-engine` | CLI 엔진 |
| `build/macos-wired-tethering-helper` | 권한 헬퍼 |
| `dist/` | `make dist` 산출물 (gitignore) |

## Makefile 타깃

| 타깃 | 역할 |
|------|------|
| `make` / `make all` | `test` 후 `app` |
| `make engine` | C 엔진만 빌드 |
| `make helper` | SMAppService 헬퍼만 빌드 |
| `make app` | 엔진·헬퍼·SwiftUI를 `.app` 번들로 조립 |
| `make test` | C/Swift 단위 테스트 |
| `make test-tsan` | C 테스트에 ThreadSanitizer |
| `make smoke` | 번들 경로·실행 파일·Info.plist 검사 |
| `make run` | 로컬 번들 실행 |
| `make install` / `make uninstall` | `scripts/install.sh` / `uninstall.sh` |
| `make dist` | ad-hoc 또는 Developer ID zip |
| `make dist-pkg` | zip + `pkgbuild` pkg |
| `make clean` | `build/` 와 `dist/` 삭제 |

버전은 Makefile의 `VERSION` / `BUILD_NUMBER`입니다. 서명 인증서가 있으면:

```bash
make dist CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"
```

비우면 ad-hoc(`-`) 서명입니다. Makefile은 Apple 공증(`notarytool`)을 호출하지 않습니다.

## CLI

장치 검색은 root가 필요 없습니다. 연결은 root가 필요합니다.

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

사내·직접 배포는 zip + `install.sh`가 기본입니다.

```bash
# 로컬·사내: ad-hoc 서명 zip
make dist

# 권장: Developer ID
make dist CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"
make dist-pkg CODESIGN_IDENTITY="Developer ID Application: Example Corp (TEAMID)"
```

`scripts/package.sh`가 `dist/`에 앱, `install.sh` / `uninstall.sh`, `설치방법.txt` / `INSTALL.txt`, README, PRIVACY, LICENSE, SHA-256을 넣습니다. `DEVELOPMENT.md`는 사용자 zip에 넣지 않습니다.

서명 순서(안쪽부터, `--deep` 사용 안 함):

1. helper (`helper/helper.entitlements`)
2. engine (`engine/engine.entitlements`)
3. app (`app/MacOSWiredTethering.entitlements`)

Hardened Runtime은 붙입니다. `notarytool` / `stapler` / `productsign`은 호출하지 않습니다.

ad-hoc zip 제한:

- 반드시 `install.sh`로 설치하세요. Finder 더블클릭만으로는 Gatekeeper가 막을 수 있습니다.
- 권한 헬퍼는 등록되지 않는 경우가 많습니다. 그때는 연결마다 관리자 암호를 넣습니다.
- 바이너리는 **빌드한 Mac의 CPU**입니다. Apple Silicon zip은 Intel에서 실행되지 않습니다.
- 회사 밖 배포에는 Developer ID와 공증이 필요합니다.

외부 배포를 준비할 때: Developer ID로 `make dist`한 뒤, 계정에서 `notarytool submit` → `stapler staple`을 따로 수행하면 됩니다.

zip 체크섬:

```bash
shasum -a 256 -c macos-wired-tethering-1.0.0-macos.zip.sha256
```

## 동작 원리

1. **USB** — IOKit으로 RNDIS 인터페이스를 찾습니다 (`0xE0/0x01/0x03`, Android CDC vendor, Miscellaneous RNDIS).
2. **RNDIS** — `INITIALIZE` → MAC QUERY → 패킷 필터 SET. 필터가 0이 아니어야 프레임이 흐릅니다.
3. **utun** — L3 점대점 터널. 시스템 설정에는 USB 이더넷 포트로 안 보일 수 있습니다.
4. **DHCP** — 엔진이 USB로 Discover/Request를 보내고, T1 Renew · T2 Rebind로 임대를 유지합니다.
5. **경로** — 「기본 연결로 사용」이면 스냅샷 후 기본 경로와 Global IPv4/DNS를 테더링으로 올리고 Wi-Fi를 끕니다. 해제·다음 시작 시 백업으로 되돌립니다.

GUI는 일반 권한입니다. utun과 라우트는 root 엔진이 담당하고, UNIX 소켓(`0600`)으로 상태를 주고받습니다.

권한 헬퍼가 등록되어 있으면 SMAppService가 엔진을 root로 띄웁니다. 없으면 앱 인증 창에서 받은 관리자 암호를 AppleScript 표준입력으로만 전달합니다. 암호는 디스크에 쓰지 않고, `ps` argv에도 올리지 않습니다.

## 안정성 메모

- 엔진은 `SIGPIPE`를 무시하고 상태 소켓에 `SO_NOSIGPIPE`를 겁니다. GUI가 소켓을 닫으면 엔진이 경로를 복구한 뒤 종료합니다.
- GUI는 소켓이 예기치 않게 닫히거나 엔진 PID가 사라지면 연결을 끊고 복구합니다. 헬퍼 해제는 세션이 살아 있으면 먼저 연결을 끊습니다.
- 세션 종료·복구 경로는 게시된 네트워크 경로와 서비스를 함께 지웁니다.

## 언어

- UI: 시스템 선호 언어의 첫 항목이 `ko`이면 한국어, 그 외는 영어 (`app/L10n.swift`, `app/*/lproj/Localizable.strings`).
- 저장 로그와 엔진 `at_log`는 영어입니다. UI에 보여줄 때만 `L10n.displayLog`로 번역합니다.

## 제한 사항

- 첫 연결(또는 헬퍼 미등록)에는 관리자 암호가 필요합니다.
- USB 3 이론 속도까지는 나오지 않습니다. 사용자 공간 경로이며, USB 2.0 High Speed 폰에서는 대략 100–300 Mbps를 목표로 합니다.
- CDC-ECM/NCM 전용 가젯(RNDIS가 아닌 보드)은 지원하지 않습니다.
- DHCP NAK이거나 임대가 만료되면 Discover부터 다시 받으며, 그동안 데이터 경로가 잠시 멈출 수 있습니다.

## 프로젝트 구조

```
engine/     RNDIS, USB, utun, DHCP, 네트워크, CLI
  include/  공개 헤더
  src/      엔진 구현
  tests/    C 단위 테스트
app/        메뉴 막대 SwiftUI GUI, 로컬라이즈, 엔타이틀먼트
  tests/    Swift 파서 테스트
helper/     SMAppService 권한 헬퍼, LaunchDaemon plist
scripts/    설치, 제거, 패키징, 스모크 검사, 설치 안내
```

주요 식별자:

| 항목 | 값 |
|------|-----|
| 앱 번들 ID | `com.macoswiredtethering.app` |
| 엔진 ID | `com.macoswiredtethering.engine` |
| 헬퍼 ID | `com.macoswiredtethering.helper` |
| 최소 OS | 14.0 |

## 라이선스

[MIT](LICENSE) © 2026 macOS wired tethering contributors

Developed by [Minkipapa](https://github.com/MinkiPaPa)
