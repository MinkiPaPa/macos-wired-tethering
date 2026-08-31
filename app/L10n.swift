/*
 * L10n.swift
 * UI strings: Korean only when the system language is Korean; otherwise English.
 * UI 문구: 시스템 언어가 한국어일 때만 한국어, 그 외는 모두 영어.
 */
import Foundation

enum L10n {
    /// True only when the first preferred language is Korean (`ko`, `ko-KR`, …).
    /// 선호 언어 목록의 첫 항목이 한국어일 때만 참.
    static var usesKorean: Bool {
        guard let first = Locale.preferredLanguages.first else { return false }
        let code = Locale(identifier: first).language.languageCode?.identifier
            ?? first.split(separator: "-").first.map(String.init)
        return code == "ko"
    }

    /// Look up `key` in ko.lproj or en.lproj. Missing table → key itself.
    /// ko.lproj 또는 en.lproj 에서 key 를 찾는다. 테이블이 없으면 key 를 그대로 쓴다.
    static func t(_ key: String) -> String {
        bundle.localizedString(forKey: key, value: key, table: "Localizable")
    }

    static func t(_ key: String, _ args: CVarArg...) -> String {
        String(format: t(key), locale: formatLocale, arguments: args)
    }

    /// Always English. Use for anything written to the log buffer or log file.
    /// 항상 영어. 로그 버퍼·로그 파일에 쓰는 문구에 사용한다.
    static func log(_ key: String) -> String {
        englishBundle.localizedString(forKey: key, value: key, table: "Localizable")
    }

    static func log(_ key: String, _ args: CVarArg...) -> String {
        String(format: log(key), locale: Locale(identifier: "en"), arguments: args)
    }

    /// Map engine/helper raw text (English, Korean leftovers, or keys) to the UI language.
    /// 엔진·헬퍼가 보낸 원문(영어, 예전 한국어, 고정 키)을 UI 언어로 바꾼다.
    static func display(_ raw: String) -> String {
        resolve(raw, english: false)
    }

    /// Same mapping as `display`, but the result is always English for storage.
    /// display 와 같은 매핑이지만, 저장용으로 항상 영어를 반환한다.
    static func displayLog(_ raw: String) -> String {
        resolve(raw, english: true)
    }

    private static func resolve(_ raw: String, english: Bool) -> String {
        let s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.isEmpty { return s }
        let pick: (String) -> String = { english ? log($0) : t($0) }
        if s.hasPrefix("helper.") || s.hasPrefix("engine.") || s.hasPrefix("error.") {
            let localized = pick(s)
            if localized != s { return localized }
        }
        if let key = engineKeys[s] {
            return pick(key)
        }
        return s
    }

    private static var formatLocale: Locale {
        usesKorean ? Locale(identifier: "ko") : Locale(identifier: "en")
    }

    private static func lprojBundle(_ name: String) -> Bundle {
        if let url = Bundle.main.url(forResource: name, withExtension: "lproj"),
           let found = Bundle(url: url) {
            return found
        }
        return Bundle.main
    }

    /// Never let Apple pick `ko` because it is second in the language list.
    /// 선호 목록 두 번째에 한국어가 있어도, 첫 언어가 아니면 영어 번들을 쓴다.
    private static var bundle: Bundle { lprojBundle(usesKorean ? "ko" : "en") }

    private static var englishBundle: Bundle { lprojBundle("en") }

    /* Engine status.error is English; keep Korean leftovers so old sessions still map. */
    /* 엔진 status.error 는 영어다. 예전 한국어 세션도 매핑되도록 남겨 둔다. */
    private static let engineKeys: [String: String] = [
        "USB 케이블이 분리되었습니다": "engine.usb_gone",
        "USB 테더링이 꺼졌습니다. 휴대폰에서 USB 테더링을 다시 켜세요": "engine.tether_off",
        "USB 수신 버퍼를 할당하지 못했습니다": "engine.usb_rx_buf",
        "DHCP 임대를 인터페이스에 적용하지 못했습니다": "engine.dhcp_apply",
        "DHCP 임대를 갱신하지 못했습니다": "engine.dhcp_renew",
        "엔진 오류": "engine.generic",
        "엔진을 시작하지 못했습니다": "engine.start_failed",
        "엔진 옵션이 없습니다": "engine.no_opts",
        "메모리가 부족합니다": "engine.oom",
        "전송 버퍼를 할당하지 못했습니다": "engine.tx_buf",
        "가상 터널(utun)을 열 수 없습니다 (관리자 권한 필요)": "engine.utun",
        "네트워크 세션을 시작하지 못했습니다 (경로 백업 실패)": "engine.net_session",
        "작업 스레드를 시작하지 못했습니다": "engine.thread",
        "연결할 RNDIS 장치가 없습니다. USB 테더링을 켜세요.": "engine.no_rndis",
        "장치가 RNDIS 모드가 아닙니다. 휴대폰에서 USB 테더링을 활성화하세요.": "engine.not_rndis",
        "USB RNDIS 장치를 열 수 없습니다": "engine.usb_open",
        "RNDIS 초기화에 실패했습니다": "engine.rndis_init",
        "DHCP 클라이언트를 만들지 못했습니다": "engine.dhcp_create",
        "The USB cable was unplugged": "engine.usb_gone",
        "USB tethering is off. Turn USB tethering back on on the phone": "engine.tether_off",
        "Could not allocate the USB receive buffer": "engine.usb_rx_buf",
        "Could not apply the DHCP lease to the interface": "engine.dhcp_apply",
        "Could not renew the DHCP lease": "engine.dhcp_renew",
        "Engine error": "engine.generic",
        "Could not start the engine": "engine.start_failed",
        "Engine options are missing": "engine.no_opts",
        "Out of memory": "engine.oom",
        "Could not allocate the transmit buffer": "engine.tx_buf",
        "Could not open the utun tunnel (administrator access required)": "engine.utun",
        "Could not start the network session (route backup failed)": "engine.net_session",
        "Could not start worker threads": "engine.thread",
        "No RNDIS device to connect. Turn on USB tethering.": "engine.no_rndis",
        "The device is not in RNDIS mode. Enable USB tethering on the phone.": "engine.not_rndis",
        "Could not open the USB RNDIS device": "engine.usb_open",
        "RNDIS initialization failed": "engine.rndis_init",
        "Could not create the DHCP client": "engine.dhcp_create",
        "강제 종료로 기본 경로·DNS가 남아 있을 수 있습니다. 다시 시도를 누르면 복구합니다": "hint.route_restore",
        "A forced quit may have left the default route and DNS. Press Retry to restore them": "hint.route_restore",
        "엔진이 응답하지 않습니다. 관리자 인증 또는 헬퍼 등록을 확인하세요": "error.engine_silent",
        "관리자 권한이 거부되었거나 엔진이 시작되지 않았습니다": "error.auth_or_start",
        "엔진이 시작 중에 종료되었습니다": "error.engine_died_start",
        "테더링 엔진이 예기치 않게 종료되었습니다": "error.engine_unexpected",
        "엔진 실행 파일을 찾을 수 없습니다": "error.engine_missing",
        "엔진 실행 파일을 찾을 수 없습니다. make 로 먼저 빌드하세요.": "error.engine_missing_build",
        "관리자 실행 스크립트를 만들지 못했습니다": "error.script",
        "상태 소켓 경로가 너무 깁니다": "error.socket_path",
        "권한 헬퍼에 연결하지 못했습니다": "error.helper_connect",
        "권한 헬퍼 응답이 지연되어 중단했습니다": "error.helper_timeout",
        "헬퍼가 엔진 PID 를 반환하지 않았습니다": "error.helper_pid",
        "헬퍼가 root 가 아닙니다": "helper.error.not_root",
        "허용되지 않은 소켓 경로입니다": "helper.error.bad_socket",
        "허용되지 않은 로그 경로입니다": "helper.error.bad_log",
        "번들 엔진을 찾지 못했거나 서명이 맞지 않습니다": "helper.error.engine_missing",
        "로그 파일을 열 수 없습니다": "helper.error.log_open",
        "네트워크 복구가 시간 초과되었습니다": "helper.error.restore_timeout",
        "helper.error.not_root": "helper.error.not_root",
        "helper.error.bad_socket": "helper.error.bad_socket",
        "helper.error.bad_log": "helper.error.bad_log",
        "helper.error.engine_missing": "helper.error.engine_missing",
        "helper.error.log_open": "helper.error.log_open",
        "helper.error.restore_timeout": "helper.error.restore_timeout"
    ]
}
