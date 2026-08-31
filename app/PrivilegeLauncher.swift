/*
 * PrivilegeLauncher.swift
 * Starts the tethering engine with administrator rights.
 * 관리자 권한으로 테더링 엔진을 시작한다.
 *
 * utun / USB / routes need root. The GUI stays unprivileged and talks over a UNIX socket.
 * utun/USB/경로는 root가 필요하다. GUI는 일반 권한으로 UNIX 소켓만 사용한다.
 *
 * Important / 중요:
 * AppleScript `do shell script ... &` kills the child when the admin shell
 * exits. The engine must stay in the foreground of that shell.
 * 백그라운드(&)로 띄우면 관리자 셸이 끝나는 순간 엔진도 죽는다.
 * 엔진은 그 셸의 포그라운드 프로세스로 유지해야 한다.
 *
 * The AppleScript is written to osascript stdin (`osascript -`), not argv,
 * so the administrator password is not visible in `ps`. Paths still go through
 * `quoted form of` so a world-writable tmp launch script is not required.
 * AppleScript 는 osascript argv 가 아니라 표준입력(`osascript -`)으로 넘긴다.
 * 관리자 암호가 `ps` 에 보이지 않는다. 경로는 quoted form of 로 이스케이프한다.
 */
import Foundation
import Darwin

struct PrivilegedLaunch {
    let process: Process
    let logPath: String
}

enum PrivilegeLauncher {
    /// Resolve the engine binary. An `.app` bundle uses only `Contents/MacOS`.
    /// 엔진 실행 파일을 찾는다. `.app` 번들에서는 `Contents/MacOS` 만 본다.
    static func engineURL() -> URL? {
        resolveEngineURL(
            bundleURL: Bundle.main.bundleURL,
            cwd: FileManager.default.currentDirectoryPath
        )
    }

    static func resolveEngineURL(
        bundleURL: URL,
        cwd: String,
        isExecutable: (String) -> Bool = { FileManager.default.isExecutableFile(atPath: $0) }
    ) -> URL? {
        let names = [AppIdentity.engineBinaryName]
        let bundled = names.map { bundleURL.appendingPathComponent("Contents/MacOS/\($0)") }
        if bundleURL.path.hasSuffix(".app") {
            return bundled.first { isExecutable($0.path) }
        }
        var candidates = bundled
        for name in names {
            candidates.append(bundleURL.deletingLastPathComponent().appendingPathComponent(name))
            candidates.append(URL(fileURLWithPath: cwd).appendingPathComponent("build/\(name)"))
        }
        return candidates.first { isExecutable($0.path) }
    }

    static func logURL() -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("\(AppIdentity.filePrefix)-engine.log")
    }

    /// Truncate or create the engine log as 0600. Rejects a symlink via O_NOFOLLOW.
    /// 엔진 로그를 0600 으로 비우거나 만든다. 심볼릭 링크는 O_NOFOLLOW 로 거절한다.
    static func prepareLogFile() -> URL {
        let url = logURL()
        let fd = url.path.withCString {
            open($0, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR)
        }
        if fd >= 0 {
            _ = fchmod(fd, S_IRUSR | S_IWUSR)
            close(fd)
        }
        return url
    }

    /// Run a command as the current user (no admin). Used for `list`.
    /// 관리자 없이 현재 사용자로 명령을 실행한다. `list`에 사용.
    static func runUser(_ arguments: [String]) throws -> String {
        guard let engine = engineURL() else {
            throw NSError(domain: AppIdentity.errorDomain, code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "error.engine_missing"])
        }
        let proc = Process()
        proc.executableURL = engine
        proc.arguments = arguments
        let out = Pipe()
        proc.standardOutput = out
        proc.standardError = Pipe()
        try proc.run()
        proc.waitUntilExit()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        return String(data: data, encoding: .utf8) ?? ""
    }

    /// Start `connect` as root using credentials from the custom auth window.
    /// 커스텀 인증 창에서 받은 계정으로 connect 를 root 로 시작한다.
    static func launchPrivilegedConnect(
        socketPath: String,
        location: UInt32,
        preferDefaultRoute: Bool,
        account: String,
        password: String
    ) throws -> PrivilegedLaunch {
        guard let engine = engineURL() else {
            throw NSError(domain: AppIdentity.errorDomain, code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "error.engine_missing_build"])
        }

        let logURL = Self.prepareLogFile()

        /* Remove leftover world-writable launch scripts from older builds. */
        /* 이전 빌드가 남긴 월드 라이터블 런치 스크립트를 지운다. */
        let tmp = FileManager.default.temporaryDirectory
        let staleNames = [
            "\(AppIdentity.filePrefix)-launch.sh"
        ]
        for name in staleNames {
            try? FileManager.default.removeItem(at: tmp.appendingPathComponent(name))
        }

        let extra = preferDefaultRoute ? " --default-route" : ""
        let script = connectScript(
            enginePath: engine.path,
            socketPath: socketPath,
            location: location,
            logPath: logURL.path,
            extra: extra,
            account: account,
            password: password
        )
        guard let scriptData = script.data(using: .utf8) else {
            throw NSError(domain: AppIdentity.errorDomain, code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "error.script"])
        }

        /* osascript reads the script (including the password) from stdin, not argv. */
        /* osascript 는 암호가 들어 있는 스크립트를 argv 가 아니라 표준입력으로 읽는다. */
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        proc.arguments = ["-"]
        let stdin = Pipe()
        proc.standardInput = stdin
        proc.standardOutput = FileHandle.nullDevice
        proc.standardError = FileHandle.nullDevice
        try proc.run()
        try stdin.fileHandleForWriting.write(contentsOf: scriptData)
        try stdin.fileHandleForWriting.close()
        return PrivilegedLaunch(process: proc, logPath: logURL.path)
    }

    /// Build the admin `do shell script` AppleScript. Paths use `quoted form of`.
    /// 관리자 `do shell script` AppleScript 를 만든다. 경로는 quoted form of 를 쓴다.
    private static func connectScript(
        enginePath: String,
        socketPath: String,
        location: UInt32,
        logPath: String,
        extra: String,
        account: String,
        password: String
    ) -> String {
        """
        set engine to quoted form of \(ParseHelpers.appleScriptLiteral(enginePath))
        set sock to quoted form of \(ParseHelpers.appleScriptLiteral(socketPath))
        set loc to \(ParseHelpers.appleScriptLiteral(String(location)))
        set logf to quoted form of \(ParseHelpers.appleScriptLiteral(logPath))
        set extra to \(ParseHelpers.appleScriptLiteral(extra))
        set uname to \(ParseHelpers.appleScriptLiteral(account))
        set pass to \(ParseHelpers.appleScriptLiteral(password))
        do shell script (engine & " --socket " & sock & " connect --location " & loc & extra & " >> " & logf & " 2>&1") user name uname password pass with administrator privileges
        """
    }

    static func tailLog(path: String, maxBytes: Int = 4000) -> String {
        guard let handle = FileHandle(forReadingAtPath: path) else { return "" }
        defer { try? handle.close() }
        let data = handle.readDataToEndOfFile()
        if data.count <= maxBytes {
            return String(data: data, encoding: .utf8) ?? ""
        }
        let slice = data.suffix(maxBytes)
        return String(data: slice, encoding: .utf8) ?? ""
    }
}
