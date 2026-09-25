import Foundation

// The lapis gateway on the Mac (apps/remote/lapis_remote.py), reached over
// Tailscale. It admits only the Mac owner's iOS devices, so there is nothing
// to sign in to here.

struct WorkspaceListing: Decodable {
    let categories: [AgentCategory]
    let activeCategory: String
}

struct AgentCategory: Decodable, Identifiable {
    let id: String
    let name: String
    let agents: [Agent]
}

struct Agent: Decodable, Identifiable, Hashable {
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
