import SwiftUI

// The Mac's categories, arranged from here as on the Mac: drag one to order
// them, tap one to rename it, swipe an empty one away, or add one. The Mac
// keeps its rules: a category goes only once its agents are moved out, and
// one always stays.
struct CategoriesView: View {
    @Environment(WorkspaceModel.self) private var model
    @State private var renaming: AgentCategory?
    @State private var name = ""
    @State private var adding = false

    private var categories: [AgentCategory] { model.listing?.categories ?? [] }

    var body: some View {
        // Callbacks run after refreshes and optimistic edits can replace the
        // listing. Resolve their rendered offsets against this same snapshot.
        let shown = categories
        List {
            Section {
                if let error = model.error {
                    Text(error)
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .accessibilityIdentifier("workspaceError")
                }
                ForEach(shown) { category in
                    Button {
                        name = category.name
                        renaming = category
                    } label: {
                        HStack {
                            Text(category.name)
                                .foregroundStyle(.white)
                            Spacer()
                            Text(count(category))
                                .font(.system(size: 12, design: .monospaced))
                                .foregroundStyle(Theme.quiet)
                        }
                    }
                    .accessibilityIdentifier("edit-category-\(category.name)")
                    .listRowBackground(Theme.panel)
                    .deleteDisabled(!category.agents.isEmpty || shown.count == 1)
                }
                .onMove { from, to in
                    guard let first = from.first,
                          shown.indices.contains(first),
                          to >= 0, to <= shown.count else { return }
                    // SwiftUI counts the source row in `to`; the Mac inserts
                    // after removing it, as move(fromOffsets:toOffset:) does.
                    let moving = shown[first]
                    let index = to > first ? to - 1 : to
                    Task { await model.placeCategory(moving.id, at: index) }
                }
                .onDelete { offsets in
                    for offset in offsets where shown.indices.contains(offset) {
                        let category = shown[offset]
                        Task { await model.removeCategory(category.id) }
                    }
                }
            } footer: {
                Text("Drag to order, tap to rename. A category can be removed once its agents are moved out, and one always stays.")
            }
            Section {
                Button {
                    name = ""
                    adding = true
                } label: {
                    Label("New category", systemImage: "folder.badge.plus")
                }
                .accessibilityIdentifier("addCategory")
                .listRowBackground(Theme.panel)
            }
        }
        .environment(\.editMode, .constant(.active))
        .scrollContentBackground(.hidden)
        .background(Theme.background.ignoresSafeArea())
        .navigationTitle("Categories")
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(Theme.background, for: .navigationBar)
        .alert("Rename category", isPresented: Binding(get: { renaming != nil },
                                                       set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $name)
                .accessibilityIdentifier("categoryNewName")
            Button("Save") {
                let chosen = name.trimmingCharacters(in: .whitespaces)
                if let category = renaming, !chosen.isEmpty, chosen != category.name {
                    Task { await model.renameCategory(category.id, to: String(chosen.prefix(80))) }
                }
                renaming = nil
            }
            .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
            Button("Cancel", role: .cancel) { renaming = nil }
        }
        .alert("New category", isPresented: $adding) {
            NewCategoryFields(name: $name) { _ in }
        }
        .modifier(MacNotice())
        .task { await model.refresh() }
    }

    private func count(_ category: AgentCategory) -> String {
        category.agents.isEmpty ? "EMPTY" : "\(category.agents.count) AGENT\(category.agents.count == 1 ? "" : "S")"
    }
}

// Why the Mac refused a change made here.
struct MacNotice: ViewModifier {
    @Environment(WorkspaceModel.self) private var model

    func body(content: Content) -> some View {
        content.alert("lapis",
                      isPresented: Binding(get: { model.notice != nil },
                                           set: { if !$0 { model.notice = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.notice ?? "")
        }
    }
}
