/*
 * TetherController.swift
 * Orchestrates device scan, privileged engine, and JSON status.
 * 장치 검색, 권한 있는 엔진, JSON 상태를 조율한다.
 */
import Foundation
import AppKit
import SwiftUI
import Darwin

@MainActor
final class TetherController: ObservableObject {
    @Published var devices: [USBDevice] = []
    @Published var selectedID: String?
    @Published var status = EngineStatus()
    @Published var logs: [LogLine] = []
    @Published var preferDefaultRoute = true {
        didSet {
            UserDefaults.standard.set(preferDefaultRoute, forKey: AppIdentity.prefDefaultRouteKey)
        }
    }
    @Published var lastRateRx: Double = 0
    @Published var lastRateTx: Double = 0
    @Published var peakRateRx: Double = 0
    @Published var peakRateTx: Double = 0
    @Published var menuIconFrame: Int = 0
    @Published var helperStatus: HelperStatus = .unavailable
    @Published var pendingRouteRestore = false

    private var session: EngineSession?
    private var privilegedProcess: Process?
    private var engineWatchdog: Task<Void, Never>?
    private var sawEngineTraffic = false
    private var userStopping = false
    private var pollTask: Task<Void, Never>?
    private var lastBytes: (rx: UInt64, tx: UInt64, at: Date)?
    private var enginePid: pid_t = 0
    private var lastSocketPath: String?
    private var menuIconTask: Task<Void, Never>?
    private var logWindow: NSWindow?
    private var aboutWindow: NSWindow?

    private var connectGen: UInt64 = 0
    private var restoreWifiOnDisconnect = false
    private var connectedLocation: UInt32 = 0
    private var usbMissingPolls = 0
    private var disconnectRunning = false

    var connectionState: ConnectionState { status.state }
    var isSessionActive: Bool {
        status.state == .connecting || status.state == .connected || status.state == .stopping
    }
    /* True when quit must wait for the privileged engine to restore routes. */
    /* 종료 전에 권한 엔진이 경로를 복구할 때까지 기다려야 하면 참. */
    var needsEngineTeardown: Bool {
        session != nil || privilegedProcess != nil || enginePid > 1 || isSessionActive
    }
    var canConnect: Bool {
        guard let dev = selectedDevice else { return false }
        return dev.kind == .rndis && !isSessionActive
    }
    var selectedDevice: USBDevice? {
        devices.first { $0.id == selectedID } ?? devices.first
    }
    var statusSummary: String {
        switch status.state {
        case .connected:
            return status.ip.isEmpty ? L10n.t("status.connected") : L10n.t("status.connected_ip", status.ip)
        case .connecting:
            return status.ip.isEmpty ? L10n.t("status.connecting") : L10n.t("status.renewing", status.ip)
        case .error: return status.error.isEmpty ? L10n.t("status.error") : status.error
        case .stopping: return L10n.t("status.stopping")
        case .idle: return devices.isEmpty ? L10n.t("status.idle_empty") : L10n.t("status.idle")
        }
    }
    var liveRateLine: String {
        L10n.t("metric.live", Self.formatRate(lastRateRx), Self.formatRate(lastRateTx))
    }

    var peakRateLine: String {
        L10n.t("metric.peak", Self.formatRate(peakRateRx), Self.formatRate(peakRateTx))
    }

    var byteLine: String {
        L10n.t("metric.bytes", Self.formatBytes(status.rxBytes), Self.formatBytes(status.txBytes))
    }

    var dropLine: String {
        L10n.t("metric.drops", "\(status.rxErrors)", "\(status.txErrors)")
    }

    var helperStatusLine: String { helperStatus.label }

    var linkSummary: String {
        if status.linkMbps == 0 { return "—" }
        if status.linkMbps <= 12 {
            return L10n.t("metric.link_usb1", status.linkMbps)
        }
        if status.linkMbps <= 480 {
            return L10n.t("metric.link_usb2", status.linkMbps)
        }
        return L10n.t("metric.link_usb3", status.linkMbps)
    }

    /// Open (or focus) the standalone log window.
    /// 독립 로그 창을 열거나 앞으로 가져온다.
    func openLogWindow() {
        if logWindow == nil {
            let hosting = NSHostingController(
                rootView: LogWindowView().environmentObject(self)
            )
            let window = NSWindow(contentViewController: hosting)
            window.title = L10n.t("log.window_title", AppIdentity.displayName)
            window.setContentSize(NSSize(width: 720, height: 480))
            window.minSize = NSSize(width: 520, height: 320)
            window.styleMask = [.titled, .closable, .resizable, .miniaturizable]
            window.isReleasedWhenClosed = false
            window.center()
            logWindow = window
        }
        NSApp.activate(ignoringOtherApps: true)
        logWindow?.makeKeyAndOrderFront(nil)
    }

    /// Show the custom About box. Content-sized, so credits do not scroll.
    /// 맞춤 정보 창을 연다. 내용 높이로 맞춰 크레딧에 스크롤이 없다.
    func openAbout() {
        if aboutWindow == nil {
            aboutWindow = AboutPanel.makeWindow()
        }
        NSApp.activate(ignoringOtherApps: true)
        aboutWindow?.center()
        aboutWindow?.makeKeyAndOrderFront(nil)
    }

    /// Pulse the menu-bar glyph while connecting or tearing down.
    /// 연결 중·종료 중에는 메뉴 막대 글리프를 파동 애니메이션한다.
    private func syncMenuIconAnimation() {
        let animating = status.state == .connecting || status.state == .stopping
        if animating {
            guard menuIconTask == nil else { return }
            menuIconTask = Task { [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 280_000_000)
                    guard !Task.isCancelled else { break }
                    await MainActor.run {
                        guard let self else { return }
                        self.menuIconFrame = (self.menuIconFrame + 1) % 4
                    }
                }
            }
        } else {
            menuIconTask?.cancel()
            menuIconTask = nil
            if menuIconFrame != 0 { menuIconFrame = 0 }
        }
    }

    init() {
        if UserDefaults.standard.object(forKey: AppIdentity.prefDefaultRouteKey) != nil {
            preferDefaultRoute = UserDefaults.standard.bool(forKey: AppIdentity.prefDefaultRouteKey)
        }
        appendLog(level: "info", L10n.log("log.starting", AppIdentity.displayName))
        AppDelegate.shared?.controller = self
        helperStatus = HelperClient.status()
        refreshDevices()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                guard let self else { break }
                if self.session == nil || self.isSessionActive {
                    self.refreshDevices()
                }
                self.noteHelperEngineGoneIfNeeded()
            }
        }
    }

    func refreshDevices() {
        Task {
            do {
                let raw = try PrivilegeLauncher.runUser(["list", "--json"])
                var parsed = ParseHelpers.parseDevices(from: raw)
                if parsed.isEmpty {
                    let text = try PrivilegeLauncher.runUser(["list"])
                    parsed = ParseHelpers.parseDevicesFromTable(text)
                }
                let previous = self.devices
                self.devices = parsed
                if self.selectedID == nil || !parsed.contains(where: { $0.id == self.selectedID }) {
                    self.selectedID = parsed.first(where: { $0.kind == .rndis })?.id ?? parsed.first?.id
                }
                if parsed.map(\.id) != previous.map(\.id) {
                    if parsed.isEmpty {
                        self.appendLog(level: "info", L10n.log("log.no_devices"))
                    } else {
                        for dev in parsed {
                            self.appendLog(level: "info", L10n.log("log.device_seen", dev.name, dev.kind.logLabel, dev.vidPid))
                        }
                    }
                }
                self.noteUsbUnplugIfNeeded(devices: parsed)
            } catch {
                self.appendLog(level: "error", L10n.displayLog(error.localizedDescription))
            }
        }
    }

    /// If the phone vanished or left RNDIS while connecting or connected, fail.
    /// 연결 중·연결됨인데 USB가 없거나 RNDIS가 아니면 세션을 실패로 내린다.
    private func noteUsbUnplugIfNeeded(devices: [USBDevice]) {
        guard status.state == .connected || status.state == .connecting,
              connectedLocation != 0 else { return }
        let match = devices.first { $0.location == connectedLocation }
        if let match, match.kind == .rndis {
            usbMissingPolls = 0
            return
        }
        usbMissingPolls += 1
        /* One empty poll can be a list glitch; two in a row is real. */
        /* 한 번 빈 목록은 조회 실수일 수 있어, 두 번 연속일 때만 본다. */
        guard usbMissingPolls >= 2 else { return }
        let reason: String
        if match != nil {
            reason = TetherMessages.tetheringOff
        } else {
            reason = TetherMessages.cableUnplug
        }
        failUsbSession(reason: reason)
    }

    private func failUsbSession(reason: String) {
        if status.state == .error { return }
        status.state = .error
        status.error = reason
        appendLog(level: "error", reason)
        connectGen += 1
        Task { await disconnectAsync(resetToIdle: false) }
    }

    /// Helper-spawned engine has no Process.terminationHandler; poll the PID.
    /// 헬퍼가 띄운 엔진은 Process.terminationHandler 가 없어 PID 를 주기적으로 본다.
    private func noteHelperEngineGoneIfNeeded() {
        guard !userStopping, !disconnectRunning else { return }
        guard status.state == .connected || status.state == .connecting else { return }
        guard enginePid > 1, !Self.pidIsAlive(enginePid) else { return }
        failUnexpectedEngineExit()
    }

    private func failUnexpectedEngineExit() {
        if status.state == .error || userStopping { return }
        status.state = .error
        status.error = L10n.t("error.engine_unexpected")
        appendLog(level: "error", L10n.log("error.engine_unexpected"))
        connectGen += 1
        Task { await disconnectAsync(resetToIdle: false) }
    }

    func connectSelected() {
        guard let device = selectedDevice else {
            appendLog(level: "warn", L10n.log("log.no_usb"))
            return
        }
        if device.kind != .rndis {
            appendLog(level: "warn", L10n.log("log.not_rndis", device.name))
            return
        }
        connectGen += 1
        let gen = connectGen
        let location = device.location
        let name = device.name
        let prefer = preferDefaultRoute
        /* Snapshot radio power in this GUI session; CoreWLAN restore needs it. */
        /* GUI 세션에서 라디오 전원을 기록한다. CoreWLAN 복구에 쓴다. */
        restoreWifiOnDisconnect = prefer && WiFiPower.isOn()
        connectedLocation = location
        status = EngineStatus(state: .connecting)
        syncMenuIconAnimation()
        sawEngineTraffic = false
        usbMissingPolls = 0
        appendLog(level: "info", L10n.log("log.connecting", name))

        /* Tear down any leftover engine without blocking the menu-bar UI. */
        /* 남은 엔진을 정리하되 메뉴 막대 UI를 막지 않는다. */
        Task { [weak self] in
            guard let self else { return }
            await self.disconnectAsync(resetToIdle: false)
            guard self.connectGen == gen else { return }
            self.userStopping = false
            self.helperStatus = HelperClient.status()

            do {
                let session = try self.openEngineSession()
                if self.helperStatus == .enabled {
                    do {
                        try await self.launchViaHelper(
                            session: session, location: location, prefer: prefer
                        )
                        return
                    } catch {
                        self.appendLog(
                            level: "warn",
                            L10n.log("log.helper_fallback", L10n.displayLog(error.localizedDescription))
                        )
                    }
                } else {
                    self.appendLog(level: "info", L10n.log("log.auth_request"))
                }

                guard let creds = await AdminAuthPrompt.request() else {
                    self.status = EngineStatus(state: .idle)
                    self.session?.stop()
                    self.session = nil
                    self.syncMenuIconAnimation()
                    self.appendLog(level: "info", L10n.log("log.auth_cancel"))
                    return
                }
                guard self.connectGen == gen else { return }
                try self.launchViaOsascript(
                    session: session, location: location, prefer: prefer, creds: creds
                )
            } catch {
                self.status.state = .error
                self.status.error = L10n.display(error.localizedDescription)
                self.appendLog(level: "error", L10n.displayLog(error.localizedDescription))
                self.session?.stop()
                self.session = nil
                self.syncMenuIconAnimation()
            }
        }
    }

    /// Open the GUI status socket before starting the privileged engine.
    /// 권한 엔진을 올리기 전에 GUI 상태 소켓을 연다.
    private func openEngineSession() throws -> EngineSession {
        let session = try EngineSession()
        self.session = session
        session.onEvent = { [weak self] event in
            Task { @MainActor in
                self?.handle(event)
            }
        }
        try session.startListening()
        self.lastSocketPath = session.socketPath
        self.enginePid = 0
        return session
    }

    private func launchViaHelper(
        session: EngineSession, location: UInt32, prefer: Bool
    ) async throws {
        appendLog(level: "info", L10n.log("log.helper_start"))
        let pid = try await HelperClient.startConnect(
            socketPath: session.socketPath,
            location: location,
            preferDefaultRoute: prefer
        )
        enginePid = pid
        privilegedProcess = nil
        startEngineWatchdog()
        appendLog(level: "info", L10n.log("log.helper_pid", pid))
    }

    private func launchViaOsascript(
        session: EngineSession, location: UInt32, prefer: Bool, creds: AdminCredentials
    ) throws {
        let launch = try PrivilegeLauncher.launchPrivilegedConnect(
            socketPath: session.socketPath,
            location: location,
            preferDefaultRoute: prefer,
            account: creds.account,
            password: creds.password
        )
        privilegedProcess = launch.process
        launch.process.terminationHandler = { [weak self] proc in
            Task { @MainActor in
                self?.handlePrivilegedExit(proc)
            }
        }
        startEngineWatchdog()
    }

    private func startEngineWatchdog() {
        engineWatchdog = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 40_000_000_000)
            await MainActor.run {
                guard let self, self.status.state == .connecting, !self.sawEngineTraffic else { return }
                self.status.state = .error
                self.status.error = L10n.t("error.engine_silent")
                self.appendLog(level: "error", L10n.log("error.engine_silent"))
                let tail = PrivilegeLauncher.tailLog(path: PrivilegeLauncher.logURL().path)
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                if !tail.isEmpty {
                    self.appendLog(level: "error", L10n.log("log.engine_log", tail))
                }
                self.connectGen += 1
                Task { await self.disconnectAsync(resetToIdle: false) }
            }
        }
    }

    /// Register or remove the launchd helper.
    /// launchd 헬퍼를 등록하거나 제거한다.
    func toggleHelper() {
        Task {
            do {
                if helperStatus == .enabled {
                    if isSessionActive {
                        appendLog(level: "info", L10n.log("log.helper_remove_busy"))
                        await disconnectAsync(resetToIdle: true)
                    }
                    try HelperClient.unregister()
                    helperStatus = HelperClient.status()
                    appendLog(level: "info", L10n.log("log.helper_removed"))
                    return
                }
                try HelperClient.register()
                helperStatus = HelperClient.status()
                if helperStatus == .requiresApproval {
                    appendLog(level: "info", L10n.log("log.helper_allow"))
                    HelperClient.openLoginItemsSettings()
                } else if helperStatus == .enabled {
                    appendLog(level: "info", L10n.log("log.helper_registered"))
                } else {
                    appendLog(level: "warn", L10n.log("log.helper_status", helperStatus.logLabel))
                }
            } catch {
                helperStatus = HelperClient.status()
                appendLog(level: "error", L10n.log("log.helper_change_fail", L10n.displayLog(error.localizedDescription)))
                appendLog(level: "info", L10n.log("log.helper_adhoc"))
            }
        }
    }

    func disconnect() {
        connectGen += 1
        Task { await disconnectAsync(resetToIdle: true) }
    }

    /// Ask the engine to restore routes, then wait without blocking the main thread.
    /// 엔진에 경로 복구를 요청한 뒤, 메인 스레드를 막지 않고 기다린다.
    func disconnectAsync(resetToIdle: Bool = true) async {
        if disconnectRunning {
            while disconnectRunning {
                try? await Task.sleep(nanoseconds: 50_000_000)
            }
            if resetToIdle && (status.state == .connected || status.state == .connecting || status.state == .stopping) {
                status = EngineStatus(state: .idle)
                resetRates()
            }
            syncMenuIconAnimation()
            return
        }
        disconnectRunning = true
        defer { disconnectRunning = false }

        userStopping = true
        engineWatchdog?.cancel()
        engineWatchdog = nil

        if resetToIdle && (status.state == .connected || status.state == .connecting) {
            status.state = .stopping
            syncMenuIconAnimation()
        }

        /* User-session radio first. Root networksetup often cannot turn Wi-Fi back on. */
        /* 사용자 세션 라디오를 먼저 켠다. root networksetup 은 다시 켜지 못하는 경우가 많다. */
        let shouldWifi = restoreWifiOnDisconnect || WiFiPower.markerPresent()
        restoreWifiOnDisconnect = false
        if shouldWifi {
            let note = WiFiPower.turnOn()
            appendLog(level: "info", L10n.log("log.wifi_restore", note))
        }

        let sock = session?.socketPath ?? lastSocketPath
        var pid = enginePid
        if pid <= 1, let sock {
            pid = Self.readPidfile(sock)
        }

        /* Do not kill osascript first; the engine is that shell's foreground process. */
        /* osascript 를 먼저 죽이지 않는다. 엔진은 그 셸의 포그라운드 프로세스이다. */
        session?.stop()
        session = nil

        let proc = privilegedProcess
        if pid > 1 {
            /* Socket "stop" lets at_engine_stop restore routes before any SIGKILL. */
            /* 소켓 stop 으로 at_engine_stop 이 경로를 복구한 뒤에야 SIGKILL 한다. */
            await Self.waitUntilGone(pid: pid, proc: proc, seconds: 8.0)
            if Self.pidIsAlive(pid) && helperStatus == .enabled {
                /* Unprivileged kill of a root engine fails with EPERM. Ask the helper. */
                /* 일반 권한이 root 엔진에 보내는 kill 은 EPERM 이다. 헬퍼에 맡긴다. */
                try? await HelperClient.stopEngine()
                await Self.waitUntilGone(pid: pid, proc: proc, seconds: 2.0)
            }
            if Self.pidIsAlive(pid) {
                _ = Darwin.kill(pid, SIGTERM)
                await Self.waitUntilGone(pid: pid, proc: proc, seconds: 8.0)
            }
            if Self.pidIsAlive(pid) {
                appendLog(level: "warn", L10n.log("log.force_kill"))
                _ = Darwin.kill(pid, SIGKILL)
                await Self.waitUntilGone(pid: pid, proc: proc, seconds: 0.5)
            }
            /* SIGKILL skipped at_engine_stop; restore scutil backup if the process is gone. */
            /* SIGKILL 은 at_engine_stop 을 건너뛰므로, 프로세스가 사라진 뒤에만 백업을 복구한다. */
            if Self.pidIsAlive(pid) {
                pendingRouteRestore = true
                appendLog(level: "warn", L10n.log("log.force_kill_fail"))
            } else if helperStatus == .enabled {
                do {
                    try await HelperClient.restoreNetwork()
                    pendingRouteRestore = false
                    appendLog(level: "info", L10n.log("log.restored_after_kill"))
                } catch {
                    pendingRouteRestore = true
                    appendLog(level: "warn", L10n.log("log.restore_fail", L10n.displayLog(error.localizedDescription)))
                }
            } else {
                pendingRouteRestore = true
                appendLog(level: "warn", L10n.log("hint.route_restore"))
            }
        }
        if let proc, proc.isRunning {
            proc.terminate()
        }
        privilegedProcess = nil
        enginePid = 0
        connectedLocation = 0
        usbMissingPolls = 0

        if resetToIdle && (status.state == .connected || status.state == .connecting || status.state == .stopping) {
            status = EngineStatus(state: .idle)
            resetRates()
            appendLog(level: "info", L10n.log("log.disconnected"))
        }
        syncMenuIconAnimation()
    }

    /// True if the process exists. EPERM means it exists but we cannot signal it (root).
    /// 프로세스가 있으면 true. EPERM 은 존재하지만 신호를 못 보낸다(root).
    private static func pidIsAlive(_ pid: pid_t) -> Bool {
        if pid <= 1 { return false }
        if kill(pid, 0) == 0 { return true }
        return errno == EPERM
    }

    private static func readPidfile(_ socketPath: String) -> pid_t {
        let path = socketPath + ".pid"
        guard let raw = try? String(contentsOfFile: path, encoding: .utf8) else { return 0 }
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return pid_t(trimmed) ?? 0
    }

    private static func waitUntilGone(pid: pid_t, proc: Process?, seconds: TimeInterval) async {
        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            let procGone = proc.map { !$0.isRunning } ?? true
            let pidGone = pid <= 1 || !pidIsAlive(pid)
            if procGone && pidGone { return }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
    }

    private func handlePrivilegedExit(_ proc: Process) {
        /* Ignore stale osascript exits from a previous connect. */
        /* 이전 연결의 osascript 종료는 무시한다. */
        if privilegedProcess !== proc { return }
        if userStopping {
            userStopping = false
            return
        }
        let code = proc.terminationStatus
        let tail = PrivilegeLauncher.tailLog(path: PrivilegeLauncher.logURL().path)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if status.state == .connecting {
            status.state = .error
            if code != 0 && !sawEngineTraffic {
                status.error = L10n.t("error.auth_or_start")
                appendLog(level: "error", L10n.log("error.auth_or_start"))
            } else {
                status.error = L10n.t("error.engine_died_start")
                appendLog(level: "error", L10n.log("error.engine_died_start"))
            }
            if !tail.isEmpty {
                appendLog(level: "error", L10n.log("log.engine_log", tail))
            }
            Task { await disconnectAsync(resetToIdle: false) }
        } else if status.state == .connected {
            status.state = .error
            status.error = L10n.t("error.engine_unexpected")
            appendLog(level: "error", L10n.log("error.engine_unexpected"))
            if !tail.isEmpty {
                appendLog(level: "error", L10n.log("log.engine_log", tail))
            }
            Task { await disconnectAsync(resetToIdle: false) }
        }
        syncMenuIconAnimation()
    }

    private func handle(_ event: EngineEvent) {
        sawEngineTraffic = true
        switch event {
        case .log(let level, let message):
            appendLog(level: level, L10n.displayLog(message))
        case .status(let s):
            var incoming = s
            if !ParseHelpers.shouldApplyEngineStatus(
                current: status.state,
                incoming: incoming.state,
                userStopping: userStopping
            ) {
                return
            }
            if incoming.state == .error {
                incoming = Self.mergeErrorStatus(previous: status, incoming: incoming)
                incoming.error = L10n.display(incoming.error)
            }
            updateRates(from: incoming)
            status = incoming
            if incoming.pid > 1 { enginePid = incoming.pid }
            if incoming.state == .error {
                engineWatchdog?.cancel()
                engineWatchdog = nil
            }
            if incoming.state == .connected {
                pendingRouteRestore = false
            }
            syncMenuIconAnimation()
        case .closed:
            session = nil
            if ParseHelpers.shouldKeepStateOnSocketClosed(
                current: status.state,
                userStopping: userStopping
            ) {
                syncMenuIconAnimation()
                return
            }
            if status.state == .connected || status.state == .connecting {
                failUnexpectedEngineExit()
            } else if status.state != .error {
                status.state = .idle
            }
            syncMenuIconAnimation()
        }
    }

    /// Sparse final error JSON omits counters; keep the last session totals.
    /// 마지막 오류 JSON 은 카운터가 빠져 있어, 직전 세션 합계를 남긴다.
    private static func mergeErrorStatus(previous: EngineStatus, incoming: EngineStatus) -> EngineStatus {
        var s = incoming
        if s.rxBytes == 0 && previous.rxBytes > 0 { s.rxBytes = previous.rxBytes }
        if s.txBytes == 0 && previous.txBytes > 0 { s.txBytes = previous.txBytes }
        if s.rxErrors == 0 && previous.rxErrors > 0 { s.rxErrors = previous.rxErrors }
        if s.txErrors == 0 && previous.txErrors > 0 { s.txErrors = previous.txErrors }
        if s.linkMbps == 0 && previous.linkMbps > 0 { s.linkMbps = previous.linkMbps }
        if s.mtu == 0 && previous.mtu > 0 { s.mtu = previous.mtu }
        if s.iface.isEmpty && !previous.iface.isEmpty { s.iface = previous.iface }
        if s.ip.isEmpty && !previous.ip.isEmpty { s.ip = previous.ip }
        if s.gateway.isEmpty && !previous.gateway.isEmpty { s.gateway = previous.gateway }
        if s.dns.isEmpty && !previous.dns.isEmpty { s.dns = previous.dns }
        if s.error.isEmpty { s.error = previous.error }
        return s
    }

    private func resetRates() {
        lastRateRx = 0
        lastRateTx = 0
        peakRateRx = 0
        peakRateTx = 0
        lastBytes = nil
    }

    private func updateRates(from s: EngineStatus) {
        if s.state == .error {
            lastRateRx = 0
            lastRateTx = 0
            lastBytes = nil
            return
        }
        if s.state != .connected && s.state != .connecting {
            resetRates()
            return
        }
        let now = Date()
        if let last = lastBytes {
            let dt = now.timeIntervalSince(last.at)
            if dt > 0.2 {
                lastRateRx = Double(s.rxBytes &- last.rx) / dt
                lastRateTx = Double(s.txBytes &- last.tx) / dt
                if lastRateRx > peakRateRx { peakRateRx = lastRateRx }
                if lastRateTx > peakRateTx { peakRateTx = lastRateTx }
            }
        }
        lastBytes = (s.rxBytes, s.txBytes, now)
    }

    func appendLog(level: String, _ message: String) {
        let trimmed = L10n.displayLog(message).trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return }
        if let last = logs.last, last.level == level, last.message == trimmed { return }
        logs.append(LogLine(level: level, message: trimmed))
        if logs.count > 400 { logs.removeFirst(logs.count - 400) }
    }

    static func formatBytes(_ n: UInt64) -> String {
        let d = Double(n)
        if d > 1_000_000_000 { return String(format: "%.2f GB", d / 1_000_000_000) }
        if d > 1_000_000 { return String(format: "%.2f MB", d / 1_000_000) }
        if d > 1_000 { return String(format: "%.1f KB", d / 1_000) }
        return "\(n) B"
    }

    static func formatRate(_ n: Double) -> String {
        if n > 1_000_000 { return String(format: "%.2f Mbps", n * 8 / 1_000_000) }
        if n > 1_000 { return String(format: "%.1f kbps", n * 8 / 1_000) }
        return String(format: "%.0f bps", n * 8)
    }

}

enum EngineEvent {
    case log(level: String, message: String)
    case status(EngineStatus)
    case closed
}

/// Accepts one UNIX-socket client (the privileged engine) and parses JSON lines.
/// 권한 있는 엔진 클라이언트 하나를 받고 JSON 줄을 파싱한다.
final class EngineSession {
    let socketPath: String
    var onEvent: ((EngineEvent) -> Void)?
    /// Serializes listener/client fds across the accept thread and `stop()`.
    /// accept 스레드와 `stop()` 이 listener/client fd 를 같이 쓰지 않게 직렬화한다.
    private let fdLock = NSLock()
    private var listener: Int32 = -1
    private var client: Int32 = -1
    private var thread: Thread?

    init() throws {
        socketPath = NSTemporaryDirectory() + "\(AppIdentity.filePrefix)-\(ProcessInfo.processInfo.processIdentifier).sock"
        unlink(socketPath)
    }

    func startListening() throws {
        let listenFd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard listenFd >= 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathOk = withUnsafeMutablePointer(to: &addr.sun_path.0) { dst in
            ParseHelpers.writeUnixPath(socketPath, to: dst, capacity: ParseHelpers.unixPathCapacity)
        }
        guard pathOk else {
            close(listenFd)
            throw NSError(
                domain: AppIdentity.errorDomain, code: 6,
                userInfo: [NSLocalizedDescriptionKey: "error.socket_path"]
            )
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let bindRc = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(listenFd, $0, len)
            }
        }
        guard bindRc == 0 else {
            close(listenFd)
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        guard listen(listenFd, 1) == 0 else {
            close(listenFd)
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        /* Root engine can connect to a user-owned 0600 socket (DAC bypass). */
        /* root 엔진은 사용자 소유 0600 소켓에 접속할 수 있다 (DAC 우회). */
        _ = chmod(socketPath, 0o600)
        fdLock.lock()
        listener = listenFd
        fdLock.unlock()
        thread = Thread { [weak self] in
            self?.acceptLoop()
        }
        thread?.start()
    }

    func stop() {
        fdLock.lock()
        let clientFd = client
        client = -1
        let listenFd = listener
        listener = -1
        fdLock.unlock()
        if clientFd >= 0 {
            sendStopAndClose(clientFd)
        }
        if listenFd >= 0 {
            shutdown(listenFd, SHUT_RDWR)
            close(listenFd)
        }
        unlink(socketPath)
    }

    /// Tell the engine to restore routes, then unblock `read()`.
    /// 엔진에 경로 복구를 알린 뒤 `read()` 가 깨어나게 한다.
    private func sendStopAndClose(_ fd: Int32) {
        let msg = "stop\n"
        _ = msg.withCString { Darwin.send(fd, $0, strlen($0), 0) }
        shutdown(fd, SHUT_RDWR)
        close(fd)
    }

    private func acceptLoop() {
        fdLock.lock()
        let listenFd = listener
        fdLock.unlock()
        guard listenFd >= 0 else {
            onEvent?(.closed)
            return
        }
        var addr = sockaddr_un()
        var len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let fd = withUnsafeMutablePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                accept(listenFd, $0, &len)
            }
        }
        if fd < 0 {
            onEvent?(.closed)
            return
        }
        fdLock.lock()
        client = fd
        fdLock.unlock()
        var buffer = Data()
        var tmp = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = read(fd, &tmp, tmp.count)
            if n <= 0 { break }
            buffer.append(contentsOf: tmp[0..<n])
            while let range = buffer.firstIndex(of: 0x0A) {
                let lineData = buffer.subdata(in: buffer.startIndex..<range)
                buffer.removeSubrange(buffer.startIndex...range)
                if let line = String(data: lineData, encoding: .utf8) {
                    dispatch(line)
                }
            }
        }
        /* Close only if we still own the fd; `stop()` may have closed it already. */
        /* 아직 이 스레드가 소유할 때만 닫는다. `stop()` 이 이미 닫았을 수 있다. */
        fdLock.lock()
        let stillOurs = (client == fd)
        if stillOurs { client = -1 }
        fdLock.unlock()
        if stillOurs {
            close(fd)
        }
        onEvent?(.closed)
    }

    private func dispatch(_ line: String) {
        switch ParseHelpers.parseEngineLine(line) {
        case .log(let level, let message):
            onEvent?(.log(level: level, message: message))
        case .status(let s):
            onEvent?(.status(s))
        case .none:
            break
        }
    }

    deinit { stop() }
}
