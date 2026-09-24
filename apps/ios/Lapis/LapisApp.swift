import SwiftUI

@main
struct LapisApp: App {
    @State private var model = WorkspaceModel()

    var body: some Scene {
        WindowGroup {
            AgentListView()
                .environment(model)
                .preferredColorScheme(.dark)
                .tint(Theme.accent)
        }
    }
}

enum Theme {
    static let accent = Color(red: 0.36, green: 0.52, blue: 1.0)
    static let background = Color.black
    static let panel = Color(red: 0.055, green: 0.067, blue: 0.094)
    static let panelDeep = Color(red: 0.027, green: 0.031, blue: 0.047)
    static let edge = Color(white: 0.18)
    static let quiet = Color(white: 0.55)
    static let live = Color(red: 0.30, green: 0.85, blue: 0.55)

    // Each harness's mark and card edge colour.
    static func harness(_ id: String) -> Color {
        switch id {
        case "claude": Color(red: 0.85, green: 0.47, blue: 0.34)
        case "kimi": Color(red: 0.40, green: 0.60, blue: 1.0)
        case "omp": Color(red: 0.55, green: 0.85, blue: 0.65)
        case "agy": Color(red: 0.30, green: 0.55, blue: 1.0)
        case "codex", "grok", "opencode": Color(white: 0.9)
        default: accent
        }
    }
}

extension Color {
    init?(hex: String) {
        guard hex.count == 7, hex.hasPrefix("#"), let value = UInt32(hex.dropFirst(), radix: 16) else {
            return nil
        }
        self.init(
            red: Double((value >> 16) & 0xFF) / 255,
            green: Double((value >> 8) & 0xFF) / 255,
            blue: Double(value & 0xFF) / 255)
    }
}
