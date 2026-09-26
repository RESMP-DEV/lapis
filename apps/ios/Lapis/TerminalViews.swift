import SwiftUI

// A plain shell for a quick command, never an agent: pick the machine (the
// Mac, or one of its ssh machines), and its terminal opens, started there
// when it has none.
struct TerminalPickerView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let onOpened: (Agent) -> Void
    @State private var opening: String?
    @State private var failure: String?

    private var machines: [String] { [""] + model.machines.map(\.name) }

    var body: some View {
        NavigationStack {
            List {
                ForEach(machines, id: \.self) { machine in
                    Button {
                        Task { await open(machine) }
                    } label: {
                        HStack(spacing: 12) {
                            Image(systemName: machine.isEmpty ? "desktopcomputer" : "server.rack")
                                .foregroundStyle(Theme.accent)
                                .frame(width: 24)
                            Text(machine.isEmpty ? "This Mac" : machine)
                                .font(.system(size: 17, weight: .semibold))
                                .foregroundStyle(.white)
                            Spacer()
                            if opening == machine {
                                ProgressView()
                            } else if model.terminals.contains(where: { $0.machine == machine && $0.running }) {
                                Text("OPEN")
                                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                                    .tracking(1)
                                    .foregroundStyle(Theme.live)
                            }
                        }
                        .padding(.vertical, 6)
                    }
                    .disabled(opening != nil)
                    .accessibilityIdentifier("terminal-\(machine.isEmpty ? "mac" : machine)")
                    .listRowBackground(Theme.panel)
                }
                if let failure {
                    Text(failure)
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .listRowBackground(Color.clear)
                        .accessibilityIdentifier("terminalError")
                }
            }
            .scrollContentBackground(.hidden)
            .background(Theme.background.ignoresSafeArea())
            .navigationTitle("Terminal on")
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(Theme.background, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
        .presentationBackground(Theme.background)
        .task { await model.prefetch() }
    }

    private func open(_ machine: String) async {
        opening = machine
        failure = nil
        do {
            let terminal = try await model.openTerminal(machine: machine)
            opening = nil
            onOpened(terminal)
            dismiss()
        } catch {
            opening = nil
            failure = describe(error)
        }
    }
}

// The Mac's recent Claude and Codex conversations, newest first; one tapped
// comes back as a new agent in its folder, in `category`.
struct ResumeView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let category: String
    let onStarted: (Agent) -> Void
    @State private var conversations: [Conversation]?
    @State private var query = ""
    @State private var progress: String?
    @State private var failure: String?

    private var shown: [Conversation] {
        let words = query.lowercased().split(separator: " ").map(String.init)
        return (conversations ?? []).filter { item in
            let text = (item.title + " " + item.directory + " " + item.harness).lowercased()
            return words.allSatisfy { text.contains($0) }
        }
    }

    var body: some View {
        NavigationStack {
            List {
                if let failure {
                    Text(failure)
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .listRowBackground(Color.clear)
                        .accessibilityIdentifier("resumeError")
                }
                if conversations == nil {
                    ProgressView("Reading conversations…")
                        .listRowBackground(Color.clear)
                } else if shown.isEmpty {
                    Text(query.isEmpty ? "No Claude or Codex conversations on the Mac yet" : "No conversation matches")
                        .foregroundStyle(Theme.quiet)
                        .listRowBackground(Color.clear)
                }
                ForEach(shown) { item in
                    Button {
                        Task { await resume(item) }
                    } label: {
                        HStack(alignment: .top, spacing: 12) {
                            HarnessBadge(harness: item.harness, accent: Theme.harness(item.harness), size: 32)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(item.title.isEmpty ? "(no message)" : item.title)
                                    .font(.system(size: 16, weight: .semibold))
                                    .foregroundStyle(.white)
                                    .lineLimit(2)
                                Text(place(item.directory, on: ""))
                                    .font(.system(size: 12.5, design: .monospaced))
                                    .foregroundStyle(Theme.quiet)
                                    .lineLimit(1)
                                    .truncationMode(.head)
                            }
                            Spacer(minLength: 6)
                            Text(ResumeView.age(item.age))
                                .font(.system(size: 12, design: .monospaced))
                                .foregroundStyle(Theme.quiet)
                        }
                        .padding(.vertical, 4)
                    }
                    .disabled(progress != nil)
                    .accessibilityIdentifier("conversation-\(item.id)")
                    .listRowBackground(Theme.panel)
                }
            }
            .scrollContentBackground(.hidden)
            .searchable(text: $query, prompt: "Title, folder or CLI")
            .background(Theme.background.ignoresSafeArea())
            .navigationTitle(progress ?? "Resume")
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(Theme.background, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                        .disabled(progress != nil)
                }
            }
        }
        .presentationBackground(Theme.background)
        .interactiveDismissDisabled(progress != nil)
        .task { await load() }
    }

    private func load() async {
        guard let gateway = model.gateway else { return }
        do {
            conversations = try await gateway.conversations()
        } catch {
            conversations = []
            failure = describe(error)
        }
    }

    private func resume(_ item: Conversation) async {
        failure = nil
        progress = "Starting…"
        do {
            let agent = try await model.resume(item, category: category) { message in progress = message }
            progress = nil
            onStarted(agent)
            dismiss()
        } catch {
            progress = nil
            failure = describe(error)
        }
    }

    // "now", "5 min", "3 h", "yesterday", "4 d", else weeks.
    static func age(_ seconds: Int) -> String {
        switch seconds {
        case ..<60: "now"
        case ..<3600: "\(seconds / 60) min"
        case ..<86_400: "\(seconds / 3600) h"
        case ..<172_800: "yesterday"
        case ..<604_800: "\(seconds / 86_400) d"
        default: "\(seconds / 604_800) wk"
        }
    }
}
