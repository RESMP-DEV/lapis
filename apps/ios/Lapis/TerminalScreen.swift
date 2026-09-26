import SwiftUI
import UIKit

// The agent's screen as the Mac's terminal engine drew it. The phone only
// draws cells; the terminal itself runs on the Mac.
struct TerminalMetrics {
    let fontSize: CGFloat
    let cellWidth: CGFloat
    let lineHeight: CGFloat

    init(fontSize: CGFloat) {
        self.fontSize = fontSize
        let font = UIFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
        cellWidth = ("M" as NSString).size(withAttributes: [.font: font]).width
        lineHeight = ceil(font.lineHeight)
    }

    func grid(for size: CGSize) -> (columns: Int, rows: Int) {
        (max(20, Int((size.width - 8) / cellWidth)), max(6, Int(size.height / lineHeight)))
    }
}

// One terminal row. Backgrounds fill the whole row height and each run sits
// at its own column, so shaded blocks join without seams between rows.
struct TerminalRow: View {
    let runs: [Run]
    let columns: Int
    let metrics: TerminalMetrics
    let foreground: Color
    let background: Color
    @Environment(\.displayScale) private var scale

    var body: some View {
        Canvas { context, size in
            var next = 0
            for run in runs {
                let column = run.column ?? next
                let cells = run.width ?? run.text.count
                next = column + cells
                var ink = run.foreground.flatMap(Color.init(hex:)) ?? foreground
                var paper = run.background.flatMap(Color.init(hex:))
                if run.flags & Run.cursor != 0 {
                    (ink, paper) = (paper ?? background, ink)
                }
                if let paper {
                    let rect = CGRect(x: CGFloat(column) * metrics.cellWidth, y: 0,
                                      width: CGFloat(cells) * metrics.cellWidth, height: size.height)
                    context.fill(Path(CellGlyphs.snapped(rect, scale)), with: .color(paper))
                }
                if run.text.allSatisfy({ $0 == " " }) { continue }
                if run.flags & Run.faint != 0 { ink = ink.opacity(0.55) }
                draw(run, at: column, cells: cells, ink: ink, height: size.height, in: context)
            }
        }
        .frame(width: CGFloat(columns) * metrics.cellWidth, height: metrics.lineHeight)
    }

    // Text is drawn in stretches; block and box characters as cell shapes.
    private func draw(_ run: Run, at column: Int, cells: Int, ink: Color, height: CGFloat,
                      in context: GraphicsContext) {
        let characters = Array(run.text)
        guard characters.count == cells, characters.contains(where: CellGlyphs.isDrawn) else {
            drawText(run.text, run: run, at: column, ink: ink, height: height, in: context)
            return
        }
        var pending = ""
        var pendingColumn = column
        for (offset, character) in characters.enumerated() {
            if CellGlyphs.isDrawn(character) {
                drawText(pending, run: run, at: pendingColumn, ink: ink, height: height, in: context)
                pending = ""
                let cell = CGRect(x: CGFloat(column + offset) * metrics.cellWidth, y: 0,
                                  width: metrics.cellWidth, height: height)
                CellGlyphs.draw(character, in: cell, color: ink, scale: scale, context: context)
                pendingColumn = column + offset + 1
            } else {
                pending.append(character)
            }
        }
        drawText(pending, run: run, at: pendingColumn, ink: ink, height: height, in: context)
    }

    private func drawText(_ string: String, run: Run, at column: Int, ink: Color, height: CGFloat,
                          in context: GraphicsContext) {
        guard !string.isEmpty, !string.allSatisfy({ $0 == " " }) else { return }
        var text = Text(TerminalRow.textPresentation(string))
            .font(.system(size: metrics.fontSize, weight: run.flags & Run.bold != 0 ? .bold : .regular,
                          design: .monospaced))
            .foregroundStyle(ink)
        if run.flags & Run.italic != 0 { text = text.italic() }
        if run.flags & Run.underline != 0 { text = text.underline() }
        if run.flags & Run.strike != 0 { text = text.strikethrough() }
        context.draw(text, at: CGPoint(x: CGFloat(column) * metrics.cellWidth, y: height / 2),
                     anchor: .leading)
    }

    // Symbols that are text by default but also have an emoji form, such as
    // Claude Code's ⏺ before each message or ⏸, keep the text form the Mac's
    // terminal draws, one cell wide: iOS would otherwise draw them as emoji
    // tiles two cells wide. A program asking for the emoji (U+FE0F) keeps it.
    static func textPresentation(_ string: String) -> String {
        guard string.unicodeScalars.contains(where: emojiCapableText) else { return string }
        var shown = ""
        for character in string {
            shown.append(character)
            if character.unicodeScalars.count == 1, let scalar = character.unicodeScalars.first,
               emojiCapableText(scalar) {
                shown.unicodeScalars.append("\u{FE0E}")
            }
        }
        return shown
    }

    private static func emojiCapableText(_ scalar: Unicode.Scalar) -> Bool {
        scalar.value > 0x7F && scalar.properties.isEmoji && !scalar.properties.isEmojiPresentation
    }
}

struct TerminalScreen: View {
    let frame: ScreenFrame?
    let history: [HistoryChunk]
    let historyEnd: Bool
    let loadingHistory: Bool
    // Columns that fit the phone; wider rows wrap instead of scrolling sideways.
    let fitColumns: Int
    let metrics: TerminalMetrics
    let loadOlder: () async -> Void
    @State private var nearTop = false

    private struct Row: Identifiable {
        let id: String
        let runs: [Run]
        let columns: Int
    }

    @State private var cache = HistoryCache()

    // Loaded pages are immutable. Retain wrapping and accessibility text while
    // live frames change; a width or page-set change rebuilds this view's cache.
    private final class HistoryCache {
        var pages: [UUID] = []
        var width = 0
        var rows: [Row] = []
        var text = ""
        var lastLiveText: String?
        var accessibleText = ""

        func prepare(_ history: [HistoryChunk], width: Int, liveText: String) {
            let ids = history.map(\.cacheID)
            if ids != pages || width != self.width {
                pages = ids
                self.width = width
                rows = history.flatMap { chunk in
                    TerminalScreen.rows(chunk.lines, columns: chunk.columns,
                                        width: width, prefix: "h\(chunk.page)")
                }
                text = history.flatMap(\.lines).map { $0.map(\.text).joined() }
                    .joined(separator: "\n")
                lastLiveText = nil
            }
            if lastLiveText != liveText {
                accessibleText = text.isEmpty ? liveText : text + "\n" + liveText
                lastLiveText = liveText
            }
        }
    }

    private static func rows(_ lines: [[Run]], columns: Int, width: Int, prefix: String) -> [Row] {
        lines.enumerated().flatMap { index, line in
            wrap(line, columns: columns, limit: width).enumerated().map { part, runs in
                Row(id: "\(prefix)-\(index).\(part)", runs: runs, columns: min(columns, width))
            }
        }
    }

    // Splits a row wider than the phone into rows of `limit` cells, the way a
    // terminal reflows, dropping trailing blank cells first.
    static func wrap(_ line: [Run], columns: Int, limit: Int) -> [[Run]] {
        guard columns > limit, limit > 0 else { return [line] }
        var runs = line
        while let last = runs.last, last.background == nil, last.flags & Run.cursor == 0,
              last.text.allSatisfy({ $0 == " " }) {
            runs.removeLast()
        }
        var rows: [[Run]] = [[]]
        var next = 0
        for run in runs {
            var column = run.column ?? next
            let cells = run.width ?? run.text.count
            next = column + cells
            var characters = Array(run.text)
            // Wide characters: keep the run whole on the row it starts in.
            guard characters.count == cells else {
                place(run, text: run.text, column: column, width: cells, limit: limit, into: &rows)
                continue
            }
            while !characters.isEmpty {
                let room = limit - column % limit
                let piece = String(characters.prefix(room))
                place(run, text: piece, column: column, width: piece.count, limit: limit, into: &rows)
                characters.removeFirst(min(room, characters.count))
                column += room
            }
        }
        return rows.isEmpty ? [[]] : rows
    }

    private static func place(_ run: Run, text: String, column: Int, width: Int, limit: Int,
                              into rows: inout [[Run]]) {
        let row = column / limit
        while rows.count <= row { rows.append([]) }
        rows[row].append(Run(text: text, foreground: run.foreground, background: run.background,
                             flags: run.flags, column: column % limit, width: width))
    }

    private var contentWidth: CGFloat { CGFloat(fitColumns) * metrics.cellWidth + 8 }

    var body: some View {
        let _ = cache.prepare(history, width: fitColumns, liveText: frame?.text ?? "")
        let liveRows = frame.map {
            Self.rows($0.lines, columns: $0.columns, width: fitColumns, prefix: "live")
        } ?? []
        let background = frame.flatMap { Color(hex: $0.background) } ?? Theme.background
        let foreground = frame.flatMap { Color(hex: $0.foreground) } ?? .white
        // A full-screen program that takes the wheel shows only its screen:
        // a drag scrolls the program (AgentView), not the archive above it.
        let fullScreen = frame?.wheel == true
        ScrollView(.vertical) {
            LazyVStack(alignment: .leading, spacing: 0) {
                if frame != nil && !fullScreen {
                    historyEdge
                }
                if !fullScreen {
                    ForEach(cache.rows) { row in
                        TerminalRow(runs: row.runs, columns: row.columns, metrics: metrics,
                                    foreground: foreground, background: background)
                    }
                }
                ForEach(liveRows) { row in
                    TerminalRow(runs: row.runs, columns: row.columns, metrics: metrics,
                                foreground: foreground, background: background)
                }
            }
            .padding(.horizontal, 4)
            .frame(width: contentWidth, alignment: .leading)
        }
        .modifier(FollowsBottom())
        .modifier(LoadsNearTop(nearTop: $nearTop, loadOlder: loadOlder))
        .onChange(of: frame?.revision) {
            // Output may have archived rows while the top is in view.
            if nearTop && history.isEmpty { Task { await loadOlder() } }
        }
        .scrollIndicators(.hidden)
        .scrollDisabled(fullScreen)
        .background(background)
        .accessibilityElement(children: .ignore)
        .accessibilityIdentifier("terminal")
        .accessibilityLabel("Agent screen")
        .accessibilityValue(cache.accessibleText)
    }

    // Shown above the oldest loaded row: "Earlier output" while more can load.
    @ViewBuilder private var historyEdge: some View {
        if historyEnd {
            Text("Start of history")
                .font(.caption2.monospaced())
                .foregroundStyle(Theme.quiet)
                .frame(height: 24)
        } else {
            // Only while a page loads; an empty archive shows nothing.
            HStack(spacing: 6) {
                if loadingHistory {
                    ProgressView().controlSize(.mini)
                    Text("Earlier output")
                        .font(.caption2.monospaced())
                        .foregroundStyle(Theme.quiet)
                }
            }
            .frame(height: 24, alignment: .leading)
            .modifier(LoadsOnAppear(key: history.count, loadOlder: loadOlder))
        }
    }
}

// Within a screen of the top, the page before loads; each page moves the
// top away again (the bottom stays put), so history loads as it is read.
private struct LoadsNearTop: ViewModifier {
    @Binding var nearTop: Bool
    let loadOlder: () async -> Void

    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.onScrollGeometryChange(for: Bool.self) { geometry in
                geometry.contentOffset.y + geometry.contentInsets.top < max(geometry.containerSize.height, 200)
            } action: { _, near in
                nearTop = near
                if near { Task { await loadOlder() } }
            }
        } else {
            content
        }
    }
}

// Before iOS 18 there is no scroll geometry; the edge loads when it appears.
private struct LoadsOnAppear: ViewModifier {
    let key: Int
    let loadOlder: () async -> Void

    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content
        } else {
            content.task(id: key) { await loadOlder() }
        }
    }
}

// New output and newly loaded history keep the bottom of the screen in place.
private struct FollowsBottom: ViewModifier {
    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content
                .defaultScrollAnchor(.bottom, for: .initialOffset)
                .defaultScrollAnchor(.bottom, for: .sizeChanges)
        } else {
            content.defaultScrollAnchor(.bottom)
        }
    }
}
