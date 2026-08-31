/*
 * AdminAuthWindow.swift
 * Custom administrator prompt (replaces the system osascript dialog).
 * 시스템 osascript 암호 창 대신 쓰는 관리자 인증 패널.
 */
import SwiftUI
import AppKit
import Carbon
import OpenDirectory

struct AdminCredentials {
    let account: String
    let password: String
}

/* Card size for the auth prompt. Change these two values to resize the window. */
/* 인증 창 크기. 창 크기를 바꿀 때는 아래 두 값만 조정한다. */
enum AuthDialogMetrics {
    /// Width: Korean prompt stays on one line and fields stay centered.
    /// 너비: 한글 안내가 한 줄로 들어가고 입력 칸이 가운데 유지된다.
    static let width: CGFloat = 420
    /// Height: icon, copy, fields, and buttons without leftover empty space.
    /// 높이: 아이콘·문구·입력·버튼만 남기고 아래 빈 공간을 줄인다.
    static let height: CGFloat = 368
}

/* Switch to English (ASCII) IME for the password field, then restore it. */
/* 암호 칸에서는 영문(ASCII) 입력기를 강제하고, 창을 닫으면 되돌린다. */
enum EnglishIME {
    private static var saved: TISInputSource?

    /// Remember the current IME and select an ASCII-capable keyboard.
    /// 현재 입력기를 기억한 뒤 ASCII(영문) 키보드로 바꾼다.
    static func captureAndForceASCII() {
        if saved == nil {
            saved = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue()
        }
        if let ascii = TISCopyCurrentASCIICapableKeyboardInputSource()?.takeRetainedValue() {
            TISSelectInputSource(ascii)
        }
    }

    /// Put back the IME that was active before the password prompt.
    /// 암호 창을 열기 전에 쓰던 입력기로 되돌린다.
    static func restore() {
        if let saved {
            TISSelectInputSource(saved)
        }
        saved = nil
    }
}

/* Restrict the field editor to Roman/English sources so Hangul cannot be selected. */
/* 필드 편집기는 로마자(영문) 입력기만 허용해 한글 전환을 막는다. */
private func restrictFieldEditorToRoman(_ field: NSTextField) {
    if let editor = field.currentEditor() as? NSTextView {
        editor.allowedInputSourceLocales = [NSAllRomanInputSourcesLocaleIdentifier]
    }
}

@MainActor
enum AdminAuthPrompt {
    private static var panel: NSPanel?
    private static var shield: NSWindow?
    private static var continuation: CheckedContinuation<AdminCredentials?, Never>?

    /// Show the designed prompt. nil = user cancelled.
    /// 디자인된 인증 창을 연다. nil 이면 사용자가 취소한 것이다.
    static func request() async -> AdminCredentials? {
        if continuation != nil {
            dismiss(result: nil)
        }
        return await withCheckedContinuation { cont in
            continuation = cont
            present()
        }
    }

    static func dismiss(result: AdminCredentials?) {
        EnglishIME.restore()
        continuation?.resume(returning: result)
        continuation = nil
        panel?.orderOut(nil)
        panel = nil
        shield?.orderOut(nil)
        shield = nil
    }

    private static func present() {
        presentShield()

        let root = AdminAuthView(
            fullName: NSFullUserName(),
            account: NSUserName(),
            onCancel: { dismiss(result: nil) },
            onConfirm: { creds in dismiss(result: creds) }
        )
        let cardSize = NSSize(width: AuthDialogMetrics.width, height: AuthDialogMetrics.height)
        let hosting = NSHostingController(rootView: root)
        hosting.view.frame = NSRect(origin: .zero, size: cardSize)
        hosting.view.wantsLayer = true
        hosting.view.layer?.backgroundColor = NSColor.clear.cgColor

        let panel = AuthPanel(
            contentRect: NSRect(origin: .zero, size: cardSize),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        panel.title = AppIdentity.displayName
        panel.isMovableByWindowBackground = true
        panel.isFloatingPanel = true
        panel.level = .popUpMenu
        panel.hidesOnDeactivate = false
        panel.hasShadow = true
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.appearance = NSAppearance(named: .aqua)
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        panel.contentViewController = hosting
        panel.center()
        EnglishIME.captureAndForceASCII()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        Self.panel = panel
    }

    /// Dim the desktop so the card reads as a modal, like the system prompt.
    /// 시스템 인증 창처럼 보이도록 바탕을 어둡게 깐다.
    private static func presentShield() {
        let frame = NSScreen.main?.frame ?? NSRect(x: 0, y: 0, width: 800, height: 600)
        let shield = NSWindow(
            contentRect: frame,
            styleMask: .borderless,
            backing: .buffered,
            defer: false
        )
        shield.isOpaque = false
        shield.backgroundColor = NSColor.black.withAlphaComponent(0.45)
        shield.level = .modalPanel
        shield.ignoresMouseEvents = false
        shield.hasShadow = false
        shield.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        shield.orderFront(nil)
        Self.shield = shield
    }
}

final class AuthPanel: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

final class ASCIISecureTextField: NSSecureTextField {
    override func becomeFirstResponder() -> Bool {
        let ok = super.becomeFirstResponder()
        if ok {
            EnglishIME.captureAndForceASCII()
            restrictFieldEditorToRoman(self)
        }
        return ok
    }
}

struct EnglishSecureField: NSViewRepresentable {
    @Binding var text: String
    var placeholder: String
    var isEnabled: Bool
    @Binding var isFocused: Bool
    var onSubmit: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> ASCIISecureTextField {
        let field = ASCIISecureTextField()
        field.isBordered = false
        field.isBezeled = false
        field.drawsBackground = false
        field.focusRingType = .none
        field.font = NSFont.systemFont(ofSize: 13)
        field.textColor = .black
        field.placeholderString = placeholder
        field.usesSingleLineMode = true
        field.cell?.wraps = false
        field.cell?.isScrollable = true
        field.delegate = context.coordinator
        return field
    }

    func updateNSView(_ nsView: ASCIISecureTextField, context: Context) {
        context.coordinator.parent = self
        if nsView.stringValue != text {
            nsView.stringValue = text
        }
        nsView.isEnabled = isEnabled
        nsView.placeholderString = placeholder
        guard isFocused, isEnabled, let window = nsView.window else { return }
        let responder = window.firstResponder
        if responder !== nsView && responder !== nsView.currentEditor() {
            window.makeFirstResponder(nsView)
        }
    }

    final class Coordinator: NSObject, NSTextFieldDelegate {
        var parent: EnglishSecureField
        init(_ parent: EnglishSecureField) {
            self.parent = parent
        }

        func controlTextDidBeginEditing(_ obj: Notification) {
            parent.isFocused = true
            if let field = obj.object as? NSTextField {
                EnglishIME.captureAndForceASCII()
                restrictFieldEditorToRoman(field)
            }
        }

        func controlTextDidChange(_ obj: Notification) {
            guard let field = obj.object as? NSTextField else { return }
            parent.text = field.stringValue
        }

        func controlTextDidEndEditing(_ obj: Notification) {
            parent.isFocused = false
        }

        func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            if commandSelector == #selector(NSResponder.insertNewline(_:)) {
                parent.onSubmit()
                return true
            }
            return false
        }
    }
}

struct AdminAuthView: View {
    let fullName: String
    let account: String
    let onCancel: () -> Void
    let onConfirm: (AdminCredentials) -> Void

    @State private var password = ""
    @State private var errorText = ""
    @State private var busy = false
    @State private var shake: CGFloat = 0
    @State private var passwordFocused = true

    private let cardGray = Color(red: 0.69, green: 0.69, blue: 0.70)
    private let fieldFill = Color(red: 0.78, green: 0.78, blue: 0.79)
    private let fieldFocusFill = Color(red: 0.93, green: 0.93, blue: 0.94)
    private let macBlue = Color(red: 0.204, green: 0.471, blue: 0.965)

    var body: some View {
        VStack(spacing: 0) {
            lockBadge
                .padding(.top, 22)
            Text(AppIdentity.displayName)
                .font(.system(size: 15, weight: .bold))
                .foregroundStyle(.black)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                .padding(.horizontal, 16)
                .padding(.top, 10)
            Text(L10n.t("auth.intro"))
                .font(.system(size: 13))
                .foregroundStyle(.black)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .minimumScaleFactor(0.9)
                .padding(.horizontal, 28)
                .padding(.top, 8)
            Text(L10n.t("auth.prompt"))
                .font(.system(size: 13))
                .foregroundStyle(.black)
                .multilineTextAlignment(.center)
                .lineLimit(1)
                .padding(.horizontal, 28)
                .padding(.top, 4)

            VStack(spacing: 8) {
                nameField
                passwordField
            }
            .padding(.horizontal, 36)
            .padding(.top, 16)
            .offset(x: shake)

            Text(errorText.isEmpty ? " " : errorText)
                .font(.system(size: 11))
                .foregroundStyle(Color(red: 0.75, green: 0.12, blue: 0.12))
                .opacity(errorText.isEmpty ? 0 : 1)
                .padding(.top, 6)
                .padding(.horizontal, 28)
                .multilineTextAlignment(.center)

            HStack(spacing: 10) {
                Button(L10n.t("auth.cancel")) { onCancel() }
                    .buttonStyle(AuthGrayButtonStyle())
                    .keyboardShortcut(.cancelAction)
                    .disabled(busy)
                Button(busy ? L10n.t("auth.checking") : L10n.t("auth.ok")) { submit() }
                    .buttonStyle(AuthBlueButtonStyle(blue: macBlue))
                    .keyboardShortcut(.defaultAction)
                    .disabled(busy || password.isEmpty)
            }
            .padding(.horizontal, 36)
            .padding(.top, 10)
            .padding(.bottom, 22)
        }
        .frame(width: AuthDialogMetrics.width, height: AuthDialogMetrics.height)
        .background(
            RoundedRectangle(cornerRadius: 28, style: .continuous)
                .fill(cardGray)
        )
        .clipShape(RoundedRectangle(cornerRadius: 28, style: .continuous))
    }

    private var lockBadge: some View {
        ZStack(alignment: .bottomTrailing) {
            Image(systemName: "lock.fill")
                .font(.system(size: 54, weight: .regular))
                .foregroundStyle(
                    LinearGradient(
                        colors: [
                            Color(red: 0.98, green: 0.84, blue: 0.32),
                            Color(red: 0.82, green: 0.58, blue: 0.10)
                        ],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                )
                .shadow(color: .black.opacity(0.22), radius: 2, y: 1)
            if let icon = NSImage(named: "AppIcon") {
                Image(nsImage: icon)
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 28, height: 28)
                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 6, style: .continuous)
                            .stroke(Color.white.opacity(0.75), lineWidth: 0.8)
                    )
                    .shadow(color: .black.opacity(0.25), radius: 1, y: 1)
                    .offset(x: 8, y: 6)
            } else {
                RoundedRectangle(cornerRadius: 5, style: .continuous)
                    .fill(Color.black)
                    .frame(width: 28, height: 22)
                    .overlay(
                        Text("USB")
                            .font(.system(size: 8, weight: .bold, design: .monospaced))
                            .foregroundStyle(Color(red: 0.35, green: 0.95, blue: 0.45))
                    )
                    .offset(x: 8, y: 6)
            }
        }
        .frame(width: 78, height: 68)
    }

    private var nameField: some View {
        Text(fullName)
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(.black)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 10)
            .frame(height: 28)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(fieldFill)
            )
    }

    private var passwordField: some View {
        EnglishSecureField(
            text: $password,
            placeholder: L10n.t("auth.password"),
            isEnabled: !busy,
            isFocused: $passwordFocused,
            onSubmit: submit
        )
        .padding(.horizontal, 10)
        .frame(height: 28)
        .background(
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(passwordFocused ? fieldFocusFill : fieldFill)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .stroke(passwordFocused ? macBlue : Color.clear, lineWidth: 3)
        )
        .disabled(busy)
    }

    private func submit() {
        let pass = password
        guard !pass.isEmpty, !busy else { return }
        busy = true
        errorText = ""
        let account = account
        Task.detached(priority: .userInitiated) {
            let result = AdminAuth.verify(account: account, password: pass)
            await MainActor.run {
                switch result {
                case .ok:
                    onConfirm(AdminCredentials(account: account, password: pass))
                case .badPassword:
                    busy = false
                    errorText = L10n.t("auth.bad_password")
                    passwordFocused = true
                    EnglishIME.captureAndForceASCII()
                    shakeError()
                case .notAdmin:
                    busy = false
                    errorText = L10n.t("auth.not_admin")
                    passwordFocused = true
                    EnglishIME.captureAndForceASCII()
                    shakeError()
                }
            }
        }
    }

    private func shakeError() {
        withAnimation(.default) { shake = 8 }
        withAnimation(.default.delay(0.05)) { shake = -8 }
        withAnimation(.default.delay(0.10)) { shake = 5 }
        withAnimation(.default.delay(0.15)) { shake = 0 }
    }
}

struct AuthGrayButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13))
            .frame(maxWidth: .infinity, minHeight: 26)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(Color(white: configuration.isPressed ? 0.62 : 0.78))
            )
            .foregroundStyle(Color.black.opacity(0.85))
    }
}

struct AuthBlueButtonStyle: ButtonStyle {
    let blue: Color
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .frame(maxWidth: .infinity, minHeight: 26)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(blue.opacity(configuration.isPressed ? 0.82 : 1))
            )
            .foregroundStyle(.white)
    }
}

enum AdminAuthOutcome {
    case ok
    case badPassword
    case notAdmin
}

/* Validate the login password without showing the system padlock dialog. */
/* 시스템 자물쇠 창 없이 로그인 암호가 맞는지 확인한다. */
enum AdminAuth {
    /// Returns whether `account` may run administrator shell commands.
    /// `account` 가 관리자 셸을 실행할 수 있는지 반환한다.
    static func verify(account: String, password: String) -> AdminAuthOutcome {
        guard passwordOk(account: account, password: password) else {
            return .badPassword
        }
        guard isAdmin(account: account) else {
            return .notAdmin
        }
        return .ok
    }

    /// Silent OpenDirectory password check. Avoids putting the password on `dscl` argv.
    /// OpenDirectory 로 암호를 확인한다. `dscl` argv 에 암호를 올리지 않는다.
    private static func passwordOk(account: String, password: String) -> Bool {
        do {
            let session = ODSession.default()
            let node = try ODNode(session: session, type: ODNodeType(kODNodeTypeAuthentication))
            let record = try node.record(
                withRecordType: kODRecordTypeUsers,
                name: account,
                attributes: nil
            )
            try record.verifyPassword(password)
            return true
        } catch {
            return false
        }
    }

    /// True if the short username is in the `admin` group.
    /// 짧은 사용자 이름이 `admin` 그룹에 있으면 true.
    private static func isAdmin(account: String) -> Bool {
        if isAdminOpenDirectory(account) { return true }
        let groups = runOutput("/usr/bin/id", ["-Gn", account])
        let names = groups.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        return names.contains("admin")
    }

    /// Nested/directory admin groups via OpenDirectory. Falls back to id -Gn.
    /// OpenDirectory 로 중첩·디렉터리 admin 그룹을 본다. 실패 시 id -Gn.
    private static func isAdminOpenDirectory(_ account: String) -> Bool {
        do {
            let session = ODSession.default()
            let node = try ODNode(session: session, type: ODNodeType(kODNodeTypeAuthentication))
            let user = try node.record(
                withRecordType: kODRecordTypeUsers,
                name: account,
                attributes: [kODAttributeTypePrimaryGroupID]
            )
            if let gids = try? user.values(forAttribute: kODAttributeTypePrimaryGroupID) as? [String],
               gids.contains("80") {
                return true
            }
            let admin = try node.record(
                withRecordType: kODRecordTypeGroups,
                name: "admin",
                attributes: nil
            )
            /* Swift imports the BOOL API as throws: success means member. */
            /* Swift 는 BOOL API 를 throw 로 가져온다. 성공이면 구성원이다. */
            try user.isMemberRecord(admin)
            return true
        } catch {
            return false
        }
    }

    private static func runOutput(_ path: String, _ args: [String]) -> String {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: path)
        proc.arguments = args
        let out = Pipe()
        proc.standardOutput = out
        proc.standardError = FileHandle.nullDevice
        do {
            try proc.run()
            proc.waitUntilExit()
        } catch {
            return ""
        }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        return String(data: data, encoding: .utf8) ?? ""
    }
}
