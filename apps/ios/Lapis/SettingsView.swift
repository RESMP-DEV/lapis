import SwiftUI

struct SettingsView: View {
    @Environment(WorkspaceModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var host = ""
    @State private var result: String?
    @State private var checking = false

    var body: some View {
        NavigationStack {
            Form {
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
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        model.host = host.trimmingCharacters(in: .whitespacesAndNewlines)
                        dismiss()
                        Task { await model.refresh() }
                    }
                }
            }
            .onAppear { host = model.host }
        }
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
