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

struct TerminalScreen: View {
    let frame: ScreenFrame?
    let metrics: TerminalMetrics

    var body: some View {
        let background = frame.flatMap { Color(hex: $0.background) } ?? Theme.background
        ScrollView([.vertical, .horizontal]) {
            VStack(alignment: .leading, spacing: 0) {
                if let frame {
                    let foreground = Color(hex: frame.foreground) ?? .white
                    let fallback = Color(hex: frame.background) ?? .black
                    ForEach(frame.lines.indices, id: \.self) { index in
                        Text(line(frame.lines[index], foreground: foreground, background: fallback))
                            .lineLimit(1)
                            .fixedSize()
                            .frame(height: metrics.lineHeight, alignment: .leading)
                    }
                }
            }
            .padding(.horizontal, 4)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .defaultScrollAnchor(.bottomLeading)
        .scrollIndicators(.hidden)
        .background(background)
        .accessibilityElement(children: .ignore)
        .accessibilityIdentifier("terminal")
        .accessibilityLabel("Agent screen")
        .accessibilityValue(frame?.text ?? "")
    }

    private func line(_ runs: [Run], foreground: Color, background: Color) -> AttributedString {
        var result = AttributedString()
        for run in runs {
            var part = AttributedString(run.text)
            var ink = run.foreground.flatMap(Color.init(hex:)) ?? foreground
            var paper = run.background.flatMap(Color.init(hex:))
            if run.flags & Run.cursor != 0 {
                (ink, paper) = (paper ?? background, ink)
            }
            if run.flags & Run.faint != 0 { ink = ink.opacity(0.55) }
            part.foregroundColor = ink
            if let paper { part.backgroundColor = paper }
            var font = Font.system(
                size: metrics.fontSize, weight: run.flags & Run.bold != 0 ? .bold : .regular,
                design: .monospaced)
            if run.flags & Run.italic != 0 { font = font.italic() }
            part.font = font
            if run.flags & Run.underline != 0 { part.underlineStyle = .single }
            if run.flags & Run.strike != 0 { part.strikethroughStyle = .single }
            result += part
        }
        return result
    }
}
