/*
 * Models.swift
 * Device / connection models shared by the UI.
 * UI가 공유하는 장치·연결 모델.
 */
import Foundation
import Darwin

enum AppIdentity {
    /// User-visible product name shown in the menu bar, auth prompt, and logs.
    /// 메뉴 막대·인증 창·로그에 쓰는 제품 이름.
    static let displayName = "macOS wired tethering"
    static let errorDomain = "MacOSWiredTethering"
    /// Filesystem prefix for sockets, logs, and markers.
    /// 소켓·로그·마커에 쓰는 파일 이름 접두사.
    static let filePrefix = "macos-wired-tethering"
    /// Engine binary inside the app bundle and `build/`.
    /// 앱 번들과 `build/` 안의 엔진 실행 파일 이름.
    static let engineBinaryName = "macos-wired-tethering-engine"
    /// Preferred marker: user Application Support (not world-writable /tmp).
    /// 기본 마커: 사용자 Application Support (/tmp 가 아님).
    static var wifiMarkerPath: String {
        NSHomeDirectory() + "/Library/Application Support/macos-wired-tethering/wifi-restore"
    }
    static let wifiMarkerTmpPath = "/tmp/macos-wired-tethering-wifi-restore"
    /// Launch daemon Mach service / SMAppService plist (Developer ID builds).
    /// launchd 데몬 Mach 서비스·SMAppService plist (Developer ID 빌드).
    static let helperMachService = "com.macoswiredtethering.helper"
    static let helperPlistName = "com.macoswiredtethering.helper.plist"
    static let helperBinaryName = "macos-wired-tethering-helper"
    static let prefDefaultRouteKey = "preferDefaultRoute"

    static var shortVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0.0"
    }
    static var buildNumber: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "1"
    }
}

enum USBKind: String, Equatable {
    case rndis
    case android
    case unknown

    var label: String {
        switch self {
        case .rndis: return L10n.t("kind.rndis")
        case .android: return L10n.t("kind.android")
        case .unknown: return L10n.t("kind.unknown")
        }
    }

    /// English only — stored in the log buffer.
    /// 영어만. 로그 버퍼에 저장한다.
    var logLabel: String {
        switch self {
        case .rndis: return L10n.log("kind.rndis")
        case .android: return L10n.log("kind.android")
        case .unknown: return L10n.log("kind.unknown")
        }
    }
}

struct USBDevice: Identifiable, Equatable {
    var id: String { "\(location)-\(vid)-\(pid)-\(serial)" }
    var vid: UInt16
    var pid: UInt16
    var location: UInt32
    var name: String
    var vendor: String
    var serial: String
    var kind: USBKind

    var vidPid: String {
        String(format: "%04x:%04x", vid, pid)
    }
}

enum ConnectionState: String {
    case idle
    case connecting
    case connected
    case stopping
    case error
}

enum TetherMessages {
    static var cableUnplug: String { L10n.t("engine.usb_gone") }
    static var tetheringOff: String { L10n.t("engine.tether_off") }
    static var routeRestoreHint: String { L10n.t("hint.route_restore") }
}

struct EngineStatus: Equatable {
    var state: ConnectionState = .idle
    var iface: String = ""
    var ip: String = ""
    var gateway: String = ""
    var dns: String = ""
    var error: String = ""
    var rxBytes: UInt64 = 0
    var txBytes: UInt64 = 0
    var rxErrors: UInt64 = 0
    var txErrors: UInt64 = 0
    var pid: pid_t = 0
    var linkMbps: UInt32 = 0
    var mtu: Int = 0
}

struct LogLine: Identifiable, Equatable {
    let id = UUID()
    let date = Date()
    let level: String
    let message: String
}
