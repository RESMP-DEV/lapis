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

    init() {
        let saved = UserDefaults.standard.string(forKey: WorkspaceModel.hostKey)
        let bundled = Bundle.main.object(forInfoDictionaryKey: "LapisDefaultHost") as? String
        host = saved ?? bundled ?? ""
    }

    var gateway: Gateway? { try? Gateway(host: host) }

    func refresh() async {
        guard let gateway else {
            error = GatewayError.invalidHost.localizedDescription
            return
        }
        do {
            listing = try await gateway.agents()
            error = nil
        } catch is CancellationError {
        } catch let failure as URLError where failure.code == .cancelled {
        } catch {
            self.error = describe(error)
        }
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

    init(agent: Agent, gateway: Gateway?) {
        self.agent = agent
        self.gateway = gateway
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
                        self.followNewHistory()
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
