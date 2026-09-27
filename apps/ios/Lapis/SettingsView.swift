import SwiftUI

struct SettingsView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @AppStorage("terminalFontSize") private var fontSize = 12.0
    @State private var host = ""
    @State private var result: String?
    @State private var checking = false

    var body: some View {
        NavigationStack {
            Form {
                Group {
                    Section {
                        TextField("your-mac.your-tailnet.ts.net", text: $host)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .keyboardType(.URL)
                            .accessibilityIdentifier("hostField")
                    } header: {
                        Text("Mac")
                    } footer: {
                        Text("""
                            lapis reaches the gateway on your Mac over Tailscale. This iPhone must be \
                            signed in to Tailscale with the same account as the Mac; nothing else is \
                            needed to sign in.
                            """)
                    }
                    Section {
                        Button(checking ? "Checking…" : "Check connection") { Task { await check() } }
                            .disabled(checking || host.isEmpty)
                        if let result {
                            Text(result).font(.footnote)
                        }
                    }
                    Section {
                        Stepper(value: $fontSize, in: 8...20, step: 1) {
                            LabeledContent("Text size", value: "\(Int(fontSize)) pt")
                        }
                        .accessibilityIdentifier("textSize")
                    } header: {
                        Text("This iPhone")
                    } footer: {
                        Text("The agents' text here. Each agent's menu changes it too.")
                    }
                    if model.listing != nil {
                        Section {
                            NavigationLink {
                                CategoriesView()
                            } label: {
                                LabeledContent("Categories", value: "\(model.listing?.categories.count ?? 0)")
                            }
                            .accessibilityIdentifier("categories")
                        } header: {
                            Text("Workspace")
                        }
                    }
                    macSection
                }
                .listRowBackground(Theme.panel)
            }
            .scrollContentBackground(.hidden)
            .background(Theme.background.ignoresSafeArea())
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(Theme.background, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        model.host = host.trimmingCharacters(in: .whitespacesAndNewlines)
                        dismiss()
                        Task { await model.refresh() }
                    }
                }
            }
            .modifier(MacNotice())
            .onAppear { host = model.host }
            .task { await model.loadMacSettings() }
        }
        .presentationBackground(Theme.background)
    }

    // The Mac's own settings that matter away from it. Its window's look
    // (theme, layout, fonts, shortcuts) is set at the Mac.
    @ViewBuilder private var macSection: some View {
        if let settings = model.macSettings {
            Section {
                Toggle("Keep the Mac awake", isOn: macSwitch(\.keepAwake, "keepAwake"))
                    .accessibilityIdentifier("keepAwake")
                Toggle("Chime when an agent needs you", isOn: macSwitch(\.alertSound, "alertSound"))
                    .accessibilityIdentifier("alertSound")
                if settings.alertSound {
                    Stepper(value: Binding(get: { settings.alertRepeat },
                                           set: { times in change(\.alertRepeat, times, "alertRepeat") }),
                            in: 1...10) {
                        Text(settings.alertRepeat == 1 ? "Chime once" : "Chime up to \(settings.alertRepeat) times")
                    }
                    .accessibilityIdentifier("alertRepeat")
                }
                Toggle("Chime when a turn finishes", isOn: macSwitch(\.finishSound, "finishSound"))
                    .accessibilityIdentifier("finishSound")
                Toggle("Notify from the background", isOn: macSwitch(\.notify, "notify"))
                    .accessibilityIdentifier("notify")
                Toggle("Show plan usage", isOn: macSwitch(\.showUsage, "showUsage"))
                    .accessibilityIdentifier("showUsage")
            } header: {
                Text("On the Mac")
            } footer: {
                Text("""
                    Saved in the Mac's lapis.json, as its Settings window saves them. Awake holds \
                    only on power, and the display still sleeps. Chimes and notifications sound \
                    on the Mac.
                    """)
            }
        } else if let failure = model.settingsError {
            Section {
                Text(failure)
                    .font(.footnote)
                    .foregroundStyle(Theme.quiet)
            } header: {
                Text("On the Mac")
            }
        }
    }

    private func macSwitch(_ key: WritableKeyPath<MacSettings, Bool>, _ name: String) -> Binding<Bool> {
        Binding(get: { model.macSettings?[keyPath: key] ?? false },
                set: { on in change(key, on, name) })
    }

    private func change<Value>(_ key: WritableKeyPath<MacSettings, Value>, _ value: Value, _ name: String) {
        model.macSettings?[keyPath: key] = value
        Task { await model.changeMacSettings([name: value]) }
    }

    private func check() async {
        checking = true
        defer { checking = false }
        do {
            let listing = try await Gateway(host: host).agents()
            let count = listing.categories.reduce(0) { $0 + $1.agents.count }
            result = "Connected. \(count) agent\(count == 1 ? "" : "s") in \(listing.categories.count) categor\(listing.categories.count == 1 ? "y" : "ies")."
        } catch {
            result = describe(error)
        }
    }
}
