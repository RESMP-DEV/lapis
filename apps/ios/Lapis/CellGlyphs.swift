import SwiftUI

// Block elements and box drawing are drawn as shapes filling their cell, as
// terminals do, so they join across rows and neighbouring cells. Font glyphs
// for them are shorter than a row and leave lines through logos and borders.
enum CellGlyphs {
    static func isDrawn(_ character: Character) -> Bool {
        guard character.unicodeScalars.count == 1, let value = character.unicodeScalars.first?.value
        else { return false }
        return (0x2580...0x259F).contains(value) || lines[value] != nil || arcs[value] != nil
    }

    static func draw(_ character: Character, in cell: CGRect, color: Color, scale: CGFloat,
                     context: GraphicsContext) {
        guard let value = character.unicodeScalars.first?.value else { return }
        if (0x2580...0x259F).contains(value) {
            block(value, in: cell, color: color, scale: scale, context: context)
        } else if let arms = lines[value] {
            box(arms, in: cell, color: color, scale: scale, context: context)
        } else if let arc = arcs[value] {
            rounded(arc, in: cell, color: color, scale: scale, context: context)
        }
    }

    // MARK: Block elements (U+2580–U+259F)

    private static func block(_ value: UInt32, in cell: CGRect, color: Color, scale: CGFloat,
                              context: GraphicsContext) {
        let w = cell.width, h = cell.height
        func fill(_ x: CGFloat, _ y: CGFloat, _ width: CGFloat, _ height: CGFloat, _ alpha: Double = 1) {
            let rect = snapped(CGRect(x: cell.minX + x, y: cell.minY + y, width: width, height: height), scale)
            context.fill(Path(rect), with: .color(color.opacity(alpha)))
        }
        switch value {
        case 0x2580: fill(0, 0, w, h / 2)
        case 0x2581...0x2588:
            let part = h * CGFloat(value - 0x2580) / 8
            fill(0, h - part, w, part)
        case 0x2589...0x258F:
            fill(0, 0, w * CGFloat(0x2590 - value) / 8, h)
        case 0x2590: fill(w / 2, 0, w / 2, h)
        case 0x2591: fill(0, 0, w, h, 0.25)
        case 0x2592: fill(0, 0, w, h, 0.5)
        case 0x2593: fill(0, 0, w, h, 0.75)
        case 0x2594: fill(0, 0, w, h / 8)
        case 0x2595: fill(w * 7 / 8, 0, w / 8, h)
        default:
            // Quadrants: upper left, upper right, lower left, lower right.
            let quadrants: [UInt32: (Bool, Bool, Bool, Bool)] = [
                0x2596: (false, false, true, false), 0x2597: (false, false, false, true),
                0x2598: (true, false, false, false), 0x2599: (true, false, true, true),
                0x259A: (true, false, false, true), 0x259B: (true, true, true, false),
                0x259C: (true, true, false, true), 0x259D: (false, true, false, false),
                0x259E: (false, true, true, false), 0x259F: (false, true, true, true),
            ]
            guard let (ul, ur, ll, lr) = quadrants[value] else { return }
            if ul { fill(0, 0, w / 2, h / 2) }
            if ur { fill(w / 2, 0, w / 2, h / 2) }
            if ll { fill(0, h / 2, w / 2, h / 2) }
            if lr { fill(w / 2, h / 2, w / 2, h / 2) }
        }
    }

    // MARK: Box drawing (U+2500–U+257F)

    // Arms up, right, down, left: 0 none, 1 light, 2 heavy, 3 double.
    private struct Arms {
        let up, right, down, left: UInt8
        init(_ up: UInt8, _ right: UInt8, _ down: UInt8, _ left: UInt8) {
            (self.up, self.right, self.down, self.left) = (up, right, down, left)
        }
    }

    private static let lines: [UInt32: Arms] = {
        var table: [UInt32: Arms] = [
            0x2500: Arms(0, 1, 0, 1), 0x2501: Arms(0, 2, 0, 2), 0x2502: Arms(1, 0, 1, 0), 0x2503: Arms(2, 0, 2, 0),
            0x250C: Arms(0, 1, 1, 0), 0x250F: Arms(0, 2, 2, 0), 0x2510: Arms(0, 0, 1, 1), 0x2513: Arms(0, 0, 2, 2),
            0x2514: Arms(1, 1, 0, 0), 0x2517: Arms(2, 2, 0, 0), 0x2518: Arms(1, 0, 0, 1), 0x251B: Arms(2, 0, 0, 2),
            0x251C: Arms(1, 1, 1, 0), 0x2523: Arms(2, 2, 2, 0), 0x2524: Arms(1, 0, 1, 1), 0x252B: Arms(2, 0, 2, 2),
            0x252C: Arms(0, 1, 1, 1), 0x2533: Arms(0, 2, 2, 2), 0x2534: Arms(1, 1, 0, 1), 0x253B: Arms(2, 2, 0, 2),
            0x253C: Arms(1, 1, 1, 1), 0x254B: Arms(2, 2, 2, 2),
            0x2550: Arms(0, 3, 0, 3), 0x2551: Arms(3, 0, 3, 0), 0x2554: Arms(0, 3, 3, 0), 0x2557: Arms(0, 0, 3, 3),
            0x255A: Arms(3, 3, 0, 0), 0x255D: Arms(3, 0, 0, 3), 0x2560: Arms(3, 3, 3, 0), 0x2563: Arms(3, 0, 3, 3),
            0x2566: Arms(0, 3, 3, 3), 0x2569: Arms(3, 3, 0, 3), 0x256C: Arms(3, 3, 3, 3),
            0x2574: Arms(0, 0, 0, 1), 0x2575: Arms(1, 0, 0, 0), 0x2576: Arms(0, 1, 0, 0), 0x2577: Arms(0, 0, 1, 0),
            0x2578: Arms(0, 0, 0, 2), 0x2579: Arms(2, 0, 0, 0), 0x257A: Arms(0, 2, 0, 0), 0x257B: Arms(0, 0, 2, 0),
        ]
        // Dashed lines are drawn solid.
        for (dashed, solid) in [(0x2504, 0x2500), (0x2505, 0x2501), (0x2506, 0x2502), (0x2507, 0x2503),
                                (0x2508, 0x2500), (0x2509, 0x2501), (0x250A, 0x2502), (0x250B, 0x2503),
                                (0x254C, 0x2500), (0x254D, 0x2501), (0x254E, 0x2502), (0x254F, 0x2503)] {
            table[UInt32(dashed)] = table[UInt32(solid)]
        }
        return table
    }()

    private static func box(_ arms: Arms, in cell: CGRect, color: Color, scale: CGFloat,
                            context: GraphicsContext) {
        let light = max(1 / scale, (cell.width * 0.12 * scale).rounded() / scale)
        let cx = cell.midX, cy = cell.midY
        func thickness(_ weight: UInt8) -> CGFloat { weight == 2 ? light * 2 : light }
        func fill(_ rect: CGRect) { context.fill(Path(snapped(rect, scale)), with: .color(color)) }
        // Each arm runs from the far edge to past the centre, so joints close.
        func horizontal(from x0: CGFloat, to x1: CGFloat, weight: UInt8) {
            guard weight > 0 else { return }
            let t = thickness(weight)
            let offsets: [CGFloat] = weight == 3 ? [-t * 1.5, t * 0.5] : [-t / 2]
            for offset in offsets { fill(CGRect(x: min(x0, x1), y: cy + offset, width: abs(x1 - x0), height: t)) }
        }
        func vertical(from y0: CGFloat, to y1: CGFloat, weight: UInt8) {
            guard weight > 0 else { return }
            let t = thickness(weight)
            let offsets: [CGFloat] = weight == 3 ? [-t * 1.5, t * 0.5] : [-t / 2]
            for offset in offsets { fill(CGRect(x: cx + offset, y: min(y0, y1), width: t, height: abs(y1 - y0))) }
        }
        let reach = light * 2
        horizontal(from: cell.minX, to: cx + (arms.left > 0 ? reach : 0), weight: arms.left)
        horizontal(from: cx - (arms.right > 0 ? reach : 0), to: cell.maxX, weight: arms.right)
        vertical(from: cell.minY, to: cy + (arms.up > 0 ? reach : 0), weight: arms.up)
        vertical(from: cy - (arms.down > 0 ? reach : 0), to: cell.maxY, weight: arms.down)
    }

    // Rounded corners: which two edges the arc joins.
    private enum Arc { case rightDown, leftDown, leftUp, rightUp }
    private static let arcs: [UInt32: Arc] = [0x256D: .rightDown, 0x256E: .leftDown, 0x256F: .leftUp, 0x2570: .rightUp]

    private static func rounded(_ arc: Arc, in cell: CGRect, color: Color, scale: CGFloat,
                                context: GraphicsContext) {
        let t = max(1 / scale, (cell.width * 0.12 * scale).rounded() / scale)
        let cx = cell.midX, cy = cell.midY
        let radius = min(cell.width, cell.height) / 2
        var path = Path()
        switch arc {
        case .rightDown:
            path.move(to: CGPoint(x: cell.maxX, y: cy))
            path.addArc(tangent1End: CGPoint(x: cx, y: cy), tangent2End: CGPoint(x: cx, y: cell.maxY), radius: radius)
            path.addLine(to: CGPoint(x: cx, y: cell.maxY))
        case .leftDown:
            path.move(to: CGPoint(x: cell.minX, y: cy))
            path.addArc(tangent1End: CGPoint(x: cx, y: cy), tangent2End: CGPoint(x: cx, y: cell.maxY), radius: radius)
            path.addLine(to: CGPoint(x: cx, y: cell.maxY))
        case .leftUp:
            path.move(to: CGPoint(x: cell.minX, y: cy))
            path.addArc(tangent1End: CGPoint(x: cx, y: cy), tangent2End: CGPoint(x: cx, y: cell.minY), radius: radius)
            path.addLine(to: CGPoint(x: cx, y: cell.minY))
        case .rightUp:
            path.move(to: CGPoint(x: cell.maxX, y: cy))
            path.addArc(tangent1End: CGPoint(x: cx, y: cy), tangent2End: CGPoint(x: cx, y: cell.minY), radius: radius)
            path.addLine(to: CGPoint(x: cx, y: cell.minY))
        }
        context.stroke(path, with: .color(color), lineWidth: t)
    }

    // Edges on device pixels, so cells that meet leave no seam.
    static func snapped(_ rect: CGRect, _ scale: CGFloat) -> CGRect {
        let minX = (rect.minX * scale).rounded() / scale
        let minY = (rect.minY * scale).rounded() / scale
        let maxX = (rect.maxX * scale).rounded() / scale
        let maxY = (rect.maxY * scale).rounded() / scale
        return CGRect(x: minX, y: minY, width: max(maxX - minX, 1 / scale), height: max(maxY - minY, 1 / scale))
    }
}
