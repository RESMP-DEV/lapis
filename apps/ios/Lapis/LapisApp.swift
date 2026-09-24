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
    static let panel = Color(white: 0.07)
    static let edge = Color(white: 0.18)
    static let quiet = Color(white: 0.55)
    static let live = Color(red: 0.30, green: 0.85, blue: 0.55)
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
