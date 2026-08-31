/*
 * LogWindow.swift
 * Separate window that shows the full tethering log.
 * 테더링 로그 전체를 보여주는 별도 창.
 */
import SwiftUI

struct LogWindowView: View {
    @EnvironmentObject var controller: TetherController

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text(L10n.t("log.title"))
                    .font(.headline)
                Spacer()
                Text(L10n.t("log.line_count", controller.logs.count))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            Divider()
            LogPane()
        }
        .frame(minWidth: 560, minHeight: 360)
    }
}
