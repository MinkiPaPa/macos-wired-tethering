/*
 * ParseHelpers.swift
 * Pure parsers and AppleScript escaping used by the GUI and unit tests.
 * GUI와 단위 테스트가 쓰는 순수 파서·AppleScript 이스케이프.
 */
import Foundation

enum ParseHelpers {
    /// Escape a Swift string as an AppleScript string literal.
    /// Swift 문자열을 AppleScript 문자열 리터럴로 이스케이프한다.
    static func appleScriptLiteral(_ raw: String) -> String {
        var out = "\""
        for ch in raw.unicodeScalars {
            switch ch {
            case "\\": out += "\\\\"
            case "\"": out += "\\\""
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            default: out.unicodeScalars.append(ch)
            }
        }
        out += "\""
        return out
    }

    static func parseDevices(from raw: String) -> [USBDevice] {
        var result: [USBDevice] = []
        for line in raw.split(whereSeparator: \.isNewline) {
            guard let data = String(line).data(using: .utf8),
                  let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  (obj["type"] as? String) == "device" else { continue }
            let kindRaw = obj["kind"] as? String ?? "unknown"
            result.append(USBDevice(
                vid: nsNumber(obj["vid"]),
                pid: nsNumber(obj["pid"]),
                location: nsNumber32(obj["location"]),
                name: obj["name"] as? String ?? "Android",
                vendor: obj["vendor"] as? String ?? "",
                serial: obj["serial"] as? String ?? "",
                kind: USBKind(rawValue: kindRaw) ?? .unknown
            ))
        }
        return result
    }

    /// Fallback parser for the human-readable `list` table.
    /// 사람이 읽는 list 표 형식의 대체 파서.
    static func parseDevicesFromTable(_ raw: String) -> [USBDevice] {
        var result: [USBDevice] = []
        for line in raw.split(whereSeparator: \.isNewline) {
            let s = String(line).trimmingCharacters(in: .whitespaces)
            if s.isEmpty || s.hasPrefix("VID:") || s.hasPrefix("감지") || s.hasPrefix("USB") { continue }
            let parts = s.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            guard parts.count >= 5 else { continue }
            let vp = parts[0].split(separator: ":")
            guard vp.count == 2,
                  let vid = UInt16(vp[0], radix: 16),
                  let pid = UInt16(vp[1], radix: 16),
                  let location = UInt32(parts[1], radix: 16) else { continue }
            let kind = USBKind(rawValue: parts[2]) ?? .unknown
            let serial = parts[3] == "-" ? "" : parts[3]
            let name = parts[4...].joined(separator: " ")
            result.append(USBDevice(
                vid: vid, pid: pid, location: location,
                name: name, vendor: "", serial: serial, kind: kind
            ))
        }
        return result
    }

    /// Darwin `sockaddr_un.sun_path` is 104 bytes including the NUL.
    /// Darwin `sockaddr_un.sun_path` 는 NUL 포함 104바이트.
    static let unixPathCapacity = 104

    static func unixPathFits(_ path: String) -> Bool {
        path.utf8.count < unixPathCapacity
    }

    /// Copy `path` into `dest` and force a trailing NUL. False if it does not fit.
    /// path 를 dest 에 복사하고 끝에 NUL 을 넣는다. 길이 초과면 거짓.
    static func writeUnixPath(_ path: String, to dest: UnsafeMutablePointer<CChar>, capacity: Int) -> Bool {
        let bytes = Array(path.utf8)
        if bytes.count >= capacity { return false }
        dest.initialize(repeating: 0, count: capacity)
        var i = 0
        for b in bytes {
            dest[i] = CChar(bitPattern: b)
            i += 1
        }
        dest[i] = 0
        return true
    }

    /// True if an engine status line should replace the GUI connection state.
    /// 엔진 상태 줄이 GUI 연결 상태를 바꿔도 되는지면 참.
    static func shouldApplyEngineStatus(
        current: ConnectionState,
        incoming: ConnectionState,
        userStopping: Bool
    ) -> Bool {
        if incoming == .error { return true }
        if current == .error { return false }
        if userStopping { return false }
        if incoming == .idle && current == .connecting { return false }
        return true
    }

    /// Socket close during user stop / error / stopping is teardown, not a new event.
    /// Unexpected close while connecting or connected must surface as a failure.
    /// 사용자 해제·오류·종료 중의 소켓 종료는 새 사건이 아니다.
    /// 연결 중·연결됨에서 갑자기 닫히면 실패로 보여야 한다.
    static func shouldKeepStateOnSocketClosed(
        current: ConnectionState,
        userStopping: Bool
    ) -> Bool {
        if userStopping { return true }
        switch current {
        case .error, .stopping:
            return true
        case .connecting, .connected, .idle:
            return false
        }
    }

    enum EngineLine: Equatable {
        case log(level: String, message: String)
        case status(EngineStatus)
    }

    static func parseEngineLine(_ line: String) -> EngineLine? {
        guard let data = line.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = obj["type"] as? String else { return nil }
        if type == "log" {
            return .log(
                level: obj["level"] as? String ?? "info",
                message: obj["message"] as? String ?? ""
            )
        }
        if type == "status" {
            var s = EngineStatus()
            s.state = ConnectionState(rawValue: obj["state"] as? String ?? "idle") ?? .idle
            s.iface = obj["iface"] as? String ?? ""
            s.ip = obj["ip"] as? String ?? ""
            s.gateway = obj["gateway"] as? String ?? ""
            s.dns = obj["dns"] as? String ?? ""
            s.error = obj["error"] as? String ?? ""
            s.rxBytes = uint64(obj["rx_bytes"])
            s.txBytes = uint64(obj["tx_bytes"])
            s.rxErrors = uint64(obj["rx_errors"])
            s.txErrors = uint64(obj["tx_errors"])
            s.pid = pid_t(truncatingIfNeeded: uint64(obj["pid"]))
            s.linkMbps = UInt32(truncatingIfNeeded: uint64(obj["link_mbps"]))
            s.mtu = Int(truncatingIfNeeded: uint64(obj["mtu"]))
            return .status(s)
        }
        return nil
    }

    static func uint64(_ v: Any?) -> UInt64 {
        if let n = v as? UInt64 { return n }
        if let n = v as? Int { return UInt64(n) }
        if let n = v as? Double { return UInt64(n) }
        return 0
    }

    private static func nsNumber(_ v: Any?) -> UInt16 {
        if let n = v as? NSNumber { return n.uint16Value }
        if let n = v as? Int { return UInt16(truncatingIfNeeded: n) }
        return 0
    }

    private static func nsNumber32(_ v: Any?) -> UInt32 {
        if let n = v as? NSNumber { return n.uint32Value }
        if let n = v as? Int { return UInt32(truncatingIfNeeded: n) }
        return 0
    }
}
