import SwiftUI

// Starting an agent from the phone: the Mac's lapis starts it as a new tab in
// the chosen category, and the phone opens it once it runs.
struct NewAgentView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let categories: [AgentCategory]
    let onStarted: (Agent) -> Void
    @State private var category: String
    @State private var harnesses: [Harness]?
    @State private var harness = ""
    @State private var folder = ""
    @State private var progress: String?
    @State private var failure: String?

    init(categories: [AgentCategory], category: String, onStarted: @escaping (Agent) -> Void) {
        self.categories = categories
        self.onStarted = onStarted
        _category = State(initialValue: category)
    }

    private var installed: [Harness] { (harnesses ?? []).filter(\.installed) }
    private var directory: String { folder.trimmingCharacters(in: .whitespacesAndNewlines) }
    private var canStart: Bool {
        !harness.isEmpty && !directory.isEmpty && !category.isEmpty && progress == nil
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    section("Agent") { harnessGrid }
                    section("Folder") { folderField }
                    section("Category") { categoryChips }
                    if let failure {
                        Text(failure)
                            .font(.footnote)
                            .foregroundStyle(.orange)
                            .accessibilityIdentifier("newAgentError")
                    }
                    startButton
                }
                .padding(16)
            }
            .scrollDismissesKeyboard(.interactively)
            .background(Theme.background.ignoresSafeArea())
            .navigationTitle("New agent")
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

    private func section(_ title: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                .textCase(.uppercase)
                .tracking(1.6)
                .foregroundStyle(Theme.quiet)
            content()
        }
    }

    @ViewBuilder private var harnessGrid: some View {
        if harnesses == nil && failure == nil {
            ProgressView().tint(Theme.quiet)
        } else if installed.isEmpty {
            Text("No agent CLIs were found on the Mac.")
                .font(.footnote)
                .foregroundStyle(Theme.quiet)
        } else {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 92), spacing: 10)], spacing: 10) {
                ForEach(installed) { item in
                    let chosen = item.id == harness
                    let accent = Theme.harness(item.id)
                    Button {
                        harness = item.id
                    } label: {
                        VStack(spacing: 8) {
                            HarnessBadge(harness: item.id, accent: accent)
                            Text(item.name)
                                .font(.system(size: 12, weight: .semibold))
                                .foregroundStyle(chosen ? .white : Theme.quiet)
                                .lineLimit(1)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                        .background {
                            Chamfered(cut: 10).fill(Theme.panel)
                            Chamfered(cut: 10)
                                .stroke(chosen ? accent.opacity(0.9) : Theme.edge, lineWidth: chosen ? 1.5 : 1)
                                .shadow(color: chosen ? accent.opacity(0.4) : .clear, radius: 6)
                        }
                        .contentShape(Chamfered(cut: 10))
                    }
                    .buttonStyle(CardPress())
                    .accessibilityIdentifier("harness-\(item.id)")
                    .accessibilityAddTraits(chosen ? .isSelected : [])
                }
            }
        }
    }

    private var folderField: some View {
        VStack(alignment: .leading, spacing: 8) {
            TextField("~/dev/project", text: $folder)
                .font(.system(size: 15, design: .monospaced))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.URL)
                .submitLabel(.go)
                .onSubmit { if canStart { Task { await start() } } }
                .padding(12)
                .background {
                    Chamfered(cut: 10).fill(Theme.panel)
                    Chamfered(cut: 10).stroke(Theme.edge, lineWidth: 1)
                }
                .accessibilityIdentifier("folder")
            ForEach(model.recentFolders, id: \.self) { place in
                Button {
                    folder = place
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "folder")
                            .font(.system(size: 11))
                        Text(place)
                            .font(.system(size: 13, design: .monospaced))
                            .lineLimit(1)
                            .truncationMode(.head)
                        Spacer(minLength: 0)
                    }
                    .foregroundStyle(place == directory ? Theme.accent : Theme.quiet)
                    .padding(.vertical, 4)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("recent-\(place)")
            }
        }
    }

    private var categoryChips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(categories) { item in
                    let chosen = item.id == category
                    Button {
                        category = item.id
                    } label: {
                        Text(item.name)
                            .font(.system(size: 14, weight: chosen ? .semibold : .regular))
                            .foregroundStyle(chosen ? .white : Theme.quiet)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 8)
                            .background {
                                Chamfered(cut: 7).fill(Theme.panel)
                                Chamfered(cut: 7).stroke(chosen ? Theme.accent : Theme.edge, lineWidth: chosen ? 1.5 : 1)
                            }
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("category-\(item.name)")
                    .accessibilityAddTraits(chosen ? .isSelected : [])
                }
            }
        }
    }

    private var startButton: some View {
        Button {
            Task { await start() }
        } label: {
            HStack(spacing: 10) {
                if progress != nil {
                    ProgressView().tint(.black)
                }
                Text(progress ?? "Start")
                    .font(.system(size: 16, weight: .bold))
            }
            .foregroundStyle(canStart || progress != nil ? .black : Theme.quiet)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 14)
            .background {
                Chamfered(cut: 12).fill(canStart || progress != nil ? Theme.accent : Theme.panel)
                Chamfered(cut: 12).stroke(canStart ? Theme.accent : Theme.edge, lineWidth: 1)
            }
            .contentShape(Chamfered(cut: 12))
        }
        .buttonStyle(CardPress())
        .disabled(!canStart)
        .accessibilityIdentifier("startAgent")
    }

    private func load() async {
        if folder.isEmpty, let recent = model.recentFolders.first {
            folder = recent
        }
        guard let gateway = model.gateway else {
            failure = GatewayError.invalidHost.localizedDescription
            return
        }
        do {
            let listed = try await gateway.harnesses()
            harnesses = listed
            let ids = listed.filter(\.installed).map(\.id)
            // The CLI most recently used here, else the first one installed.
            let recent = model.listing?.categories.flatMap(\.agents).last?.harness
            if harness.isEmpty {
                harness = recent.flatMap { ids.contains($0) ? $0 : nil } ?? ids.first ?? ""
            }
        } catch {
            failure = describe(error)
        }
    }

    private func start() async {
        failure = nil
        progress = "Starting…"
        do {
            let agent = try await model.start(
                NewAgent(harness: harness, directory: directory, category: category)
            ) { message in progress = message }
            progress = nil
            onStarted(agent)
            dismiss()
        } catch {
            progress = nil
            failure = describe(error)
        }
    }
}
