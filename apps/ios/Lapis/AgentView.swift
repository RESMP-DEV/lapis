import SwiftUI
import UIKit

struct AgentView: View {
    @State private var session: AgentSession
    @State private var draft = ""
    @State private var keyboardShown = false
    @FocusState private var composing: Bool
    @AppStorage("terminalFontSize") private var fontSize = 12.0
    @Environment(\.scenePhase) private var scenePhase

    init(agent: Agent, gateway: Gateway?) {
        _session = State(initialValue: AgentSession(agent: agent, gateway: gateway))
    }

    private var metrics: TerminalMetrics { TerminalMetrics(fontSize: fontSize) }

    var body: some View {
        VStack(spacing: 0) {
            GeometryReader { proxy in
                TerminalScreen(frame: session.frame, history: session.history,
                               historyEnd: session.historyEnd, metrics: metrics) {
                    await session.loadOlder()
                }
                    .onAppear { fit(proxy.size) }
                    .onChange(of: proxy.size) { _, size in fit(size) }
                    .onChange(of: fontSize) { _, _ in fit(proxy.size, force: true) }
                    .onTapGesture { composing = true }
            }
            banner
            // The Mac's terminal encodes named keys for the agent's current modes.
            KeyBar { input in session.send(input) }
            .disabled(!session.isLive)
            composer
        }
        .background(Theme.background)
        .navigationTitle(session.agent.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button("Larger text", systemImage: "textformat.size.larger") {
                        fontSize = min(fontSize + 1, 20)
                    }
                    Button("Smaller text", systemImage: "textformat.size.smaller") {
                        fontSize = max(fontSize - 1, 8)
                    }
                    Button("Send screen to Mac", systemImage: "camera.viewfinder") {
                        Task { await sendScreen() }
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityIdentifier("viewMenu")
            }
        }
        .onDisappear { session.close() }
        .onChange(of: scenePhase) { _, phase in
            // Leave the agent while in the background; pick it up again on return.
            if phase == .background {
                session.close()
            } else if phase == .active, let size = session.size {
                if case .closed(_, reopen: false) = session.state { return }
                session.open(columns: size.columns, rows: size.rows)
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { _ in
            keyboardShown = true
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in
            keyboardShown = false
        }
        .alert("lapis", isPresented: Binding(
            get: { session.notice != nil }, set: { if !$0 { session.notice = nil } })
        ) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(session.notice ?? "")
        }
    }

    // Saves a screenshot and the exact screen data on the Mac, where a
    // rendering problem can be inspected (runtime/phone-captures).
    private func sendScreen() async {
        try? await Task.sleep(for: .milliseconds(450))  // let the menu close
        guard let gateway = session.gateway,
              let window = UIApplication.shared.connectedScenes
                  .compactMap({ $0 as? UIWindowScene }).flatMap(\.windows).first(where: \.isKeyWindow)
        else { return }
        let image = UIGraphicsImageRenderer(bounds: window.bounds).image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: false)
        }
        guard let png = image.pngData() else { return }
        var body: [String: Any] = [
            "png": png.base64EncodedString(),
            "agent": session.agent.id,
            "harness": session.agent.harness,
            "fontSize": fontSize,
            "cellWidth": metrics.cellWidth,
            "lineHeight": metrics.lineHeight,
            "scale": window.screen.scale,
            "screen": [window.bounds.width, window.bounds.height],
            "historyPages": session.history.count,
        ]
        if let json = session.lastFrameJSON, let frame = try? JSONSerialization.jsonObject(with: json) {
            body["frame"] = frame
        }
        do {
            let name = try await gateway.capture(body)
            session.notice = "Sent to the Mac as \(name)."
        } catch {
            session.notice = describe(error)
        }
    }

    // The keyboard only covers the screen; the agent keeps its size. A new
    // width, or a new height with the keyboard down, resizes the terminal.
    private func fit(_ size: CGSize, force: Bool = false) {
        guard size.width > 0, size.height > 0 else { return }
        let grid = metrics.grid(for: size)
        guard let current = session.size else {
            session.open(columns: grid.columns, rows: grid.rows)
            return
        }
        // Focus comes before the keyboard's notification, so either one means
        // the keyboard is (about to be) up and the height change is not real.
        let keyboard = keyboardShown || composing
        if force || grid.columns != current.columns || (!keyboard && grid.rows != current.rows) {
            session.resize(columns: grid.columns, rows: keyboard ? current.rows : grid.rows)
        }
    }

    @ViewBuilder private var banner: some View {
        switch session.state {
        case .connecting:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Opening \(session.agent.title)…")
            }
            .font(.footnote)
            .foregroundStyle(Theme.quiet)
            .frame(maxWidth: .infinity)
            .padding(8)
            .background(Theme.panel)
        case let .closed(reason, _):
            VStack(spacing: 8) {
                Text(reason)
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .accessibilityIdentifier("closedReason")
                if let size = session.size {
                    Button("Open here again") { session.open(columns: size.columns, rows: size.rows) }
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("reopen")
                }
            }
            .frame(maxWidth: .infinity)
            .padding(10)
            .background(Theme.panel)
        case .live:
            if !session.shared {
                Text("This agent started before sync, so the Mac shows it as taken over. Restarted agents stay in sync.")
                    .font(.caption)
                    .foregroundStyle(Theme.quiet)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: .infinity)
                    .padding(6)
                    .background(Theme.panel)
                    .accessibilityIdentifier("syncNotice")
            }
        }
    }

    private var composer: some View {
        HStack(alignment: .bottom, spacing: 8) {
            TextField("Message", text: $draft, axis: .vertical)
                .lineLimit(1...6)
                .focused($composing)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Theme.panel, in: RoundedRectangle(cornerRadius: 18))
                .overlay(RoundedRectangle(cornerRadius: 18).stroke(Theme.edge))
                .accessibilityIdentifier("composer")
            Button {
                session.send(Input(text: draft))
                draft = ""
            } label: {
                Image(systemName: "keyboard")
                    .frame(width: 36, height: 36)
            }
            .disabled(draft.isEmpty || !session.isLive)
            .accessibilityLabel("Type without Enter")
            .accessibilityIdentifier("type")
            Button {
                session.submit(draft)
                draft = ""
            } label: {
                Image(systemName: "arrow.up.circle.fill")
                    .font(.system(size: 30))
            }
            .disabled(!session.isLive)
            .accessibilityLabel("Send")
            .accessibilityIdentifier("send")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Theme.background)
    }
}

struct KeyBar: View {
    let send: (Input) -> Void

    private struct Item: Identifiable {
        let id: String
        let label: String
        let input: Input
    }

    private var items: [Item] {
        [
            Item(id: "esc", label: "esc", input: .key(.escape)),
            Item(id: "ctrl-c", label: "^C", input: Input(text: "\u{03}")),
            Item(id: "up", label: "↑", input: .key(.up)),
            Item(id: "down", label: "↓", input: .key(.down)),
            Item(id: "left", label: "←", input: .key(.left)),
            Item(id: "right", label: "→", input: .key(.right)),
            Item(id: "enter", label: "⏎", input: .key(.enter)),
            Item(id: "backspace", label: "⌫", input: .key(.backspace)),
            Item(id: "tab", label: "tab", input: .key(.tab)),
            Item(id: "shift-tab", label: "⇧tab", input: .key(.tab, shift: true)),
            Item(id: "ctrl-u", label: "^U", input: Input(text: "\u{15}")),
            Item(id: "ctrl-d", label: "^D", input: Input(text: "\u{04}")),
        ]
    }

    var body: some View {
        ScrollView(.horizontal) {
            HStack(spacing: 5) {
                ForEach(items) { item in
                    Button {
                        send(item.input)
                    } label: {
                        Text(item.label)
                            .font(.system(size: 14, weight: .medium, design: .monospaced))
                            .frame(minWidth: 32, minHeight: 34)
                            .padding(.horizontal, 3)
                            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 7))
                            .overlay(RoundedRectangle(cornerRadius: 7).stroke(Theme.edge))
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("key-\(item.id)")
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
        }
        .scrollIndicators(.hidden)
        .background(Theme.background)
        .accessibilityIdentifier("keybar")
    }
}
