import SwiftUI

struct AgentListView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingSettings = false

    var body: some View {
        NavigationStack {
            content
                .navigationTitle("lapis")
                .navigationDestination(for: Agent.self) { agent in
                    AgentView(agent: agent, gateway: model.gateway)
                }
                .toolbar {
                    ToolbarItem(placement: .topBarTrailing) {
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
            List {
                if let error = model.error {
                    Text(error)
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .listRowBackground(Theme.panel)
                }
                ForEach(listing.categories) { category in
                    Section(category.name) {
                        if category.agents.isEmpty {
                            Text("No agents").foregroundStyle(Theme.quiet)
                        }
                        ForEach(category.agents) { agent in
                            NavigationLink(value: agent) {
                                AgentRow(agent: agent)
                            }
                            .accessibilityIdentifier("agent-\(agent.title)")
                            .listRowBackground(Theme.panel)
                        }
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .background(Theme.background)
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

struct AgentRow: View {
    let agent: Agent

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(agent.running ? Theme.live : Theme.quiet.opacity(0.4))
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 2) {
                Text(agent.title)
                    .font(.body.weight(.medium))
                Text("\(agent.harness) · \(agent.place)")
                    .font(.caption.monospaced())
                    .foregroundStyle(Theme.quiet)
            }
            Spacer()
            if agent.onPhone {
                Image(systemName: "iphone")
                    .foregroundStyle(Theme.accent)
            } else if !agent.running {
                Text("stopped")
                    .font(.caption)
                    .foregroundStyle(Theme.quiet)
            }
        }
        .padding(.vertical, 2)
    }
}
