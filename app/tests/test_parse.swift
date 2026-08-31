/*
 * test_parse.swift
 * Device JSON/table parsers and AppleScript escaping.
 * 장치 JSON·표 파서와 AppleScript 이스케이프.
 */
import Foundation

@main
enum TestParse {
static func expect(_ cond: Bool, _ name: String) {
    if !cond {
        fputs("FAIL \(name)\n", stderr)
        exit(1)
    }
}

static func main() {
    let lit = ParseHelpers.appleScriptLiteral("a\"b\\c\n")
    expect(lit == "\"a\\\"b\\\\c\\n\"", "appleScriptLiteral escape")
    expect(ParseHelpers.appleScriptLiteral("") == "\"\"", "empty literal")

    let json = """
    {"type":"devices_begin","count":1}
    {"type":"device","vid":1256,"pid":26720,"location":337641472,"name":"Galaxy","vendor":"Samsung","serial":"ABC","kind":"rndis","control":0,"data":1}
    {"type":"log","message":"skip"}
    {"type":"devices_end"}
    """
    let devices = ParseHelpers.parseDevices(from: json)
    expect(devices.count == 1, "one device")
    expect(devices[0].vid == 1256, "vid")
    expect(devices[0].pid == 26720, "pid")
    expect(devices[0].location == 337641472, "location")
    expect(devices[0].name == "Galaxy", "name")
    expect(devices[0].kind == .rndis, "kind")
    expect(devices[0].serial == "ABC", "serial")

    /* USB gadget class `android` is not the old product name. */
    /* USB 가젯 클래스 `android` 는 예전 제품 이름이 아니다. */
    let androidJson = """
    {"type":"device","vid":1,"pid":2,"location":3,"name":"Phone","vendor":"OEM","serial":"S","kind":"android"}
    """
    let androidDevs = ParseHelpers.parseDevices(from: androidJson)
    expect(androidDevs.count == 1, "android device")
    expect(androidDevs[0].kind == .android, "android kind")

    let table = """
    VID:PID  Location   Kind       Serial     Name
    04e8:6860 141f0000   rndis      XYZ        Galaxy S24
    18d1:4ee1 141f0001   android    SER        Pixel
    감지된 안드로이드/RNDIS 장치가 없습니다.
    """
    let fromTable = ParseHelpers.parseDevicesFromTable(table)
    expect(fromTable.count == 2, "table two")
    expect(fromTable[0].vid == 0x04e8, "table vid")
    expect(fromTable[0].pid == 0x6860, "table pid")
    expect(fromTable[0].name == "Galaxy S24", "table name")
    expect(fromTable[0].kind == .rndis, "table kind")
    expect(fromTable[1].kind == .android, "table android kind")
    expect(fromTable[1].name == "Pixel", "table android name")

    expect(HelperSecurity.isAllowedIPCPath("/var/folders/xx/T/macos-wired-tethering-1.sock", suffix: ".sock"), "tmp sock")
    expect(HelperSecurity.isAllowedIPCPath("/tmp/macos-wired-tethering-engine.log", suffix: ".log"), "tmp log")
    expect(!HelperSecurity.isAllowedIPCPath("/etc/passwd", suffix: ""), "etc")
    expect(!HelperSecurity.isAllowedIPCPath("/tmp/../etc/passwd.sock", suffix: ".sock"), "dotdot")
    expect(!HelperSecurity.isAllowedIPCPath("relative.sock", suffix: ".sock"), "relative")
    expect(!HelperSecurity.isAllowedIPCPath("/tmp/engine", suffix: ".sock"), "suffix")

    expect(
        HelperSecurity.clientCodeSigningRequirement(teamIdentifier: nil)
            == "identifier \"com.macoswiredtethering.app\"",
        "req no team"
    )
    expect(
        HelperSecurity.clientCodeSigningRequirement(teamIdentifier: "ABCD123456")
            == "identifier \"com.macoswiredtethering.app\" and anchor apple generic and certificate leaf[subject.OU] = \"ABCD123456\"",
        "req team"
    )
    expect(
        HelperSecurity.clientCodeSigningRequirement(teamIdentifier: "bad;id")
            == "identifier \"com.macoswiredtethering.app\"",
        "req reject injection"
    )
    expect(HelperSecurity.isSafeTeamID("ABCD123456"), "team ok")
    expect(!HelperSecurity.isSafeTeamID("ABC"), "team short")
    expect(!HelperSecurity.isSafeTeamID("ABCD123456;"), "team punct")

    let tmp = FileManager.default.temporaryDirectory
    let dest = tmp.appendingPathComponent("macos-wired-tethering-sec-dest.log")
    let link = URL(fileURLWithPath: "/tmp/macos-wired-tethering-sec-link.log")
    try? "x".write(to: dest, atomically: true, encoding: .utf8)
    try? FileManager.default.removeItem(at: link)
    do {
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: dest)
        /* /tmp link resolving inside /var/folders is still a temp tree — allowed. */
        /* /tmp 링크가 /var/folders 로 풀려도 임시 트리이므로 허용. */
        expect(HelperSecurity.isAllowedIPCPath(link.path, suffix: ".log"), "symlink in temp")
        let escape = URL(fileURLWithPath: "/tmp/macos-wired-tethering-sec-escape.log")
        try? FileManager.default.removeItem(at: escape)
        try FileManager.default.createSymbolicLink(
            at: escape, withDestinationURL: URL(fileURLWithPath: "/etc/hosts")
        )
        expect(!HelperSecurity.isAllowedIPCPath(escape.path, suffix: ".log"), "symlink escape")
        expect(HelperSecurity.openWritableIPC(escape.path, suffix: ".log") == nil, "nofollow escape")
        try? FileManager.default.removeItem(at: escape)
        let owned = tmp.appendingPathComponent("macos-wired-tethering-sec-owned.log")
        try? FileManager.default.removeItem(at: owned)
        if let h = HelperSecurity.openWritableIPC(owned.path, suffix: ".log") {
            try? h.close()
            let mode = (try? FileManager.default.attributesOfItem(atPath: owned.path)[.posixPermissions] as? NSNumber)?.uint16Value ?? 0
            expect((mode & 0o777) == 0o600, "log mode 0600")
            try? FileManager.default.removeItem(at: owned)
        } else {
            fputs("FAIL open owned log\n", stderr)
            exit(1)
        }
    } catch {
        fputs("FAIL symlink fixture \(error)\n", stderr)
        exit(1)
    }
    try? FileManager.default.removeItem(at: dest)
    try? FileManager.default.removeItem(at: link)

    expect(ParseHelpers.unixPathFits("/var/folders/xx/T/macos-wired-tethering-1.sock"), "unix short")
    expect(!ParseHelpers.unixPathFits(String(repeating: "a", count: 104)), "unix 104")
    var pathBuf = [CChar](repeating: 1, count: ParseHelpers.unixPathCapacity)
    expect(ParseHelpers.writeUnixPath("/tmp/a.sock", to: &pathBuf, capacity: pathBuf.count), "write path")
    expect(String(cString: pathBuf) == "/tmp/a.sock", "path nul")
    expect(!ParseHelpers.writeUnixPath(String(repeating: "b", count: 200), to: &pathBuf, capacity: pathBuf.count), "path overflow")

    switch ParseHelpers.parseEngineLine("not json") {
    case .none: break
    default:
        fputs("FAIL junk line\n", stderr)
        exit(1)
    }
    switch ParseHelpers.parseEngineLine("{\"type\":\"log\",\"level\":\"warn\",\"message\":\"hi\"}") {
    case .log(let level, let message):
        expect(level == "warn" && message == "hi", "log line")
    default:
        fputs("FAIL log line\n", stderr)
        exit(1)
    }
    expect(ParseHelpers.shouldApplyEngineStatus(current: .error, incoming: .idle, userStopping: true) == false, "error holds vs idle")
    expect(ParseHelpers.shouldApplyEngineStatus(current: .error, incoming: .connected, userStopping: true) == false, "error holds vs connected")
    expect(ParseHelpers.shouldApplyEngineStatus(current: .error, incoming: .error, userStopping: true), "error accepts error")
    expect(ParseHelpers.shouldApplyEngineStatus(current: .connecting, incoming: .idle, userStopping: false) == false, "connecting holds vs leftover idle")
    expect(ParseHelpers.shouldApplyEngineStatus(current: .connected, incoming: .idle, userStopping: true) == false, "stop ignores idle")
    expect(ParseHelpers.shouldApplyEngineStatus(current: .connected, incoming: .connected, userStopping: false), "live connected")
    expect(ParseHelpers.shouldKeepStateOnSocketClosed(current: .stopping, userStopping: false), "closed keeps stopping")
    expect(ParseHelpers.shouldKeepStateOnSocketClosed(current: .error, userStopping: false), "closed keeps error")
    expect(ParseHelpers.shouldKeepStateOnSocketClosed(current: .connected, userStopping: false) == false, "closed drops unexpected connected")
    expect(ParseHelpers.shouldKeepStateOnSocketClosed(current: .connecting, userStopping: false) == false, "closed drops unexpected connecting")
    expect(ParseHelpers.shouldKeepStateOnSocketClosed(current: .connecting, userStopping: true), "closed keeps connecting while stopping")

    switch ParseHelpers.parseEngineLine("{\"type\":\"status\",\"state\":\"connected\",\"iface\":\"utun7\",\"ip\":\"10.1.2.3\",\"mtu\":1500,\"rx_errors\":2,\"tx_errors\":3,\"pid\":99,\"link_mbps\":480}") {
    case .status(let s):
        expect(s.state == .connected, "st state")
        expect(s.iface == "utun7", "st iface")
        expect(s.ip == "10.1.2.3", "st ip")
        expect(s.mtu == 1500, "st mtu")
        expect(s.rxErrors == 2 && s.txErrors == 3, "st err")
        expect(s.pid == 99 && s.linkMbps == 480, "st link")
    default:
        fputs("FAIL status line\n", stderr)
        exit(1)
    }

    let appBundle = URL(fileURLWithPath: "/Applications/macOS wired tethering.app")
    let bundled = PrivilegeLauncher.resolveEngineURL(
        bundleURL: appBundle, cwd: "/tmp",
        isExecutable: { $0.hasSuffix("Contents/MacOS/macos-wired-tethering-engine") }
    )
    expect(bundled?.path.hasSuffix("Contents/MacOS/macos-wired-tethering-engine") == true, "app bundle engine")
    let ignoredCwd = PrivilegeLauncher.resolveEngineURL(
        bundleURL: appBundle, cwd: "/tmp",
        isExecutable: { $0.contains("/tmp/build/") }
    )
    expect(ignoredCwd == nil, "app ignores cwd build")
    let dev = PrivilegeLauncher.resolveEngineURL(
        bundleURL: URL(fileURLWithPath: "/Users/dev"), cwd: "/work",
        isExecutable: { $0 == "/work/build/macos-wired-tethering-engine" }
    )
    expect(dev?.path == "/work/build/macos-wired-tethering-engine", "dev cwd engine")

    /* Stored logs stay English even when the UI string was Korean. */
    /* UI가 한국어여도 저장 로그는 영어다. */
    let loggedUnplug = L10n.displayLog("USB 케이블이 분리되었습니다")
    expect(!loggedUnplug.contains("케이블"), "log unplug english")
    expect(loggedUnplug.contains("USB") || loggedUnplug == "engine.usb_gone", "log unplug mapped")
    let loggedKey = L10n.displayLog("error.engine_silent")
    expect(!loggedKey.contains("엔진"), "log silent english")

    print("test_parse: all checks passed")
}
}
