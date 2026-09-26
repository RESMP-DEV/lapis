import Foundation

// The lapis gateway on the Mac (apps/remote/lapis_remote.py), reached over
// Tailscale. It admits only the Mac owner's iOS devices, so there is nothing
// to sign in to here.

struct WorkspaceListing: Codable {
    let categories: [AgentCategory]
    let activeCategory: String
}

struct AgentCategory: Codable, Identifiable {
    let id: String
    let name: String
    let agents: [Agent]

    func with(_ agents: [Agent]) -> AgentCategory {
        AgentCategory(id: id, name: name, agents: agents)
    }
}

struct Agent: Codable, Identifiable, Hashable {
    let id: String
    let title: String
    let harness: String
    let directory: String
    let running: Bool
    let onPhone: Bool
    // "" on this Mac; the ssh host otherwise. Older gateways send neither.
    let machine: String?
    // "~/dev/infinity" here, "devbox:~/lapis" on another machine.
    let place: String?

    var location: String { place ?? directory }
}

// An agent CLI the Mac can start, as its lapis reports it.
struct Harness: Codable, Identifiable, Hashable {
    let id: String
    let name: String
    let installed: Bool
    // Models the CLI lists (its default first) and the approval modes it
    // has; older Macs send none.
    let models: [ModelChoice]?
    let modes: [AgentMode]?
}

struct ModelChoice: Codable, Hashable, Identifiable {
    let id: String
    let name: String
    // The CLI's own default: started without naming a model.
    let isDefault: Bool

    enum CodingKeys: String, CodingKey {
        case id, name
        case isDefault = "default"
    }
}

struct AgentMode: Codable, Hashable, Identifiable {
    let id: String
    let name: String
}

// lapis.json's newAgent defaults: the CLI, and the folder on this Mac and on
// each ssh machine ("~/dev" style).
struct AgentDefaults: Codable {
    let harness: String?
    let folder: String?
    let mode: String?
    let machines: [String: String]?
}

// An agent to start, as a new tab in `category` on the Mac: on the Mac, or
// over ssh on `machine`.
struct NewAgent: Encodable {
    let harness: String
    let directory: String
    let category: String
    let machine: String?
    let model: String?
    let mode: String?
    // A past conversation to resume, by the CLI's own id.
    var resume: String? = nil
}

// A plain shell on the Mac or one of its ssh machines, for a quick command;
// never an agent. One per machine.
struct TerminalInfo: Codable, Identifiable, Hashable {
    let id: String
    let machine: String // "" for the Mac
    let name: String
    let running: Bool
    let onPhone: Bool

    // Opened in the same live screen as an agent.
    var asAgent: Agent {
        Agent(id: id, title: machine.isEmpty ? "Terminal" : "Terminal on \(machine)", harness: "shell",
              directory: "~", running: running, onPhone: onPhone,
              machine: machine, place: machine.isEmpty ? "this Mac" : machine)
    }
}

// A past Claude or Codex conversation on the Mac, to resume as a new agent.
struct Conversation: Codable, Identifiable, Hashable {
    let harness: String
    let id: String
    let directory: String // "dev/lapis" under home, or absolute
    let title: String
    let age: Int // seconds since it was last written
}

// An ssh host the Mac can start agents on, most used first.
struct Machine: Codable, Identifiable, Hashable {
    let name: String
    let uses: Int
    let available: Bool
    var id: String { name }
}

struct FrequentFolder: Codable, Hashable {
    let path: String
    let count: Int
}

// A machine's folders; `unchanged` when the phone already holds `version`.
struct FolderPayload: Codable {
    let version: String
    let unchanged: Bool?
    let home: String?
    let folders: [String]?
    let frequent: [FrequentFolder]?
    let harnesses: [String: String]?
    // How active each folder has been (recent and frequent agent work).
    let activity: [String: Double]?
}

// The Mac's settings the phone can change, kept in lapis.json there: staying
// awake so the phone can reach it, its alerts, and its plan usage meter. How
// the Mac's window looks is set on the Mac.
struct MacSettings: Codable, Equatable {
    var keepAwake: Bool
    var alertSound: Bool
    var alertRepeat: Int
    var finishSound: Bool
    var notify: Bool
    var showUsage: Bool
}

struct StartedAgent: Decodable {
    let id: String
    // The CLI updates itself before the agent starts.
    let updating: Bool
}

struct ScreenFrame: Decodable {
    struct Cursor: Decodable {
        let x: Int
        let y: Int
        let visible: Bool
    }

    let revision: UInt64?
    let columns: Int
    let rows: Int
    let cursor: Cursor
    let alternateScreen: Bool
    let applicationCursor: Bool
    let foreground: String
    let background: String
    let lines: [[Run]]

    var text: String {
        lines.map { line in line.map(\.text).joined() }.joined(separator: "\n")
    }
}

// One styled stretch of a row: [text, foreground, background, flags].
struct Run: Decodable {
    static let bold = 1, italic = 2, faint = 4, underline = 8, strike = 16, cursor = 32

    let text: String
    let foreground: String?
    let background: String?
    let flags: Int
    // Starting column and width in cells; absent from older gateways.
    let column: Int?
    let width: Int?

    init(text: String, foreground: String?, background: String?, flags: Int, column: Int?, width: Int?) {
        (self.text, self.foreground, self.background, self.flags, self.column, self.width) =
            (text, foreground, background, flags, column, width)
    }

    init(from decoder: Decoder) throws {
        var values = try decoder.unkeyedContainer()
        text = try values.decode(String.self)
        foreground = try values.decodeIfPresent(String.self)
        background = try values.decodeIfPresent(String.self)
        flags = try values.decode(Int.self)
        column = values.isAtEnd ? nil : try values.decode(Int.self)
        width = values.isAtEnd ? nil : try values.decode(Int.self)
    }
}

// An archived page of output above the live screen, oldest first on screen.
struct HistoryPage: Decodable {
    let page: UInt64
    let message: String
    let end: Bool
    let busy: Bool
    let columns: Int?
    let lines: [[Run]]?
}

struct StreamStatus: Decodable {
    let state: String
    let message: String
}

struct Attached: Decodable {
    // False when the agent's service predates joining, so the phone took the
    // agent from the Mac instead of showing it alongside.
    let shared: Bool
}

enum StreamEvent {
    case attached(Attached)
    // The decoded screen and the exact JSON it came from (sent with captures).
    case frame(ScreenFrame, Data)
    case status(StreamStatus)
}

enum Key: String {
    case up, down, left, right, home, end, pageUp, pageDown, delete, enter, tab, backspace, escape
}

struct Input: Encodable {
    var text: String?
    var paste: String?
    var key: String?
    var modifiers: Int?
    var resize: [Int]?

    static func key(_ key: Key, shift: Bool = false) -> Input {
        Input(key: key.rawValue, modifiers: shift ? 1 : 0)
    }
}

enum GatewayError: LocalizedError {
    case invalidHost
    case unsupportedScheme(String)
    case refused(Int, String)
    case unreadable

    var errorDescription: String? {
        switch self {
        case .invalidHost: "Set the Mac's Tailscale name in Settings."
        case let .unsupportedScheme(scheme): "Unsupported gateway URL scheme: \(scheme). Use http or https."
        case let .refused(code, message): message.isEmpty ? "The Mac answered \(code)." : message
        case .unreadable: "The Mac's answer could not be read."
        }
    }
}

struct Gateway {
    static let defaultPort = 7349

    let base: URL

    // "mac.tailnet.ts.net", "mac.tailnet.ts.net:7349" or a URL.
    init(host: String) throws {
        var text = host.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { throw GatewayError.invalidHost }
        if !text.contains("://") { text = "http://" + text }
        guard var parts = URLComponents(string: text), let name = parts.host, !name.isEmpty else {
            throw GatewayError.invalidHost
        }
        guard let scheme = parts.scheme?.lowercased(), scheme == "http" || scheme == "https" else {
            throw GatewayError.unsupportedScheme(parts.scheme ?? "")
        }
        parts.scheme = scheme
        parts.port = parts.port ?? Gateway.defaultPort
        parts.path = ""
        guard let url = parts.url else { throw GatewayError.invalidHost }
        base = url
    }

    private static let requests: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        configuration.waitsForConnectivity = false
        return URLSession(configuration: configuration)
    }()

    // The gateway pings every 10 seconds, so a quiet screen is not a timeout.
    private static let streams: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 40
        configuration.timeoutIntervalForResource = 7 * 24 * 3600
        configuration.waitsForConnectivity = false
        return URLSession(configuration: configuration)
    }()

    private func request(_ path: String, query: [URLQueryItem] = []) -> URLRequest {
        var parts = URLComponents(url: base.appendingPathComponent(path), resolvingAgainstBaseURL: false)!
        if !query.isEmpty { parts.queryItems = query }
        var request = URLRequest(url: parts.url!)
        request.setValue("ios", forHTTPHeaderField: "X-Lapis-Client")
        request.cachePolicy = .reloadIgnoringLocalCacheData
        return request
    }

    private static func check(_ response: URLResponse, _ data: Data) throws {
        guard let http = response as? HTTPURLResponse else { throw GatewayError.unreadable }
        guard http.statusCode == 200 else {
            let body = try? JSONDecoder().decode([String: String].self, from: data)
            throw GatewayError.refused(http.statusCode, body?["error"] ?? "")
        }
    }

    func agents() async throws -> WorkspaceListing {
        let (data, response) = try await Gateway.requests.data(for: request("api/agents"))
        try Gateway.check(response, data)
        return try JSONDecoder().decode(WorkspaceListing.self, from: data)
    }

    func harnesses() async throws -> (harnesses: [Harness], defaults: AgentDefaults?) {
        struct Listing: Decodable {
            let harnesses: [Harness]
            let defaults: AgentDefaults?
        }
        let (data, response) = try await Gateway.requests.data(for: request("api/harnesses"))
        try Gateway.check(response, data)
        let listing = try JSONDecoder().decode(Listing.self, from: data)
        return (listing.harnesses, listing.defaults)
    }

    func start(_ agent: NewAgent) async throws -> StartedAgent {
        var request = request("api/agents")
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(agent)
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        return try JSONDecoder().decode(StartedAgent.self, from: data)
    }

    // A new category on the Mac; its id.
    func createCategory(named name: String) async throws -> String {
        struct Made: Decodable { let id: String }
        var request = request("api/categories")
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(["name": name])
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        return try JSONDecoder().decode(Made.self, from: data).id
    }

    // Names the agent on the Mac; the name stays over its conversation's title.
    func rename(agent: String, title: String) async throws {
        var request = request("api/agents/\(agent)/rename")
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(["title": title])
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
    }

    // Ends the agent on the Mac, as Command-W does there.
    func close(agent: String) async throws {
        var request = request("api/agents/\(agent)/close")
        request.httpMethod = "POST"
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
    }

    // The Mac's quick-command terminals.
    func terminals() async throws -> [TerminalInfo] {
        struct Listing: Decodable { let terminals: [TerminalInfo] }
        let (data, response) = try await Gateway.requests.data(for: request("api/terminals"))
        try Gateway.check(response, data)
        return try JSONDecoder().decode(Listing.self, from: data).terminals
    }

    // The machine's terminal ("" is the Mac), started there when it has none; its id.
    func openTerminal(machine: String) async throws -> String {
        struct Opened: Decodable { let id: String }
        var request = request("api/terminals")
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(["machine": machine])
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        return try JSONDecoder().decode(Opened.self, from: data).id
    }

    // The Mac's recent Claude and Codex conversations, newest first.
    func conversations() async throws -> [Conversation] {
        struct Listing: Decodable { let conversations: [Conversation] }
        let (data, response) = try await Gateway.requests.data(for: request("api/conversations"))
        try Gateway.check(response, data)
        return try JSONDecoder().decode(Listing.self, from: data).conversations
    }

    func machines() async throws -> [Machine] {
        struct Listing: Decodable { let machines: [Machine] }
        let (data, response) = try await Gateway.requests.data(for: request("api/machines"))
        try Gateway.check(response, data)
        return try JSONDecoder().decode(Listing.self, from: data).machines
    }

    // Another machine's first report is read over ssh, so allow it time.
    func folders(machine: String, have: String?) async throws -> FolderPayload {
        var query: [URLQueryItem] = []
        if !machine.isEmpty { query.append(URLQueryItem(name: "machine", value: machine)) }
        if let have { query.append(URLQueryItem(name: "have", value: have)) }
        var request = request("api/folders", query: query)
        request.timeoutInterval = 45
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        return try JSONDecoder().decode(FolderPayload.self, from: data)
    }

    // The agent's current screen, read without resizing it.
    func screen(agent: String) async throws -> ScreenFrame {
        let (data, response) = try await Gateway.requests.data(for: request("api/agents/\(agent)/screen"))
        try Gateway.check(response, data)
        return try JSONDecoder().decode(ScreenFrame.self, from: data)
    }

    func send(_ input: Input, to agent: String) async throws {
        var request = request("api/agents/\(agent)/input")
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(input)
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
    }

    // The page before `before` (0: the newest), or with `after`, the page after it.
    func history(agent: String, before: UInt64 = 0, after: UInt64? = nil) async throws -> HistoryPage {
        let item = after.map { URLQueryItem(name: "after", value: String($0)) }
            ?? URLQueryItem(name: "before", value: String(before))
        let request = request("api/agents/\(agent)/history", query: [item])
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        return try JSONDecoder().decode(HistoryPage.self, from: data)
    }

    // A change to the Mac's workspace or settings, as JSON; the Mac's answer.
    @discardableResult
    private func post(_ path: String, _ body: [String: Any] = [:]) async throws -> Data {
        var request = request(path)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        return data
    }

    func renameCategory(_ id: String, to name: String) async throws {
        try await post("api/categories/\(id)/rename", ["name": name])
    }

    // Only an empty category goes, and one always stays; the Mac says why not.
    func removeCategory(_ id: String) async throws {
        try await post("api/categories/\(id)/remove")
    }

    func placeCategory(_ id: String, at index: Int) async throws {
        try await post("api/categories/\(id)/place", ["index": index])
    }

    // Puts the agent at `index` among the category's other agents, or last.
    func place(agent: String, category: String, at index: Int?) async throws {
        var body: [String: Any] = ["category": category]
        if let index { body["index"] = index }
        try await post("api/agents/\(agent)/place", body)
    }

    // Starts a stopped agent again, resuming its conversation where its CLI can.
    func restart(agent: String) async throws {
        try await post("api/agents/\(agent)/restart")
    }

    func settings() async throws -> MacSettings {
        struct Reply: Decodable { let settings: MacSettings }
        let (data, response) = try await Gateway.requests.data(for: request("api/settings"))
        try Gateway.check(response, data)
        return try JSONDecoder().decode(Reply.self, from: data).settings
    }

    // Changes some settings; the answer is all of them.
    func change(settings changes: [String: Any]) async throws -> MacSettings {
        struct Reply: Decodable { let settings: MacSettings }
        return try JSONDecoder().decode(Reply.self, from: try await post("api/settings", changes)).settings
    }

    // A screenshot and what the phone drew, saved on the Mac for debugging.
    func capture(_ body: [String: Any]) async throws -> String {
        var request = request("api/captures")
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, response) = try await Gateway.requests.data(for: request)
        try Gateway.check(response, data)
        let reply = try JSONDecoder().decode([String: String].self, from: data)
        return reply["saved"] ?? ""
    }

    // Server-sent events: each data line is one complete JSON event.
    func stream(agent: String, columns: Int, rows: Int) -> AsyncThrowingStream<StreamEvent, Error> {
        let request = request(
            "api/agents/\(agent)/stream",
            query: [URLQueryItem(name: "columns", value: String(columns)),
                    URLQueryItem(name: "rows", value: String(rows))])
        return AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    let (bytes, response) = try await Gateway.streams.bytes(for: request)
                    if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                        var data = Data()
                        for try await byte in bytes {
                            data.append(byte)
                            if data.count > 4096 { break }
                        }
                        try Gateway.check(response, data)
                    }
                    let decoder = JSONDecoder()
                    var name = ""
                    for try await line in bytes.lines {
                        if line.hasPrefix("event: ") {
                            name = String(line.dropFirst(7))
                        } else if line.hasPrefix("data: ") {
                            let data = Data(line.dropFirst(6).utf8)
                            switch name {
                            case "attached":
                                continuation.yield(.attached(try decoder.decode(Attached.self, from: data)))
                            case "frame":
                                continuation.yield(.frame(try decoder.decode(ScreenFrame.self, from: data), data))
                            case "status":
                                continuation.yield(.status(try decoder.decode(StreamStatus.self, from: data)))
                            default:
                                break
                            }
                        }
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
