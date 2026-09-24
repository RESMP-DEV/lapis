import XCTest

// Drives the app against a gateway and fake agents that
// scripts/check_ios_remote.py starts on the Mac; it passes LAPIS_HOST.
final class LapisUITests: XCTestCase {
    private var app: XCUIApplication!

    override func setUpWithError() throws {
        continueAfterFailure = false
        let host = ProcessInfo.processInfo.environment["LAPIS_HOST"] ?? "127.0.0.1:7350"
        app = XCUIApplication()
        app.launchArguments = ["-gatewayHost", host, "-terminalFontSize", "12"]
        app.launch()
    }

    private func snap(_ name: String) {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    private func waitFor(_ element: XCUIElement, valueContaining text: String, timeout: TimeInterval = 20) {
        let predicate = NSPredicate(format: "value CONTAINS %@", text)
        let expectation = XCTNSPredicateExpectation(predicate: predicate, object: element)
        let result = XCTWaiter().wait(for: [expectation], timeout: timeout)
        XCTAssertEqual(result, .completed, "screen never showed \(text); it showed: \(element.value ?? "")")
    }

    private func openAgent(_ title: String) -> XCUIElement {
        let row = app.buttons["agent-\(title)"]
        XCTAssertTrue(row.waitForExistence(timeout: 30), "agent \(title) is listed")
        row.tap()
        let terminal = app.descendants(matching: .any)["terminal"]
        XCTAssertTrue(terminal.waitForExistence(timeout: 15))
        return terminal
    }

    // The key bar scrolls; bring a key into view before tapping it.
    private func key(_ name: String) {
        let key = app.buttons["key-\(name)"]
        XCTAssertTrue(key.waitForExistence(timeout: 5))
        let bar = app.scrollViews["keybar"]
        let width = app.windows.firstMatch.frame.width
        for _ in 0..<4 {
            if key.frame.maxX > width {
                bar.swipeLeft()
            } else if key.frame.minX < 0 {
                bar.swipeRight()
            } else {
                break
            }
        }
        key.tap()
    }

    // Tests share one fake agent; start each from an empty input line.
    private func clearLine() {
        key("ctrl-u")
    }

    private func submit(_ text: String) {
        let composer = app.descendants(matching: .any)["composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 5))
        composer.tap()
        composer.typeText(text)
        app.buttons["send"].tap()
    }

    func testListOpenTypeAndKeys() throws {
        XCTAssertTrue(app.staticTexts["Build"].waitForExistence(timeout: 30), "categories are sections")
        XCTAssertTrue(app.buttons["agent-parked"].exists, "a stopped agent is listed")
        snap("1-agent-list")

        let terminal = openAgent("echo agent")
        waitFor(terminal, valueContaining: "new conversation")
        clearLine()
        snap("2-agent-open")

        submit("hello from the phone")
        waitFor(terminal, valueContaining: "echo: hello from the phone")
        snap("3-typed-and-sent")

        // Typed without Enter, then cleared with the key bar's ^U.
        let composer = app.descendants(matching: .any)["composer"]
        composer.tap()
        composer.typeText("discard me")
        app.buttons["type"].tap()
        waitFor(terminal, valueContaining: "› discard me")
        clearLine()
        submit("kept")
        waitFor(terminal, valueContaining: "echo: kept")
        XCTAssertFalse(String(describing: terminal.value ?? "").contains("discard mekept"))
        snap("4-keys")
    }

    func testSettingsChecksTheConnection() throws {
        let settings = app.buttons["settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20))
        settings.tap()
        let check = app.buttons["Check connection"]
        XCTAssertTrue(check.waitForExistence(timeout: 5))
        check.tap()
        let connected = app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH %@", "Connected."))
        XCTAssertTrue(connected.firstMatch.waitForExistence(timeout: 10), "the check reaches the gateway")
        XCTAssertTrue(connected.firstMatch.label.hasSuffix("in 2 categories."))
        snap("8-settings")
        app.buttons["Done"].tap()
    }

    // Another view opening the agent ends this one with an explanation; the
    // same path shows when the Mac takes an agent back.
    func testTakenElsewhereThenReopened() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let identifier = environment["LAPIS_ECHO_ID"], let host = environment["LAPIS_HOST"] else {
            throw XCTSkip("needs the harness")
        }
        let terminal = openAgent("echo agent")
        waitFor(terminal, valueContaining: "new conversation")
        clearLine()

        var request = URLRequest(url: URL(string: "http://\(host)/api/agents/\(identifier)/stream")!)
        request.setValue("ios", forHTTPHeaderField: "X-Lapis-Client")
        let other = URLSession.shared.dataTask(with: request)
        other.resume()
        let reason = app.staticTexts["closedReason"]
        XCTAssertTrue(reason.waitForExistence(timeout: 15), "the phone says the agent moved")
        snap("9-taken-elsewhere")
        other.cancel()

        app.buttons["reopen"].tap()
        XCTAssertTrue(app.buttons["send"].waitForExistence(timeout: 5))
        let reopened = NSPredicate(format: "isEnabled == true")
        XCTAssertEqual(
            XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: reopened, object: app.buttons["send"])], timeout: 15),
            .completed)
        submit("back again")
        waitFor(terminal, valueContaining: "echo: back again")
    }

    func testWideOutputAndReturningToTheList() throws {
        let terminal = openAgent("echo agent")
        waitFor(terminal, valueContaining: "new conversation")
        clearLine()
        submit("wide")
        waitFor(terminal, valueContaining: "box:")
        snap("5-wide-text")
        app.navigationBars.buttons.element(boundBy: 0).tap()
        XCTAssertTrue(app.buttons["agent-echo agent"].waitForExistence(timeout: 10))
    }

    // Output that scrolled off the top loads as the phone scrolls up.
    func testScrollingBackLoadsHistory() throws {
        let terminal = openAgent("echo agent")
        waitFor(terminal, valueContaining: "new conversation")
        clearLine()
        submit("marker before count")
        waitFor(terminal, valueContaining: "echo: marker before count")
        submit("count")
        waitFor(terminal, valueContaining: "count 120")
        let marker = NSPredicate(format: "value CONTAINS %@", "echo: marker before count")
        for _ in 0..<12 where !marker.evaluate(with: terminal) {
            terminal.swipeDown(velocity: .fast)
        }
        snap("13-history-scrolled")
        waitFor(terminal, valueContaining: "echo: marker before count", timeout: 10)
        snap("13-history")
    }

    // "Send screen to Mac" saves a screenshot and the screen data on the Mac.
    func testSendScreenToMac() throws {
        let terminal = openAgent("echo agent")
        waitFor(terminal, valueContaining: "new conversation")
        let menu = app.buttons["viewMenu"]
        XCTAssertTrue(menu.waitForExistence(timeout: 5))
        menu.tap()
        app.buttons["Send screen to Mac"].tap()
        let sent = app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH %@", "Sent to the Mac"))
        XCTAssertTrue(sent.firstMatch.waitForExistence(timeout: 15), "the Mac saved the capture")
        app.buttons["OK"].tap()
    }

    // The Mac stays attached while the phone uses the agent, and what either
    // types shows on both. The harness's Mac-side client answers the phone.
    // The phone starts an agent through the Mac's lapis: it opens as a new tab
    // in the chosen category there, and on the phone once it runs.
    func testStartAnAgentFromThePhone() throws {
        let folder = ProcessInfo.processInfo.environment["LAPIS_NEW_AGENT_FOLDER"] ?? ""
        try XCTSkipIf(folder.isEmpty, "the check provides a folder and the Mac's lapis")
        let plus = app.buttons["newAgent-Later"]
        XCTAssertTrue(plus.waitForExistence(timeout: 30), "each category offers a new agent")
        plus.tap()
        let grok = app.buttons["harness-grok"]
        XCTAssertTrue(grok.waitForExistence(timeout: 15), "the Mac's CLIs are offered")
        grok.tap()
        XCTAssertTrue(app.buttons["category-Later"].isSelected, "the tapped category is chosen")
        let field = app.textFields["folder"]
        XCTAssertTrue(field.waitForExistence(timeout: 5))
        field.tap()
        if let current = field.value as? String, current != field.placeholderValue {
            field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: current.count))
        }
        field.typeText(folder)
        snap("9-new-agent")
        app.buttons["startAgent"].tap()
        let terminal = app.descendants(matching: .any)["terminal"]
        XCTAssertTrue(terminal.waitForExistence(timeout: 60), "the new agent opens on the phone")
        waitFor(terminal, valueContaining: "lapis fake agent")
        submit("started from the phone")
        waitFor(terminal, valueContaining: "echo: started from the phone")
        snap("10-new-agent-open")
    }

    func testSyncedWithTheMac() throws {
        guard ProcessInfo.processInfo.environment["LAPIS_MAC_CLIENT"] == "1" else {
            throw XCTSkip("needs the harness's Mac-side client")
        }
        let terminal = openAgent("echo agent")
        waitFor(terminal, valueContaining: "new conversation")
        clearLine()
        XCTAssertFalse(app.staticTexts["syncNotice"].exists, "the agent is shared, not taken over")
        submit("ping from phone")
        waitFor(terminal, valueContaining: "echo: pong from mac", timeout: 20)
        XCTAssertFalse(app.staticTexts["closedReason"].exists, "the phone view stayed open")
        snap("12-synced-with-mac")
    }

    func testClaudeAgent() throws {
        let row = app.buttons["agent-claude fake"]
        guard row.waitForExistence(timeout: 20) else {
            throw XCTSkip("no Claude agent in this run")
        }
        let terminal = openAgent("claude fake")
        waitFor(terminal, valueContaining: "Claude Code", timeout: 40)
        snap("10-claude-open")
        submit("hello claude")
        waitFor(terminal, valueContaining: "Fake model reply to: hello claude", timeout: 40)
        snap("11-claude-reply")
    }

    func testCodexAgent() throws {
        let row = app.buttons["agent-codex fake"]
        guard row.waitForExistence(timeout: 20) else {
            throw XCTSkip("no Codex agent in this run")
        }
        let terminal = openAgent("codex fake")
        waitFor(terminal, valueContaining: "OpenAI Codex", timeout: 40)
        snap("6-codex-open")
        submit("hello codex")
        waitFor(terminal, valueContaining: "Fake model reply to: hello codex", timeout: 40)
        snap("7-codex-reply")
    }
}
