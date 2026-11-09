import QtQuick
import QtQuick.Controls
import QtQml.Models
import QtQuick.Dialogs
import QtQuick.Effects

// The selected agent's conversation as a readable thread: the person's
// prompts as bubbles, the agent's replies as prose, and its work as quiet
// one-line steps ("Ran", "Edited", "Read") that open for detail. Text is
// selectable and every message can be copied; a prompt can be edited into a
// new agent. The terminal stays where input goes.
// Kinds the reader does not know show as plain blocks so new CLI features
// never vanish.
FocusScope {
    id: root

    property var model: null
    // Appearance, from the visual settings: a theme and font by name and a
    // text size in pixels, 0 to follow the terminal's.
    property string themeName: "terminal"
    property string fontName: "system"
    property string codeFontName: "system"
    property int sizeSetting: 0
    // What "match the terminal" matches.
    property int terminalFontSize: 14
    property string terminalFamily: "Menlo"
    property color terminalBackground: "#1a1b26"
    property color terminalAccent: "#7aa2f7"
    // The platform's interface font (SF Pro on the Mac).
    property string systemFamily: Qt.application.font.family
    property int radius: 8
    readonly property alias list: list
    // Follow new items while the reader sits at the end, as a terminal does.
    property bool following: true

    // The bar at the bottom types into the agent through these, which the
    // window wires to the terminal underneath: submit(text) returns whether
    // it was sent, interrupt() stops a turn, tabAway() moves to the next
    // agent that needs you (Tab with nothing typed or offered).
    property var submit: null
    property var interrupt: null
    property var tabAway: null
    // lapis's predicted next prompt (Tab), shown in the empty bar.
    property string suggestion: ""
    property bool suggestionConfident: false
    property bool working: false        // a turn is running: Send becomes Stop
    property bool waiting: false        // a request waits in the terminal
    property string waitingReason: ""
    property bool canSend: true
    // What Command-click on a file path opens it with (an editors() id).
    property string editor: "auto"
    // The agent's terminal rows as text (screenLines()), which stream into
    // the running turn; null where there is no terminal.
    property var screen: null
    // The live tail: the terminal's rows above its input box, read a few
    // times a second while a turn runs and the view is shown.
    property var liveRows: []
    property bool liveOpen: false
    // A slash command the bar sent that the CLI has not recorded yet. Some
    // run for minutes with no turn (/compact), so it shows as running, with
    // the same live tail, until its record lands or the screen goes quiet.
    property string pendingCommand: ""
    property double pendingSince: 0
    property int quietTicks: 0
    readonly property bool busy: working || pendingCommand.length > 0
    // The bar sent a slash command; it may open a dialog in the terminal.
    signal commandSent(string text)
    // A dialog already fills the terminal, so nothing was sent.
    signal dialogFound()
    Timer {
        interval: 150
        repeat: true
        running: root.busy && root.visible && !!root.screen && root.model !== null
        triggeredOnStart: true
        onTriggered: {
            const rows = root.model.liveLines(root.screen())
            if (rows.join("\n") !== root.liveRows.join("\n")) {
                root.liveRows = rows
                root.quietTicks = 0
            } else if (!root.working && root.pendingCommand.length > 0 && ++root.quietTicks >= 20) {
                root.pendingCommand = "" // three quiet seconds: it finished unrecorded
            }
        }
        onRunningChanged: if (!running) { root.liveRows = []; root.liveOpen = false }
    }
    Connections {
        target: root.pendingCommand.length > 0 ? root.model : null
        function onRowsInserted() { root.pendingCommand = "" }
        function onModelReset() { root.pendingCommand = "" }
    }
    onWorkingChanged: if (working) pendingCommand = ""
    // The clock the running turn's elapsed time follows.
    property double now: Date.now()
    property double turnSince: 0
    Timer {
        interval: 1000
        repeat: true
        running: root.busy && root.visible
        triggeredOnStart: true
        onTriggered: {
            root.now = Date.now()
            root.turnSince = root.working ? (root.model ? root.model.runningSince() : 0) : root.pendingSince
        }
    }
    // The agent's folder, for its project's own slash commands, and whether
    // its CLI takes the slash commands lapis knows (Claude Code's).
    property string folder: ""
    property bool slashCommands: false

    // Slash commands: typing "/" and a few letters lists the three best
    // matches above the bar. The list is read once per agent folder.
    property var commandList: []
    property string commandFolder: "\u0000"
    property int slashIndex: 0
    property bool slashDismissed: false
    readonly property string slashQuery: {
        const text = input.text
        return slashCommands && text.startsWith("/") && !/\s/.test(text) ? text.slice(1).toLowerCase() : "\u0000"
    }
    onSlashQueryChanged: {
        slashIndex = 0
        if (slashQuery === "\u0000")
            slashDismissed = false
        else if (commandFolder !== folder && model) {
            commandList = model.slashCommands(folder)
            commandFolder = folder
        }
    }
    readonly property var slashMatches: slashQuery === "\u0000" || slashDismissed ? [] : rankSlash(slashQuery)
    function slashScore(name, description, q) {
        if (q.length === 0)
            return ["compact", "model", "effort", "context", "clear", "resume"].indexOf(name) >= 0
                   ? 10 - ["compact", "model", "effort", "context", "clear", "resume"].indexOf(name) : -1
        if (name === q) return 1000
        if (name.startsWith(q)) return 800 - name.length
        const at = name.indexOf(q)
        if (at >= 0) return 600 - at * 4 - name.length
        // Letters in order ("rp" finds reload-plugins), tighter is better.
        let i = 0, gaps = 0, last = -1
        for (let k = 0; k < name.length && i < q.length; ++k)
            if (name[k] === q[i]) { gaps += last < 0 ? k : k - last - 1; last = k; ++i }
        if (i === q.length) return 400 - gaps * 8 - name.length
        return description.toLowerCase().indexOf(q) >= 0 ? 100 : -1
    }
    function rankSlash(q) {
        const scored = []
        for (const c of commandList) {
            const score = slashScore(c.name, c.description || "", q)
            if (score >= 0)
                scored.push({ score: score, command: c })
        }
        scored.sort((a, b) => b.score - a.score)
        return scored.slice(0, 3).map(s => s.command)
    }
    function completeSlash(name) {
        input.text = "/" + name + " "
        input.cursorPosition = input.text.length
        input.forceActiveFocus()
    }

    // The bar's line editing, as the terminal does it on a Mac: Command
    // moves or deletes to the line's start or end (Up/Down: the whole text),
    // Option by word, and Control-A/E/K/U/W as the shells' emacs keys. Shift
    // extends the selection. Returns whether the key was handled.
    function wordChar(c) { return /\w/.test(c) || c.charCodeAt(0) > 127 }
    function wordBack(text, at) {
        while (at > 0 && !wordChar(text[at - 1])) --at
        while (at > 0 && wordChar(text[at - 1])) --at
        return at
    }
    function wordForward(text, at) {
        while (at < text.length && !wordChar(text[at])) ++at
        while (at < text.length && wordChar(text[at])) ++at
        return at
    }
    function lineStart(text, at) { return text.lastIndexOf("\n", at - 1) + 1 }
    function lineEnd(text, at) { const end = text.indexOf("\n", at); return end < 0 ? text.length : end }
    function editKey(field, key, mods) {
        const text = field.text
        const at = field.cursorPosition
        const command = (mods & ~Qt.ShiftModifier) === Qt.MetaModifier
        const option = (mods & ~Qt.ShiftModifier) === Qt.AltModifier
        const control = mods === Qt.ControlModifier
        const shift = (mods & Qt.ShiftModifier) !== 0
        let target = -1
        if (command && key === Qt.Key_Left) target = lineStart(text, at)
        else if (command && key === Qt.Key_Right) target = lineEnd(text, at)
        else if (command && key === Qt.Key_Up) target = 0
        else if (command && key === Qt.Key_Down) target = text.length
        else if (option && key === Qt.Key_Left) target = wordBack(text, at)
        else if (option && key === Qt.Key_Right) target = wordForward(text, at)
        else if (control && key === Qt.Key_A) target = lineStart(text, at)
        else if (control && key === Qt.Key_E) target = lineEnd(text, at)
        if (target >= 0) {
            if (shift)
                field.moveCursorSelection(target, TextEdit.SelectCharacters)
            else
                field.cursorPosition = target
            return true
        }
        // Deleting: a selection goes whole; otherwise the span the key names.
        let from = -1, to = -1
        if (command && !shift && key === Qt.Key_Backspace) { from = lineStart(text, at); to = at }
        else if (command && !shift && key === Qt.Key_Delete) { from = at; to = lineEnd(text, at) }
        else if (option && !shift && key === Qt.Key_Backspace) { from = wordBack(text, at); to = at }
        else if (option && !shift && key === Qt.Key_Delete) { from = at; to = wordForward(text, at) }
        else if (control && key === Qt.Key_U) { from = lineStart(text, at); to = at }
        else if (control && key === Qt.Key_K) { from = at; to = lineEnd(text, at) }
        else if (control && key === Qt.Key_W) { from = wordBack(text, at); to = at }
        else return false
        if (field.selectedText.length > 0 && (command || option)) {
            field.remove(field.selectionStart, field.selectionEnd)
        } else if (to > from) {
            field.remove(from, to)
        } else if (to === from && from === at && (command || control)) {
            // At a line's start (or end), Command-Backspace (or -Delete)
            // joins it with the line before (or after), as on macOS.
            if (key === Qt.Key_Backspace || key === Qt.Key_U) { if (at > 0) field.remove(at - 1, at) }
            else if (at < text.length) field.remove(at, at + 1)
        }
        return true
    }
    readonly property alias composer: input
    // The same controls for every CLI: the models its agent can switch to
    // ({id, name}), its effort levels, and what it reported last (from the
    // model). The window turns a choice into that CLI's own command.
    property var models: []
    property var efforts: []
    signal modelChosen(string id)
    signal effortChosen(string level)
    // Files to send with the next message, as local paths.
    property var attachments: []
    function attach(path) {
        if (path.length === 0 || attachments.indexOf(path) >= 0)
            return
        attachments = attachments.concat([path])
    }
    function detach(path) { attachments = attachments.filter(p => p !== path) }
    // Short model names, as the apps show them: "Opus 5.5", "Fable 5.1".
    function modelLabel(id) {
        for (let i = 0; i < models.length; ++i)
            if (models[i].id === id && models[i].name)
                return models[i].name
        const m = /^claude-([a-z]+)-(\d+)(?:-(\d+))?/.exec(id || "")
        if (m)
            return m[1].charAt(0).toUpperCase() + m[1].slice(1) + " " + m[2] + (m[3] ? "." + m[3] : "")
        return id || qsTr("Model")
    }
    // An effort level as the Codex app names it: "xhigh" is "Extra High".
    function effortLabel(level) {
        return level === "xhigh" ? qsTr("Extra High")
             : level.length > 0 ? level.charAt(0).toUpperCase() + level.slice(1) : ""
    }
    // "12s", "3m 12s", "1h 4m", "2d 3h": the scale follows the length.
    function durationLabel(ms) {
        const s = Math.max(0, Math.round(ms / 1000))
        return s < 60 ? s + "s"
             : s < 3600 ? Math.floor(s / 60) + "m " + (s % 60) + "s"
             : s < 86400 ? Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m"
             : Math.floor(s / 86400) + "d " + Math.floor((s % 86400) / 3600) + "h"
    }
    function tokensLabel(n) {
        return n >= 1000000 ? (n / 1000000).toFixed(n % 1000000 === 0 ? 0 : 1) + "M"
             : n >= 1000 ? Math.round(n / 1000) + "k" : String(n)
    }

    // Escape asks to go back to the terminal.
    signal dismissed()
    // Tab put the suggestion in the bar, and a second Tab sent it unchanged.
    signal suggestionFilled()
    signal suggestionSent()
    // Start a new agent from just before a prompt, with it in the input.
    signal editRequested(string record, string text)

    readonly property var fonts: model ? model.fonts() : ({ serif: "Georgia", mono: "Menlo" })
    // Inspired by the Codex and Claude desktop apps; "terminal" takes the
    // shown terminal's own background so the thread sits where it did.
    readonly property var themes: [
        { name: "terminal", label: qsTr("Match terminal"), dark: true },
        { name: "tokyo-night", label: "Tokyo Night", dark: true, background: "#1a1b26", raised: "#24283b",
          code: "#16161e", border: "#292e42", text: "#c0caf5", muted: "#737aa2", accent: "#7aa2f7",
          added: "#449dab", removed: "#914c54", fault: "#f7768e" },
        { name: "codex", label: "Codex", dark: true, background: "#181818", raised: "#2a2a2a",
          code: "#111111", border: "#303030", text: "#ececec", muted: "#8f8f8f", accent: "#339cff",
          added: "#3fb950", removed: "#f85149", fault: "#ff6b6b" },
        { name: "claude", label: "Claude", dark: true, background: "#262624", raised: "#30302e",
          code: "#1f1e1d", border: "#3e3e38", text: "#e8e6dc", muted: "#a6a39a", accent: "#d97757",
          added: "#7fa86a", removed: "#c46a5b", fault: "#e0705f" },
        { name: "graphite", label: qsTr("Graphite"), dark: true, background: "#1e1f22", raised: "#2b2d31",
          code: "#17181a", border: "#35373c", text: "#dcdee3", muted: "#8b8f98", accent: "#5e9cff",
          added: "#4fb57a", removed: "#d0605e", fault: "#ef6b73" },
        { name: "black", label: qsTr("Black"), dark: true, background: "#000000", raised: "#161616",
          code: "#0b0b0b", border: "#262626", text: "#e6e6e6", muted: "#8a8a8a", accent: "#7aa2f7",
          added: "#4fb57a", removed: "#d0605e", fault: "#ef6b73" },
        { name: "claude-light", label: qsTr("Claude light"), dark: false, background: "#faf9f5", raised: "#efede4",
          code: "#f1efe7", border: "#e2dfd4", text: "#29261b", muted: "#76736a", accent: "#c96442",
          added: "#2e7d32", removed: "#b3412f", fault: "#b3261e" },
        { name: "codex-light", label: qsTr("Codex light"), dark: false, background: "#ffffff", raised: "#f2f2f2",
          code: "#f7f7f7", border: "#e5e5e5", text: "#0d0d0d", muted: "#6e6e6e", accent: "#0169cc",
          added: "#1a7f37", removed: "#cf222e", fault: "#c62828" }
    ]
    readonly property var readingFonts: [
        { name: "system", label: qsTr("System"), family: systemFamily },
        { name: "serif", label: qsTr("Serif"), family: fonts.serif },
        { name: "avenir", label: "Avenir Next", family: "Avenir Next" },
        { name: "helvetica", label: "Helvetica Neue", family: "Helvetica Neue" }
    ]
    readonly property var codeFonts: [
        { name: "system", label: "SF Mono", family: fonts.mono },
        { name: "terminal", label: qsTr("Terminal font"), family: terminalFamily },
        { name: "menlo", label: "Menlo", family: "Menlo" }
    ]
    function pick(choices, name) {
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].name === name)
                return choices[i]
        return choices[0]
    }
    // A theme's full colors; the terminal theme is derived from its background.
    function colorsOf(theme) {
        if (theme.name !== "terminal")
            return theme
        const base = terminalBackground
        return {
            name: theme.name, label: theme.label, dark: true, background: base,
            raised: Qt.tint(base, "#14ffffff"), code: Qt.tint(base, "#38000000"),
            border: Qt.tint(base, "#1effffff"), text: "#d6dbe5", muted: "#8b93a7",
            accent: terminalAccent, added: "#449dab", removed: "#b4545f", fault: "#f7768e"
        }
    }
    readonly property var colors: colorsOf(pick(themes, themeName))
    readonly property color backgroundColor: colors.background
    readonly property color raisedColor: colors.raised
    readonly property color codeColor: colors.code
    readonly property color borderColor: colors.border
    readonly property color textColor: colors.text
    readonly property color mutedTextColor: colors.muted
    readonly property color accentColor: colors.accent
    readonly property color addedColor: colors.added
    readonly property color removedColor: colors.removed
    readonly property color faultColor: colors.fault
    readonly property color activityColor: colors.accent
    readonly property string sansFamily: pick(readingFonts, fontName).family
    readonly property string monoFamily: pick(codeFonts, codeFontName).family
    // One scale for everything, from the body size, as the Codex app keeps
    // its 15/14/12 steps: prose, step rows, code.
    readonly property int fontSize: sizeSetting > 0 ? sizeSetting : Math.max(12, terminalFontSize)
    readonly property int stepSize: Math.round(fontSize * 0.93)
    readonly property int codeSize: Math.round(fontSize * 0.86)
    readonly property int columnWidth: Math.round(fontSize * 48)
    // A message's row of buttons (Copy, Fork, Edit).
    readonly property int actionHeight: Math.round(codeSize * 1.3) + 8
    // Changes whenever prose has to be laid out again.
    readonly property string styleKey: [sansFamily, monoFamily, fontSize, codeSize, codeColor, accentColor,
                                        borderColor, raisedColor].join("|")
    readonly property var proseStyle: ({ sans: sansFamily, mono: monoFamily, size: fontSize, codeSize: codeSize,
                                         codeColor: codeColor, linkColor: accentColor,
                                         borderColor: borderColor, quoteColor: mutedTextColor })
    // Code takes Tokyo Night's colors, its Day variant on a light code card.
    readonly property bool lightCode: codeColor.r * 0.299 + codeColor.g * 0.587 + codeColor.b * 0.114 > 0.5
    readonly property color codeTextColor: lightCode ? "#3760bf" : "#c0caf5"

    // Charts agents draw follow this theme: lapis keeps a matplotlib style
    // and a palette written from it.
    readonly property string chartKey: [backgroundColor, textColor, mutedTextColor, borderColor].join("|")
    onChartKeyChanged: chartTimer.restart()
    Timer {
        id: chartTimer
        interval: 300
        onTriggered: if (root.model) root.model.writeChartStyle({
            background: root.backgroundColor, text: root.textColor, muted: root.mutedTextColor,
            grid: root.borderColor,
            light: root.backgroundColor.r * 0.299 + root.backgroundColor.g * 0.587 + root.backgroundColor.b * 0.114 > 0.5 })
    }
    onModelChanged: chartTimer.restart()

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function copy(text) { if (model) model.copy(text) }

    Keys.onEscapePressed: dismissed()

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    // Another conversation opens at its newest message.
    Connections {
        target: root.model
        function onPathChanged() {
            root.following = true
            Qt.callLater(list.positionViewAtEnd)
        }
    }

    ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: composerArea.top
        model: root.model
        clip: true
        // Dragging selects text; the wheel, trackpad and scroll bar scroll.
        interactive: false
        reuseItems: true
        cacheBuffer: 2000
        header: Item { height: 24 }
        footer: liveFooter
        onCountChanged: if (root.following) Qt.callLater(positionViewAtEnd)
        onContentHeightChanged: if (root.following) Qt.callLater(positionViewAtEnd)
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            // Following would pull the view back to the end mid-drag.
            onPressedChanged: root.following = !pressed && list.atYEnd
        }

        // One delegate per kind, so a row scrolled out is reused by the next
        // row of the same kind with its parts already made.
        delegate: DelegateChooser {
            role: "kind"
            DelegateChoice { roleValue: "prompt"; delegate: Entry { block: promptBlock } }
            DelegateChoice { roleValue: "reply"; delegate: Entry { block: replyBlock } }
            DelegateChoice { roleValue: "tool"; delegate: Entry { block: stepBlock } }
            DelegateChoice { roleValue: "thinking"; delegate: Entry { block: stepBlock } }
            DelegateChoice { roleValue: "turn"; delegate: Entry { block: turnBlock } }
            DelegateChoice { roleValue: "command"; delegate: Entry { block: commandBlock } }
            DelegateChoice { roleValue: "notice"; delegate: Entry { block: noticeBlock } }
            DelegateChoice { roleValue: "divider"; delegate: Entry { block: dividerBlock } }
            DelegateChoice { roleValue: "work"; delegate: Entry { block: workBlock } }
            DelegateChoice { delegate: Entry { block: otherBlock } }
        }
    }

    // One row of the thread: the item's roles, its gap above, and its block.
    component Entry: Item {
        id: row
        required property int index
        required property string kind
        required property string title
        required property string body
        required property string detail
        required property string result
        required property var diff
        required property int added
        required property int removed
        required property int resultLines
        required property bool failed
        required property bool answered
        required property bool afterStep
        required property string record
        required property bool expanded
        required property bool inWork
        required property int steps
        required property bool queued
        required property double at
        required property double duration
        property Component block

        readonly property int gapAbove: index === 0 ? 0
                                      : afterStep ? 2
                                      : inWork ? Math.round(root.fontSize * 0.4)
                                      : kind === "prompt" ? root.fontSize * 2
                                      : kind === "work" ? Math.round(root.fontSize * 0.9)
                                      : kind === "turn" ? 10 : root.fontSize
        // Opened work sits indented under its "Worked for" row, on a rule.
        readonly property int indent: inWork ? Math.round(root.fontSize * 0.9) : 0
        readonly property int column: Math.min(root.columnWidth, row.width - 56)
        width: ListView.view ? ListView.view.width : 0
        height: loader.height + gapAbove
        // A reused row starts closed, whatever the last item opened.
        ListView.onReused: if (loader.item && loader.item.open !== undefined) loader.item.open = false
        Rectangle {
            visible: row.inWork
            x: (row.width - row.column) / 2 + 3
            width: 1
            height: row.height
            color: root.borderColor
        }
        Loader {
            id: loader
            y: row.gapAbove
            width: row.column - row.indent
            x: (row.width - row.column) / 2 + row.indent
            sourceComponent: row.block
            property var entry: row
        }
    }

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                  : event.angleDelta.y / 120 * root.fontSize * 4
            const top = list.originY
            const bottom = Math.max(top, list.originY + list.contentHeight - list.height)
            list.contentY = Math.max(top, Math.min(bottom, list.contentY - step))
            root.following = list.contentY >= bottom - 2
        }
    }

    // Read-only text that selects with the mouse and copies with Command-C.
    component Selectable: TextEdit {
        readOnly: true
        selectByMouse: true
        persistentSelection: false
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        color: root.textColor
        selectionColor: root.tint(root.accentColor, 0.4)
        selectedTextColor: root.textColor
        font.family: root.sansFamily
        font.pixelSize: root.fontSize
        Keys.onPressed: function(event) {
            if (event.matches(StandardKey.Copy)) {
                if (selectedText.length > 0) {
                    if (textFormat === TextEdit.RichText)
                        root.model.copySelection(textDocument, selectionStart, selectionEnd)
                    else
                        root.copy(selectedText)
                }
                event.accepted = true
            } else if (event.matches(StandardKey.SelectAll)) {
                selectAll()
                event.accepted = true
            } else if (event.text.length > 0 && event.text.charCodeAt(0) >= 32
                       && !(event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                // Typing while reading goes to the bar, as in the Codex app.
                input.forceActiveFocus()
                input.insert(input.cursorPosition, event.text)
                event.accepted = true
            }
        }
        onLinkActivated: link => Qt.openUrlExternally(link)
        // A link underlines while the pointer is on it.
        onHoveredLinkChanged: if (textFormat === TextEdit.RichText && root.model)
                                  root.model.hoverLink(textDocument, hoveredLink)
        HoverHandler {
            id: textHover
            cursorShape: parent.hoveredLink.length > 0
                         || ((point.modifiers & Qt.MetaModifier) && parent.pathUnder(point.position).length > 0)
                         ? Qt.PointingHandCursor : Qt.IBeamCursor
        }
        // The file or folder path under a point, or a link to a local file.
        function pathUnder(p) {
            if (!root.model)
                return ""
            if (hoveredLink.length > 0)
                return /^(file:|\/|~)/.test(hoveredLink) ? hoveredLink : ""
            return root.model.pathAt(textDocument, positionAt(p.x, p.y), root.folder)
        }
        // Command-click opens a path in the chosen editor (a folder in the
        // Finder), as Command-click does in a terminal.
        TapHandler {
            acceptedModifiers: Qt.MetaModifier
            onTapped: function(point) {
                const path = parent.pathUnder(point.position)
                if (path.length > 0)
                    root.model.openPath(path, root.editor)
            }
        }
    }

    // A quiet button on the bar: text that lights on hover, with a tooltip.
    // A drawn chevron, centered on its box rather than on a font's baseline.
    // It points right; `turned` rotates it a quarter turn clockwise (down)
    // and `flipped` half a turn more, with a short motion between.
    component Chevron: Canvas {
        id: chevronShape
        property color color: root.mutedTextColor
        property bool turned: false
        property bool flipped: false
        implicitWidth: Math.round(root.codeSize * 0.75)
        implicitHeight: implicitWidth
        rotation: (turned ? 90 : 0) + (flipped ? 180 : 0)
        Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        onColorChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const g = getContext("2d")
            g.reset()
            const s = Math.min(width, height)
            g.strokeStyle = color
            g.lineWidth = Math.max(1.4, s / 7)
            g.lineCap = "round"
            g.lineJoin = "round"
            g.beginPath()
            g.moveTo(width / 2 - s * 0.15, height / 2 - s * 0.3)
            g.lineTo(width / 2 + s * 0.15, height / 2)
            g.lineTo(width / 2 - s * 0.15, height / 2 + s * 0.3)
            g.stroke()
        }
    }

    component BarButton: Rectangle {
        id: barButton
        property string label
        property int labelSize: root.codeSize
        // A menu button: a chevron after the label, pointing up while its
        // menu is open (menus here open upward).
        property bool menu: false
        property bool menuOpen: false
        signal triggered()
        implicitWidth: barRow.implicitWidth + 16
        implicitHeight: Math.round(root.fontSize * 1.6)
        radius: 8
        color: barMouse.containsMouse || menuOpen ? root.tint(root.textColor, 0.08) : "transparent"
        Row {
            id: barRow
            anchors.centerIn: parent
            spacing: Math.round(root.codeSize * 0.35)
            Text {
                id: barText
                anchors.verticalCenter: parent.verticalCenter
                text: barButton.label
                color: barMouse.containsMouse || barButton.menuOpen ? root.textColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: barButton.labelSize
            }
            Chevron {
                visible: barButton.menu
                anchors.verticalCenter: parent.verticalCenter
                color: barText.color
                turned: true
                flipped: barButton.menuOpen
            }
        }
        MouseArea {
            id: barMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: barButton.triggered()
        }
    }

    // A menu of choices ({id, label}) with the current one marked.
    component ChoiceMenu: Menu {
        id: choiceMenu
        property var choices: []
        property string current: ""
        signal chosen(string id)
        // As wide as its longest label, so none is cut short.
        TextMetrics {
            id: widest
            font.family: root.sansFamily
            font.pixelSize: root.codeSize
            text: choiceMenu.choices.reduce((a, c) => c.label.length > a.length ? c.label : a, "")
        }
        width: Math.max(140, Math.ceil(widest.advanceWidth) + Math.round(root.codeSize * 2.6) + 24)
        background: Rectangle {
            radius: 10
            color: root.raisedColor
            border.color: root.borderColor
        }
        Instantiator {
            model: choiceMenu.choices
            delegate: MenuItem {
                id: choiceItem
                required property var modelData
                text: modelData.label
                indicator: null
                arrow: null
                onTriggered: choiceMenu.chosen(modelData.id)
                contentItem: Row {
                    spacing: Math.round(root.codeSize * 0.5)
                    Text {
                        width: Math.round(root.codeSize * 1.1)
                        anchors.verticalCenter: parent.verticalCenter
                        text: choiceItem.modelData.id === choiceMenu.current ? "✓" : ""
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: choiceItem.text
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                    }
                }
                background: Rectangle {
                    radius: 6
                    color: choiceItem.highlighted ? root.tint(root.textColor, 0.1) : "transparent"
                }
            }
            onObjectAdded: (index, object) => choiceMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => choiceMenu.removeItem(object)
        }
    }

    FolderDialog {
        id: folderPicker
        title: qsTr("Attach a folder")
        onAccepted: root.attach(root.model ? root.model.localPath(selectedFolder) : "")
    }
    FileDialog {
        id: filePicker
        title: qsTr("Attach files")
        fileMode: FileDialog.OpenFiles
        onAccepted: selectedFiles.forEach(url => root.attach(root.model ? root.model.localPath(url) : ""))
    }

    // Files dropped anywhere on the thread attach to the next message.
    DropArea {
        id: dropArea
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: function(drop) {
            if (!root.model)
                return
            drop.urls.forEach(url => root.attach(root.model.localPath(url)))
            drop.acceptProposedAction()
            input.forceActiveFocus()
        }
    }

    // A small labeled button in a message's action row.
    component Action: Rectangle {
        id: action
        property string label
        property bool done: false
        signal triggered()
        function flash() { done = true; doneTimer.restart() }
        implicitWidth: actionText.implicitWidth + 16
        implicitHeight: actionText.implicitHeight + 8
        radius: 6
        color: actionMouse.containsMouse ? root.raisedColor : "transparent"
        Text {
            id: actionText
            anchors.centerIn: parent
            text: action.done ? qsTr("Copied") : action.label
            color: actionMouse.containsMouse ? root.textColor : root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.codeSize
        }
        MouseArea {
            id: actionMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.triggered()
        }
        Timer { id: doneTimer; interval: 1200; onTriggered: action.done = false }
    }

    // Files a message names, shown in place: images, SVG and GIFs drawn at
    // their size (up to the column), audio and video played in place, other
    // files as a card that opens them. Click an image to open it full size.
    component MediaList: Column {
        id: mediaList
        property var items: []
        property bool alignRight: false
        spacing: 8
        visible: items.length > 0
        Repeater {
            model: mediaList.items
            delegate: Loader {
                required property var modelData
                readonly property var media: modelData
                anchors.right: mediaList.alignRight ? parent.right : undefined
                sourceComponent: modelData.kind === "image" || modelData.kind === "svg" ? imageMedia
                               : modelData.kind === "animation" ? animationMedia
                               : modelData.kind === "audio" || modelData.kind === "video" ? playbackMedia
                               : fileMedia
                property real room: mediaList.width
            }
        }
    }
    Component {
        id: imageMedia
        Image {
            readonly property var media: parent ? parent.media : null
            readonly property bool vector: media !== null && media.kind === "svg"
            // Its own size from the file's header, before it is decoded.
            readonly property real ownWidth: media && media.width > 0 ? media.width : implicitWidth
            readonly property real ownHeight: media && media.height > 0 ? media.height : implicitHeight
            source: media ? media.url : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            // Never decoded larger than it can be shown; an SVG is drawn at
            // twice its shown size so it stays sharp on Retina.
            sourceSize.width: vector ? Math.round(width * 2) : parent ? Math.round(parent.room * 2) : 0
            // Large images are taken as Retina (2x), as screenshots and
            // 200 dpi charts are; small ones and SVGs show at their own size.
            width: Math.min(parent ? parent.room : 0, !vector && ownWidth >= 800 ? ownWidth / 2 : ownWidth)
            height: ownWidth > 0 ? width * ownHeight / ownWidth : 0
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: if (parent.media) Qt.openUrlExternally(parent.media.url)
            }
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: root.borderColor
                radius: 4
            }
        }
    }
    Component {
        id: animationMedia
        AnimatedImage {
            readonly property var media: parent ? parent.media : null
            source: media ? media.url : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            width: Math.min(parent ? parent.room : 0, implicitWidth)
            height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: if (parent.media) Qt.openUrlExternally(parent.media.url)
            }
        }
    }
    // The player lives in its own file, so the thread still loads where Qt
    // Multimedia is missing; the file then shows as a card.
    Component {
        id: playbackMedia
        Loader {
            id: playbackLoader
            readonly property var media: parent ? parent.media : null
            readonly property real room: parent ? parent.room : 0
            source: Qt.resolvedUrl("MediaPlayback.qml")
            onLoaded: {
                if (typeof item.toggle !== "function")
                    return // the card it fell back to
                item.view = root
                item.media = Qt.binding(() => playbackLoader.media)
                item.room = Qt.binding(() => playbackLoader.room)
            }
            onStatusChanged: if (status === Loader.Error) sourceComponent = fileMedia
        }
    }
    Component {
        id: fileMedia
        Rectangle {
            readonly property var media: parent ? parent.media : null
            width: Math.min(parent ? parent.room : 0, fileRow.implicitWidth + 28)
            height: fileRow.implicitHeight + 18
            radius: root.radius - 2
            color: fileMouse.containsMouse ? root.tint(root.textColor, 0.1) : root.raisedColor
            Row {
                id: fileRow
                anchors.verticalCenter: parent.verticalCenter
                x: 14
                spacing: 10
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: !parent.parent.media ? "" : ({ audio: "♪", video: "▶", html: "◧", pdf: "▤", svg: "◇" })[parent.parent.media.kind] || "▢"
                    color: root.accentColor
                    font.pixelSize: root.stepSize + 2
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: fileRow.parent.media ? fileRow.parent.media.name : ""
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                        elide: Text.ElideMiddle
                        width: Math.min(implicitWidth, root.columnWidth * 0.7)
                    }
                    Text {
                        text: !fileRow.parent.media ? ""
                              : qsTr("%1 · %2 · opens in its app").arg(fileRow.parent.media.type)
                                .arg(root.bytesLabel(fileRow.parent.media.size))
                        color: root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize - 1
                    }
                }
            }
            MouseArea {
                id: fileMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (parent.media) Qt.openUrlExternally(parent.media.url)
            }
        }
    }
    function bytesLabel(n) {
        return n >= 1e9 ? (n / 1e9).toFixed(1) + " GB" : n >= 1e6 ? (n / 1e6).toFixed(1) + " MB"
             : n >= 1e3 ? Math.round(n / 1e3) + " KB" : n + " B"
    }

    // Code or output on a darker card that can be selected and copied.
    component Code: Rectangle {
        id: code
        property alias text: codeText.text
        property alias textColor: codeText.color
        property string language: ""
        // The label over the code; its language unless set.
        property string caption: language
        // Colored by its language (a reply's fenced code, a command).
        property bool highlighted: false
        readonly property bool hovered: codeHover.hovered
        function rehighlight() {
            if (highlighted && root.model)
                root.model.highlight(codeText.textDocument, language, root.lightCode)
        }
        onLanguageChanged: rehighlight()
        Component.onCompleted: rehighlight()
        Connections {
            target: root
            function onLightCodeChanged() { code.rehighlight() }
        }
        height: codeText.height + (caption.length > 0 ? header.height : 0) + 20
        color: root.codeColor
        radius: root.radius - 2
        HoverHandler { id: codeHover }
        Text {
            id: header
            visible: code.caption.length > 0
            x: 12; y: 8
            text: code.caption
            color: root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.codeSize - 1
        }
        // Made on hover only: scrolling past code builds no buttons.
        Loader {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4
            z: 1
            active: code.hovered
            sourceComponent: Component {
                Action {
                    label: qsTr("Copy")
                    onTriggered: { root.copy(code.text); flash() }
                }
            }
        }
        Selectable {
            id: codeText
            x: 12
            y: 10 + (code.caption.length > 0 ? header.height + 4 : 0)
            width: parent.width - 24
            wrapMode: TextEdit.WrapAnywhere
            font.family: root.monoFamily
            font.pixelSize: root.codeSize
        }
    }

    Component {
        id: promptBlock
        Column {
            id: prompt
            readonly property var entry: parent ? parent.entry : null
            spacing: 4
            HoverHandler { id: promptHover }
            Rectangle {
                id: bubble
                anchors.right: parent.right
                width: Math.min(measure.implicitWidth + 36, parent.width * 0.8)
                height: promptText.height + 22
                radius: Math.round(root.fontSize * 1.1)
                color: root.raisedColor
                // Sent while the agent worked: quieter until it takes it in.
                opacity: prompt.entry && prompt.entry.queued ? 0.6 : 1
                Behavior on opacity { NumberAnimation { duration: 160 } }
                Text {
                    id: measure
                    visible: false
                    text: prompt.entry ? prompt.entry.body : ""
                    font: promptText.font
                    textFormat: Text.PlainText
                }
                Selectable {
                    id: promptText
                    x: 18; y: 11
                    width: parent.width - 36
                    text: prompt.entry ? prompt.entry.body : ""
                }
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 6
                visible: prompt.entry !== null && prompt.entry.queued
                text: qsTr("Queued · the agent reads it at its next step")
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.codeSize - 1
            }
            // Images attached to the prompt, under its bubble.
            MediaList {
                anchors.right: parent.right
                width: parent.width * 0.6
                items: prompt.entry && root.model ? root.model.media(prompt.entry.body) : []
            }
            // The row's room is kept so hovering never moves the thread; its
            // buttons are made only while the pointer is over the message.
            Item {
                width: parent.width
                height: root.actionHeight
                Loader {
                    anchors.right: parent.right
                    active: promptHover.hovered
                    sourceComponent: Component {
                        Row {
                            spacing: 2
                            Action {
                                label: qsTr("Copy")
                                onTriggered: { root.copy(prompt.entry.body); flash() }
                            }
                            Action {
                                visible: prompt.entry && prompt.entry.record.length > 0
                                label: qsTr("Edit")
                                onTriggered: root.editRequested(prompt.entry.record, prompt.entry.body)
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: replyBlock
        Column {
            id: reply
            readonly property var entry: parent ? parent.entry : null
            spacing: Math.round(root.fontSize * 0.6)
            HoverHandler { id: replyHover }
            Repeater {
                model: reply.entry && root.model ? root.model.pieces(reply.entry.body) : []
                delegate: Loader {
                    required property var modelData
                    width: reply.width
                    sourceComponent: modelData.code ? codePiece : prosePiece
                    property var piece: modelData
                }
            }
            MediaList {
                width: parent.width
                items: reply.entry && root.model ? root.model.media(reply.entry.body) : []
            }
            Item {
                width: parent.width
                height: root.actionHeight
                Loader {
                    x: -8
                    active: replyHover.hovered
                    sourceComponent: Component {
                        Row {
                            spacing: 2
                            Action {
                                label: qsTr("Copy")
                                onTriggered: { root.copy(reply.entry.body); flash() }
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: prosePiece
        Selectable {
            id: prose
            readonly property string source: parent && parent.piece ? parent.piece.text : ""
            function restyle() {
                if (root.model)
                    root.model.style(textDocument, source, root.proseStyle)
            }
            textFormat: TextEdit.RichText
            onSourceChanged: restyle()
            Component.onCompleted: restyle()
            Connections {
                target: root
                function onStyleKeyChanged() { prose.restyle() }
            }
        }
    }

    Component {
        id: codePiece
        Code {
            text: parent && parent.piece ? parent.piece.text : ""
            language: parent && parent.piece ? parent.piece.language : ""
            highlighted: true
            textColor: root.codeTextColor
        }
    }

    // A tool call or a stretch of thinking: one quiet line that opens.
    Component {
        id: stepBlock
        Column {
            id: step
            readonly property var entry: parent ? parent.entry : null
            readonly property bool thinking: entry && entry.kind === "thinking"
            readonly property bool edit: entry && entry.diff.length > 0
            readonly property bool command: entry && entry.title === "Ran"
            readonly property bool running: entry && !thinking && !entry.answered
            readonly property bool expandable: entry && (thinking ? entry.body.length > 0
                                               : entry.result.length > 0 || entry.detail.length > 0
                                                 || command || entry.body.indexOf("\n") >= 0)
            property bool open: false
            spacing: 6

            Item {
                width: parent.width
                height: Math.max(verb.implicitHeight, root.stepSize + 8)
                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: step.expandable
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: step.open = !step.open
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    spacing: 6
                    Rectangle {
                        visible: step.running
                        width: 6; height: 6; radius: 3
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.activityColor
                    }
                    Text {
                        id: verb
                        anchors.verticalCenter: parent.verticalCenter
                        text: step.entry ? step.entry.title : ""
                        color: step.entry && step.entry.failed ? root.faultColor
                             : hover.containsMouse ? root.textColor : root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                    }
                    Text {
                        id: target
                        visible: text.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - verb.width - counts.width - took.width
                                                       - chevron.width - 36)
                        text: step.entry && !step.thinking ? step.entry.body.split("\n")[0] : ""
                        elide: Text.ElideRight
                        color: step.entry && step.entry.failed ? root.tint(root.faultColor, 0.85) : root.tint(root.textColor, 0.82)
                        font.family: root.monoFamily
                        font.pixelSize: root.codeSize
                    }
                    Row {
                        id: counts
                        visible: step.edit
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text { text: step.entry ? "+" + step.entry.added : ""; color: root.addedColor; font.pixelSize: root.codeSize; font.family: root.monoFamily; visible: step.entry && step.entry.added > 0 }
                        Text { text: step.entry ? "−" + step.entry.removed : ""; color: root.removedColor; font.pixelSize: root.codeSize; font.family: root.monoFamily; visible: step.entry && step.entry.removed > 0 }
                    }
                    // How long the step took, once it took a second or more.
                    Text {
                        id: took
                        visible: step.entry !== null && !step.thinking && step.entry.duration >= 1000
                        width: visible ? implicitWidth : 0
                        anchors.verticalCenter: parent.verticalCenter
                        text: visible ? root.durationLabel(step.entry.duration) : ""
                        color: root.tint(root.mutedTextColor, 0.75)
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize - 2
                        font.features: { "tnum": 1 }
                    }
                    Chevron {
                        id: chevron
                        visible: step.expandable && (hover.containsMouse || step.open)
                        anchors.verticalCenter: parent.verticalCenter
                        turned: step.open
                    }
                }
            }

            // A file change shows its diff without asking, as a short card;
            // other steps make no card at all.
            Loader {
                width: parent.width
                active: step.edit
                visible: active
                sourceComponent: Component {
                    Rectangle {
                        height: diffColumn.height + 12
                        radius: root.radius
                        color: root.codeColor
                        clip: true
                        Column {
                            id: diffColumn
                            y: 6
                            width: parent.width
                            Repeater {
                                model: step.entry ? step.entry.diff.slice(0, step.open ? step.entry.diff.length : 8) : []
                                Rectangle {
                                    required property var modelData
                                    width: diffColumn.width
                                    height: diffLine.implicitHeight + 2
                                    color: modelData.sign === "+" ? root.tint(root.addedColor, 0.16)
                                         : modelData.sign === "-" ? root.tint(root.removedColor, 0.22)
                                         : "transparent"
                                    Text {
                                        id: diffLine
                                        x: 12
                                        width: parent.width - 24
                                        text: modelData.text.length > 0 ? modelData.text : " "
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: root.tint(root.textColor, 0.9)
                                        font.family: root.monoFamily
                                        font.pixelSize: root.codeSize
                                        lineHeight: 1.2
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // What opens on click is made on click: a collapsed step is one line.
            Loader {
                width: parent.width
                active: step.open
                visible: active
                sourceComponent: Component {
                    Column {
                        spacing: 6
                        Selectable {
                            visible: step.entry && step.entry.detail.length > 0
                            width: parent.width
                            text: step.entry ? step.entry.detail : ""
                            color: root.mutedTextColor
                            font.pixelSize: root.stepSize - 1
                        }
                        Code {
                            visible: step.entry && !step.thinking && !step.edit
                                     && (step.command || step.entry.body.indexOf("\n") >= 0)
                            width: parent.width
                            text: step.entry ? step.entry.body : ""
                            // A command reads as shell, without a label.
                            language: step.command ? "bash" : ""
                            caption: ""
                            highlighted: step.command
                            textColor: step.command ? root.codeTextColor : root.textColor
                        }
                        Code {
                            visible: step.entry && step.entry.result.length > 0
                            width: parent.width
                            text: step.entry ? step.entry.result : ""
                            textColor: step.entry && step.entry.failed ? root.faultColor : root.tint(root.textColor, 0.75)
                        }
                        Selectable {
                            visible: step.thinking
                            width: parent.width
                            text: step.entry ? step.entry.body : ""
                            color: root.mutedTextColor
                            font.pixelSize: root.stepSize
                        }
                    }
                }
            }
        }
    }

    // The end of a running turn: "Working for 1m 12s", then the agent's
    // screen as it streams (thinking, tool output, the reply being written),
    // dim and slightly soft so it reads as work in progress. Click to read
    // all of it sharp; it goes when the turn ends and the saved items stand.
    Component {
        id: liveFooter
        Item {
            id: live
            readonly property int column: Math.min(root.columnWidth, width - 56)
            readonly property double since: root.turnSince
            readonly property var shown: root.liveOpen ? root.liveRows : root.liveRows.slice(-8)
            width: ListView.view ? ListView.view.width : 0
            height: (root.busy ? liveHeader.height + liveTail.height + 22 : 0) + 40
            Item {
                id: liveHeader
                visible: root.busy
                x: (live.width - live.column) / 2
                y: 12
                width: live.column
                height: root.stepSize + 10
                MouseArea {
                    id: liveHover
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: root.liveRows.length > 0
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.liveOpen = !root.liveOpen
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 6; height: 6; radius: 3
                        color: root.activityColor
                        SequentialAnimation on opacity {
                            running: root.busy
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.3; duration: 700; easing.type: Easing.InOutSine }
                            NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property string elapsed: live.since > 0 && root.now - live.since >= 1000
                                                          ? root.durationLabel(root.now - live.since) : ""
                        text: !root.working && root.pendingCommand.length > 0
                              ? (elapsed.length > 0 ? qsTr("Running %1 for %2").arg(root.pendingCommand).arg(elapsed)
                                                    : qsTr("Running %1").arg(root.pendingCommand))
                              : elapsed.length > 0 ? qsTr("Working for %1").arg(elapsed) : qsTr("Working")
                        color: liveHover.containsMouse ? root.textColor : root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                        font.features: { "tnum": 1 }
                    }
                    Chevron {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.liveRows.length > 0
                        turned: root.liveOpen
                        color: liveHover.containsMouse ? root.textColor : root.mutedTextColor
                    }
                }
            }
            Item {
                id: liveTail
                visible: root.busy && root.liveRows.length > 0
                x: liveHeader.x + Math.round(root.fontSize * 0.9)
                anchors.top: liveHeader.bottom
                anchors.topMargin: 6
                width: live.column - Math.round(root.fontSize * 0.9)
                height: visible ? tailText.implicitHeight : 0
                clip: true
                Rectangle {
                    x: -Math.round(root.fontSize * 0.9) + 3
                    width: 1
                    height: parent.height
                    color: root.borderColor
                }
                Text {
                    id: tailText
                    width: parent.width
                    text: live.shown.join("\n")
                    textFormat: Text.PlainText
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                    color: root.tint(root.mutedTextColor, root.liveOpen ? 1 : 0.8)
                    font.family: root.monoFamily
                    font.pixelSize: root.codeSize - 1
                    lineHeight: 1.15
                    // Soft while folded: it streams, it is not the answer yet.
                    layer.enabled: !root.liveOpen
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blur: 0.35
                        blurMax: 6
                    }
                }
            }
        }
    }

    // A finished turn's work, folded: "Worked for 3m 12s · 14 steps". Click
    // to show the thinking and steps between the prompt and the reply; each
    // step then opens on its own.
    Component {
        id: workBlock
        Item {
            id: work
            readonly property var entry: parent ? parent.entry : null
            height: Math.max(workText.implicitHeight, root.stepSize + 10)
            MouseArea {
                id: workHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (work.entry && root.model) root.model.toggleWork(work.entry.index)
            }
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Text {
                    id: workText
                    anchors.verticalCenter: parent.verticalCenter
                    text: work.entry ? work.entry.title : ""
                    color: workHover.containsMouse ? root.textColor : root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: work.entry && work.entry.steps > 0
                    // What the work did, as the Codex app sums it up; a
                    // turn of only thinking and notes counts its steps.
                    text: !work.entry ? ""
                          : work.entry.detail !== "" ? work.entry.detail
                          : work.entry.steps === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(work.entry.steps)
                    color: root.tint(root.mutedTextColor, 0.75)
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize - 1
                }
                Chevron {
                    anchors.verticalCenter: parent.verticalCenter
                    turned: work.entry !== null && work.entry.expanded
                    color: workHover.containsMouse ? root.textColor : root.mutedTextColor
                }
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: workText.parent.width + 14
                width: parent.width - x
                height: 1
                color: root.borderColor
            }
        }
    }

    Component {
        id: turnBlock
        Row {
            readonly property var entry: parent ? parent.entry : null
            spacing: 10
            Text {
                id: turnText
                text: entry ? entry.title : ""
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - turnText.width - 10
                height: 1
                color: root.borderColor
            }
        }
    }

    Component {
        id: commandBlock
        // The command, then what it printed: six lines, all of it on a click.
        Column {
            id: commandColumn
            readonly property var entry: parent ? parent.entry : null
            property bool open: false
            width: parent ? parent.width : 0
            spacing: 3
            Text {
                text: commandColumn.entry ? (commandColumn.entry.title
                      + (commandColumn.entry.body.length > 0 ? " " + commandColumn.entry.body : "")) : ""
                color: root.mutedTextColor
                font.family: root.monoFamily
                font.pixelSize: root.codeSize
            }
            Selectable {
                id: commandOutput
                readonly property bool tall: commandColumn.entry ? commandColumn.entry.resultLines > 6 : false
                visible: text.length > 0
                width: parent.width
                text: {
                    const all = commandColumn.entry ? commandColumn.entry.result : ""
                    return tall && !commandColumn.open ? all.split("\n").slice(0, 6).join("\n") : all
                }
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                color: root.tint(root.mutedTextColor, 0.85)
                font.family: root.monoFamily
                font.pixelSize: root.codeSize - 1
            }
            Text {
                visible: commandOutput.tall
                text: commandColumn.open ? qsTr("Show less")
                                         : qsTr("Show all %1 lines").arg(commandColumn.entry ? commandColumn.entry.resultLines : 0)
                color: moreHover.hovered ? root.textColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
                HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: commandColumn.open = !commandColumn.open }
            }
        }
    }

    Component {
        id: noticeBlock
        Selectable {
            readonly property var entry: parent ? parent.entry : null
            text: entry ? entry.title : ""
            color: entry && entry.failed ? root.faultColor : root.mutedTextColor
            font.pixelSize: root.stepSize
        }
    }

    Component {
        id: dividerBlock
        Item {
            readonly property var entry: parent ? parent.entry : null
            height: root.stepSize + 12
            Rectangle { y: parent.height / 2; width: parent.width; height: 1; color: root.borderColor }
            Rectangle {
                anchors.centerIn: parent
                width: dividerText.implicitWidth + 24
                height: parent.height
                color: root.backgroundColor
                Text {
                    id: dividerText
                    anchors.centerIn: parent
                    text: entry ? entry.title : ""
                    color: root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize - 1
                }
            }
        }
    }

    Component {
        id: otherBlock
        Column {
            readonly property var entry: parent ? parent.entry : null
            spacing: 6
            Text {
                text: entry ? "Unrecognized " + entry.title : ""
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize
            }
            Code {
                width: parent.width
                text: entry ? entry.detail : ""
                textColor: root.mutedTextColor
            }
        }
    }

    // The bar to type in, as in the Codex and Claude apps: Return sends to
    // the agent, Shift-Return starts a new line, Tab takes lapis's guess.
    property bool filledFromSuggestion: false
    property string notice: ""
    // Attached files go as @ mentions, which the CLI reads whatever their
    // type (images are seen as images); quoted when the path has spaces.
    function message() {
        const mentions = attachments.map(p => /\s/.test(p) ? "@\"" + p + "\"" : "@" + p)
        const text = input.text.trim()
        return mentions.length === 0 ? input.text
             : text.length === 0 ? mentions.join(" ")
             : input.text.replace(/\s+$/, "") + "\n\n" + mentions.join(" ")
    }
    function send() {
        const text = message()
        if (text.trim().length === 0 || !root.submit)
            return
        // A dialog in the terminal (a picker, settings) would take these
        // keys as its own, so show it instead and keep the message here.
        if (!root.waiting && root.slashCommands && root.screen && root.model
                && root.model.dialogShowing(root.screen())) {
            root.dialogFound()
            return
        }
        const command = root.slashCommands && /^\/[^\s\/]+/.test(text.trim()) ? text.trim().split(/\s/)[0] : ""
        if (root.submit(text)) {
            if (command.length > 0) {
                root.pendingCommand = command
                root.pendingSince = Date.now()
                root.quietTicks = 0
                root.commandSent(text.trim())
            }
            if (root.filledFromSuggestion && text === root.suggestion)
                root.suggestionSent()
            root.filledFromSuggestion = false
            root.attachments = []
            input.clear()
            root.following = true
            list.positionViewAtEnd()
            root.notice = ""
        } else {
            root.notice = root.waiting ? qsTr("Answer the request in the terminal first (Esc)")
                                       : qsTr("Not sent: the agent is not ready for input")
            noticeTimer.restart()
        }
    }
    Timer { id: noticeTimer; interval: 4000; onTriggered: root.notice = "" }

    Item {
        id: composerArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: composerBox.height + Math.round(root.fontSize * 0.8) + hints.height + 6

        // The slash commands that best match what is typed, three at most.
        Rectangle {
            id: slashList
            objectName: "visualSlashList"
            visible: root.slashMatches.length > 0
            z: 2
            width: composerBox.width
            x: composerBox.x
            anchors.bottom: composerBox.top
            anchors.bottomMargin: 6
            height: slashColumn.implicitHeight + 10
            radius: 10
            color: root.raisedColor
            border.color: root.borderColor
            Column {
                id: slashColumn
                x: 5; y: 5
                width: parent.width - 10
                Repeater {
                    model: root.slashMatches
                    delegate: Rectangle {
                        id: slashRow
                        required property var modelData
                        required property int index
                        width: slashColumn.width
                        height: Math.round(root.stepSize * 2.1)
                        radius: 6
                        color: index === root.slashIndex ? root.tint(root.textColor, 0.1) : "transparent"
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            spacing: 12
                            Text {
                                text: "/" + slashRow.modelData.name
                                color: root.textColor
                                font.family: root.monoFamily
                                font.pixelSize: root.codeSize
                            }
                            Text {
                                width: slashColumn.width - x - 20
                                text: slashRow.modelData.description
                                elide: Text.ElideRight
                                color: root.mutedTextColor
                                font.family: root.sansFamily
                                font.pixelSize: root.codeSize
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: root.slashIndex = slashRow.index
                            onClicked: root.completeSlash(slashRow.modelData.name)
                        }
                    }
                }
            }
        }

        Rectangle {
            id: composerBox
            width: Math.min(root.columnWidth, parent.width - 56)
            x: (parent.width - width) / 2
            y: 0
            height: chips.height + inputScroll.height + toolbar.height + 30
            radius: Math.round(root.fontSize * 0.9)
            color: root.raisedColor
            border.width: 1
            border.color: dropArea.containsDrag ? root.accentColor
                        : input.activeFocus ? root.tint(root.accentColor, 0.55) : root.borderColor

            // Files waiting to be sent: name, a thumbnail for an image, ×.
            Flow {
                id: chips
                x: 12; y: root.attachments.length > 0 ? 10 : 0
                width: parent.width - 24
                height: root.attachments.length > 0 ? implicitHeight + 6 : 0
                spacing: 6
                Repeater {
                    model: root.attachments
                    delegate: Rectangle {
                        id: chip
                        required property string modelData
                        readonly property bool image: root.model ? root.model.isImage(modelData) : false
                        height: Math.round(root.codeSize * 2.2)
                        width: chipRow.implicitWidth + 16
                        radius: 8
                        color: root.tint(root.textColor, 0.07)
                        border.color: root.borderColor
                        Row {
                            id: chipRow
                            x: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Image {
                                visible: chip.image
                                width: visible ? chip.height - 8 : 0
                                height: chip.height - 8
                                anchors.verticalCenter: parent.verticalCenter
                                source: chip.image ? "file://" + chip.modelData : ""
                                sourceSize.height: 64
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: chip.modelData.split("/").pop()
                                width: Math.min(implicitWidth, 220)
                                elide: Text.ElideMiddle
                                color: root.textColor
                                font.family: root.sansFamily
                                font.pixelSize: root.codeSize
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "×"
                                color: removeMouse.containsMouse ? root.textColor : root.mutedTextColor
                                font.pixelSize: root.stepSize
                                MouseArea {
                                    id: removeMouse
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.detach(chip.modelData)
                                }
                            }
                        }
                    }
                }
            }

            // lapis's guess, in full: it wraps, and the bar grows to hold it.
            Text {
                id: suggestionText
                x: inputScroll.x
                y: inputScroll.y
                width: inputScroll.width
                visible: input.text.length === 0 && input.preeditText.length === 0
                text: root.waiting ? qsTr("Waiting for your answer in the terminal")
                    : root.suggestion.length > 0 ? root.suggestion
                    : qsTr("Message the agent")
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: root.suggestion.length > 0 && !root.waiting
                       ? root.tint(root.textColor, root.suggestionConfident ? 0.62 : 0.42)
                       : root.mutedTextColor
                font: input.font
            }
            ScrollView {
                id: inputScroll
                x: 16; y: 14 + chips.height
                width: parent.width - 32
                // Two lines at least, so the bar never looks flat; it grows
                // with what is typed (or offered) up to eight.
                height: Math.min(Math.max(input.implicitHeight,
                                          suggestionText.visible ? suggestionText.implicitHeight : 0,
                                          Math.ceil(root.fontSize * 1.45 * 2)),
                                 root.fontSize * 1.45 * 8)
                TextArea {
                    id: input
                    objectName: "visualComposer"
                    focus: true
                    wrapMode: TextArea.Wrap
                    textFormat: TextEdit.PlainText
                    padding: 0
                    leftPadding: 0
                    rightPadding: 0
                    background: null
                    color: root.textColor
                    selectionColor: root.tint(root.accentColor, 0.4)
                    selectedTextColor: root.textColor
                    font.family: root.sansFamily
                    font.pixelSize: root.fontSize
                    onTextChanged: if (text.length === 0) root.filledFromSuggestion = false
                    Keys.onPressed: function(event) {
                        // AppKit marks arrows as keypad keys; lapis keeps
                        // Command as Meta and Control as Control.
                        const mods = event.modifiers & ~Qt.KeypadModifier
                        const plain = mods === Qt.NoModifier
                        const isReturn = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        // The slash list takes arrows, Tab, Return and Escape.
                        if (root.slashMatches.length > 0 && plain) {
                            const count = root.slashMatches.length
                            if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                                root.slashIndex = (root.slashIndex + (event.key === Qt.Key_Down ? 1 : count - 1)) % count
                                event.accepted = true
                                return
                            }
                            // As in the CLI's own list: Tab fills the
                            // command in to add to, Return runs it.
                            if (event.key === Qt.Key_Tab || isReturn) {
                                root.completeSlash(root.slashMatches[Math.min(root.slashIndex, count - 1)].name)
                                if (isReturn)
                                    root.send()
                                event.accepted = true
                                return
                            }
                            if (event.key === Qt.Key_Escape) {
                                root.slashDismissed = true
                                event.accepted = true
                                return
                            }
                        }
                        if (isReturn && (plain || mods === Qt.MetaModifier)) {
                            root.send()
                            event.accepted = true
                        } else if (isReturn && mods === Qt.ShiftModifier) {
                            // A plain newline: Qt's own Shift-Return puts a
                            // line separator (U+2028) that CLIs read oddly.
                            if (selectedText.length > 0)
                                remove(selectionStart, selectionEnd)
                            insert(cursorPosition, "\n")
                            event.accepted = true
                        } else if (root.editKey(input, event.key, mods)) {
                            event.accepted = true
                        } else if (event.key === Qt.Key_Tab && plain) {
                            if (root.suggestion.length > 0 && text.length === 0) {
                                text = root.suggestion
                                cursorPosition = text.length
                                root.filledFromSuggestion = true
                                root.suggestionFilled()
                            } else if (root.filledFromSuggestion && text === root.suggestion) {
                                root.send()
                            } else if (text.length === 0 && root.tabAway) {
                                root.tabAway()
                            }
                            event.accepted = true
                        } else if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                            root.copy(selectedText)
                            event.accepted = true
                        } else if (event.matches(StandardKey.Paste) && root.model) {
                            // Copied files or an image attach; text pastes as text.
                            const files = root.model.pastedFiles()
                            if (files.length > 0) {
                                files.forEach(p => root.attach(p))
                                event.accepted = true
                            }
                        } else if (event.key === Qt.Key_Backspace && text.length === 0
                                   && root.attachments.length > 0) {
                            root.detach(root.attachments[root.attachments.length - 1])
                            event.accepted = true
                        }
                    }
                }
            }

            Item {
                id: toolbar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
                anchors.leftMargin: 16
                height: sendButton.height
                // Left: attach, model, effort. Right: context use, Stop, Send.
                Row {
                    id: controls
                    anchors.left: parent.left
                    anchors.leftMargin: -6
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    BarButton {
                        objectName: "visualAttach"
                        label: "+"
                        labelSize: root.stepSize + 4
                        menuOpen: attachMenu.opened
                        onTriggered: attachMenu.popup(this, 0, -attachMenu.implicitHeight - 6)
                    }
                    BarButton {
                        objectName: "visualModel"
                        visible: root.model !== null && root.model.model.length > 0
                        label: root.modelLabel(root.model ? root.model.model : "")
                        menu: root.models.length > 0
                        menuOpen: modelMenu.opened
                        onTriggered: if (root.models.length > 0) modelMenu.popup(this, 0, -modelMenu.implicitHeight - 6)
                    }
                    BarButton {
                        objectName: "visualEffort"
                        visible: root.efforts.length > 0 || (root.model !== null && root.model.effort.length > 0)
                        label: (root.model && root.model.effort.length > 0
                                ? root.effortLabel(root.model.effort)
                                : qsTr("Effort"))
                        menu: root.efforts.length > 0
                        menuOpen: effortMenu.opened
                        onTriggered: if (root.efforts.length > 0) effortMenu.popup(this, 0, -effortMenu.implicitHeight - 6)
                    }
                }
                ChoiceMenu {
                    id: modelMenu
                    // The CLI's own default entry carries a sentence for a
                    // name; it is just "Default" here.
                    choices: root.models.map(m => ({ id: m.id, label: m.id === "default" ? qsTr("Default")
                                                                                         : (m.name || m.id) }))
                    // The model the agent reports, matched to an entry by its
                    // id or its family ("opus" in "claude-opus-5-5").
                    current: {
                        const running = root.model ? root.model.model : ""
                        const exact = root.models.find(m => m.id === running)
                        if (exact)
                            return exact.id
                        const family = root.models.find(m => m.id !== "default" && running.indexOf(m.id) >= 0)
                        return family ? family.id : ""
                    }
                    onChosen: id => root.modelChosen(id)
                }
                ChoiceMenu {
                    id: attachMenu
                    choices: [{ id: "files", label: qsTr("Files…") }, { id: "folder", label: qsTr("Folder…") }]
                    onChosen: id => id === "folder" ? folderPicker.open() : filePicker.open()
                }
                ChoiceMenu {
                    id: effortMenu
                    choices: root.efforts.map(e => ({ id: e, label: root.effortLabel(e) }))
                    current: root.model ? root.model.effort : ""
                    onChosen: id => root.effortChosen(id)
                }
                // How full the context window is: a ring, the share used and
                // the tokens against the window, always in view.
                Item {
                    id: contextMeter
                    objectName: "visualContext"
                    readonly property real used: root.model && root.model.contextWindow > 0
                                                 ? Math.min(1, root.model.contextTokens / root.model.contextWindow) : 0
                    visible: root.model !== null && root.model.contextTokens > 0
                    anchors.right: sendButton.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: visible ? ring.width + percent.implicitWidth + 6 : 0
                    height: sendButton.height
                    Canvas {
                        id: ring
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(root.codeSize * 1.3)
                        height: width
                        readonly property real fraction: contextMeter.used
                        readonly property color track: root.tint(root.textColor, 0.15)
                        readonly property color fill: contextMeter.used > 0.9 ? root.faultColor
                                                    : contextMeter.used > 0.75 ? Qt.tint(root.mutedTextColor, root.tint(root.faultColor, 0.5))
                                                    : root.mutedTextColor
                        onFractionChanged: requestPaint()
                        onFillChanged: requestPaint()
                        onPaint: {
                            const g = getContext("2d")
                            g.reset()
                            const r = width / 2 - 2
                            g.lineWidth = 2.5
                            g.strokeStyle = track
                            g.beginPath(); g.arc(width / 2, height / 2, r, 0, 2 * Math.PI); g.stroke()
                            g.strokeStyle = fill
                            g.beginPath()
                            g.arc(width / 2, height / 2, r, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * fraction)
                            g.stroke()
                        }
                    }
                    Text {
                        id: percent
                        anchors.left: ring.right
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.model
                              ? qsTr("%1% · %2 / %3").arg(Math.round(contextMeter.used * 100))
                                .arg(root.tokensLabel(root.model.contextTokens))
                                .arg(root.tokensLabel(root.model.contextWindow))
                              : ""
                        color: root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                        font.features: { "tnum": 1 }
                    }
                }
                Text {
                    anchors.left: controls.right
                    anchors.leftMargin: 8
                    anchors.right: contextMeter.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: root.notice.length > 0 ? root.notice
                        : root.waiting ? (root.waitingReason.length > 0 ? root.waitingReason
                                                                         : qsTr("The agent is waiting on a request"))
                        : root.suggestion.length > 0 && input.text.length === 0 ? qsTr("Tab to use the suggestion")
                        : root.filledFromSuggestion ? qsTr("Tab or Return to send")
                        : ""
                    color: root.notice.length > 0 || root.waiting ? root.faultColor : root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: Math.max(11, root.codeSize - 1)
                }
                // Send, or Stop while a turn runs: Return sends (a message
                // typed mid-turn queues for the agent), so the button stops.
                Rectangle {
                    id: sendButton
                    objectName: "visualSend"
                    readonly property bool stops: root.working && root.interrupt !== null
                    readonly property bool ready: stops
                                                  || ((input.text.trim().length > 0 || root.attachments.length > 0)
                                                      && root.canSend && !root.waiting)
                    anchors.right: parent.right
                    width: Math.round(root.fontSize * 1.75)
                    height: width
                    radius: width / 2
                    color: ready ? (sendMouse.containsMouse ? root.tint(root.textColor, 0.85) : root.textColor)
                                 : root.tint(root.textColor, 0.18)
                    Text {
                        anchors.centerIn: parent
                        visible: !sendButton.stops
                        text: "↑"
                        color: root.backgroundColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                        font.bold: true
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        visible: sendButton.stops
                        width: Math.round(parent.width * 0.32)
                        height: width
                        radius: 2
                        color: root.backgroundColor
                    }
                    MouseArea {
                        id: sendMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: sendButton.ready
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: sendButton.stops ? root.interrupt() : root.send()
                    }
                }
            }
        }

        Text {
            id: hints
            anchors.top: composerBox.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: composerBox.horizontalCenter
            text: qsTr("Return to send · Shift-Return for a new line · ⌘-click a path to open it · Esc for the terminal")
            color: root.tint(root.mutedTextColor, 0.8)
            font.family: root.sansFamily
            font.pixelSize: Math.max(11, root.codeSize - 3)
        }
    }

    Text {
        visible: root.model !== null && root.model.count === 0 && !root.model.loading
        anchors.centerIn: parent
        text: qsTr("Nothing saved in this conversation yet")
        color: root.mutedTextColor
        font.family: root.sansFamily
        font.pixelSize: root.stepSize
    }

    // Back to the newest item; the view follows from there.
    Rectangle {
        visible: !root.following && list.count > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: composerArea.top
        anchors.bottomMargin: 12
        width: root.fontSize * 2.2; height: width; radius: width / 2
        color: root.raisedColor
        border.color: root.borderColor
        Text { anchors.centerIn: parent; text: "↓"; color: root.textColor; font.pixelSize: root.stepSize }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.following = true; list.positionViewAtEnd() }
        }
    }
}
