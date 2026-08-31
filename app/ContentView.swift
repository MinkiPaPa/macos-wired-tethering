/*
 * ContentView.swift
 * Menu-bar popover dashboard: devices, connection, metrics, log, quit.
 * 메뉴 막대 팝오버 대시보드: 장치, 연결, 지표, 로그, 종료.
 */
import SwiftUI
import AppKit

struct MenuBarDashboard: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HeaderBar()
            Divider()
            DeviceSection()
            Divider()
            ConnectionSection()
            Divider()
            MetricsSection()
            Divider()
            FooterBar()
        }
        .frame(width: 380)
        .fixedSize(horizontal: true, vertical: true)
    }
}

struct HeaderBar: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        HStack(spacing: 10) {
            if let icon = NSImage(named: "AppIcon") {
                Image(nsImage: icon)
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 28, height: 28)
                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            } else {
                Image(systemName: statusSymbol)
                    .font(.title3)
                    .foregroundStyle(statusColor)
                    .frame(width: 28)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(AppIdentity.displayName)
                    .font(.headline)
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
                Text(controller.statusSummary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer(minLength: 8)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
    }

    private var statusColor: Color {
        switch controller.connectionState {
        case .connected: return .green
        case .connecting: return .orange
        case .error: return .red
        default: return .secondary
        }
    }

    private var statusSymbol: String {
        switch controller.connectionState {
        case .connected: return "checkmark.circle.fill"
        case .connecting: return "arrow.triangle.2.circlepath"
        case .error: return "exclamationmark.triangle.fill"
        default: return "cable.connector"
        }
    }
}

struct DeviceSection: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(L10n.t("devices.title"))
                    .font(.subheadline.weight(.semibold))
                Spacer()
                Button {
                    controller.refreshDevices()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.plain)
                .help(L10n.t("devices.refresh"))
                .accessibilityLabel(L10n.t("devices.refresh_a11y"))
            }

            if controller.devices.isEmpty {
                Text(L10n.t("devices.empty"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 4)
            } else {
                VStack(spacing: 4) {
                    ForEach(controller.devices) { device in
                        DeviceRow(device: device, selected: device.id == controller.selectedID)
                            .contentShape(Rectangle())
                            .onTapGesture {
                                controller.selectedID = device.id
                            }
                            .accessibilityAddTraits(device.id == controller.selectedID ? .isSelected : [])
                            .accessibilityLabel(L10n.t("devices.row_a11y", device.name, device.kind.label))
                    }
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }
}

struct DeviceRow: View {
    let device: USBDevice
    var selected: Bool = false

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: device.kind == .rndis ? "antenna.radiowaves.left.and.right" : "iphone")
                .foregroundStyle(device.kind == .rndis ? Color.green : Color.orange)
                .frame(width: 18)
            VStack(alignment: .leading, spacing: 1) {
                Text(device.name)
                    .font(.callout)
                    .lineLimit(1)
                Text("\(device.kind.label) · \(device.vidPid)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if selected {
                Image(systemName: "checkmark")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(selected ? Color.accentColor.opacity(0.12) : Color.clear)
        )
    }
}

struct ConnectionSection: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if controller.status.state == .error {
                HintCard(
                    title: controller.status.error.isEmpty ? L10n.t("error.connect_failed") : controller.status.error,
                    bodyText: controller.pendingRouteRestore
                        ? TetherMessages.routeRestoreHint
                        : L10n.t("error.retry_hint"),
                    tint: .red
                )
            } else if controller.pendingRouteRestore && !controller.isSessionActive {
                HintCard(
                    title: L10n.t("hint.route_title"),
                    bodyText: TetherMessages.routeRestoreHint,
                    tint: .red
                )
            } else if controller.canConnect == false && controller.selectedDevice?.kind == .android {
                HintCard(
                    title: L10n.t("hint.tether_title"),
                    bodyText: L10n.t("hint.tether_body")
                )
            }

            Toggle(L10n.t("route.toggle"), isOn: $controller.preferDefaultRoute)
                .toggleStyle(.switch)
                .disabled(controller.isSessionActive)
                .help(L10n.t("route.help"))
                .accessibilityHint(L10n.t("route.a11y"))

            Text(controller.preferDefaultRoute
                 ? L10n.t("route.hint_on")
                 : L10n.t("route.hint_off"))
                .font(.caption2)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Button {
                if controller.isSessionActive {
                    controller.disconnect()
                } else {
                    controller.connectSelected()
                }
            } label: {
                Text(connectLabel)
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(controller.isSessionActive ? .red : .accentColor)
            .disabled(!controller.isSessionActive && !controller.canConnect)
            .controlSize(.large)
            .accessibilityLabel(connectLabel)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private var connectLabel: String {
        if controller.isSessionActive { return L10n.t("action.disconnect") }
        if controller.status.state == .error { return L10n.t("action.retry") }
        return L10n.t("action.connect")
    }
}

struct MetricsSection: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                MetricTile(title: L10n.t("metric.iface"), value: empty(controller.status.iface))
                MetricTile(title: L10n.t("metric.ipv4"), value: empty(controller.status.ip))
                MetricTile(title: L10n.t("metric.gateway"), value: empty(controller.status.gateway))
                MetricTile(title: L10n.t("metric.dns"), value: empty(controller.status.dns))
                MetricTile(title: L10n.t("metric.link"), value: controller.linkSummary)
                MetricTile(title: L10n.t("metric.mtu"), value: controller.status.mtu > 0 ? "\(controller.status.mtu)" : "—")
            }
            VStack(alignment: .leading, spacing: 3) {
                Text(controller.isSessionActive
                     ? controller.liveRateLine
                     : L10n.t("metric.live", TetherController.formatRate(0), TetherController.formatRate(0)))
                    .lineLimit(1)
                Text((controller.isSessionActive || controller.status.state == .error)
                     ? controller.peakRateLine
                     : L10n.t("metric.peak", TetherController.formatRate(0), TetherController.formatRate(0)))
                    .lineLimit(1)
                Text(controller.byteLine)
                    .lineLimit(1)
                Text(controller.dropLine)
                    .lineLimit(1)
            }
            .font(.caption.monospacedDigit())
            .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private func empty(_ s: String) -> String { s.isEmpty ? "—" : s }
}

struct HintCard: View {
    let title: String
    let bodyText: String
    var tint: Color = .orange
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption.weight(.semibold))
            Text(bodyText).font(.caption2).foregroundStyle(.secondary)
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(tint.opacity(0.12)))
    }
}

struct MetricTile: View {
    let title: String
    let value: String
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.caption2)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.caption.monospacedDigit())
                .lineLimit(2)
                .minimumScaleFactor(0.8)
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(.quaternary.opacity(0.5)))
    }
}

struct LogPane: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 3) {
                    ForEach(controller.logs) { line in
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Text(Self.stamp(line.date))
                                .foregroundStyle(.secondary)
                            Text(line.level.uppercased())
                                .foregroundStyle(color(line.level))
                                .frame(width: 44, alignment: .leading)
                            Text(line.message)
                                .textSelection(.enabled)
                        }
                        .font(.system(size: 12, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .id(line.id)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
            }
            .onChange(of: controller.logs.count) { _, _ in
                if let last = controller.logs.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
            .onAppear {
                if let last = controller.logs.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
        .frame(minHeight: 320, maxHeight: .infinity)
    }

    private static func stamp(_ d: Date) -> String {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f.string(from: d)
    }

    private func color(_ level: String) -> Color {
        switch level {
        case "error": return .red
        case "warn": return .orange
        case "debug": return .secondary
        default: return .accentColor
        }
    }
}

struct FooterBar: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        HStack(spacing: 14) {
            Text(controller.helperStatusLine)
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .lineLimit(2)
            Spacer(minLength: 8)
            Button(L10n.t("footer.about")) {
                controller.openAbout()
            }
            .buttonStyle(.plain)
            .help(L10n.t("footer.about_help"))
            .accessibilityLabel(L10n.t("footer.about_a11y"))
            Button(controller.helperStatus == .enabled ? L10n.t("footer.helper_off") : L10n.t("footer.helper")) {
                controller.toggleHelper()
            }
            .buttonStyle(.plain)
            .help(L10n.t("footer.helper_help"))
            .accessibilityLabel(controller.helperStatus == .enabled ? L10n.t("footer.helper_on_a11y") : L10n.t("footer.helper_off_a11y"))
            Button(L10n.t("footer.log")) {
                controller.openLogWindow()
            }
            .buttonStyle(.plain)
            .accessibilityLabel(L10n.t("footer.log_a11y"))
            Button(L10n.t("footer.quit")) {
                /* applicationShouldTerminate waits for the engine asynchronously. */
                /* applicationShouldTerminate 가 엔진 종료를 비동기로 기다린다. */
                NSApp.terminate(nil)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(L10n.t("footer.quit_a11y"))
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }
}
