/*
 * MacOSWiredTetheringApp.swift
 * Menu-bar-only agent: no Dock icon, no main window.
 * 메뉴 막대 전용 에이전트. Dock 아이콘과 메인 창을 만들지 않는다.
 */
import SwiftUI
import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    static weak var shared: AppDelegate?
    weak var controller: TetherController?

    override init() {
        super.init()
        AppDelegate.shared = self
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        /* Accessory: menu bar only, not in the Dock. */
        /* accessory: 메뉴 막대만 보이고 Dock에는 나오지 않는다. */
        NSApp.setActivationPolicy(.accessory)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        /* Re-open from Finder should not create a window. */
        /* Finder에서 다시 열어도 창을 만들지 않는다. */
        false
    }

    /* Wait for the engine to restore routes before the GUI (and osascript) dies. */
    /* GUI(와 osascript)가 죽기 전에 엔진이 경로를 복구할 시간을 준다. */
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let controller, controller.needsEngineTeardown else {
            return .terminateNow
        }
        Task { @MainActor in
            await controller.disconnectAsync()
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }
}

@main
struct MacOSWiredTetheringApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var controller = TetherController()

    var body: some Scene {
        MenuBarExtra {
            MenuBarDashboard()
                .environmentObject(controller)
        } label: {
            /* id forces the status item to swap glyphs on each animation tick. */
            /* id 로 애니메이션 틱마다 상태 항목 글리프를 갈아끼운다. */
            Image(nsImage: MenuBarGlyph.image(
                state: controller.connectionState,
                frame: controller.menuIconFrame
            ))
            .accessibilityLabel(AppIdentity.displayName)
            .accessibilityValue(controller.statusSummary)
            .id("mb-\(controller.connectionState.rawValue)-\(controller.menuIconFrame)")
        }
        .menuBarExtraStyle(.window)
    }
}
