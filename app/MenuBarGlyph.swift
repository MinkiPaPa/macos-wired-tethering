/*
 * MenuBarGlyph.swift
 * Template menu-bar glyphs, including a connecting-wave animation.
 * 메뉴 막대 템플릿 글리프. 연결 중일 때 신호 파동 애니메이션을 포함한다.
 */
import AppKit

enum MenuBarGlyph {
    /// Status-item image for the current connection state and animation frame.
    /// 현재 연결 상태와 애니메이션 프레임에 맞는 상태 항목 이미지.
    static func image(state: ConnectionState, frame: Int) -> NSImage {
        switch state {
        case .connecting, .stopping:
            return connecting(frame: frame)
        case .connected:
            return cached("connected") { drawSignal(waves: 3) }
        case .error:
            return cached("error") { drawError() }
        default:
            return idle()
        }
    }

    /// Four-frame wave loop (0...3) used while connecting.
    /// 연결 중 사용하는 4프레임 파동 루프 (0...3).
    static func connecting(frame: Int) -> NSImage {
        let wave = ((frame % 4) + 4) % 4
        return cached("connecting-\(wave)") { drawSignal(waves: wave) }
    }

    /// Idle: bundled template icon, or a simple cable glyph.
    /// 대기: 번들 템플릿 아이콘, 없으면 간단한 케이블 글리프.
    static func idle() -> NSImage {
        if let img = NSImage(named: "MenuBarIcon") {
            let copy = img.copy() as? NSImage ?? img
            copy.isTemplate = true
            copy.size = NSSize(width: 18, height: 18)
            return copy
        }
        return cached("idle") { drawSignal(waves: 0) }
    }

    /* ---------- drawing / 그리기 ---------- */

    private static var cache: [String: NSImage] = [:]

    private static func cached(_ key: String, draw: () -> NSImage) -> NSImage {
        if let hit = cache[key] { return hit }
        let img = draw()
        cache[key] = img
        return img
    }

    /// 18pt template image backed by a 2x bitmap.
    /// 2x 비트맵을 가진 18pt 템플릿 이미지.
    private static func makeTemplate(_ draw: (CGRect) -> Void) -> NSImage {
        let point = CGSize(width: 18, height: 18)
        let px = 36
        guard let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: px,
            pixelsHigh: px,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ) else {
            return NSImage(size: point)
        }
        rep.size = point
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        NSGraphicsContext.current?.imageInterpolation = .high
        NSColor.clear.setFill()
        NSRect(origin: .zero, size: point).fill()
        draw(CGRect(origin: .zero, size: point))
        NSGraphicsContext.restoreGraphicsState()

        let img = NSImage(size: point)
        img.addRepresentation(rep)
        img.isTemplate = true
        return img
    }

    /// Phone body plus 0...3 radio arcs. Frame 0 is the body only.
    /// 휴대폰 본체와 0...3개의 전파 호. 프레임 0은 본체만.
    private static func drawSignal(waves: Int) -> NSImage {
        makeTemplate { rect in
            let phone = NSBezierPath(
                roundedRect: NSRect(x: 1.5, y: 4.0, width: 6.5, height: 10.0),
                xRadius: 1.2,
                yRadius: 1.2
            )
            phone.lineWidth = 1.35
            NSColor.black.setStroke()
            phone.stroke()

            let speaker = NSBezierPath(
                roundedRect: NSRect(x: 3.1, y: 12.2, width: 3.3, height: 0.9),
                xRadius: 0.35,
                yRadius: 0.35
            )
            NSColor.black.setFill()
            speaker.fill()

            /* Origin of the radio waves, just right of the phone. */
            /* 전파의 원점. 휴대폰 바로 오른쪽. */
            let origin = NSPoint(x: 9.2, y: 9.0)
            let radii: [CGFloat] = [3.2, 5.4, 7.6]
            let count = max(0, min(waves, radii.count))
            for i in 0..<count {
                let r = radii[i]
                let arc = NSBezierPath()
                arc.appendArc(
                    withCenter: origin,
                    radius: r,
                    startAngle: -52,
                    endAngle: 52
                )
                arc.lineWidth = 1.35
                arc.lineCapStyle = .round
                NSColor.black.setStroke()
                arc.stroke()
            }
            _ = rect
        }
    }

    private static func drawError() -> NSImage {
        makeTemplate { _ in
            let phone = NSBezierPath(
                roundedRect: NSRect(x: 2.0, y: 4.0, width: 6.5, height: 10.0),
                xRadius: 1.2,
                yRadius: 1.2
            )
            phone.lineWidth = 1.35
            NSColor.black.setStroke()
            phone.stroke()

            let slash = NSBezierPath()
            slash.move(to: NSPoint(x: 11.0, y: 5.2))
            slash.line(to: NSPoint(x: 16.2, y: 13.0))
            slash.lineWidth = 1.5
            slash.lineCapStyle = .round
            slash.stroke()
        }
    }
}
