/*
 * HelperSecurity.swift
 * Path checks and XPC code-signing requirement for the root helper.
 * root 헬퍼의 경로 검사와 XPC 코드 서명 요구.
 */
import Foundation
import Security
import Darwin

enum HelperSecurity {
    static let guiIdentifier = "com.macoswiredtethering.app"

    /// True if `path` is an absolute, non-escaping file under a user temp tree.
    /// path 가 사용자 임시 트리 아래의 절대 경로이고 `..` 가 없으면 참.
    static func isAllowedIPCPath(_ path: String, suffix: String) -> Bool {
        if path.isEmpty || path.contains("\0") { return false }
        if !path.hasPrefix("/") { return false }
        if path.contains("/../") || path.hasSuffix("/..") || path.contains("//") { return false }
        if !path.hasSuffix(suffix) { return false }
        if path.count > 512 { return false }
        if !hasAllowedTempPrefix(path) { return false }
        /* If the file exists, reject a symlink that escapes the temp tree. */
        /* 파일이 있으면 임시 트리를 벗어나는 심볼릭 링크를 거절한다. */
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: path, isDirectory: &isDir) {
            if isDir.boolValue { return false }
            let resolved = URL(fileURLWithPath: path).resolvingSymlinksInPath().path
            if resolved.contains("/../") || !hasAllowedTempPrefix(resolved) { return false }
            if !resolved.hasSuffix(suffix) { return false }
        }
        return true
    }

    static func hasAllowedTempPrefix(_ path: String) -> Bool {
        let allowedPrefixes = [
            "/var/folders/",
            "/private/var/folders/",
            "/tmp/",
            "/private/tmp/"
        ]
        return allowedPrefixes.contains { path.hasPrefix($0) }
    }

    /// Create/open a log under the temp tree with 0600. Rejects a symlink (O_NOFOLLOW).
    /// 임시 트리에 0600 로그를 만들거나 연다. 심볼릭 링크는 O_NOFOLLOW 로 거절한다.
    static func openWritableIPC(_ path: String, suffix: String) -> FileHandle? {
        guard isAllowedIPCPath(path, suffix: suffix) else { return nil }
        let fd = path.withCString {
            open($0, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR)
        }
        guard fd >= 0 else { return nil }
        _ = fchmod(fd, S_IRUSR | S_IWUSR)
        var resolved = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        if fcntl(fd, F_GETPATH, &resolved) == 0 {
            let got = String(cString: resolved)
            if !hasAllowedTempPrefix(got) || !got.hasSuffix(suffix) {
                close(fd)
                return nil
            }
        }
        return FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    }

    /// Apple Team IDs are 10 alphanumeric characters. Reject anything else.
    /// Apple Team ID 는 영숫자 10자. 그 외는 거절한다.
    static func isSafeTeamID(_ team: String) -> Bool {
        if team.count != 10 { return false }
        return team.unicodeScalars.allSatisfy { CharacterSet.alphanumerics.contains($0) }
    }

    /// XPC requirement: GUI bundle id, plus same Team ID when this helper is Developer ID signed.
    /// XPC 요구: GUI 번들 ID. 이 헬퍼가 Developer ID 이면 같은 Team ID 도 요구한다.
    static func clientCodeSigningRequirement(teamIdentifier: String?) -> String {
        var req = "identifier \"\(guiIdentifier)\""
        if let team = teamIdentifier, isSafeTeamID(team) {
            req += " and anchor apple generic and certificate leaf[subject.OU] = \"\(team)\""
        }
        return req
    }

    static func currentTeamIdentifier() -> String? {
        var code: SecCode?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code else { return nil }
        var staticCode: SecStaticCode?
        guard SecCodeCopyStaticCode(code, [], &staticCode) == errSecSuccess,
              let staticCode else { return nil }
        return teamIdentifier(fromStatic: staticCode)
    }

    static func fileTeamIdentifier(at url: URL) -> String? {
        var staticCode: SecStaticCode?
        guard SecStaticCodeCreateWithPath(url as CFURL, [], &staticCode) == errSecSuccess,
              let staticCode else { return nil }
        return teamIdentifier(fromStatic: staticCode)
    }

    /// When this helper has a Team ID, the engine must be signed by the same team.
    /// 이 헬퍼에 Team ID 가 있으면 엔진도 같은 팀으로 서명되어 있어야 한다.
    static func isTrustedEngine(at url: URL) -> Bool {
        guard FileManager.default.isExecutableFile(atPath: url.path) else { return false }
        guard let team = currentTeamIdentifier(), isSafeTeamID(team) else {
            return true
        }
        return fileTeamIdentifier(at: url) == team
    }

    private static func teamIdentifier(fromStatic code: SecStaticCode) -> String? {
        var info: CFDictionary?
        let flags = SecCSFlags(rawValue: kSecCSSigningInformation)
        guard SecCodeCopySigningInformation(code, flags, &info) == errSecSuccess,
              let info else { return nil }
        let dict = info as NSDictionary
        guard let team = dict[kSecCodeInfoTeamIdentifier] as? String, isSafeTeamID(team) else {
            return nil
        }
        return team
    }
}
