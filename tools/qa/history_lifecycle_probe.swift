import Foundation

// Exercises the production AgentSession without UIKit or a simulator. The
// Python peer deliberately holds old requests until a new attachment is
// established, then releases them; assertions observe the resulting model.
@main
struct HistoryLifecycleProbe {
    private static let control: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.waitsForConnectivity = false
        return URLSession(configuration: configuration)
    }()

    struct Counts: Decodable {
        let streams: Int
        let history: Int
        let frames: Int
        let machines: Int
        let folders: Int
        let revisions: [Int]
        let input: Int?
        let terminals: Int?
    }

    // Advances from a zero reference while using ContinuousClock for the
    // actual suspension, so timing assertions are deterministic and Task
    // cancellation is exercised rather than simulated.
    final class ControlledHistoryClock: HistoryClock, @unchecked Sendable {
        private let clock = ContinuousClock()
        private let started = ContinuousClock.Instant.now
        private(set) var sleeps: [TimeInterval] = []
        private(set) var cancellationObserved = false

        func now() -> Date {
            let elapsed = clock.now - started
            let (seconds, attoseconds) = elapsed.components
            return Date(timeIntervalSinceReferenceDate:
                            Double(seconds) + Double(attoseconds) / 1e18)
        }

        func sleep(for interval: TimeInterval) async throws {
            sleeps.append(interval)
            do {
                try await clock.sleep(for: .seconds(interval))
            } catch {
                if error is CancellationError { cancellationObserved = true }
                throw error
            }
        }
    }

    static func request(_ host: String, _ path: String) async throws -> Data {
        var url = URL(string: "http://\(host)/\(path)")!
        url.append(queryItems: [URLQueryItem(name: "nonce", value: UUID().uuidString)])
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        let (data, response) = try await Self.control.data(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw NSError(domain: "history-lifecycle-probe", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "control endpoint failed: \(path)"])
        }
        return data
    }

    static func counts(_ host: String, _ key: String) async throws -> Counts {
        try JSONDecoder().decode(Counts.self, from: await request(host, "state/\(key)"))
    }

    static func waitCounts(_ host: String, _ key: String,
                           _ counter: KeyPath<Counts, Int>, atLeast minimum: Int) async throws {
        let deadline = Date().addingTimeInterval(5)
        while true {
            if try await counts(host, key)[keyPath: counter] >= minimum { return }
            guard Date() < deadline else {
                throw NSError(domain: "history-lifecycle-probe", code: 3,
                              userInfo: [NSLocalizedDescriptionKey: "timeout waiting \(key) \(counter) >= \(minimum)"])
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    static func waitControl(_ host: String, _ path: String) async throws {
        _ = try await request(host, path)
    }

    @MainActor
    static func waitModel(_ description: String, until condition: @MainActor () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(5)
        while !condition() {
            guard Date() < deadline else {
                throw NSError(domain: "history-lifecycle-probe", code: 4,
                              userInfo: [NSLocalizedDescriptionKey: "timeout waiting \(description)"])
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    static func require(_ condition: Bool, _ description: String) throws {
        guard condition else {
            throw NSError(domain: "history-lifecycle-probe", code: 5,
                          userInfo: [NSLocalizedDescriptionKey: description])
        }
    }

    @MainActor
    static func session(_ id: String, host: String) throws -> AgentSession {
        let agent = Agent(id: id, title: id, harness: "fixture", directory: "",
                          running: true, onPhone: false, machine: nil, place: nil)
        return AgentSession(agent: agent, gateway: try Gateway(host: host))
    }

    @MainActor
    static func nextRevision(_ host: String, _ key: String) async throws -> UInt64 {
        UInt64(try await counts(host, key).frames + 1)
    }

    @MainActor
    static func openAttachment(
        _ session: AgentSession,
        host: String,
        key: String,
        revision: UInt64
    ) async throws {
        let stream = try await counts(host, key).streams + 1
        session.open(columns: 80, rows: 24)
        try await waitControl(host, "wait-stream/\(key)/\(stream)")
        try await waitModel("frame \(revision)") { session.frame?.revision == revision }
    }

    @MainActor
    static func pageNumbers(_ session: AgentSession) -> [UInt64] {
        session.history.map(\.page)
    }

    static func listingCalls(_ host: String, _ workspace: String) async throws -> Int {
        struct Counts: Decodable { let listings: Int }
        let data = try await request(host, "state-listing/\(workspace)")
        return try JSONDecoder().decode(Counts.self, from: data).listings
    }

    static func waitListingCalls(_ host: String, _ workspace: String,
                                 atLeast minimum: Int) async throws {
        let deadline = Date().addingTimeInterval(5)
        while true {
            if try await listingCalls(host, workspace) >= minimum { return }
            guard Date() < deadline else {
                throw NSError(domain: "history-lifecycle-probe", code: 3,
                              userInfo: [NSLocalizedDescriptionKey:
                                    "timeout waiting listing \(workspace) >= \(minimum)"])
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    @MainActor
    static func workspaceAtoBtoA(_ host: String, cache: DiskCache, preferences: UserDefaults) async throws -> Bool {
        try await waitControl(host, "workspace-active/held")
        let model = WorkspaceModel(preferences: preferences, cache: cache)
        model.host = host
        let refresh = Task.detached { await model.refresh() }
        try await waitControl(host, "wait-listing/held/1")

        // The host returns to its original spelling, so comparing gateway
        // identity cannot distinguish this replacement from the old work.
        model.host = "replacement.invalid"
        model.host = host
        try require(model.listing == nil && model.machines.isEmpty,
                    "host change did not clear old workspace state")
        try await waitControl(host, "release-listing/held/1")
        try await waitControl(host, "wait-listing-done/held/1")
        _ = await refresh.value
        let rejected = model.listing?.activeCategory != "held"
        try require(rejected, "old-host listing survived an A-to-B-to-A host change")

        try await waitControl(host, "workspace-active/base")
        await model.refresh()
        try await waitListingCalls(host, "base", atLeast: 1)
        try await waitModel("replacement-host workspace applied") {
            model.listing?.activeCategory == "base" && model.machines.first?.name == "base-machine"
        }
        let previous = model.listing?.activeCategory
        model.error = "pending workspace notice"
        model.host = host
        try require(model.listing?.activeCategory == previous && model.error == "pending workspace notice",
                    "saving the same host cleared live workspace state")
        // This spelling collided under the old punctuation-to-underscore key.
        model.host = String(host.map { $0.isLetter || $0.isNumber ? $0 : "_" })
        try require(model.listing == nil, "distinct hosts shared a disk-cache entry")
        model.host = host
        try require(model.listing?.activeCategory == previous,
                    "original host cache did not restore its listing")
        return rejected
    }

    @MainActor
    static func workspaceTerminalsAtoBtoA(_ host: String, cache: DiskCache, preferences: UserDefaults) async throws {
        try await waitControl(host, "workspace-active/held-terminals")
        let terminalCall = (try await counts(host, "terminals").terminals ?? 0) + 1
        let isolated = DiskCache(folder: cache.folder.appendingPathComponent("terminals"))
        let model = WorkspaceModel(preferences: preferences, cache: isolated)
        model.host = host
        let refresh = Task.detached { await model.refresh() }
        try await waitControl(host, "wait-terminals/terminals/\(terminalCall)")
        model.host = "replacement.invalid"
        model.host = host
        try await waitControl(host, "release-terminals/terminals/\(terminalCall)")
        try await waitControl(host, "wait-terminals-done/terminals/\(terminalCall)")
        _ = await refresh.value
        try require(model.listing == nil && model.terminals.isEmpty,
                    "old terminal response changed replacement workspace state")
        let files = (try? FileManager.default.contentsOfDirectory(
            at: isolated.folder, includingPropertiesForKeys: nil)) ?? []
        try require(files.isEmpty,
                    "old-host refresh wrote cache after a terminal-owned await")
        try require(model.machines.isEmpty,
                    "old terminal response scheduled replacement-host prefetch")
        try await waitControl(host, "workspace-active/base")
    }

    @MainActor
    static func independentPrefetchAfterMachinesFailure(
        _ host: String, cache: DiskCache, preferences: UserDefaults
    ) async throws {
        try await waitControl(host, "workspace-active/machine-failure")
        let isolated = DiskCache(folder: cache.folder.appendingPathComponent("machine-failure"))
        let model = WorkspaceModel(preferences: preferences, cache: isolated)
        model.host = host
        let refresh = Task.detached { await model.refresh() }
        defer { refresh.cancel() }
        try await waitCounts(host, "machine-failure", \.machines, atLeast: 1)
        try await waitCounts(host, "machine-failure", \.folders, atLeast: 1)
        try await waitCounts(host, "machine-failure", \.frames, atLeast: 1)
        _ = await refresh.value
        try await waitModel("catalog and cached screen after machines failure") {
            model.catalogs[""]?.version == "machine-failure"
                && ScreenCache.shared.frame("machine-failure")?.revision == 1
        }
        try await waitControl(host, "workspace-active/base")
    }

    @MainActor
    static func staleInputAcrossAttachment(_ host: String) async throws {
        let input = try session("input", host: host)
        try await openAttachment(input, host: host, key: "input", revision: 1)
        input.send(.key(.enter))
        try await waitControl(host, "wait-input/input/1")

        input.close()
        let replacementRevision = try await nextRevision(host, "input")
        try await openAttachment(input, host: host, key: "input", revision: replacementRevision)
        input.send(.key(.enter))
        try await waitControl(host, "wait-input/input/2")
        try await waitControl(host, "release-input/input/1")
        try await waitControl(host, "wait-input-done/input/1")
        try await waitModel("old input completion left replacement sender owned") {
            input.inputPending
        }

        // With an unguarded old defer, this call starts a duplicate sender.
        input.send(.key(.enter))
        try await waitControl(host, "release-input/input/2")
        try await waitControl(host, "wait-input-done/input/2")
        try await waitControl(host, "wait-input/input/3")
        try await waitControl(host, "wait-input-done/input/3")
        try await waitModel("queued replacement input settled") { !input.inputPending }
        let counts = try await counts(host, "input")
        try require(counts.input == 3,
                    "old input completion duplicated the replacement sender")
        input.close()
    }

    @MainActor
    static func main() async {
        do {
            try await run()
        } catch {
            FileHandle.standardError.write(Data("history-lifecycle-probe: \(error.localizedDescription)\n".utf8))
            exit(1)
        }
    }

    @MainActor
    static func run() async throws {
        guard CommandLine.arguments.count == 4 else {
            throw NSError(domain: "history-lifecycle-probe", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "usage: history-lifecycle-probe HOST:PORT CACHE_DIRECTORY DEFAULTS_SUITE"])
        }
        let host = CommandLine.arguments[1]
        let cache = DiskCache(folder: URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true))
        let suite = CommandLine.arguments[3]
        guard let preferences = UserDefaults(suiteName: suite) else {
            throw NSError(domain: "history-lifecycle-probe", code: 6)
        }
        defer { preferences.removePersistentDomain(forName: suite) }

        // A controlled old-host answer is released only after the model has
        // moved to another host and back to the original spelling.
        let workspaceRejected = try await workspaceAtoBtoA(host, cache: cache, preferences: preferences)
        try await workspaceTerminalsAtoBtoA(host, cache: cache, preferences: preferences)
        try await independentPrefetchAfterMachinesFailure(
            host, cache: cache, preferences: preferences
        )

        // The existing input retirement contract, with both requests held so
        // the old completion cannot race the replacement sender.
        try await staleInputAcrossAttachment(host)

        // A caller-owned older-history request survives a close/reopen in the
        // transport, but its completion belongs only to the old generation.
        let stale = try session("stale", host: host)
        try await openAttachment(stale, host: host, key: "stale", revision: 1)
        try await waitControl(host, "release-history/stale/1")
        try await waitControl(host, "wait-history-done/stale/1")
        try await waitModel("first page loaded") { pageNumbers(stale) == [10] && !stale.loadingHistory }

        let staleOlder = Task { await stale.loadOlder() }
        try await waitControl(host, "wait-history/stale/2")
        try await waitModel("old request owns loading") { stale.loadingHistory }
        stale.close()
        var replacementRevision = try await nextRevision(host, "stale")
        try await openAttachment(stale, host: host, key: "stale", revision: replacementRevision)
        try await waitCounts(host, "stale", \.history, atLeast: 3)
        try require(stale.loadingHistory && stale.history.isEmpty,
                    "replacement request did not own loading while held")
        try await waitControl(host, "release-history/stale/2")
        try await waitControl(host, "wait-history-done/stale/2")
        await staleOlder.value
        try require(!stale.history.contains(where: { $0.page == 77 }),
                    "old attachment's page 77 entered the replacement history")
        try require(stale.loadingHistory, "old completion cleared replacement loading state")
        try await waitControl(host, "release-history/stale/3")
        try await waitControl(host, "wait-history-done/stale/3")
        try await waitModel("replacement history loaded") {
            pageNumbers(stale) == [9] && !stale.loadingHistory
        }

        // Trigger the tracked newer path, replace the attachment while that
        // request is outstanding, then release an obsolete page 98 response.
        let followFrameRevision = UInt64(try await counts(host, "stale").frames + 1)
        try await waitControl(host, "frame/stale/2")
        try await waitModel("follow-up frame observed") { stale.frame?.revision == followFrameRevision }
        try await waitControl(host, "wait-history/stale/4")
        try await waitCounts(host, "stale", \.history, atLeast: 4)
        try await waitModel("old newer request owns loading") { stale.loadingHistory }
        stale.close()
        replacementRevision = try await nextRevision(host, "stale")
        try await openAttachment(stale, host: host, key: "stale", revision: replacementRevision)
        try await waitCounts(host, "stale", \.history, atLeast: 5)
        try await waitControl(host, "wait-history-done/stale/5")
        try await waitModel("second replacement loaded") { !stale.loadingHistory }
        try await waitControl(host, "release-history/stale/4")
        try await waitControl(host, "wait-history-done/stale/4")
        try await Task.sleep(for: .milliseconds(250))
        try require(!stale.history.contains(where: { $0.page == 98 }),
                    "old attachment's newer page 98 entered the replacement history")
        stale.close()

        // The first eligible frame after quiet time catches up immediately.
        // The next frame in that interval waits exactly the remainder, and a
        // stale close cancels that pending request before it reaches the wire.
        let pacingClock = ControlledHistoryClock()
        let pacing = try session("pacing", host: host)
        pacing.historyClock = pacingClock
        try await openAttachment(pacing, host: host, key: "pacing", revision: 1)
        try await waitControl(host, "wait-history-done/pacing/1")
        try await waitControl(host, "frame/pacing/1")
        try await waitModel("first eligible frame observed") {
            pacing.frame?.revision == 2
        }
        try await waitControl(host, "wait-history-done/pacing/2")
        try await waitModel("idle catch-up loaded") {
            pageNumbers(pacing) == [10, 11] && !pacing.loadingHistory
        }
        try require(pacingClock.sleeps.isEmpty,
                    "first eligible idle catch-up was delayed")
        let pacingIdleImmediate = pacingClock.sleeps.isEmpty

        try await waitControl(host, "frame/pacing/2")
        try await waitModel("remainder request observed") {
            pacing.frame?.revision == 3
        }
        try await waitModel("remainder timer armed") { pacingClock.sleeps.count == 1 }
        try require((0.9...1).contains(pacingClock.sleeps[0]),
                    "burst catch-up did not wait only the interval remainder")
        try await waitControl(host, "wait-history-done/pacing/3")
        try await waitModel("bounded sustained catch-up loaded") {
            pageNumbers(pacing) == [10, 11, 12] && !pacing.loadingHistory
        }

        try await waitControl(host, "frame/pacing/3")
        try await waitModel("cancellation request observed") {
            pacing.frame?.revision == 4
        }
        try await waitModel("stale-generation timer armed") {
            pacingClock.sleeps.count == 2
        }
        pacing.close()
        try await Task.sleep(for: .milliseconds(50))
        let pacingCounts = try await counts(host, "pacing")
        try require(pacingClock.cancellationObserved,
                    "closing did not cancel the pending newer-history timer")
        try require(pacingCounts.history == 5,
                    "cancelled newer timer issued a sixth history request")
        let pacingSustainedBounded = pageNumbers(pacing) == [10, 11, 12]

        // A blocked first prefetch must not prevent a second acknowledged
        // attachment from prefetching. The old page remains excluded.
        let prefetch = try session("prefetch", host: host)
        try await openAttachment(prefetch, host: host, key: "prefetch", revision: 1)
        try await waitControl(host, "wait-history/prefetch/1")
        let prefetchReplacement = try await nextRevision(host, "prefetch")
        try await openAttachment(prefetch, host: host, key: "prefetch", revision: prefetchReplacement)
        try await waitControl(host, "wait-history/prefetch/2")
        try await waitControl(host, "release-history/prefetch/2")
        try await waitControl(host, "wait-history-done/prefetch/2")
        try await waitModel("replacement prefetch loaded") { pageNumbers(prefetch) == [2] && !prefetch.loadingHistory }
        try await waitControl(host, "release-history/prefetch/1")
        try await waitControl(host, "wait-history-done/prefetch/1")
        try await Task.sleep(for: .milliseconds(250))
        try require(!prefetch.history.contains(where: { $0.page == 77 }),
                    "old attachment's page 77 entered after replacement")
        let prefetchCounts = try await counts(host, "prefetch")
        try require(prefetchCounts.history == 2, "replacement did not issue exactly one prefetch")
        prefetch.close()

        // One load gathers pages in descending order and keeps room for retry.
        let paging = try session("paging", host: host)
        try await openAttachment(paging, host: host, key: "paging", revision: 1)
        try await waitCounts(host, "paging", \.history, atLeast: 3)
        try await waitControl(host, "wait-history-done/paging/3")
        try await waitModel("paged history loaded") { !paging.loadingHistory }
        try require(pageNumbers(paging) == [28, 29, 30], "older paging order or query progression changed")
        try require(!paging.historyEnd, "a non-empty older page was mistaken for the history start")
        await paging.loadOlder()
        try await waitControl(host, "wait-history-done/paging/4")
        try require(paging.historyEnd, "terminal older page did not set historyEnd")
        await paging.loadOlder()
        try await Task.sleep(for: .milliseconds(100))
        try require(try await counts(host, "paging").history == 4, "historyEnd load was retried")
        paging.close()

        // An empty first page remains retryable after its rate-limit interval.
        let retry = try session("retry", host: host)
        try await openAttachment(retry, host: host, key: "retry", revision: 1)
        try await waitControl(host, "wait-history-done/retry/1")
        try await waitModel("empty history settled") { !retry.loadingHistory && !retry.historyEnd }
        try await Task.sleep(for: .milliseconds(1050))
        await retry.loadOlder()
        try await waitControl(host, "wait-history-done/retry/2")
        try await waitModel("empty-history retry loaded") { pageNumbers(retry) == [10] }
        await retry.loadOlder()
        try await waitControl(host, "wait-history-done/retry/3")
        try require(retry.historyEnd, "terminal empty-history retry did not set historyEnd")
        retry.close()

        let staleCounts = try await counts(host, "stale")
        let staleOlderRejected = !stale.history.contains(where: { $0.page == 77 })
        let staleNewerRejected = !stale.history.contains(where: { $0.page == 98 })
        print("""
        {"staleOlderRejected":\(staleOlderRejected), \
        "staleNewerRejected":\(staleNewerRejected), \
        "workspaceAtoBtoARejected":\(workspaceRejected), \
        "workspaceTerminalsAtoBtoARejected":true, \
        "independentPrefetchAfterMachinesFailure":true, \
        "staleInputRejected":\(try await counts(host, "input").input == 3), \
        "attachmentRevisions":\(staleCounts.revisions), \
        "replacementPrefetchRequests":\(prefetchCounts.history), \
        "pagingRequests":\(try await counts(host, "paging").history), \
        "emptyRetryRequests":\(try await counts(host, "retry").history), \
        "pacingIdleImmediate":\(pacingIdleImmediate), \
        "pacingSustainedBounded":\(pacingSustainedBounded), \
        "pacingStaleCancelled":\(pacingClock.cancellationObserved)}
        """)
    }
}
