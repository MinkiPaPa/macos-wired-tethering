/*
 * WiFiPower.swift
 * User-session Wi-Fi radio control via CoreWLAN (not the root engine).
 * 사용자 세션에서 CoreWLAN 으로 Wi-Fi 라디오를 제어한다. root 엔진이 아니다.
 *
 * networksetup from an osascript admin shell can turn the radio off, but on
 * modern macOS turning it back on often only sticks from the Aqua/GUI session.
 * osascript 관리자 셸의 networksetup 은 라디오를 끌 수는 있어도, 최신 macOS 에서는
 * 다시 켜는 동작이 Aqua/GUI 세션에서만 유지되는 경우가 많다.
 */
import Foundation
import CoreWLAN
import Darwin

enum WiFiPower {
    private static var markerPaths: [String] {
        [
            AppIdentity.wifiMarkerPath,
            AppIdentity.wifiMarkerTmpPath
        ]
    }

    /// True if the hardware radio reports power on.
    /// 하드웨어 라디오가 켜짐이면 참.
    static func isOn() -> Bool {
        if let iface = primaryInterface(), iface.powerOn() { return true }
        return false
    }

    /// Turn the radio on from this GUI process. Best-effort; logs via callback.
    /// 이 GUI 프로세스에서 라디오를 켠다. 실패해도 진행하며 콜백으로 남긴다.
    @discardableResult
    static func turnOn() -> String {
        var notes: [String] = []
        let client = CWWiFiClient.shared()
        var ifaces = client.interfaces() ?? []
        if ifaces.isEmpty, let one = client.interface() {
            ifaces = [one]
        }
        var ok = false
        for iface in ifaces {
            let name = iface.interfaceName ?? "?"
            do {
                try iface.setPower(true)
                /* setPower can return before the radio finishes coming up. */
                /* setPower 는 라디오가 다 켜지기 전에 돌아올 수 있다. */
                usleep(250_000)
                if iface.powerOn() {
                    notes.append(L10n.log("log.wifi_on", name))
                    ok = true
                } else {
                    notes.append(L10n.log("log.wifi_pending", name))
                }
            } catch {
                notes.append(L10n.log("log.wifi_err", name, error.localizedDescription))
            }
        }
        if !ok {
            let dev = markerDevice() ?? "en0"
            if runNetworksetup(device: dev, on: true) {
                notes.append(L10n.log("log.wifi_ns_on", dev))
                ok = true
            } else {
                notes.append(L10n.log("log.wifi_ns_fail", dev))
            }
        }
        if ok { clearMarker() }
        return notes.joined(separator: " · ")
    }

    static func markerPresent() -> Bool {
        markerPaths.contains { FileManager.default.fileExists(atPath: $0) }
    }

    static func markerDevice() -> String? {
        for path in markerPaths {
            guard let raw = try? String(contentsOfFile: path, encoding: .utf8) else { continue }
            let d = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if !d.isEmpty { return d }
        }
        return nil
    }

    static func clearMarker() {
        for path in markerPaths {
            try? FileManager.default.removeItem(atPath: path)
        }
    }

    private static func primaryInterface() -> CWInterface? {
        let client = CWWiFiClient.shared()
        if let list = client.interfaces(), let first = list.first { return first }
        return client.interface()
    }

    private static func runNetworksetup(device: String, on: Bool) -> Bool {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/sbin/networksetup")
        proc.arguments = ["-setairportpower", device, on ? "on" : "off"]
        proc.standardOutput = Pipe()
        proc.standardError = Pipe()
        do {
            try proc.run()
            proc.waitUntilExit()
            return proc.terminationStatus == 0
        } catch {
            return false
        }
    }
}
