import Foundation

// One machine's folders as the gateway reports them, prepared once so that
// browsing and fuzzy search run on the phone without waiting on the network.
// Paths are relative to that machine's home ("dev/lapis"; "" is home) or
// absolute when they lie outside it.
final class FolderCatalog: Sendable {
    let machine: String // "" for this Mac
    let version: String
    let frequent: [FrequentFolder]
    // The CLIs found on another machine, by harness id; nil for this Mac.
    let harnesses: [String: String]?
    let paths: [String]
    private let lowered: [[UInt8]]
    private let nameStart: [Int]
    private let boost: [Int]
    private let children: [String: [String]]

    init(machine: String, payload: FolderPayload) {
        self.machine = machine
        version = payload.version
        frequent = payload.frequent ?? []
        harnesses = payload.harnesses
        let paths = payload.folders ?? []
        self.paths = paths
        lowered = paths.map { Array($0.lowercased().utf8) }
        nameStart = paths.map { path in
            path.lastIndex(of: "/").map { path.utf8.distance(from: path.startIndex, to: $0) + 1 } ?? 0
        }
        let counts = Dictionary(frequent.map { ($0.path, $0.count) }, uniquingKeysWith: max)
        boost = paths.map { min(counts[$0] ?? 0, 40) }
        var children: [String: [String]] = [:]
        for path in paths {
            let parent = path.lastIndex(of: "/").map { String(path[..<$0]) } ?? ""
            children[parent, default: []].append(path)
        }
        // Visible folders alphabetically, then hidden ones, also alphabetically.
        self.children = children.mapValues { list in
            list.sorted { left, right in
                let (a, b) = (FolderCatalog.name(left), FolderCatalog.name(right))
                let (hiddenA, hiddenB) = (a.hasPrefix("."), b.hasPrefix("."))
                if hiddenA != hiddenB { return !hiddenA }
                return a.localizedCaseInsensitiveCompare(b) == .orderedAscending
            }
        }
    }

    static func name(_ path: String) -> String {
        path.split(separator: "/").last.map(String.init) ?? path
    }

    func children(of folder: String) -> [String] { children[folder] ?? [] }

    func hasChildren(_ folder: String) -> Bool { children[folder] != nil }

    // Folders matching the letters of `query` in order: every match (to
    // narrow the next, longer query) and the best `limit`, best first. Matches
    // at word starts, runs of letters and letters in the folder's own name
    // score higher, as do folders with more agents started in them; shorter
    // paths win ties.
    func search(_ query: String, among: [Int]? = nil, limit: Int = 60) -> (all: [Int], best: [Int]) {
        let needle = Array(query.lowercased().utf8).filter { $0 != 0x20 }
        guard !needle.isEmpty else { return ([], []) }
        var scored: [(index: Int, score: Int)] = []
        for index in among ?? Array(paths.indices) {
            if let score = FolderCatalog.score(needle, lowered[index], name: nameStart[index]) {
                scored.append((index, score + boost[index]))
            }
        }
        let all = scored.map(\.index)
        scored.sort { $0.score != $1.score ? $0.score > $1.score : paths[$0.index].count < paths[$1.index].count }
        return (all, scored.prefix(limit).map(\.index))
    }

    static func score(_ needle: [UInt8], _ haystack: [UInt8], name: Int) -> Int? {
        // Letters found in the folder's own name are worth most.
        if let inName = walk(needle, haystack, from: name) {
            return inName + 24 - haystack.count / 10
        }
        return walk(needle, haystack, from: 0).map { $0 - haystack.count / 10 }
    }

    private static func walk(_ needle: [UInt8], _ haystack: [UInt8], from start: Int) -> Int? {
        var position = start
        var score = 0
        var previous = -2
        for letter in needle {
            while position < haystack.count && haystack[position] != letter { position += 1 }
            if position == haystack.count { return nil }
            score += 1
            if position == 0 || boundary(haystack[position - 1]) { score += 8 }
            if position == previous + 1 { score += 6 }
            previous = position
            position += 1
        }
        return score
    }

    private static func boundary(_ byte: UInt8) -> Bool {
        byte == 0x2F || byte == 0x2D || byte == 0x5F || byte == 0x2E || byte == 0x20 // / - _ . space
    }
}
