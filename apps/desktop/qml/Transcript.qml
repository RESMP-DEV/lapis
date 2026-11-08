import QtQuick
import QtQuick.Controls
import QtQml.Models

// The selected agent's conversation as a readable thread: the person's
// prompts as bubbles, the agent's replies as prose, and its work as quiet
// one-line steps ("Ran", "Edited", "Read") that open for detail. Text is
// selectable and every message can be copied; a reply can be forked and a
// prompt edited, each into a new agent. The terminal stays where input goes.
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
    property bool working: false        // a turn is running: Stop is offered
    property bool waiting: false        // a request waits in the terminal
    property string waitingReason: ""
    property bool canSend: true
    readonly property alias composer: input

    // Escape asks to go back to the terminal.
    signal dismissed()
    // Tab put the suggestion in the bar, and a second Tab sent it unchanged.
    signal suggestionFilled()
    signal suggestionSent()
    // Start a new agent from this conversation up to a reply's record.
    signal forkRequested(string record)
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
    readonly property string styleKey: [sansFamily, monoFamily, fontSize, codeSize, codeColor, accentColor].join("|")
    readonly property var proseStyle: ({ sans: sansFamily, mono: monoFamily, size: fontSize, codeSize: codeSize,
                                         codeColor: codeColor, linkColor: accentColor })

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
        footer: Item { height: 40 }
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
        property Component block

        readonly property int gapAbove: index === 0 ? 0
                                      : afterStep ? 2
                                      : kind === "prompt" ? root.fontSize * 2
                                      : kind === "turn" ? 10 : root.fontSize
        width: ListView.view ? ListView.view.width : 0
        height: loader.height + gapAbove
        // A reused row starts closed, whatever the last item opened.
        ListView.onReused: if (loader.item && loader.item.open !== undefined) loader.item.open = false
        Loader {
            id: loader
            y: row.gapAbove
            width: Math.min(root.columnWidth, row.width - 56)
            x: (row.width - width) / 2
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
        HoverHandler {
            cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
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

    // Code or output on a darker card that can be selected and copied.
    component Code: Rectangle {
        id: code
        property alias text: codeText.text
        property alias textColor: codeText.color
        property string language: ""
        readonly property bool hovered: codeHover.hovered
        height: codeText.height + (language.length > 0 ? header.height : 0) + 20
        color: root.codeColor
        radius: root.radius - 2
        HoverHandler { id: codeHover }
        Text {
            id: header
            visible: code.language.length > 0
            x: 12; y: 8
            text: code.language
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
            y: 10 + (code.language.length > 0 ? header.height + 4 : 0)
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
                            Action {
                                visible: reply.entry && reply.entry.record.length > 0
                                label: qsTr("Fork from here")
                                onTriggered: root.forkRequested(reply.entry.record)
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
                        width: Math.min(implicitWidth, parent.width - verb.width - counts.width - chevron.width - 30)
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
                    Text {
                        id: chevron
                        visible: step.expandable && (hover.containsMouse || step.open)
                        anchors.verticalCenter: parent.verticalCenter
                        text: step.open ? "⌄" : "›"
                        color: root.mutedTextColor
                        font.pixelSize: root.stepSize
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
        Text {
            readonly property var entry: parent ? parent.entry : null
            text: entry ? (entry.title + (entry.body.length > 0 ? " " + entry.body : "")) : ""
            color: root.mutedTextColor
            font.family: root.monoFamily
            font.pixelSize: root.codeSize
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
    function send() {
        const text = input.text
        if (text.trim().length === 0 || !root.submit)
            return
        if (root.submit(text)) {
            if (root.filledFromSuggestion && text === root.suggestion)
                root.suggestionSent()
            root.filledFromSuggestion = false
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

        Rectangle {
            id: composerBox
            width: Math.min(root.columnWidth, parent.width - 56)
            x: (parent.width - width) / 2
            y: 0
            height: inputScroll.height + toolbar.height + 18
            radius: Math.round(root.fontSize * 0.9)
            color: root.raisedColor
            border.width: 1
            border.color: input.activeFocus ? root.tint(root.accentColor, 0.55) : root.borderColor

            ScrollView {
                id: inputScroll
                x: 14; y: 10
                width: parent.width - 28
                height: Math.min(Math.max(input.implicitHeight, root.fontSize * 1.6),
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
                    placeholderText: root.waiting ? qsTr("Waiting for your answer in the terminal")
                                   : root.suggestion.length > 0 ? root.suggestion
                                   : qsTr("Message the agent")
                    placeholderTextColor: root.suggestion.length > 0 && !root.waiting
                                          ? root.tint(root.textColor, root.suggestionConfident ? 0.62 : 0.42)
                                          : root.mutedTextColor
                    onTextChanged: if (text.length === 0) root.filledFromSuggestion = false
                    Keys.onPressed: function(event) {
                        const plain = !(event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier
                                                           | Qt.MetaModifier | Qt.AltModifier))
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && plain) {
                            root.send()
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
                        }
                    }
                }
            }

            Item {
                id: toolbar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                anchors.leftMargin: 14
                height: sendButton.height
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - sendButton.width - stopButton.width - 24
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
                Rectangle {
                    id: stopButton
                    visible: root.working && root.interrupt !== null
                    width: visible ? stopText.implicitWidth + 20 : 0
                    height: sendButton.height
                    anchors.right: sendButton.left
                    anchors.rightMargin: 8
                    radius: height / 2
                    color: stopMouse.containsMouse ? root.tint(root.textColor, 0.12) : "transparent"
                    border.color: root.borderColor
                    Text {
                        id: stopText
                        anchors.centerIn: parent
                        text: qsTr("Stop")
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                    }
                    MouseArea {
                        id: stopMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.interrupt()
                    }
                }
                Rectangle {
                    id: sendButton
                    readonly property bool ready: input.text.trim().length > 0 && root.canSend && !root.waiting
                    anchors.right: parent.right
                    width: Math.round(root.fontSize * 1.6)
                    height: width
                    radius: width / 2
                    color: ready ? root.textColor : root.tint(root.textColor, 0.18)
                    Text {
                        anchors.centerIn: parent
                        text: "↑"
                        color: root.backgroundColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: sendButton.ready
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.send()
                    }
                }
            }
        }

        Text {
            id: hints
            anchors.top: composerBox.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: composerBox.horizontalCenter
            text: qsTr("Return to send · Shift-Return for a new line · Esc for the terminal")
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
