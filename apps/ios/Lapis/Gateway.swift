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

    var place: String {
        let name = (directory as NSString).lastPathComponent
        return name.isEmpty ? directory : name
    }
}

struct ScreenFrame: Decodable {
    struct Cursor: Decodable {
        let x: Int
        let y: Int
        let visible: Bool
    }

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

    init(from decoder: Decoder) throws {
        var values = try decoder.unkeyedContainer()
        text = try values.decode(String.self)
        foreground = try values.decodeIfPresent(String.self)
        background = try values.decodeIfPresent(String.self)
        flags = try values.decode(Int.self)
    }
}

struct StreamStatus: Decodable {
    let state: String
    let message: String
}

enum StreamEvent {
    case frame(ScreenFrame)
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
    case refused(Int, String)
    case unreadable

    var errorDescription: String? {
        switch self {
        case .invalidHost: "Set the Mac's Tailscale name in Settings."
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
        parts.scheme = "http"
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
                            case "frame":
                                continuation.yield(.frame(try decoder.decode(ScreenFrame.self, from: data)))
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
