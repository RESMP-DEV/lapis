import XCTest
import Network

// Answers, empirically, whether the app's ATS keys allow cleartext HTTP to a
// raw private-range IP address, which is how a ZeroTier address (10.x, e.g.
// 10.243.203.187) reaches the Mac. In Xcode the bundle is hosted in the Lapis
// app (project.yml TEST_HOST), so the app's Info.plist governs the load;
// scripts/check_ios_remote.py runs it in a runner copy carrying the app's
// exact NSAppTransportSecurity dict, because that toolchain cannot inject
// XCTest into an ad-hoc app host. Its result is the evidence recorded in
// apps/ios/project.yml next to NSAppTransportSecurity.
final class ATSTests: XCTestCase {
    func testCleartextHTTPToRawPrivateIPAnswers() throws {
        let address = try XCTUnwrap(Self.nonLoopbackIPv4(),
                                    "this Mac has no usable non-loopback IPv4 address")
        let server = try StubHTTPServer(address: address.text)
        server.start()
        defer { server.stop() }
        XCTAssertEqual(XCTWaiter().wait(for: [server.ready], timeout: 5), .completed,
                       "the stub listener never started: \(server.state)")
        XCTAssertNotEqual(server.port, 0, "the stub listener got no port")

        let answered = expectation(description: "GET http://\(address.text):\(server.port)/")
        var body: Data?
        var failure: Error?
        // An ephemeral session, like Gateway.swift's; ATS applies the same.
        let session = URLSession(configuration: .ephemeral)
        let task = session.dataTask(with: URL(string: "http://\(address.text):\(server.port)/")!) {
            body = $0
            failure = $2
            answered.fulfill()
        }
        task.resume()
        wait(for: [answered], timeout: 15)

        // The ATS refusal is NSURLErrorAppTransportSecurityRequiresSecureConnection
        // (-1022): the cleartext load never reached the network.
        if let failure {
            let error = try XCTUnwrap(failure as? URLError, "unexpected failure: \(failure)")
            XCTFail("cleartext HTTP to \(address.text) was blocked by ATS: "
                + "code \(error.code.rawValue), \(error.localizedDescription)")
            return
        }
        XCTAssertEqual(String(data: body ?? Data(), encoding: .utf8), "ok",
                       "the request reached the stub but not as the stub's answer")
        // The recorded ATS evidence: which address answered in cleartext.
        print("ATS probe: cleartext HTTP to \(address.text) "
            + "answered under the app's NSAppTransportSecurity keys.")
    }

    private struct Address: Equatable {
        let text: String
        let isPrivate: Bool
    }

    // The Mac's own non-loopback IPv4, preferring an RFC 1918 private one:
    // that is what a ZeroTier or LAN address looks like from the phone.
    private static func nonLoopbackIPv4() -> Address? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        var best: Address?
        var entry: UnsafeMutablePointer<ifaddrs>? = first
        while let current = entry {
            defer { entry = current.pointee.ifa_next }
            guard let raw = current.pointee.ifa_addr,
                  raw.pointee.sa_family == UInt8(AF_INET) else { continue }
            guard current.pointee.ifa_flags & UInt32(IFF_LOOPBACK) == 0 else { continue }
            let socket = raw.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
            // s_addr's bytes are already the octets in network order.
            let octets = withUnsafeBytes(of: socket.sin_addr.s_addr) { Array($0) }
            let address = Address(
                text: octets.map(String.init).joined(separator: "."),
                isPrivate: octets[0] == 10
                    || (octets[0] == 172 && (16...31).contains(octets[1]))
                    || (octets[0] == 192 && octets[1] == 168))
            if address.isPrivate { return address }
            // A link-local address answers nothing off this interface.
            if best == nil, !(octets[0] == 169 && octets[1] == 254) { best = address }
        }
        return best
    }
}

// A tiny HTTP/1.1 stub: answers every request with 200 "ok" and closes, so
// the test needs no part of the Mac's gateway.
final class StubHTTPServer {
    private let listener: NWListener
    private let queue = DispatchQueue(label: "lapis.ats.stub")
    let ready = XCTestExpectation(description: "stub listener ready")
    private(set) var state = "starting"

    var port: UInt16 { listener.port?.rawValue ?? 0 }

    // Port .any: the OS picks a free one, so the test never collides.
    init(address: String) throws {
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = NWEndpoint.hostPort(
            host: NWEndpoint.Host(address), port: .any)
        listener = try NWListener(using: parameters)
        listener.stateUpdateHandler = { [ready, weak self] updated in
            switch updated {
            case .ready:
                self?.state = "ready"
                ready.fulfill()
            case let .failed(error):
                self?.state = "failed: \(error)"
                ready.fulfill()
            case let .waiting(error):
                self?.state = "waiting: \(error)"
            default:
                break
            }
        }
        listener.newConnectionHandler = { Self.serve($0) }
    }

    func start() { listener.start(queue: queue) }

    func stop() { listener.cancel() }

    // Reply once the request's headers have arrived, then close.
    private static func serve(_ connection: NWConnection) {
        connection.start(queue: .global())
        let headerEnd = Data("\r\n\r\n".utf8)
        func receive() {
            connection.receive(minimumIncompleteLength: 1, maximumLength: 16_384) {
                data, _, error, _ in
                if let data, data.range(of: headerEnd) != nil {
                    let reply = Data((
                        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                            + "Content-Type: text/plain\r\nConnection: close\r\n\r\nok"
                    ).utf8)
                    connection.send(content: reply,
                                    completion: .contentProcessed { _ in connection.cancel() })
                    return
                }
                if error == nil { receive() } else { connection.cancel() }
            }
        }
        receive()
    }
}
