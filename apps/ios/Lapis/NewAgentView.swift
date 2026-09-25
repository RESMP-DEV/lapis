import SwiftUI

// Starting an agent from the phone: on the Mac or over ssh on another machine,
// in a folder picked by browsing or fuzzy search, as a new tab in a category
// in lapis on the Mac. What it shows was fetched ahead, so nothing here waits
// on the network until Start.
struct NewAgentView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let categories: [AgentCategory]
    let onStarted: (Agent) -> Void
    @State private var category: String
    @State private var machine = "" // "" is this Mac
    @State private var harness = ""
    // Relative to the machine's home ("" is home), or absolute.
    @State private var folder = ""
    @State private var query = ""
    @State private var results: [String] = []
    // The last query's matches in one catalog version, to narrow the next.
    @State private var narrowing: (version: String, query: String, all: [Int])?
    @State private var progress: String?
    @State private var failure: String?
    @FocusState private var finding: Bool

    // While finding a folder the sheet is only the search and its results,
    // clear of the keyboard; choosing one brings the rest back.
    private var searching: Bool { finding || !query.isEmpty }

    init(categories: [AgentCategory], category: String, onStarted: @escaping (Agent) -> Void) {
        self.categories = categories
        self.onStarted = onStarted
        _category = State(initialValue: category)
    }

    private var catalog: FolderCatalog? { model.catalogs[machine] }
    private var offered: [Harness] {
        let known = model.harnesses ?? []
        if machine.isEmpty { return known.filter(\.installed) }
        guard let found = catalog?.harnesses else { return [] }
        return known.filter { found[$0.id] != nil }
    }
    private var canStart: Bool { !harness.isEmpty && !category.isEmpty && progress == nil }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    if !searching {
                        if !model.machines.isEmpty {
                            section("Machine") { machineChips }
                        }
                        section("Agent") { harnessGrid }
                        section("Category") { categoryChips }
                    }
                    section("Folder") { folderPicker }
                    if let failure {
                        Text(failure)
                            .font(.footnote)
                            .foregroundStyle(.orange)
                            .accessibilityIdentifier("newAgentError")
                    }
                }
                .padding(16)
            }
            .scrollDismissesKeyboard(.interactively)
            .safeAreaInset(edge: .bottom) {
                if !searching {
                    startButton
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .background(Theme.background)
                }
            }
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
        .task { await prepare() }
        .task(id: query) { await search() }
        .onChange(of: model.harnesses) { _, _ in pickHarness() }
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

    // MARK: Machine, agent and category

    private var machineChips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                chip("This Mac", chosen: machine.isEmpty, light: true, id: "machine-mac") {
                    choose(machine: "")
                }
                ForEach(model.machines) { item in
                    chip(item.name, chosen: machine == item.name, light: item.available,
                         id: "machine-\(item.name)") {
                        choose(machine: item.name)
                    }
                }
            }
        }
    }

    private func chip(_ title: String, chosen: Bool, light: Bool? = nil, id: String,
                      action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 7) {
                if let light {
                    Circle()
                        .fill(light ? Theme.live : Theme.quiet.opacity(0.4))
                        .frame(width: 6, height: 6)
                }
                Text(title)
                    .font(.system(size: 14, weight: chosen ? .semibold : .regular))
                    .foregroundStyle(chosen ? .white : Theme.quiet)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 8)
            .background {
                Chamfered(cut: 7).fill(Theme.panel)
                Chamfered(cut: 7).stroke(chosen ? Theme.accent : Theme.edge, lineWidth: chosen ? 1.5 : 1)
            }
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(id)
        .accessibilityAddTraits(chosen ? .isSelected : [])
    }

    @ViewBuilder private var harnessGrid: some View {
        if model.harnesses == nil || (!machine.isEmpty && catalog == nil) {
            if let error = model.catalogErrors[machine], !machine.isEmpty {
                Text(error).font(.footnote).foregroundStyle(.orange)
            } else {
                HStack(spacing: 8) {
                    ProgressView().tint(Theme.quiet)
                    Text(machine.isEmpty ? "Asking the Mac…" : "Reading \(machine)…")
                        .font(.footnote)
                        .foregroundStyle(Theme.quiet)
                }
            }
        } else if offered.isEmpty {
            Text(machine.isEmpty ? "No agent CLIs were found on the Mac."
                                 : "No agent CLIs were found on \(machine).")
                .font(.footnote)
                .foregroundStyle(Theme.quiet)
        } else {
            // One scrolling row, so the folder picker stays in view.
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 10) {
                    ForEach(offered) { item in
                        let chosen = item.id == harness
                        let accent = Theme.harness(item.id)
                        Button {
                            harness = item.id
                        } label: {
                            VStack(spacing: 7) {
                                HarnessBadge(harness: item.id, accent: accent)
                                Text(item.name)
                                    .font(.system(size: 11, weight: .semibold))
                                    .foregroundStyle(chosen ? .white : Theme.quiet)
                                    .lineLimit(1)
                            }
                            .frame(width: 84)
                            .padding(.vertical, 10)
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
                .padding(.vertical, 6)
            }
        }
    }

    private var categoryChips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(categories) { item in
                    chip(item.name, chosen: item.id == category, id: "category-\(item.name)") {
                        category = item.id
                    }
                }
            }
        }
    }

    // MARK: Folder

    // "~/dev/lapis" here, "devbox:~/dev/lapis" on another machine.
    private func place(_ path: String) -> String {
        let shown = path.isEmpty ? "~" : path.hasPrefix("/") ? path : "~/" + path
        return machine.isEmpty ? shown : machine + ":" + shown
    }

    private func parent(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        let up = String(path[..<slash])
        return up.isEmpty && path.hasPrefix("/") ? "/" : up
    }

    @ViewBuilder private var folderPicker: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 13))
                    .foregroundStyle(Theme.quiet)
                TextField("Find a folder", text: $query)
                    .font(.system(size: 15, design: .monospaced))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                    .submitLabel(.search)
                    .focused($finding)
                    .accessibilityIdentifier("folderSearch")
                if searching {
                    Button("Done") {
                        query = ""
                        finding = false
                    }
                    .font(.system(size: 14))
                    .accessibilityIdentifier("folderSearchDone")
                }
            }
            .padding(12)
            .background {
                Chamfered(cut: 10).fill(Theme.panel)
                Chamfered(cut: 10).stroke(Theme.edge, lineWidth: 1)
            }
            if query.isEmpty {
                currentFolder
                frequentFolders
                browser
            } else {
                searchResults
            }
        }
    }

    private var currentFolder: some View {
        HStack(spacing: 10) {
            Button {
                folder = parent(folder)
            } label: {
                Image(systemName: "arrow.up")
                    .font(.system(size: 13, weight: .semibold))
                    .frame(width: 32, height: 32)
                    .background(Chamfered(cut: 6).fill(Theme.panel))
            }
            .buttonStyle(.plain)
            .foregroundStyle(folder.isEmpty || folder == "/" ? Theme.quiet.opacity(0.4) : .white)
            .disabled(folder.isEmpty || folder == "/")
            .accessibilityLabel("Up one folder")
            .accessibilityIdentifier("folderUp")
            Text(place(folder))
                .font(.system(size: 14, weight: .semibold, design: .monospaced))
                .foregroundStyle(Theme.accent)
                .lineLimit(1)
                .truncationMode(.head)
                .accessibilityIdentifier("currentFolder")
            Spacer(minLength: 0)
        }
        .padding(.top, 4)
    }

    // Where agents were started most, marked out above the folder listing.
    @ViewBuilder private var frequentFolders: some View {
        if let frequent = catalog?.frequent, !frequent.isEmpty {
            ForEach(frequent.prefix(6), id: \.path) { item in
                let chosen = item.path == folder
                Button {
                    folder = item.path
                } label: {
                    HStack(spacing: 10) {
                        Image(systemName: "star.fill")
                            .font(.system(size: 10))
                            .foregroundStyle(Theme.accent)
                        Text(place(item.path))
                            .font(.system(size: 13, design: .monospaced))
                            .foregroundStyle(.white)
                            .lineLimit(1)
                            .truncationMode(.head)
                        Spacer(minLength: 4)
                        Text("\(item.count)")
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .foregroundStyle(Theme.accent)
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)
                    .background {
                        Chamfered(cut: 7).fill(Theme.accent.opacity(chosen ? 0.22 : 0.1))
                        Chamfered(cut: 7).stroke(Theme.accent.opacity(chosen ? 0.9 : 0.35), lineWidth: 1)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("frequent-\(item.path)")
                .accessibilityAddTraits(chosen ? .isSelected : [])
            }
        }
    }

    // The current folder's folders: visible ones alphabetically, then hidden.
    @ViewBuilder private var browser: some View {
        if let catalog {
            ForEach(catalog.children(of: folder), id: \.self) { child in
                folderRow(child, catalog: catalog, id: "folder-\(child)")
            }
        } else if let error = model.catalogErrors[machine] {
            Text(error).font(.footnote).foregroundStyle(.orange)
        } else {
            ProgressView().tint(Theme.quiet)
        }
    }

    private func folderRow(_ path: String, catalog: FolderCatalog, id: String, full: Bool = false) -> some View {
        let name = FolderCatalog.name(path)
        let hidden = name.hasPrefix(".")
        return Button {
            folder = path
            query = ""
            finding = false
        } label: {
            HStack(spacing: 10) {
                Image(systemName: "folder")
                    .font(.system(size: 12))
                    .foregroundStyle(hidden ? Theme.quiet.opacity(0.6) : Theme.quiet)
                if full {
                    (Text(name).foregroundStyle(.white)
                        + Text("  " + place(parent(path))).foregroundStyle(Theme.quiet))
                        .font(.system(size: 13, design: .monospaced))
                        .lineLimit(1)
                        .truncationMode(.middle)
                } else {
                    Text(name)
                        .font(.system(size: 14, design: .monospaced))
                        .foregroundStyle(hidden ? Theme.quiet.opacity(0.7) : .white)
                        .lineLimit(1)
                }
                Spacer(minLength: 4)
                if catalog.hasChildren(path) {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Theme.quiet.opacity(0.6))
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 9)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(id)
    }

    @ViewBuilder private var searchResults: some View {
        let typed = query.trimmingCharacters(in: .whitespaces)
        if typed.hasPrefix("/") || typed.hasPrefix("~") {
            Button {
                folder = typed == "~" ? "" : typed.hasPrefix("~/") ? String(typed.dropFirst(2)) : typed
                query = ""
                finding = false
            } label: {
                Label("Use \(machine.isEmpty ? "" : machine + ":")\(typed)", systemImage: "return")
                    .font(.system(size: 13, design: .monospaced))
                    .foregroundStyle(Theme.accent)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("useTyped")
        }
        if let catalog {
            ForEach(results, id: \.self) { path in
                folderRow(path, catalog: catalog, id: "result-\(path)", full: true)
            }
        }
    }

    // MARK: Start

    private var startButton: some View {
        Button {
            Task { await start() }
        } label: {
            HStack(spacing: 10) {
                if progress != nil {
                    ProgressView().tint(.black)
                }
                Text(progress ?? "Start in \(place(folder))")
                    .font(.system(size: 16, weight: .bold))
                    .lineLimit(1)
                    .truncationMode(.head)
            }
            .foregroundStyle(canStart || progress != nil ? .black : Theme.quiet)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 14)
            .padding(.horizontal, 12)
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

    // MARK: Behaviour

    private func prepare() async {
        pickHarness()
        folder = catalog?.frequent.first?.path ?? ""
        if model.harnesses == nil || catalog == nil {
            await model.prefetch()
            pickHarness()
            if folder.isEmpty { folder = catalog?.frequent.first?.path ?? "" }
        }
    }

    // The CLI last used on this machine, else the first one there.
    private func pickHarness() {
        let ids = offered.map(\.id)
        guard !ids.contains(harness) else { return }
        let recent = (model.listing?.categories.flatMap(\.agents) ?? [])
            .last { ($0.machine ?? "") == machine }?.harness
        harness = recent.flatMap { ids.contains($0) ? $0 : nil } ?? ids.first ?? ""
    }

    private func choose(machine name: String) {
        guard name != machine else { return }
        machine = name
        query = ""
        narrowing = nil
        results = []
        harness = ""
        pickHarness()
        folder = catalog?.frequent.first?.path ?? ""
        if catalog == nil {
            Task {
                await model.loadCatalog(name)
                guard machine == name else { return }
                pickHarness()
                folder = catalog?.frequent.first?.path ?? ""
            }
        }
    }

    // Local and quick: each keystroke narrows the last letters' matches.
    private func search() async {
        let typed = query.trimmingCharacters(in: .whitespaces)
        guard let catalog, !typed.isEmpty else {
            results = []
            narrowing = nil
            return
        }
        let among = narrowing.flatMap {
            $0.version == catalog.version && typed.hasPrefix($0.query) ? $0.all : nil
        }
        let found = await Task.detached(priority: .userInitiated) {
            let found = catalog.search(typed, among: among)
            return (all: found.all, best: found.best.map { catalog.paths[$0] })
        }.value
        guard !Task.isCancelled else { return }
        results = found.best
        narrowing = (catalog.version, typed, found.all)
    }

    // What the Mac is sent: ~ for home, as the desktop form takes it.
    private var directory: String {
        folder.isEmpty ? "~" : folder.hasPrefix("/") ? folder : "~/" + folder
    }

    private func start() async {
        failure = nil
        progress = "Starting…"
        do {
            let agent = try await model.start(
                NewAgent(harness: harness, directory: directory, category: category,
                         machine: machine.isEmpty ? nil : machine)
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
