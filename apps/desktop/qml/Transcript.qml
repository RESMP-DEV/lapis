import QtQuick
import QtQuick.Controls

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

    // Escape asks to go back to the terminal; the view itself never types.
    signal dismissed()
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

    ListView {
        id: list
        anchors.fill: parent
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
            onPressedChanged: if (!pressed) root.following = list.atYEnd
        }

        delegate: Item {
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

            readonly property int gapAbove: index === 0 ? 0
                                          : afterStep ? 2
                                          : kind === "prompt" ? root.fontSize * 2
                                          : kind === "turn" ? 10 : root.fontSize
            width: ListView.view.width
            height: loader.height + gapAbove
            Loader {
                id: loader
                y: row.gapAbove
                width: Math.min(root.columnWidth, row.width - 56)
                x: (row.width - width) / 2
                sourceComponent: row.kind === "prompt" ? promptBlock
                               : row.kind === "reply" ? replyBlock
                               : row.kind === "thinking" || row.kind === "tool" ? stepBlock
                               : row.kind === "turn" ? turnBlock
                               : row.kind === "command" ? commandBlock
                               : row.kind === "notice" ? noticeBlock
                               : row.kind === "divider" ? dividerBlock
                               : otherBlock
                property var entry: row
            }
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
            if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                copy()
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
        Action {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4
            z: 1
            visible: code.hovered || done
            label: qsTr("Copy")
            onTriggered: { root.copy(code.text); flash() }
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
            Row {
                anchors.right: parent.right
                spacing: 2
                opacity: promptHover.hovered || copyPrompt.done ? 1 : 0
                Action {
                    id: copyPrompt
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
            Row {
                spacing: 2
                x: -8
                opacity: replyHover.hovered || copyReply.done ? 1 : 0
                Action {
                    id: copyReply
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

            // A file change shows its diff without asking, as a short card.
            Rectangle {
                visible: step.edit
                width: parent.width
                height: visible ? diffColumn.height + 12 : 0
                radius: root.radius
                color: root.codeColor
                clip: true
                Column {
                    id: diffColumn
                    y: 6
                    width: parent.width
                    Repeater {
                        model: step.entry && step.edit ? step.entry.diff.slice(0, step.open ? step.entry.diff.length : 8) : []
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

            Selectable {
                visible: step.open && step.entry && step.entry.detail.length > 0
                width: parent.width
                text: step.entry ? step.entry.detail : ""
                color: root.mutedTextColor
                font.pixelSize: root.stepSize - 1
            }
            Code {
                visible: step.open && step.entry && !step.thinking && !step.edit
                         && (step.command || step.entry.body.indexOf("\n") >= 0)
                width: parent.width
                text: step.entry ? step.entry.body : ""
            }
            Code {
                visible: step.open && step.entry && step.entry.result.length > 0
                width: parent.width
                text: step.entry ? step.entry.result : ""
                textColor: step.entry && step.entry.failed ? root.faultColor : root.tint(root.textColor, 0.75)
            }
            Selectable {
                visible: step.open && step.thinking
                width: parent.width
                text: step.entry ? step.entry.body : ""
                color: root.mutedTextColor
                font.pixelSize: root.stepSize
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

    Text {
        visible: root.activeFocus
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 12
        text: qsTr("Esc for the terminal")
        color: root.mutedTextColor
        font.family: root.sansFamily
        font.pixelSize: Math.max(11, root.codeSize - 2)
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
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
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
