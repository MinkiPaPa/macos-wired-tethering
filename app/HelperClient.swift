/*
 * HelperClient.swift
 * SMAppService registration and XPC calls to the root helper.
 * SMAppService 등록과 root 헬퍼 XPC 호출.
 *
 * Ad-hoc / unsigned builds usually cannot enable the daemon. The GUI then
 * falls back to the existing osascript password path.
 * ad-hoc·미서명 빌드는 데몬을 켜지 못하는 경우가 많다. 그때는 기존
 * osascript 암호 경로로 넘어간다.
 */
import Foundation
import ServiceManagement

enum HelperStatus: Equatable {
    case enabled
    case requiresApproval
    case notRegistered
    case unavailable

    var label: String {
        switch self {
        case .enabled: return L10n.t("helper.enabled")
        case .requiresApproval: return L10n.t("helper.approval")
        case .notRegistered: return L10n.t("helper.unregistered")
        case .unavailable: return L10n.t("helper.unavailable")
        }
    }

    var logLabel: String {
        switch self {
        case .enabled: return L10n.log("helper.enabled")
        case .requiresApproval: return L10n.log("helper.approval")
        case .notRegistered: return L10n.log("helper.unregistered")
        case .unavailable: return L10n.log("helper.unavailable")
        }
    }
}

enum HelperClient {
    private static var service: SMAppService {
        SMAppService.daemon(plistName: AppIdentity.helperPlistName)
    }

    /// One-shot resume guard for an XPC reply or connection error.
    /// XPC 응답과 연결 오류가 둘 다 와도 continuation 은 한 번만 resume 한다.
    private static func onceFinish<T>(
        _ cont: CheckedContinuation<T, Error>
    ) -> (Result<T, Error>) -> Void {
        let lock = NSLock()
        var finished = false
        return { result in
            lock.lock()
            defer { lock.unlock() }
            if finished { return }
            finished = true
            cont.resume(with: result)
        }
    }

    /// Privileged Mach connection to the launchd helper. Caller invalidates it.
    /// launchd 헬퍼로의 권한 Mach 연결. 호출 쪽이 invalidate 한다.
    private static func privilegedProxy(
        onError: @escaping (Error) -> Void
    ) -> (NSXPCConnection, TetherHelperProtocol)? {
        let conn = NSXPCConnection(
            machServiceName: AppIdentity.helperMachService,
            options: .privileged
        )
        conn.remoteObjectInterface = NSXPCInterface(with: TetherHelperProtocol.self)
        conn.resume()
        guard let proxy = conn.remoteObjectProxyWithErrorHandler({ error in
            conn.invalidate()
            onError(error)
        }) as? TetherHelperProtocol else {
            conn.invalidate()
            onError(NSError(
                domain: AppIdentity.errorDomain, code: 3,
                userInfo: [NSLocalizedDescriptionKey: "error.helper_connect"]
            ))
            return nil
        }
        return (conn, proxy)
    }

    static func status() -> HelperStatus {
        switch service.status {
        case .enabled: return .enabled
        case .requiresApproval: return .requiresApproval
        case .notRegistered: return .notRegistered
        default: return .unavailable
        }
    }

    /// Ask launchd to install the daemon. May open System Settings for approval.
    /// launchd 에 데몬 설치를 요청한다. 승인을 위해 시스템 설정이 열릴 수 있다.
    static func register() throws {
        try service.register()
    }

    static func unregister() throws {
        try service.unregister()
    }

    static func openLoginItemsSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    /// Resume with a timeout error if the helper never replies. onceFinish wins.
    /// 헬퍼가 답이 없으면 기한 오류로 resume 한다. onceFinish 가 한 번만 이긴다.
    private static func scheduleTimeout<T>(
        seconds: TimeInterval,
        finish: @escaping (Result<T, Error>) -> Void
    ) -> Task<Void, Never> {
        Task {
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
            finish(.failure(NSError(
                domain: AppIdentity.errorDomain, code: 6,
                userInfo: [NSLocalizedDescriptionKey: "error.helper_timeout"]
            )))
        }
    }

    /// Start the engine via the already-enabled helper. Throws if XPC fails.
    /// 이미 켜진 헬퍼로 엔진을 시작한다. XPC 실패 시 throw.
    static func startConnect(
        socketPath: String,
        location: UInt32,
        preferDefaultRoute: Bool
    ) async throws -> pid_t {
        let logPath = PrivilegeLauncher.prepareLogFile().path
        return try await withCheckedThrowingContinuation { cont in
            let finish = onceFinish(cont)
            let timeout = scheduleTimeout(seconds: 15, finish: finish)
            guard let (conn, proxy) = privilegedProxy(onError: {
                timeout.cancel()
                finish(.failure($0))
            }) else {
                timeout.cancel()
                return
            }
            proxy.startConnect(
                socketPath: socketPath,
                location: location,
                defaultRoute: preferDefaultRoute,
                logPath: logPath
            ) { pid, err in
                timeout.cancel()
                conn.invalidate()
                if let err, !err.isEmpty {
                    finish(.failure(NSError(
                        domain: AppIdentity.errorDomain, code: 4,
                        userInfo: [NSLocalizedDescriptionKey: err]
                    )))
                    return
                }
                if pid <= 1 {
                    finish(.failure(NSError(
                        domain: AppIdentity.errorDomain, code: 4,
                        userInfo: [NSLocalizedDescriptionKey: "error.helper_pid"]
                    )))
                    return
                }
                finish(.success(pid_t(pid)))
            }
        }
    }

    /// Ask the root helper to SIGTERM the engine it spawned.
    /// root 헬퍼에게 자신이 띄운 엔진에 SIGTERM 을 보내게 한다.
    static func stopEngine() async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            let finish = onceFinish(cont)
            let timeout = scheduleTimeout(seconds: 15, finish: finish)
            guard let (conn, proxy) = privilegedProxy(onError: {
                timeout.cancel()
                finish(.failure($0))
            }) else {
                timeout.cancel()
                return
            }
            proxy.stopEngine { err in
                timeout.cancel()
                conn.invalidate()
                if let err, !err.isEmpty {
                    finish(.failure(NSError(
                        domain: AppIdentity.errorDomain, code: 5,
                        userInfo: [NSLocalizedDescriptionKey: err]
                    )))
                    return
                }
                finish(.success(()))
            }
        }
    }

    /// Ask the root helper to restore scutil backup after a SIGKILL'd engine.
    /// SIGKILL 된 엔진 뒤 scutil 백업 복구를 root 헬퍼에 맡긴다.
    static func restoreNetwork() async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            let finish = onceFinish(cont)
            let timeout = scheduleTimeout(seconds: 15, finish: finish)
            guard let (conn, proxy) = privilegedProxy(onError: {
                timeout.cancel()
                finish(.failure($0))
            }) else {
                timeout.cancel()
                return
            }
            proxy.restoreNetwork { err in
                timeout.cancel()
                conn.invalidate()
                if let err, !err.isEmpty {
                    finish(.failure(NSError(
                        domain: AppIdentity.errorDomain, code: 7,
                        userInfo: [NSLocalizedDescriptionKey: err]
                    )))
                    return
                }
                finish(.success(()))
            }
        }
    }
}
