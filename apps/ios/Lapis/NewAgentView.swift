import SwiftUI

// Starting an agent from the phone: on the Mac or over ssh on another machine,
// in a folder picked on its own screen, as a new tab in a category in lapis on
// the Mac. What it shows was fetched ahead, so nothing here waits on the
// network until Start. Choices stay put while others change: another machine
// keeps the CLI when it has it, and the mode and each CLI's model are
// remembered for the next agent.
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
    @State private var progress: String?
    @State private var failure: String?
    @State private var naming = false
    @State private var categoryName = ""
    // The mode across CLIs, and each CLI's model: {"claude": "claude-fable-5-1"}.
    @AppStorage("newAgent.mode") private var storedMode = ""
    @AppStorage("newAgent.models") private var storedModels = "{}"

    init(categories: [AgentCategory], category: String, onStarted: @escaping (Agent) -> Void) {
        self.categories = categories
        self.onStarted = onStarted
        _category = State(initialValue: category)
    }

    static let modes = [(id: "edits", name: "Accept edits"), (id: "auto", name: "Auto"),
                        (id: "full", name: "Full access")]

    private var catalog: FolderCatalog? { model.catalogs[machine] }
    private var offered: [Harness] {
        let known = model.harnesses ?? []
        if machine.isEmpty { return known.filter(\.installed) }
        guard let found = catalog?.harnesses else { return [] }
        return known.filter { found[$0.id] != nil }
    }
    private var chosenHarness: Harness? { offered.first { $0.id == harness } }
    private var canStart: Bool { !harness.isEmpty && !category.isEmpty && progress == nil }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    if !model.machines.isEmpty {
                        section("Machine") { machineChips }
                    }
                    section("Agent") { harnessCards }
                    if let chosen = chosenHarness {
                        if let models = chosen.models, !models.isEmpty {
                            section("Model") { modelChips(models) }
                        }
                        if !(chosen.modes ?? []).isEmpty {
                            section("Mode") { modeButtons(chosen) }
                        }
                    }
                    section("Category") { categoryChips }
                    section("Folder") { folderRow }
                    if let failure {
                        Text(failure)
                            .font(.footnote)
                            .foregroundStyle(.orange)
                            .accessibilityIdentifier("newAgentError")
                    }
                }
                .padding(16)
            }
            .safeAreaInset(edge: .bottom) {
                startButton
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                    .background(Theme.background)
            }
            .background(Theme.background.ignoresSafeArea())
            .alert("New category", isPresented: $naming) {
                NewCategoryFields(name: $categoryName) { id in category = id }
            }
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
        .onChange(of: model.harnesses) { _, _ in pickHarness() }
        .onChange(of: catalog?.version) { _, _ in pickHarness() }
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

    // MARK: Machine, agent, model, mode and category

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

    // Large cards, two and a half across, so the next one shows there is more.
    @ViewBuilder private var harnessCards: some View {
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
            ScrollViewReader { reader in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 10) {
                        ForEach(offered) { item in harnessCard(item) }
                    }
                    .scrollTargetLayout()
                    .padding(.vertical, 6)
                }
                .scrollTargetBehavior(.viewAligned)
                .onAppear { reader.scrollTo(harness, anchor: .center) }
            }
        }
    }

    private func harnessCard(_ item: Harness) -> some View {
        let chosen = item.id == harness
        let accent = Theme.harness(item.id)
        return Button {
            harness = item.id
        } label: {
            VStack(spacing: 10) {
                HarnessBadge(harness: item.id, accent: accent, size: 54)
                Text(item.name)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(chosen ? .white : Theme.quiet)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 16)
            .background {
                Chamfered(cut: 12).fill(chosen ? accent.opacity(0.12) : Theme.panel)
                Chamfered(cut: 12)
                    .stroke(chosen ? accent.opacity(0.9) : Theme.edge, lineWidth: chosen ? 1.5 : 1)
                    .shadow(color: chosen ? accent.opacity(0.4) : .clear, radius: 6)
            }
            .contentShape(Chamfered(cut: 12))
        }
        .buttonStyle(CardPress())
        .containerRelativeFrame(.horizontal, count: 5, span: 2, spacing: 10)
        .id(item.id)
        .accessibilityIdentifier("harness-\(item.id)")
        .accessibilityAddTraits(chosen ? .isSelected : [])
    }

    // Every model the CLI lists, wrapped rather than scrolled.
    private func modelChips(_ models: [ModelChoice]) -> some View {
        let chosen = chosenModel(models)?.id
        return FlowLayout(spacing: 8) {
            ForEach(models) { choice in
                chip(choice.name, chosen: choice.id == chosen, id: "model-\(choice.id)") {
                    remember(model: choice.id)
                }
            }
        }
    }

    // Always one of three; one the CLI does not have is shown unavailable.
    private func modeButtons(_ chosen: Harness) -> some View {
        let current = effectiveMode(chosen)
        return HStack(spacing: 8) {
            ForEach(Self.modes, id: \.id) { choice in
                let available = (chosen.modes ?? []).contains { $0.id == choice.id }
                let selected = choice.id == current
                Button {
                    storedMode = choice.id
                } label: {
                    Text(choice.name)
                        .font(.system(size: 14, weight: selected ? .semibold : .regular))
                        .foregroundStyle(selected ? .white : available ? Theme.quiet : Theme.quiet.opacity(0.35))
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 11)
                        .background {
                            Chamfered(cut: 7).fill(selected ? Theme.accent.opacity(0.16) : Theme.panel)
                            Chamfered(cut: 7).stroke(selected ? Theme.accent : Theme.edge,
                                                     lineWidth: selected ? 1.5 : 1)
                        }
                }
                .buttonStyle(.plain)
                .disabled(!available)
                .accessibilityIdentifier("mode-\(choice.id)")
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
    }

    // The Mac's categories, and one made here.
    private var categoryChips: some View {
        FlowLayout(spacing: 8) {
            ForEach(model.listing?.categories ?? categories) { item in
                chip(item.name, chosen: item.id == category, id: "category-\(item.name)") {
                    category = item.id
                }
            }
            chip("New category", chosen: false, id: "newCategory") {
                categoryName = ""
                naming = true
            }
        }
    }

    // The folder on its own screen: search, the most used, or browse.
    private var folderRow: some View {
        NavigationLink {
            FolderPicker(folder: $folder, machine: machine, preset: presetFolder)
        } label: {
            HStack(spacing: 10) {
                Image(systemName: "folder")
                    .font(.system(size: 13))
                    .foregroundStyle(Theme.quiet)
                Text(place(folder, on: machine))
                    .font(.system(size: 15, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Theme.accent)
                    .lineLimit(1)
                    .truncationMode(.head)
                Spacer(minLength: 4)
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(Theme.quiet)
            }
            .padding(14)
            .background {
                Chamfered(cut: 10).fill(Theme.panel)
                Chamfered(cut: 10).stroke(Theme.edge, lineWidth: 1)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(place(folder, on: machine))
        .accessibilityIdentifier("folderRow")
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
                Text(progress ?? "Start in \(place(folder, on: machine))")
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

    private var rememberedModels: [String: String] {
        (try? JSONDecoder().decode([String: String].self, from: Data(storedModels.utf8))) ?? [:]
    }

    private func remember(model id: String) {
        var models = rememberedModels
        models[harness] = id
        if let data = try? JSONEncoder().encode(models) {
            storedModels = String(decoding: data, as: UTF8.self)
        }
    }

    // The model last chosen for this CLI, else its default, else its first.
    private func chosenModel(_ models: [ModelChoice]) -> ModelChoice? {
        let remembered = rememberedModels[harness]
        return models.first { $0.id == remembered } ?? models.first(where: \.isDefault) ?? models.first
    }

    // The mode last chosen, else lapis.json's, else Full access; a CLI
    // without it uses its nearest, less access first.
    private func effectiveMode(_ chosen: Harness) -> String {
        let order = Self.modes.map(\.id)
        let preferred = !storedMode.isEmpty ? storedMode : model.defaults?.mode ?? "full"
        let at = order.firstIndex(of: preferred) ?? 0
        let offered = Set((chosen.modes ?? []).map(\.id))
        for index in [at, at - 1, at - 2, at + 1, at + 2] where order.indices.contains(index) {
            if offered.contains(order[index]) { return order[index] }
        }
        return ""
    }

    private func prepare() async {
        pickHarness()
        folder = startingFolder
        if model.harnesses == nil || catalog == nil {
            await model.prefetch()
            pickHarness()
            if folder.isEmpty { folder = startingFolder }
        }
    }

    // lapis.json's preset folder: the machine's own, else the one for every
    // machine ("~/dev"), when that machine has it.
    private var presetFolder: String? {
        let configured = model.defaults?.machines?[machine] ?? model.defaults?.folder
        guard let configured, !configured.isEmpty else { return nil }
        let path = configured == "~" ? "" : configured.hasPrefix("~/")
            ? String(configured.dropFirst(2)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            : configured
        if let catalog, !catalog.contains(path) { return nil }
        return path
    }

    // The preset folder, else where the most agents were started there, else home.
    private var startingFolder: String {
        presetFolder ?? catalog?.frequent.first?.path ?? ""
    }

    // Keeps the chosen CLI while it is offered (or while the machine's CLIs
    // are still unknown); otherwise the configured one, the one last used on
    // this machine, or the first.
    private func pickHarness() {
        let ids = offered.map(\.id)
        guard !ids.isEmpty, !ids.contains(harness) else { return }
        let recent = (model.listing?.categories.flatMap(\.agents) ?? [])
            .last { ($0.machine ?? "") == machine }?.harness
        let preferred = [model.defaults?.harness, recent].compactMap { $0 }.first { ids.contains($0) }
        harness = preferred ?? ids.first ?? ""
    }

    private func choose(machine name: String) {
        guard name != machine else { return }
        machine = name
        pickHarness()
        folder = startingFolder
        if catalog == nil {
            Task {
                await model.loadCatalog(name)
                guard machine == name else { return }
                pickHarness()
                folder = startingFolder
            }
        }
    }

    // What the Mac is sent: ~ for home, as the desktop form takes it.
    private var directory: String {
        folder.isEmpty ? "~" : folder.hasPrefix("/") ? folder : "~/" + folder
    }

    private func start() async {
        failure = nil
        progress = "Starting…"
        let chosen = chosenHarness
        let modelChoice = chosenModel(chosen?.models ?? [])
        let mode = chosen.map { effectiveMode($0) } ?? ""
        do {
            let agent = try await model.start(
                NewAgent(harness: harness, directory: directory, category: category,
                         machine: machine.isEmpty ? nil : machine,
                         model: modelChoice.flatMap { $0.isDefault ? nil : $0.id },
                         mode: mode.isEmpty ? nil : mode)
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

// "~/dev/lapis" here, "devbox:~/dev/lapis" on another machine.
func place(_ path: String, on machine: String) -> String {
    let shown = path.isEmpty ? "~" : path.hasPrefix("/") ? path : "~/" + path
    return machine.isEmpty ? shown : machine + ":" + shown
}

func parentFolder(_ path: String) -> String {
    guard let slash = path.lastIndex(of: "/") else { return "" }
    let up = String(path[..<slash])
    return up.isEmpty && path.hasPrefix("/") ? "/" : up
}

// Choosing a folder: type to find one (fuzzy, on the phone), take one of the
// ten most used, or browse from the current one. A folder with folders inside
// opens; one without, or "Use", chooses and returns.
struct FolderPicker: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @Binding var folder: String
    let machine: String
    let preset: String?
    @State private var browsing: String
    @State private var query = ""
    @State private var results: [String] = []
    // The last query's matches in one catalog version, to narrow the next.
    @State private var narrowing: (version: String, query: String, all: [Int])?

    init(folder: Binding<String>, machine: String, preset: String?) {
        _folder = folder
        self.machine = machine
        self.preset = preset
        _browsing = State(initialValue: folder.wrappedValue)
    }

    private var catalog: FolderCatalog? { model.catalogs[machine] }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                searchField
                if query.isEmpty {
                    presetRow
                    frequentFolders
                    browser
                } else {
                    searchResults
                }
            }
            .padding(16)
        }
        .scrollDismissesKeyboard(.interactively)
        .safeAreaInset(edge: .bottom) {
            if query.isEmpty {
                Button {
                    choose(browsing)
                } label: {
                    Text("Use \(place(browsing, on: machine))")
                        .font(.system(size: 16, weight: .bold))
                        .lineLimit(1)
                        .truncationMode(.head)
                        .foregroundStyle(.black)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 14)
                        .background(Chamfered(cut: 12).fill(Theme.accent))
                }
                .buttonStyle(CardPress())
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(Theme.background)
                .accessibilityIdentifier("useFolder")
            }
        }
        .background(Theme.background.ignoresSafeArea())
        .navigationTitle("Folder")
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(Theme.background, for: .navigationBar)
        .task(id: query) { await search() }
    }

    private func choose(_ path: String) {
        folder = path
        dismiss()
    }

    private var searchField: some View {
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
                .accessibilityIdentifier("folderSearch")
            if !query.isEmpty {
                Button {
                    query = ""
                } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(Theme.quiet)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Clear")
            }
        }
        .padding(12)
        .background {
            Chamfered(cut: 10).fill(Theme.panel)
            Chamfered(cut: 10).stroke(Theme.edge, lineWidth: 1)
        }
    }

    // lapis.json's folder for this machine, first and marked.
    @ViewBuilder private var presetRow: some View {
        if let preset {
            let chosen = preset == folder
            Button {
                choose(preset)
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "pin.fill")
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.accent)
                    Text(place(preset, on: machine))
                        .font(.system(size: 13, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.white)
                        .lineLimit(1)
                        .truncationMode(.head)
                    Spacer(minLength: 4)
                    Text("Preset")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(Theme.accent)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 10)
                .background {
                    Chamfered(cut: 7).fill(Theme.accent.opacity(chosen ? 0.22 : 0.1))
                    Chamfered(cut: 7).stroke(Theme.accent.opacity(chosen ? 0.9 : 0.35), lineWidth: 1)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("presetFolder")
            .accessibilityAddTraits(chosen ? .isSelected : [])
        }
    }

    // Where agents were started most, most first, marked out above browsing.
    @ViewBuilder private var frequentFolders: some View {
        if let frequent = catalog?.frequent, !frequent.isEmpty {
            ForEach(frequent.filter { $0.path != preset }.prefix(10), id: \.path) { item in
                let chosen = item.path == folder
                Button {
                    choose(item.path)
                } label: {
                    HStack(spacing: 10) {
                        Image(systemName: "star.fill")
                            .font(.system(size: 10))
                            .foregroundStyle(Theme.accent)
                        Text(place(item.path, on: machine))
                            .font(.system(size: 13, design: .monospaced))
                            .foregroundStyle(.white)
                            .lineLimit(1)
                            .truncationMode(.head)
                        Spacer(minLength: 4)
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 10)
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

    // The folder being browsed and its folders: visible ones alphabetically,
    // then hidden.
    @ViewBuilder private var browser: some View {
        HStack(spacing: 10) {
            Button {
                browsing = parentFolder(browsing)
            } label: {
                Image(systemName: "arrow.up")
                    .font(.system(size: 13, weight: .semibold))
                    .frame(width: 32, height: 32)
                    .background(Chamfered(cut: 6).fill(Theme.panel))
            }
            .buttonStyle(.plain)
            .foregroundStyle(browsing.isEmpty || browsing == "/" ? Theme.quiet.opacity(0.4) : .white)
            .disabled(browsing.isEmpty || browsing == "/")
            .accessibilityLabel("Up one folder")
            .accessibilityIdentifier("folderUp")
            Text(place(browsing, on: machine))
                .font(.system(size: 14, weight: .semibold, design: .monospaced))
                .foregroundStyle(Theme.accent)
                .lineLimit(1)
                .truncationMode(.head)
                .accessibilityIdentifier("browsingFolder")
            Spacer(minLength: 0)
        }
        .padding(.top, 10)
        if let catalog {
            ForEach(catalog.children(of: browsing), id: \.self) { child in
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
        let opens = !full && catalog.hasChildren(path)
        return Button {
            if opens { browsing = path } else { choose(path) }
        } label: {
            HStack(spacing: 10) {
                Image(systemName: "folder")
                    .font(.system(size: 12))
                    .foregroundStyle(hidden ? Theme.quiet.opacity(0.6) : Theme.quiet)
                if full {
                    (Text(name).foregroundStyle(.white)
                        + Text("  " + place(parentFolder(path), on: machine)).foregroundStyle(Theme.quiet))
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
                if opens {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Theme.quiet.opacity(0.6))
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(id)
    }

    @ViewBuilder private var searchResults: some View {
        let typed = query.trimmingCharacters(in: .whitespaces)
        if typed.hasPrefix("/") || typed.hasPrefix("~") {
            Button {
                choose(typed == "~" ? "" : typed.hasPrefix("~/") ? String(typed.dropFirst(2)) : typed)
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
}

// Rows of views that wrap to the width, left to right.
struct FlowLayout: Layout {
    var spacing: CGFloat = 8

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache _: inout ()) -> CGSize {
        let width = proposal.width ?? .infinity
        var x: CGFloat = 0
        var y: CGFloat = 0
        var row: CGFloat = 0
        var widest: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > 0, x + size.width > width {
                x = 0
                y += row + spacing
                row = 0
            }
            x += size.width + spacing
            row = max(row, size.height)
            widest = max(widest, x - spacing)
        }
        return CGSize(width: proposal.width ?? widest, height: y + row)
    }

    func placeSubviews(in bounds: CGRect, proposal _: ProposedViewSize, subviews: Subviews, cache _: inout ()) {
        var x = bounds.minX
        var y = bounds.minY
        var row: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > bounds.minX, x + size.width > bounds.maxX {
                x = bounds.minX
                y += row + spacing
                row = 0
            }
            view.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
            x += size.width + spacing
            row = max(row, size.height)
        }
    }
}
