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

    var body: some View {
        Canvas { context, size in
            var next = 0
            for run in runs {
                let column = run.column ?? next
                let cells = run.width ?? run.text.count
                next = column + cells
                let x = CGFloat(column) * metrics.cellWidth
                var ink = run.foreground.flatMap(Color.init(hex:)) ?? foreground
                var paper = run.background.flatMap(Color.init(hex:))
                if run.flags & Run.cursor != 0 {
                    (ink, paper) = (paper ?? background, ink)
                }
                if let paper {
                    // Overlap by a hair so neighbouring runs never show a gap.
                    let rect = CGRect(x: x, y: 0, width: CGFloat(cells) * metrics.cellWidth + 0.5,
                                      height: size.height)
                    context.fill(Path(rect), with: .color(paper))
                }
                if run.text.allSatisfy({ $0 == " " }) { continue }
                if run.flags & Run.faint != 0 { ink = ink.opacity(0.55) }
                var text = Text(run.text)
                    .font(.system(size: metrics.fontSize, weight: run.flags & Run.bold != 0 ? .bold : .regular,
                                  design: .monospaced))
                    .foregroundStyle(ink)
                if run.flags & Run.italic != 0 { text = text.italic() }
                if run.flags & Run.underline != 0 { text = text.underline() }
                if run.flags & Run.strike != 0 { text = text.strikethrough() }
                context.draw(text, at: CGPoint(x: x, y: size.height / 2), anchor: .leading)
            }
        }
        .frame(width: CGFloat(columns) * metrics.cellWidth, height: metrics.lineHeight)
    }
}

struct TerminalScreen: View {
    let frame: ScreenFrame?
    let history: [HistoryChunk]
    let historyEnd: Bool
    let metrics: TerminalMetrics
    let loadOlder: () async -> Void
    @State private var nearTop = false

    private struct Row: Identifiable {
        let id: String
        let runs: [Run]
        let columns: Int
    }

    private var rows: [Row] {
        var rows: [Row] = []
        for chunk in history {
            for (index, line) in chunk.lines.enumerated() {
                rows.append(Row(id: "h\(chunk.page)-\(index)", runs: line, columns: chunk.columns))
            }
        }
        if let frame {
            for (index, line) in frame.lines.enumerated() {
                rows.append(Row(id: "live-\(index)", runs: line, columns: frame.columns))
            }
        }
        return rows
    }

    // Rows are drawn lazily, so the stack is sized to the widest row
    // (history may be wider than the live screen) and pinned to the left.
    private var contentWidth: CGFloat {
        let columns = max(frame?.columns ?? 0, history.map(\.columns).max() ?? 0)
        return CGFloat(columns) * metrics.cellWidth + 8
    }

    private var accessibleText: String {
        let earlier = history.flatMap(\.lines).map { $0.map(\.text).joined() }
        return (earlier + [frame?.text ?? ""]).joined(separator: "\n")
    }

    var body: some View {
        let background = frame.flatMap { Color(hex: $0.background) } ?? Theme.background
        let foreground = frame.flatMap { Color(hex: $0.foreground) } ?? .white
        ScrollView([.vertical, .horizontal]) {
            LazyVStack(alignment: .leading, spacing: 0) {
                if frame != nil {
                    historyEdge
                }
                ForEach(rows) { row in
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
        .background(background)
        .accessibilityElement(children: .ignore)
        .accessibilityIdentifier("terminal")
        .accessibilityLabel("Agent screen")
        .accessibilityValue(accessibleText)
    }

    // Shown above the oldest loaded row: "Earlier output" while more can load.
    @ViewBuilder private var historyEdge: some View {
        if historyEnd {
            Text("Start of history")
                .font(.caption2.monospaced())
                .foregroundStyle(Theme.quiet)
                .frame(height: 24)
        } else {
            HStack(spacing: 6) {
                ProgressView().controlSize(.mini)
                Text("Earlier output")
                    .font(.caption2.monospaced())
                    .foregroundStyle(Theme.quiet)
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
                .defaultScrollAnchor(.bottomLeading, for: .initialOffset)
                .defaultScrollAnchor(.bottomLeading, for: .sizeChanges)
        } else {
            content.defaultScrollAnchor(.bottomLeading)
        }
    }
}
