import Foundation
import Observation

@MainActor
@Observable
final class WorkspaceModel {
    static let hostKey = "gatewayHost"

    var host: String {
        didSet { UserDefaults.standard.set(host, forKey: WorkspaceModel.hostKey) }
    }
    var listing: WorkspaceListing?
    var error: String?
    // Fetched ahead in the background so the new-agent sheet never waits.
    var harnesses: [Harness]?
    var machines: [Machine] = []
    var catalogs: [String: FolderCatalog] = [:] // by machine; "" is this Mac
    var catalogErrors: [String: String] = [:]
    private var fetched: [String: Date] = [:]
    private var catalogLoads: Set<String> = []
    private var prefetching = false

    init() {
        let saved = UserDefaults.standard.string(forKey: WorkspaceModel.hostKey)
        let bundled = Bundle.main.object(forInfoDictionaryKey: "LapisDefaultHost") as? String
        host = saved ?? bundled ?? ""
        if UserDefaults.standard.bool(forKey: "resetCache") { DiskCache.clear() }
        // The last known state shows at once; it is refreshed right after.
        listing = DiskCache.load(WorkspaceListing.self, cacheName("listing"))
        harnesses = DiskCache.load([Harness].self, cacheName("harnesses"))
        machines = DiskCache.load([Machine].self, cacheName("machines")) ?? []
        let names = [""] + machines.map(\.name)
        let folders = names.compactMap { name in
            DiskCache.load(FolderPayload.self, cacheName("folders-" + name)).map { (name, $0) }
        }
        Task { [weak self] in
            let built = await Task.detached {
                folders.map { FolderCatalog(machine: $0.0, payload: $0.1) }
            }.value
            for catalog in built where self?.catalogs[catalog.machine] == nil {
                self?.catalogs[catalog.machine] = catalog
            }
        }
    }

    var gateway: Gateway? { try? Gateway(host: host) }

    // Cached files belong to the Mac they came from.
    private func cacheName(_ name: String) -> String {
        String(host.lowercased().map { $0.isLetter || $0.isNumber ? $0 : "_" }) + "-" + name
    }

    func refresh() async {
        guard let gateway else {
            error = GatewayError.invalidHost.localizedDescription
            return
        }
        do {
            let current = try await gateway.agents()
            listing = current
            error = nil
            DiskCache.save(current, cacheName("listing"))
            Task { await prefetch() }
        } catch is CancellationError {
        } catch let failure as URLError where failure.code == .cancelled {
        } catch {
            self.error = describe(error)
        }
    }

    private func due(_ key: String, _ age: TimeInterval) -> Bool {
        fetched[key].map { Date().timeIntervalSince($0) > age } ?? true
    }

    // What the phone may need next: the Mac's CLIs, its ssh machines, folder
    // indexes (this Mac's and the likeliest machines'), and each running
    // agent's screen, so opening one shows it at once.
    func prefetch() async {
        guard let gateway, !prefetching else { return }
        prefetching = true
        defer { prefetching = false }
        if due("harnesses", 600), let list = try? await gateway.harnesses() {
            harnesses = list
            fetched["harnesses"] = Date()
            DiskCache.save(list, cacheName("harnesses"))
        }
        if due("machines", 120), let list = try? await gateway.machines() {
            machines = list
            fetched["machines"] = Date()
            DiskCache.save(list, cacheName("machines"))
        }
        await loadCatalog("", olderThan: 300)
        for machine in machines.filter(\.available).prefix(3) {
            await loadCatalog(machine.name, olderThan: 600)
        }
        await prefetchScreens()
    }

    // A machine's folders; only a changed index is downloaded again.
    func loadCatalog(_ machine: String, olderThan age: TimeInterval = 0) async {
        let key = "folders-" + machine
        guard let gateway, !catalogLoads.contains(machine), due(key, age) else { return }
        catalogLoads.insert(machine)
        defer { catalogLoads.remove(machine) }
        do {
            let payload = try await gateway.folders(machine: machine, have: catalogs[machine]?.version)
            fetched[key] = Date()
            catalogErrors[machine] = nil
            if payload.unchanged == true { return }
            let catalog = await Task.detached { FolderCatalog(machine: machine, payload: payload) }.value
            catalogs[machine] = catalog
            DiskCache.save(payload, cacheName(key))
        } catch {
            catalogErrors[machine] = describe(error)
        }
    }

    private func prefetchScreens() async {
        guard let gateway else { return }
        let running = (listing?.categories.flatMap(\.agents) ?? []).filter(\.running)
        for agent in running.prefix(8) where ScreenCache.shared.age(agent.id) > 20 {
            if let frame = try? await gateway.screen(agent: agent.id) {
                ScreenCache.shared.store(agent.id, frame)
            }
        }
    }
}

// The last screen seen of each agent, shown at once when it is opened again
// while the live screen connects.
@MainActor
final class ScreenCache {
    static let shared = ScreenCache()
    private var frames: [String: (frame: ScreenFrame, at: Date)] = [:]

    func frame(_ agent: String) -> ScreenFrame? { frames[agent]?.frame }

    func age(_ agent: String) -> TimeInterval {
        frames[agent].map { Date().timeIntervalSince($0.at) } ?? .infinity
    }

    func store(_ agent: String, _ frame: ScreenFrame) { frames[agent] = (frame, Date()) }
}

// Small JSON files in Application Support, so a cold launch shows the last
// known list and folder indexes without waiting for the Mac.
enum DiskCache {
    static let folder: URL = {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("cache", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }()

    static func load<T: Decodable>(_ type: T.Type, _ name: String) -> T? {
        guard let data = try? Data(contentsOf: folder.appendingPathComponent(name + ".json")) else {
            return nil
        }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    static func save<T: Encodable>(_ value: T, _ name: String) {
        guard let data = try? JSONEncoder().encode(value) else { return }
        try? data.write(to: folder.appendingPathComponent(name + ".json"), options: .atomic)
    }

    static func clear() {
        try? FileManager.default.removeItem(at: folder)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    }
}

extension WorkspaceModel {
    // Starts an agent through the Mac's lapis and waits until it runs; a CLI
    // may update itself first, for up to two minutes.
    func start(_ new: NewAgent, progress: @MainActor (String) -> Void) async throws -> Agent {
        guard let gateway else { throw GatewayError.invalidHost }
        let started = try await gateway.start(new)
        progress(started.updating ? "Updating the CLI first…" : "Starting…")
        // The folder now has one more agent; rank it again.
        Task { await loadCatalog(new.machine ?? "") }
        let deadline = Date().addingTimeInterval(150)
        while Date() < deadline {
            try Task.checkCancellation()
            let current = try await gateway.agents()
            listing = current
            if let agent = current.categories.flatMap(\.agents).first(where: { $0.id == started.id }),
               agent.running {
                return agent
            }
            try await Task.sleep(for: .milliseconds(600))
        }
        throw GatewayError.refused(0, "The agent has not started yet. It is in the list on the Mac.")
    }
}

func describe(_ error: Error) -> String {
    if let failure = error as? URLError {
        switch failure.code {
        case .cannotConnectToHost, .cannotFindHost, .timedOut, .networkConnectionLost,
             .notConnectedToInternet, .dnsLookupFailed:
            return "The Mac did not answer. Check that Tailscale is on here and the Mac is awake."
        default:
            return failure.localizedDescription
        }
    }
    return error.localizedDescription
}

// One agent shown on the phone. Showing it joins the agent's session beside
// the Mac, so both stay in sync; a session started before joining existed is
// taken from the Mac instead, until Reconnect agent there.
@MainActor
@Observable
final class AgentSession {
    enum State: Equatable {
        case connecting
        case live
        case closed(String, reopen: Bool)
    }

    let agent: Agent
    let gateway: Gateway?
    var frame: ScreenFrame?
    var state: State = .connecting
    var notice: String?
    var shared = true
    // Archived output above the live screen, oldest first and contiguous with
    // it: pages archived after loading are fetched as newer pages.
    var history: [HistoryChunk] = []
    // The oldest page is loaded. Nothing archived yet is not the start: more
    // output can still scroll off, so an empty history is retried.
    var historyEnd = false
    private(set) var loadingHistory = false
    private var lastEmptyCheck = Date.distantPast
    private var newerTask: Task<Void, Never>?
    private(set) var lastFrameJSON: Data?
    private var task: Task<Void, Never>?
    private(set) var size: (columns: Int, rows: Int)?

    private var historyPrefetched = false

    init(agent: Agent, gateway: Gateway?) {
        self.agent = agent
        self.gateway = gateway
        // The last screen seen shows at once while the live one connects.
        frame = ScreenCache.shared.frame(agent.id)
    }

    var isLive: Bool { state == .live }

    func open(columns: Int, rows: Int) {
        guard let gateway else {
            state = .closed(GatewayError.invalidHost.localizedDescription, reopen: false)
            return
        }
        task?.cancel()
        size = (columns, rows)
        state = .connecting
        history = []
        historyEnd = false
        let events = gateway.stream(agent: agent.id, columns: columns, rows: rows)
        task = Task { [weak self] in
            do {
                for try await event in events {
                    guard let self else { return }
                    switch event {
                    case let .attached(attached):
                        self.shared = attached.shared
                    case let .frame(frame, json):
                        self.frame = frame
                        self.lastFrameJSON = json
                        self.state = .live
                        ScreenCache.shared.store(self.agent.id, frame)
                        self.followNewHistory()
                        // The newest archived page, before the first scroll up asks.
                        if !self.historyPrefetched {
                            self.historyPrefetched = true
                            Task { await self.loadOlder() }
                        }
                    case let .status(status):
                        self.state = .closed(AgentSession.explain(status), reopen: status.state == "disconnected")
                        return
                    }
                }
                self?.closedByGateway()
            } catch is CancellationError {
            } catch let failure as URLError where failure.code == .cancelled {
            } catch {
                self?.state = .closed(describe(error), reopen: true)
            }
        }
    }

    private func closedByGateway() {
        if state == .live || state == .connecting {
            state = .closed("The Mac closed the connection.", reopen: true)
        }
    }

    func close() {
        task?.cancel()
        task = nil
    }

    static func explain(_ status: StreamStatus) -> String {
        switch status.state {
        case "replaced": "The Mac took this agent back. Open it here again to continue."
        case "ended": "This agent has ended."
        case "released": "This agent was opened in another view."
        case "overloaded": "The agent's session was overloaded and closed the view."
        default: status.message.isEmpty ? "The agent's session closed." : status.message
        }
    }

    func resize(columns: Int, rows: Int) {
        guard size?.columns != columns || size?.rows != rows else { return }
        size = (columns, rows)
        if isLive { send(Input(resize: [columns, rows])) }
    }

    func send(_ input: Input) {
        guard let gateway else { return }
        let id = agent.id
        Task { [weak self] in
            do {
                try await gateway.send(input, to: id)
            } catch {
                self?.notice = describe(error)
            }
        }
    }

    // Loads the page before the oldest one shown; the gateway archives what
    // scrolled off the top of the agent's terminal.
    func loadOlder() async {
        guard let gateway, isLive, !historyEnd, !loadingHistory else { return }
        loadingHistory = true
        defer { loadingHistory = false }
        // While nothing is archived, ask at most once a second, but always
        // ask again after the latest output (callers repeat on new output).
        if history.isEmpty {
            let wait = 1 - Date().timeIntervalSince(lastEmptyCheck)
            if wait > 0 { try? await Task.sleep(for: .seconds(wait)) }
        }
        // Pages can hold only a few rows; gather more than a screen per load
        // so the loaded rows move the top out of view until the next scroll.
        var before = history.first?.page ?? 0
        var gathered = 0
        var attempts = 0
        while gathered < 80 && attempts < 40 {
            attempts += 1
            guard let reply = try? await gateway.history(agent: agent.id, before: before) else { return }
            if reply.busy {
                try? await Task.sleep(for: .milliseconds(300))
                continue
            }
            if let lines = reply.lines, let columns = reply.columns, reply.page != 0 {
                history.insert(HistoryChunk(page: reply.page, columns: columns, lines: lines), at: 0)
                gathered += lines.count
                before = reply.page
                continue
            }
            if history.isEmpty {
                lastEmptyCheck = Date()
            } else {
                historyEnd = true
            }
            return
        }
    }

    // While history is shown, pages archived since it loaded are appended so
    // it stays contiguous with the live screen.
    private func followNewHistory() {
        guard !history.isEmpty, newerTask == nil else { return }
        newerTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(1))
            await self?.loadNewer()
            self?.newerTask = nil
        }
    }

    private func loadNewer() async {
        guard let gateway, isLive, let newest = history.last?.page, !loadingHistory else { return }
        loadingHistory = true
        defer { loadingHistory = false }
        var after = newest
        for _ in 0..<20 {
            guard let reply = try? await gateway.history(agent: agent.id, after: after),
                  !reply.busy, reply.page != 0, let lines = reply.lines, let columns = reply.columns
            else { return }
            history.append(HistoryChunk(page: reply.page, columns: columns, lines: lines))
            after = reply.page
        }
    }

    // Paste the message and press Enter, as typing it would.
    func submit(_ message: String) {
        if message.isEmpty {
            send(.key(.enter))
        } else {
            send(Input(paste: message, key: Key.enter.rawValue))
        }
    }
}

struct HistoryChunk: Identifiable {
    let page: UInt64
    let columns: Int
    let lines: [[Run]]
    var id: UInt64 { page }
}
