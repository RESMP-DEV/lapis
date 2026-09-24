import SwiftUI

struct AgentListView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingSettings = false
    @State private var path: [Agent] = []
    @State private var newAgent: NewAgentTarget?
    @State private var started: Agent?

    var body: some View {
        NavigationStack(path: $path) {
            content
                .background(Theme.background.ignoresSafeArea())
                .navigationBarTitleDisplayMode(.inline)
                .toolbarBackground(Theme.background, for: .navigationBar)
                .navigationDestination(for: Agent.self) { agent in
                    AgentView(agent: agent, gateway: model.gateway)
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
                        Button {
                            if let listing = model.listing {
                                newAgent = NewAgentTarget(category: listing.activeCategory)
                            }
                        } label: {
                            Image(systemName: "plus")
                        }
                        .disabled(model.listing == nil)
                        .accessibilityLabel("New agent")
                        .accessibilityIdentifier("newAgent")
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
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 10) {
                    if let error = model.error {
                        Text(error)
                            .font(.footnote)
                            .foregroundStyle(.orange)
                    }
                    ForEach(listing.categories) { category in
                        HStack {
                            Text(category.name)
                                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                                .textCase(.uppercase)
                                .tracking(1.6)
                                .foregroundStyle(Theme.quiet)
                            Spacer()
                            Button {
                                newAgent = NewAgentTarget(category: category.id)
                            } label: {
                                Image(systemName: "plus")
                                    .font(.system(size: 13, weight: .semibold))
                                    .foregroundStyle(Theme.quiet)
                                    .frame(width: 36, height: 28)
                                    .contentShape(Rectangle())
                            }
                            .accessibilityLabel("New agent in \(category.name)")
                            .accessibilityIdentifier("newAgent-\(category.name)")
                        }
                        .padding(.top, 14)
                        .padding(.leading, 4)
                        if category.agents.isEmpty {
                            Text("No agents")
                                .font(.footnote)
                                .foregroundStyle(Theme.quiet)
                                .padding(.leading, 4)
                        }
                        ForEach(category.agents) { agent in
                            NavigationLink(value: agent) {
                                AgentCard(agent: agent)
                            }
                            .buttonStyle(CardPress())
                            .accessibilityIdentifier("agent-\(agent.title)")
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
            }
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
                    .frame(width: 22, height: 22)
                    .foregroundStyle(accent)
            } else {
                Text(String(harness.prefix(2)).uppercased())
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .foregroundStyle(accent)
            }
        }
        .frame(width: 42, height: 42)
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
