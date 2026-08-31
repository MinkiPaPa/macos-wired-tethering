/*
 * AboutWindow.swift
 * About box sized to its content so credits never scroll.
 * 내용 높이에 맞춘 정보 창. 크레딧에 스크롤이 생기지 않는다.
 */
import SwiftUI
import AppKit

enum AboutIdentity {
    static let githubURL = URL(string: "https://github.com/MinkiPaPa")!
    static let githubDisplay = "https://github.com/MinkiPaPa"
}

struct AboutView: View {
    private var copyright: String {
        Bundle.main.object(forInfoDictionaryKey: "NSHumanReadableCopyright") as? String
            ?? "Copyright © 2026 macOS wired tethering contributors. MIT License."
    }

    var body: some View {
        VStack(spacing: 0) {
            Image(nsImage: NSApplication.shared.applicationIconImage)
                .resizable()
                .interpolation(.high)
                .frame(width: 80, height: 80)
                .padding(.bottom, 10)

            Text(AppIdentity.displayName)
                .font(.system(size: 15, weight: .bold))
                .multilineTextAlignment(.center)

            Text(L10n.t("about.version", AppIdentity.shortVersion, AppIdentity.buildNumber))
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .padding(.top, 3)
                .padding(.bottom, 14)

            VStack(spacing: 3) {
                Text(L10n.t("about.privacy1"))
                Text(L10n.t("about.privacy2"))
            }
            .font(.system(size: 11))
            .multilineTextAlignment(.center)
            .padding(.bottom, 12)

            Text(L10n.t("about.developer"))
                .font(.system(size: 11))
            Link(AboutIdentity.githubDisplay, destination: AboutIdentity.githubURL)
                .font(.system(size: 11))
                .accessibilityLabel(L10n.t("about.github_a11y"))
                .padding(.top, 2)
                .padding(.bottom, 16)

            Text(copyright)
                .font(.system(size: 9))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .frame(width: 420)
        .padding(.horizontal, 22)
        .padding(.top, 22)
        .padding(.bottom, 18)
        .fixedSize(horizontal: true, vertical: true)
    }
}

enum AboutPanel {
    /// Build a non-scrolling About window that hugs its SwiftUI content.
    /// 스크롤 없이 SwiftUI 내용 높이에 맞춘 정보 창을 만든다.
    static func makeWindow() -> NSWindow {
        let host = NSHostingController(rootView: AboutView())
        host.sizingOptions = [.intrinsicContentSize]
        let window = NSPanel(contentViewController: host)
        window.styleMask = [.titled, .closable]
        window.title = AppIdentity.displayName
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isMovableByWindowBackground = true
        window.isReleasedWhenClosed = false
        window.standardWindowButton(.miniaturizeButton)?.isEnabled = false
        window.standardWindowButton(.zoomButton)?.isEnabled = false
        window.standardWindowButton(.miniaturizeButton)?.isHidden = true
        window.standardWindowButton(.zoomButton)?.isHidden = true
        return window
    }
}
