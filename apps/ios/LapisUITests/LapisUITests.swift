import XCTest

// Drives the app against a gateway and fake agents that
// scripts/check_ios_remote.py starts on the Mac; it passes LAPIS_HOST.
final class LapisUITests: XCTestCase {
    private var app: XCUIApplication!

    override func setUpWithError() throws {
        continueAfterFailure = false
        let host = ProcessInfo.processInfo.environment["LAPIS_HOST"] ?? "127.0.0.1:7350"
        app = XCUIApplication()
        app.launchArguments = ["-gatewayHost", host, "-terminalFontSize", "12", "-resetCache", "1"]
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

    private func waitForGone(_ element: XCUIElement, timeout: TimeInterval) -> Bool {
        let expectation = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"),
                                                    object: element)
        return XCTWaiter().wait(for: [expectation], timeout: timeout) == .completed
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
        let terminal = try openEchoAgent()
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
        let terminal = try openEchoAgent()
        clearLine()
        submit("wide")
        waitFor(terminal, valueContaining: "box:")
        snap("5-wide-text")
        app.navigationBars.buttons.element(boundBy: 0).tap()
        XCTAssertTrue(app.buttons["agent-echo agent"].waitForExistence(timeout: 10))
    }

    // Output that scrolled off the top loads as the phone scrolls up.
    func testScrollingBackLoadsHistory() throws {
        let terminal = try openEchoAgent()
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
        _ = try openEchoAgent()
        let menu = app.buttons["viewMenu"]
        XCTAssertTrue(menu.waitForExistence(timeout: 5))
        menu.tap()
        let capture = app.buttons["Send screen to Mac"]
        let ready = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == true AND hittable == true"), object: capture
        )
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 5), .completed)
        capture.tap()
        let sent = app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH %@", "Sent to the Mac"))
        XCTAssertTrue(sent.firstMatch.waitForExistence(timeout: 15), "the Mac saved the capture")
        app.buttons["OK"].tap()
    }

    // Types into the shared fake agent through the gateway, as the phone does,
    // without focusing the composer (which would raise the keyboard).
    private func typeToEcho(_ text: String) throws {
        let environment = ProcessInfo.processInfo.environment
        let host = try XCTUnwrap(environment["LAPIS_HOST"])
        let identifier = try XCTUnwrap(environment["LAPIS_ECHO_ID"])
        var request = URLRequest(url: try XCTUnwrap(URL(string: "http://\(host)/api/agents/\(identifier)/input")))
        request.httpMethod = "POST"
        request.setValue("ios", forHTTPHeaderField: "X-Lapis-Client")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: ["text": text])
        let sent = expectation(description: "type into the fixture agent")
        URLSession.shared.dataTask(with: request) { _, response, error in
            XCTAssertNil(error)
            XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
            sent.fulfill()
        }.resume()
        wait(for: [sent], timeout: 10)
    }

    // The shared fake agent, open and live. Tests take turns on one agent, and
    // the phone taking it at a different size than the Mac's moves its screen
    // into history; this agent, unlike real CLIs, never redraws, so its first
    // line is no proof the screen is live. An echoed marker is.
    private func openEchoAgent() throws -> XCUIElement {
        let terminal = openAgent("echo agent")
        let marker = "live " + String(UUID().uuidString.prefix(6)).lowercased()
        try typeToEcho(marker + "\n")
        waitFor(terminal, valueContaining: "echo: " + marker)
        return terminal
    }

    // Read the PTY itself through a fixture command, without focusing the
    // composer merely to measure it. Each observation has a unique marker.
    private func terminalSize(_ terminal: XCUIElement) throws -> (columns: Int, rows: Int) {
        let marker = String(UUID().uuidString.prefix(6)).lowercased()
        try typeToEcho("size \(marker)\n")
        waitFor(terminal, valueContaining: "grid-\(marker)")
        let text = String(describing: terminal.value ?? "")
        let line = try XCTUnwrap(text.components(separatedBy: "\n").last { $0.contains("grid-\(marker)") })
        let fields = line.split(whereSeparator: \.isWhitespace)
        XCTAssertGreaterThanOrEqual(fields.count, 3)
        return (try XCTUnwrap(Int(fields[fields.count - 2])), try XCTUnwrap(Int(fields[fields.count - 1])))
    }

    func testRotationWhileComposing() throws {
        XCUIDevice.shared.orientation = .portrait
        defer { XCUIDevice.shared.orientation = .portrait }
        let terminal = try openEchoAgent()
        let portrait = try terminalSize(terminal)
        let composer = app.descendants(matching: .any)["composer"]
        composer.tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 10))
        let covered = try terminalSize(terminal)
        XCTAssertEqual(covered.columns, portrait.columns)
        XCTAssertEqual(covered.rows, portrait.rows, "keyboard appearance preserves PTY rows")
        XCUIDevice.shared.orientation = .landscapeLeft
        let rotated = XCTNSPredicateExpectation(
            predicate: NSPredicate { _, _ in
                self.app.windows.firstMatch.frame.width > self.app.windows.firstMatch.frame.height
            }, object: nil)
        XCTAssertEqual(XCTWaiter().wait(for: [rotated], timeout: 10), .completed)
        let landscape = try terminalSize(terminal)
        XCTAssertGreaterThan(landscape.columns, covered.columns)
        XCTAssertLessThan(landscape.rows, covered.rows, "rotation changes rows while composing")
    }

    // The Mac stays attached while the phone uses the agent, and what either
    // types shows on both. The harness's Mac-side client answers the phone.
    // The phone starts an agent through the Mac's lapis: it opens as a new tab
    // in the chosen category there, and on the phone once it runs.
    func testStartAnAgentFromThePhone() throws {
        let folder = ProcessInfo.processInfo.environment["LAPIS_NEW_AGENT_FOLDER"] ?? ""
        try XCTSkipIf(folder.isEmpty, "the check provides a folder and the Mac's lapis")
        let add = app.buttons["add"]
        XCTAssertTrue(add.waitForExistence(timeout: 30), "one plus")
        XCTAssertFalse(app.buttons["newAgent-Later"].exists, "and none beside each category")
        add.tap()
        app.buttons["New agent"].tap()
        let grok = app.buttons["harness-grok"]
        XCTAssertTrue(grok.waitForExistence(timeout: 15), "the Mac's CLIs are offered")
        grok.tap()
        app.buttons["category-Later"].tap()
        XCTAssertTrue(app.buttons["category-Later"].isSelected, "the tapped category is chosen")
        XCTAssertTrue(app.buttons["mode-full"].isSelected || app.buttons["mode-edits"].isSelected)
        let edits = app.buttons["mode-edits"]
        XCTAssertTrue(edits.waitForExistence(timeout: 5), "the CLI's approval modes are offered")
        XCTAssertTrue(app.buttons["mode-auto"].exists && app.buttons["mode-full"].exists,
                      "always the same three modes")
        edits.tap()
        XCTAssertTrue(edits.isSelected)
        app.buttons["folderRow"].tap()
        let search = app.textFields["folderSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5), "the folder opens its own screen")
        search.tap()
        search.typeText(folder)
        let typed = app.buttons["useTyped"]
        XCTAssertTrue(typed.waitForExistence(timeout: 5), "a typed path can be used as is")
        typed.tap()
        XCTAssertTrue(app.buttons["startAgent"].waitForExistence(timeout: 5), "back to the form")
        snap("9-new-agent")
        app.buttons["startAgent"].tap()
        let terminal = app.descendants(matching: .any)["terminal"]
        XCTAssertTrue(terminal.waitForExistence(timeout: 60), "the new agent opens on the phone")
        waitFor(terminal, valueContaining: "lapis fake agent")
        submit("started from the phone")
        waitFor(terminal, valueContaining: "echo: started from the phone")
        snap("10-new-agent-open")
        // Back in the list, it swipes away like a notification, and the Mac
        // closes it.
        app.navigationBars.buttons.element(boundBy: 0).tap()
        let card = app.buttons["agent-phone-project"]
        XCTAssertTrue(card.waitForExistence(timeout: 15))
        card.swipeLeft()
        let close = app.buttons["Close"]
        XCTAssertTrue(close.waitForExistence(timeout: 5), "swiping offers Close")
        close.tap()
        XCTAssertTrue(waitForGone(card, timeout: 20), "the closed agent leaves the list")
        // A category from the phone.
        app.buttons["add"].tap()
        app.buttons["New category"].tap()
        let name = app.alerts.textFields.firstMatch
        XCTAssertTrue(name.waitForExistence(timeout: 5), "the new category asks for a name")
        name.typeText("Ideas")
        app.alerts.buttons["Create"].tap()
        XCTAssertTrue(app.staticTexts["category-Ideas"].waitForExistence(timeout: 20),
                      "the new category is listed")
    }

    // A plain shell for a quick command: the plus offers Terminal, the Mac is
    // picked, the shell answers, and it swipes away like an agent.
    func testTerminalFromThePhone() throws {
        let folder = ProcessInfo.processInfo.environment["LAPIS_NEW_AGENT_FOLDER"] ?? ""
        try XCTSkipIf(folder.isEmpty, "the check provides the Mac's lapis")
        let add = app.buttons["add"]
        XCTAssertTrue(add.waitForExistence(timeout: 30))
        add.tap()
        app.buttons["Terminal"].tap()
        let mac = app.buttons["terminal-mac"]
        XCTAssertTrue(mac.waitForExistence(timeout: 15), "the Mac is offered first")
        snap("11-terminal-machines")
        mac.tap()
        let terminal = app.descendants(matching: .any)["terminal"]
        XCTAssertTrue(terminal.waitForExistence(timeout: 60), "the terminal opens on the phone")
        submit("echo lapis-$((6*7))")
        waitFor(terminal, valueContaining: "lapis-42", timeout: 30)
        snap("12-terminal-open")
        app.navigationBars.buttons.element(boundBy: 0).tap()
        let row = app.buttons["terminal-row-mac"]
        XCTAssertTrue(row.waitForExistence(timeout: 15), "open terminals are listed above the agents")
        row.swipeLeft()
        let close = app.buttons["Close"]
        XCTAssertTrue(close.waitForExistence(timeout: 5), "swiping offers Close")
        close.tap()
        XCTAssertTrue(waitForGone(row, timeout: 20), "the closed terminal leaves the list")
    }

    // An agent is renamed from the phone by swiping it right; the Mac keeps
    // that name over its conversation's title.
    func testRenameFromThePhone() throws {
        let row = app.buttons["agent-parked"]
        XCTAssertTrue(row.waitForExistence(timeout: 30))
        for (from, to) in [("parked", "renamed here"), ("renamed here", "parked")] {
            let card = app.buttons["agent-\(from)"]
            XCTAssertTrue(card.waitForExistence(timeout: 20))
            card.swipeRight()
            let rename = app.buttons["Rename"]
            XCTAssertTrue(rename.waitForExistence(timeout: 5), "swiping right offers Rename")
            rename.tap()
            let field = app.alerts.textFields.firstMatch
            XCTAssertTrue(field.waitForExistence(timeout: 5))
            field.tap()
            let current = (field.value as? String) ?? ""
            field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: current.count + 2))
            field.typeText(to)
            app.alerts.buttons["Save"].tap()
            XCTAssertTrue(app.buttons["agent-\(to)"].waitForExistence(timeout: 20), "the new name is listed")
            if to != "parked" { snap("14-renamed") }
        }
    }

    // Swiping over an agent's screen moves to the next agent in its category,
    // and back, without the list.
    func testSwipeBetweenAgents() throws {
        let terminal = try openEchoAgent()
        let position = app.staticTexts["agentPosition"]
        XCTAssertTrue(position.waitForExistence(timeout: 10), "the position in the category shows")
        XCTAssertEqual(position.label, "1 of 2")
        let at = { (label: String) in
            XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == %@", label),
                                      object: self.app.staticTexts["agentPosition"])
        }
        terminal.swipeLeft()
        XCTAssertEqual(XCTWaiter().wait(for: [at("2 of 2")], timeout: 10), .completed, "the next agent")
        let second = app.descendants(matching: .any)["terminal"]
        waitFor(second, valueContaining: "new conversation")
        snap("15-swiped")
        second.swipeRight()
        XCTAssertEqual(XCTWaiter().wait(for: [at("1 of 2")], timeout: 10), .completed, "and back")
    }

    private func eventually(_ what: String, timeout: TimeInterval = 20, _ condition: () -> Bool) {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return }
            RunLoop.current.run(until: Date().addingTimeInterval(0.25))
        }
        XCTFail(what)
    }

    private func replaceAlertText(with text: String) {
        let field = app.alerts.textFields.firstMatch
        XCTAssertTrue(field.waitForExistence(timeout: 5))
        field.tap()
        let current = (field.value as? String) ?? ""
        field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: current.count + 2))
        field.typeText(text)
    }

    private func menuItem(_ label: String) -> XCUIElement {
        let item = app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", label)).firstMatch
        XCTAssertTrue(item.waitForExistence(timeout: 5), "the menu offers \(label)")
        return item
    }

    private func drag(_ name: String, onto target: String) {
        let handle = { (row: String) in
            self.app.cells.containing(.button, identifier: "edit-category-\(row)")
                .buttons.matching(NSPredicate(format: "label CONTAINS[c] %@", "reorder")).firstMatch
        }
        XCTAssertTrue(handle(name).waitForExistence(timeout: 5), "\(name) has a drag handle")
        handle(name).press(forDuration: 0.8, thenDragTo: handle(target))
    }

    // Categories are arranged from the phone as on the Mac: renamed from a
    // category's menu, added and ordered under Arrange categories, and removed
    // once empty; one with agents stays.
    func testArrangeCategoriesFromThePhone() throws {
        let menu = app.buttons["category-menu-Later"]
        XCTAssertTrue(menu.waitForExistence(timeout: 30), "each category has a menu")
        menu.tap()
        menuItem("Rename").tap()
        replaceAlertText(with: "Someday")
        app.alerts.buttons["Save"].tap()
        XCTAssertTrue(app.staticTexts["category-Someday"].waitForExistence(timeout: 20), "renamed on the Mac")
        app.buttons["category-menu-Build"].tap()
        XCTAssertFalse(menuItem("Remove").isEnabled, "a category with agents stays")
        menuItem("Arrange categories").tap()
        let add = app.buttons["addCategory"]
        XCTAssertTrue(add.waitForExistence(timeout: 10), "the categories open")
        add.tap()
        let name = app.alerts.textFields.firstMatch
        XCTAssertTrue(name.waitForExistence(timeout: 5))
        name.typeText("Scratch")
        app.alerts.buttons["Create"].tap()
        XCTAssertTrue(app.buttons["edit-category-Scratch"].waitForExistence(timeout: 20), "a category is added")
        drag("Someday", onto: "Build")
        let row = { (category: String) in self.app.buttons["edit-category-\(category)"] }
        eventually("Someday moves to the top") { row("Someday").frame.minY < row("Build").frame.minY }
        snap("16-categories")
        app.buttons["Done"].tap()
        let header = { (category: String) in self.app.staticTexts["category-\(category)"] }
        eventually("the list follows the Mac's order") { header("Someday").frame.minY < header("Build").frame.minY }
        app.buttons["category-menu-Scratch"].tap()
        menuItem("Remove").tap()
        XCTAssertTrue(waitForGone(header("Scratch"), timeout: 20), "the empty category is removed")
        // As it was, for the other tests: renamed by tapping it, and second.
        app.buttons["category-menu-Someday"].tap()
        menuItem("Arrange categories").tap()
        XCTAssertTrue(row("Someday").waitForExistence(timeout: 10))
        row("Someday").tap()
        replaceAlertText(with: "Later")
        app.alerts.buttons["Save"].tap()
        XCTAssertTrue(row("Later").waitForExistence(timeout: 20), "renamed from the list")
        drag("Later", onto: "Build")
        eventually("Later moves back below Build") { row("Build").frame.minY < row("Later").frame.minY }
        app.buttons["Done"].tap()
        eventually("the list has it back") { header("Build").frame.minY < header("Later").frame.minY }
    }

    // An agent moves to another category, and along its own, from its menu.
    func testMoveAgentsFromThePhone() throws {
        let parked = app.buttons["agent-parked"]
        XCTAssertTrue(parked.waitForExistence(timeout: 30))
        let later = app.staticTexts["category-Later"]
        parked.press(forDuration: 1.2)
        menuItem("Move to").tap()
        menuItem("Build").tap()
        eventually("it is listed under Build") { parked.frame.maxY < later.frame.minY }
        parked.press(forDuration: 1.2)
        XCTAssertFalse(menuItem("Move later").isEnabled, "the last agent cannot move later")
        menuItem("Move earlier").tap()
        let second = app.buttons["agent-second agent"]
        eventually("it moves before the agent above it") { parked.frame.minY < second.frame.minY }
        snap("17-moved")
        parked.press(forDuration: 1.2)
        menuItem("Move to").tap()
        menuItem("Later").tap()
        eventually("and back to Later") { parked.frame.minY > later.frame.minY }
    }

    // The Mac's own settings that matter away from it change from the phone
    // and are saved there (the check reads the Mac's lapis.json afterwards).
    func testMacSettingsFromThePhone() throws {
        let settings = app.buttons["settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20))
        settings.tap()
        let usage = app.switches["showUsage"]
        for _ in 0..<3 where !usage.exists {
            app.swipeUp()
            _ = usage.waitForExistence(timeout: 5)
        }
        XCTAssertTrue(usage.exists, "the Mac's settings load")
        XCTAssertEqual(usage.value as? String, "1")
        usage.switches.firstMatch.tap()
        eventually("usage turns off") { usage.value as? String == "0" }
        app.buttons["alertRepeat-Increment"].tap()
        XCTAssertTrue(app.staticTexts["Chime up to 4 times"].waitForExistence(timeout: 10))
        snap("18-mac-settings")
        app.buttons["Done"].tap()
        settings.tap()
        for _ in 0..<3 where !usage.exists {
            app.swipeUp()
            _ = usage.waitForExistence(timeout: 5)
        }
        XCTAssertEqual(usage.value as? String, "0", "the Mac kept it")
        app.buttons["Done"].tap()
    }

    // A stopped agent restarts from its menu, as Restart agent does on the Mac.
    func testRestartAStoppedAgent() throws {
        let parked = app.buttons["agent-parked"]
        XCTAssertTrue(parked.waitForExistence(timeout: 30))
        XCTAssertFalse(parked.label.contains("running"), "it starts stopped")
        parked.press(forDuration: 1.2)
        menuItem("Restart").tap()
        eventually("it runs again", timeout: 30) { parked.label.contains("running") }
    }

    // Symbols that also have an emoji form (Claude Code's ⏺ before each
    // message) are drawn as text in their one cell, as on the Mac.
    func testSymbolsDrawAsText() throws {
        let terminal = try openEchoAgent()
        try typeToEcho("symbols ⏺ ⏸ ⏵ ✻ ▶ ✔ ⚠ ↩ ☑ ● x\n")
        waitFor(terminal, valueContaining: "symbols ⏺")
        // Asked for as emoji, it stays one.
        try typeToEcho("asked ⏺\u{FE0F} x\n")
        waitFor(terminal, valueContaining: "asked")
        snap("19-symbols")
    }

    // Past conversations on the Mac are offered to resume from the plus.
    func testResumeIsOffered() throws {
        let add = app.buttons["add"]
        XCTAssertTrue(add.waitForExistence(timeout: 30))
        add.tap()
        app.buttons["Resume conversation"].tap()
        XCTAssertTrue(app.navigationBars.staticTexts["Resume"].waitForExistence(timeout: 15),
                      "the resume list opens")
        app.buttons["Cancel"].tap()
    }

    // Folders come from the Mac's index: the most used ones first and marked,
    // then the current folder's folders, the most active first, with hidden
    // ones last; typing finds folders by their letters. Machines are ordered
    // by how often ssh reached them.
    func testFoldersAndMachines() throws {
        try XCTSkipIf(ProcessInfo.processInfo.environment["LAPIS_FOLDER_FIXTURE"] == nil,
                      "the check provides a folder fixture")
        let add = app.buttons["add"]
        XCTAssertTrue(add.waitForExistence(timeout: 30))
        XCTAssertTrue(app.buttons["agent-echo agent"].waitForExistence(timeout: 30))
        add.tap()
        app.buttons["New agent"].tap()
        let beta = app.buttons["machine-beta"]
        let alpha = app.buttons["machine-alpha"]
        XCTAssertTrue(beta.waitForExistence(timeout: 10) && alpha.exists, "ssh machines are offered")
        XCTAssertLessThan(beta.frame.minX, alpha.frame.minX, "the more used machine comes first")
        // Choices stay while others change: another machine keeps the CLI,
        // another CLI keeps the mode.
        let grok = app.buttons["harness-grok"]
        XCTAssertTrue(grok.waitForExistence(timeout: 15))
        grok.tap()
        app.buttons["mode-full"].tap()
        beta.tap()
        app.buttons["machine-mac"].tap()
        XCTAssertTrue(grok.waitForExistence(timeout: 5) && grok.isSelected, "the CLI stays across machines")
        XCTAssertTrue(app.buttons["mode-full"].isSelected, "and so does the mode")
        let folderRow = app.buttons["folderRow"]
        XCTAssertTrue(folderRow.waitForExistence(timeout: 20))
        folderRow.tap()
        let preset = app.buttons["presetFolder"]
        XCTAssertTrue(preset.waitForExistence(timeout: 20), "the config's preset folder is offered")
        XCTAssertTrue(preset.isSelected && preset.label.contains("~/dev"), "and chosen to start with")
        let frequent = app.buttons["frequent-b"]
        XCTAssertTrue(frequent.waitForExistence(timeout: 20), "the most used folders follow")
        XCTAssertTrue(app.buttons["frequent-dev/lapis"].exists)
        XCTAssertLessThan(frequent.frame.minY, app.buttons["frequent-dev/lapis"].frame.minY,
                          "more agents started there, higher in the list")
        XCTAssertFalse(frequent.label.contains(where: \.isNumber), "no counts, only the order")
        app.buttons["folderUp"].tap()
        let visible = app.buttons["folder-dev"]
        let hidden = app.buttons["folder-.hidden"]
        XCTAssertTrue(visible.waitForExistence(timeout: 5) && hidden.exists)
        // The folders with the most recent agent work first, then by name.
        XCTAssertLessThan(app.buttons["folder-b"].frame.minY, visible.frame.minY, "the busiest folder first")
        XCTAssertLessThan(visible.frame.minY, app.buttons["folder-a"].frame.minY, "then the rest by name")
        XCTAssertLessThan(app.buttons["folder-a"].frame.minY, hidden.frame.minY, "hidden folders come last")
        snap("11-folders")
        let search = app.textFields["folderSearch"]
        search.tap()
        search.typeText("dvlp")
        let found = app.buttons["result-dev/lapis"]
        XCTAssertTrue(found.waitForExistence(timeout: 5), "fuzzy search finds the folder")
        snap("12-folder-search")
        found.tap()
        XCTAssertTrue(folderRow.waitForExistence(timeout: 5), "choosing returns to the form")
        XCTAssertTrue(folderRow.label.hasSuffix("~/dev/lapis"))
        app.buttons["Cancel"].tap()
    }

    func testSyncedWithTheMac() throws {
        guard ProcessInfo.processInfo.environment["LAPIS_MAC_CLIENT"] == "1" else {
            throw XCTSkip("needs the harness's Mac-side client")
        }
        let terminal = try openEchoAgent()
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
