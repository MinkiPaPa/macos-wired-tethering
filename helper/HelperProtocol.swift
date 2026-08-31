/*
 * HelperProtocol.swift
 * XPC contract between the unprivileged GUI and the root launchd helper.
 * 일반 권한 GUI 와 root launchd 헬퍼 사이의 XPC 계약.
 */
import Foundation

@objc protocol TetherHelperProtocol {
    /// Start the bundled engine as root. reply(pid, error). pid is 0 on failure.
    /// 번들 엔진을 root 로 시작한다. reply(pid, error). 실패 시 pid 는 0.
    func startConnect(
        socketPath: String,
        location: UInt32,
        defaultRoute: Bool,
        logPath: String,
        with reply: @escaping (Int32, String?) -> Void
    )

    /// SIGTERM the engine this helper started. reply(error). nil error means success.
    /// 이 헬퍼가 띄운 엔진에 SIGTERM 을 보낸다. reply(error). error 가 nil 이면 성공.
    func stopEngine(with reply: @escaping (String?) -> Void)

    /// Run bundled `restore` as root after SIGKILL skipped at_engine_stop.
    /// SIGKILL 이 at_engine_stop 을 건너뛴 뒤 번들 restore 를 root 로 실행한다.
    func restoreNetwork(with reply: @escaping (String?) -> Void)
}
