import SwiftUI

struct AgentListView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingSettings = false
    @State private var path: [Agent] = []
    @State private var newAgent: NewAgentTarget?
    @State private var started: Agent?
    @State private var naming = false
    @State private var categoryName = ""
    @State private var choosingTerminal = false
    @State private var renaming: Agent?
    @State private var newName = ""
    @State private var resuming: NewAgentTarget?

    var body: some View {
        NavigationStack(path: $path) {
            content
                .background(Theme.background.ignoresSafeArea())
                .navigationBarTitleDisplayMode(.inline)
                .toolbarBackground(Theme.background, for: .navigationBar)
                .navigationDestination(for: Agent.self) { agent in
                    AgentPager(agent: agent)
                }
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) {
                        Image("LapisMark")
                            .resizable()
                            .interpolation(.high)
                            .frame(width: 24, height: 24)
                            .accessibilityLabel("lapis")
                    }
                    ToolbarItemGroup(placement: .topBarTrailing) {
                        // The one plus: a new agent or a new category.
                        Menu {
                            Button {
                                if let listing = model.listing {
                                    newAgent = NewAgentTarget(category: listing.activeCategory)
                                }
                            } label: {
                                Label("New agent", systemImage: "terminal")
                            }
                            Button {
                                if let listing = model.listing {
                                    resuming = NewAgentTarget(category: listing.activeCategory)
                                }
                            } label: {
                                Label("Resume conversation", systemImage: "clock.arrow.circlepath")
                            }
                            Button {
                                choosingTerminal = true
                            } label: {
                                Label("Terminal", systemImage: "apple.terminal")
                            }
                            Button {
                                categoryName = ""
                                naming = true
                            } label: {
                                Label("New category", systemImage: "folder.badge.plus")
                            }
                        } label: {
                            Image(systemName: "plus")
                        }
                        .disabled(model.listing == nil)
                        .accessibilityLabel("Add")
                        .accessibilityIdentifier("add")
                        Button {
                            showingSettings = true
                        } label: {
                            Image(systemName: "gearshape")
                        }
                        .accessibilityIdentifier("settings")
                    }
                }
                .sheet(isPresented: $showingSettings) {
                    SettingsView()
                }
                .alert("New category", isPresented: $naming) {
                    NewCategoryFields(name: $categoryName) { _ in }
                }
                .alert("Rename agent", isPresented: Binding(get: { renaming != nil },
                                                            set: { if !$0 { renaming = nil } })) {
                    TextField("Name", text: $newName)
                        .accessibilityIdentifier("agentName")
                    Button("Save") {
                        let chosen = newName.trimmingCharacters(in: .whitespaces)
                        if let agent = renaming, !chosen.isEmpty {
                            Task { await model.rename(agent, to: String(chosen.prefix(80))) }
                        }
                        renaming = nil
                    }
                    .disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty)
                    Button("Cancel", role: .cancel) { renaming = nil }
                } message: {
                    Text("It keeps this name instead of its conversation's title.")
                }
                // The agent opens once the sheet has gone.
                .sheet(item: $newAgent, onDismiss: {
                    if let agent = started {
                        started = nil
                        path.append(agent)
                    }
                }) { target in
                    NewAgentView(categories: model.listing?.categories ?? [],
                                 category: target.category) { agent in
                        started = agent
                    }
                }
                .sheet(item: $resuming, onDismiss: openStarted) { target in
                    ResumeView(category: target.category) { agent in started = agent }
                }
                .sheet(isPresented: $choosingTerminal, onDismiss: openStarted) {
                    TerminalPickerView { terminal in started = terminal }
                }
        }
        .task(id: scenePhase) {
            // Keep the list current while it is on screen and the app is active.
            guard scenePhase == .active else { return }
            while !Task.isCancelled {
                await model.refresh()
                try? await Task.sleep(for: .seconds(8))
            }
        }
    }

    // What a sheet started opens once the sheet has gone.
    private func openStarted() {
        if let agent = started {
            started = nil
            path.append(agent)
        }
    }

    @ViewBuilder private var content: some View {
        if model.host.isEmpty {
            ContentUnavailableView {
                Label("Connect to your Mac", systemImage: "desktopcomputer")
            } description: {
                Text("Enter the Mac's Tailscale name in Settings.")
            } actions: {
                Button("Settings") { showingSettings = true }
            }
        } else if let listing = model.listing {
            // A list, so an agent can be swiped away like a notification:
            // swiping left offers Close, and a full swipe closes it.
            List {
                if let error = model.error {
                    Text(error)
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                }
                // Plain shells for a quick command, above the agents.
                if !model.terminals.isEmpty {
                    Text("Terminals")
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .textCase(.uppercase)
                        .tracking(1.6)
                        .foregroundStyle(Theme.quiet)
                        .padding(.top, 14)
                        .listRowInsets(EdgeInsets(top: 0, leading: 20, bottom: 2, trailing: 16))
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                    ForEach(model.terminals) { terminal in
                        Button {
                            path.append(terminal.asAgent)
                        } label: {
                            AgentCard(agent: terminal.asAgent)
                        }
                        .buttonStyle(CardPress())
                        .accessibilityIdentifier("terminal-row-\(terminal.machine.isEmpty ? "mac" : terminal.machine)")
                        .listRowInsets(EdgeInsets(top: 5, leading: 16, bottom: 5, trailing: 16))
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                        .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                            Button(role: .destructive) {
                                Task { await model.closeTerminal(terminal) }
                            } label: {
                                Label("Close", systemImage: "xmark")
                            }
                        }
                    }
                }
                ForEach(listing.categories) { category in
                    Text(category.name)
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .textCase(.uppercase)
                        .tracking(1.6)
                        .foregroundStyle(Theme.quiet)
                        .padding(.top, 14)
                        .listRowInsets(EdgeInsets(top: 0, leading: 20, bottom: 2, trailing: 16))
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                        .accessibilityIdentifier("category-\(category.name)")
                    if category.agents.isEmpty {
                        Text("No agents")
                            .font(.footnote)
                            .foregroundStyle(Theme.quiet)
                            .listRowInsets(EdgeInsets(top: 0, leading: 20, bottom: 4, trailing: 16))
                            .listRowBackground(Color.clear)
                            .listRowSeparator(.hidden)
                    }
                    ForEach(category.agents) { agent in
                        Button {
                            path.append(agent)
                        } label: {
                            AgentCard(agent: agent)
                        }
                        .buttonStyle(CardPress())
                        .accessibilityIdentifier("agent-\(agent.title)")
                        .listRowInsets(EdgeInsets(top: 5, leading: 16, bottom: 5, trailing: 16))
                        .listRowBackground(Color.clear)
                        .listRowSeparator(.hidden)
                        .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                            Button(role: .destructive) {
                                Task { await model.close(agent) }
                            } label: {
                                Label("Close", systemImage: "xmark")
                            }
                        }
                        .swipeActions(edge: .leading) {
                            Button {
                                newName = agent.title
                                renaming = agent
                            } label: {
                                Label("Rename", systemImage: "pencil")
                            }
                            .tint(Theme.accent)
                        }
                        .contextMenu {
                            Button {
                                newName = agent.title
                                renaming = agent
                            } label: {
                                Label("Rename", systemImage: "pencil")
                            }
                        }
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .refreshable { await model.refresh() }
        } else if let error = model.error {
            ContentUnavailableView {
                Label("Can't reach lapis", systemImage: "wifi.exclamationmark")
            } description: {
                Text(error)
            } actions: {
                Button("Try again") { Task { await model.refresh() } }
                Button("Settings") { showingSettings = true }
            }
        } else {
            ProgressView("Connecting to \(model.host)")
                .foregroundStyle(Theme.quiet)
        }
    }
}

// A category's name, created on the Mac when confirmed.
struct NewCategoryFields: View {
    @Environment(WorkspaceModel.self) private var model
    @Binding var name: String
    let created: (String) -> Void

    var body: some View {
        TextField("Name", text: $name)
            .accessibilityIdentifier("categoryName")
        Button("Create") {
            let chosen = name.trimmingCharacters(in: .whitespaces)
            Task {
                do {
                    created(try await model.createCategory(named: chosen))
                } catch {
                    model.error = describe(error)
                }
            }
        }
        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
        Button("Cancel", role: .cancel) {}
    }
}

// Which category a new agent goes to.
struct NewAgentTarget: Identifiable {
    let category: String
    var id: String { category }
}

// A card with cut corners and a harness-tinted edge, in the desktop's
// command-room style: identity on the left, place in path form, a status light.
struct AgentCard: View {
    let agent: Agent
    @Environment(\.isPressedCard) private var pressed

    private var accent: Color { Theme.harness(agent.harness) }

    var body: some View {
        HStack(spacing: 14) {
            HarnessBadge(harness: agent.harness, accent: accent)
            VStack(alignment: .leading, spacing: 3) {
                Text(agent.title)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                placeText
                    .font(.system(size: 12.5, design: .monospaced))
                    .lineLimit(1)
                    .truncationMode(.head)
            }
            Spacer(minLength: 8)
            StatusLight(agent: agent)
            Image(systemName: "chevron.right")
                .font(.system(size: 12, weight: .bold))
                .foregroundStyle(Theme.quiet.opacity(0.7))
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background {
            let shape = Chamfered(cut: 12)
            shape.fill(LinearGradient(colors: [Theme.panel.opacity(1), Theme.panelDeep],
                                      startPoint: .top, endPoint: .bottom))
            shape.stroke(accent.opacity(pressed ? 0.9 : 0.28), lineWidth: pressed ? 1.5 : 1)
                .shadow(color: accent.opacity(pressed ? 0.45 : 0), radius: 8)
        }
        .contentShape(Chamfered(cut: 12))
    }

    // "~/dev/infinity" here; the machine in the accent colour elsewhere.
    private var placeText: Text {
        let location = agent.location
        if let machine = agent.machine, !machine.isEmpty, location.hasPrefix(machine + ":") {
            let rest = String(location.dropFirst(machine.count + 1))
            return Text(machine + ":").foregroundStyle(accent) + Text(rest).foregroundStyle(Theme.quiet)
        }
        return Text(location).foregroundStyle(Theme.quiet)
    }
}

struct HarnessBadge: View {
    let harness: String
    let accent: Color
    var size: CGFloat = 42

    var body: some View {
        ZStack {
            Chamfered(cut: 7)
                .fill(Color.black)
            Chamfered(cut: 7)
                .stroke(accent.opacity(0.35), lineWidth: 1)
            if UIImage(named: "Mark-\(harness)") != nil {
                Image("Mark-\(harness)")
                    .resizable()
                    .renderingMode(.template)
                    .scaledToFit()
                    .frame(width: size * 0.52, height: size * 0.52)
                    .foregroundStyle(accent)
            } else {
                Text(String(harness.prefix(2)).uppercased())
                    .font(.system(size: size / 3, weight: .bold, design: .monospaced))
                    .foregroundStyle(accent)
            }
        }
        .frame(width: size, height: size)
        .accessibilityLabel(harness)
    }
}

struct StatusLight: View {
    let agent: Agent

    var body: some View {
        HStack(spacing: 6) {
            if agent.onPhone {
                Image(systemName: "iphone")
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.accent)
            }
            if !agent.running {
                Text("OFF")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1)
                    .foregroundStyle(Theme.quiet)
            }
            Rectangle()
                .fill(agent.running ? Theme.live : Theme.quiet.opacity(0.35))
                .frame(width: 7, height: 7)
                .rotationEffect(.degrees(45))
                .shadow(color: agent.running ? Theme.live.opacity(0.8) : .clear, radius: 4)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(agent.running ? "running" : "stopped")
    }
}

// A rectangle with its top-left and bottom-right corners cut at 45 degrees.
struct Chamfered: Shape {
    let cut: CGFloat

    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX + cut, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY - cut))
        path.addLine(to: CGPoint(x: rect.maxX - cut, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.minY + cut))
        path.closeSubpath()
        return path
    }
}

// Pressing a card lights its edge and settles it slightly, quickly.
struct CardPress: ButtonStyle {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .environment(\.isPressedCard, configuration.isPressed)
            .scaleEffect(configuration.isPressed && !reduceMotion ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

private struct PressedCardKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var isPressedCard: Bool {
        get { self[PressedCardKey.self] }
        set { self[PressedCardKey.self] = newValue }
    }
}

// An agent's screen; swiping left or right over it moves to the next or
// previous agent in its category (or among the terminals), like pages, so the
// list is only needed to change category.
struct AgentPager: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var current: Agent
    @State private var forward = true

    init(agent: Agent) {
        _current = State(initialValue: agent)
    }

    private var siblings: [Agent] {
        if current.id.hasPrefix("terminal-") { return model.terminals.map(\.asAgent) }
        let category = model.listing?.categories.first { $0.agents.contains { $0.id == current.id } }
        return category?.agents ?? [current]
    }

    var body: some View {
        let list = siblings
        let index = list.firstIndex { $0.id == current.id }
        let shown = index.map { list[$0] } ?? current
        ZStack {
            AgentView(agent: current, gateway: model.gateway) { step in
                guard let index, list.indices.contains(index + step) else { return }
                forward = step > 0
                UIImpactFeedbackGenerator(style: .light).impactOccurred()
                if reduceMotion {
                    current = list[index + step]
                } else {
                    withAnimation(.easeOut(duration: 0.2)) { current = list[index + step] }
                }
            }
            .id(current.id)
            .transition(.asymmetric(insertion: .move(edge: forward ? .trailing : .leading),
                                    removal: .move(edge: forward ? .leading : .trailing)))
        }
        .clipped()
        .toolbar {
            if let index, list.count > 1 {
                ToolbarItem(placement: .principal) {
                    VStack(spacing: 1) {
                        Text(shown.title)
                            .font(.headline)
                            .lineLimit(1)
                        Text("\(index + 1) of \(list.count)")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(Theme.quiet)
                            .accessibilityIdentifier("agentPosition")
                    }
                }
            }
        }
    }
}
