/*
 * HelperMain.swift
 * Root launchd helper: spawn the bundled engine with a fixed argv.
 * root launchd 헬퍼. 번들 엔진만 고정 argv 로 띄운다.
 */
import Foundation
import Darwin

final class HelperService: NSObject, TetherHelperProtocol {
    private let lock = NSLock()
    private let work = DispatchQueue(label: "com.macoswiredtethering.helper.work")
    private var engineProcess: Process?

    /// SIGTERM the current engine and wait so scutil restore can finish.
    /// 현재 엔진에 SIGTERM 을 보내고, scutil 복구가 끝나도록 기다린다.
    private func stopRunningEngineSync(timeout: TimeInterval) {
        lock.lock()
        let proc = engineProcess
        engineProcess = nil
        lock.unlock()
        guard let proc, proc.isRunning else { return }
        proc.terminate()
        let deadline = Date().addingTimeInterval(timeout)
        while proc.isRunning && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.05)
        }
        if proc.isRunning {
            _ = Darwin.kill(proc.processIdentifier, SIGKILL)
            proc.waitUntilExit()
            /* SIGKILL skipped at_engine_stop; restore leftover scutil keys now. */
            /* SIGKILL 은 at_engine_stop 을 건너뛰므로, 남은 scutil 키를 지금 복구한다. */
            _ = runRestoreSync()
        }
    }

    /// Run bundled `restore`. Returns an error string, or nil on success.
    /// 번들 restore 를 실행한다. 성공이면 nil, 실패면 오류 문자열.
    private func runRestoreSync() -> String? {
        guard let engine = Self.bundledEngineURL() else {
            return "helper.error.engine_missing"
        }
        let proc = Process()
        proc.executableURL = engine
        proc.arguments = ["restore"]
        proc.standardOutput = FileHandle.nullDevice
        proc.standardError = FileHandle.nullDevice
        do {
            try proc.run()
        } catch {
            return error.localizedDescription
        }
        let deadline = Date().addingTimeInterval(8)
        while proc.isRunning && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.05)
        }
        if proc.isRunning {
            _ = Darwin.kill(proc.processIdentifier, SIGKILL)
            proc.waitUntilExit()
            return "helper.error.restore_timeout"
        }
        return nil
    }

    /// Engine must sit next to this helper in Contents/MacOS. Client cannot choose another binary.
    /// 엔진은 이 헬퍼와 같은 Contents/MacOS 에 있어야 한다. 클라이언트가 다른 바이너리를 고를 수 없다.
    private static func bundledEngineURL() -> URL? {
        let dir = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
        let url = dir.appendingPathComponent(AppIdentity.engineBinaryName)
        if HelperSecurity.isTrustedEngine(at: url) { return url }
        return nil
    }

    func startConnect(
        socketPath: String,
        location: UInt32,
        defaultRoute: Bool,
        logPath: String,
        with reply: @escaping (Int32, String?) -> Void
    ) {
        work.async { [self] in
            if geteuid() != 0 {
                reply(0, "helper.error.not_root")
                return
            }
            if !HelperSecurity.isAllowedIPCPath(socketPath, suffix: ".sock") {
                reply(0, "helper.error.bad_socket")
                return
            }
            if !HelperSecurity.isAllowedIPCPath(logPath, suffix: ".log") {
                reply(0, "helper.error.bad_log")
                return
            }
            guard let engine = Self.bundledEngineURL() else {
                reply(0, "helper.error.engine_missing")
                return
            }

            /* Let SIGTERM run at_engine_stop before a new process touches routes. */
            /* 새 프로세스가 경로를 만지기 전에 SIGTERM 으로 at_engine_stop 을 돌린다. */
            stopRunningEngineSync(timeout: 8)

            let proc = Process()
            proc.executableURL = engine
            var args = ["--socket", socketPath, "connect", "--location", String(location)]
            if defaultRoute { args.append("--default-route") }
            proc.arguments = args
            guard let logHandle = HelperSecurity.openWritableIPC(logPath, suffix: ".log") else {
                reply(0, "helper.error.log_open")
                return
            }
            logHandle.seekToEndOfFile()
            proc.standardOutput = logHandle
            proc.standardError = logHandle
            do {
                try proc.run()
            } catch {
                reply(0, error.localizedDescription)
                return
            }
            lock.lock()
            engineProcess = proc
            lock.unlock()
            reply(proc.processIdentifier, nil)
        }
    }

    func stopEngine(with reply: @escaping (String?) -> Void) {
        work.async { [self] in
            stopRunningEngineSync(timeout: 8)
            reply(nil)
        }
    }

    func restoreNetwork(with reply: @escaping (String?) -> Void) {
        work.async { [self] in
            if geteuid() != 0 {
                reply("helper.error.not_root")
                return
            }
            stopRunningEngineSync(timeout: 2)
            reply(runRestoreSync())
        }
    }
}

final class HelperListener: NSObject, NSXPCListenerDelegate {
    let service = HelperService()
    /// Cached so every connection uses the same Team ID requirement.
    /// 모든 연결이 같은 Team ID 요구를 쓰도록 캐시한다.
    let clientRequirement = HelperSecurity.clientCodeSigningRequirement(
        teamIdentifier: HelperSecurity.currentTeamIdentifier()
    )

    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection newConnection: NSXPCConnection) -> Bool {
        /* Developer ID: identifier + apple generic + this helper's Team ID. */
        /* Developer ID: 번들 ID + apple generic + 이 헬퍼의 Team ID. */
        newConnection.setCodeSigningRequirement(clientRequirement)
        newConnection.exportedInterface = NSXPCInterface(with: TetherHelperProtocol.self)
        newConnection.exportedObject = service
        newConnection.resume()
        return true
    }
}

@main
enum HelperMain {
    static func main() {
        let delegate = HelperListener()
        let listener = NSXPCListener(machServiceName: AppIdentity.helperMachService)
        listener.delegate = delegate
        listener.resume()
        RunLoop.main.run()
    }
}
