import QtQuick

// The selected agent's conversation as a readable thread: the person's
// prompts as bubbles, the agent's replies as prose, and its work as quiet
// one-line steps ("Ran", "Edited", "Read") that open for detail. Read-only;
// the terminal stays where input goes. Kinds the reader does not know show as
// plain blocks so new CLI features never vanish.
Rectangle {
    id: root

    property var model: null
    property color backgroundColor: "#1a1b26"
    property color raisedColor: "#24283b"
    property color codeColor: "#16161e"
    property color borderColor: "#292e42"
    property color textColor: "#c0caf5"
    property color mutedTextColor: "#737aa2"
    property color accentColor: "#7aa2f7"
    property color addedColor: "#449dab"
    property color removedColor: "#914c54"
    property color faultColor: "#f7768e"
    property color activityColor: "#7aa2f7"
    // Empty uses the platform's interface font.
    property string sansFamily: ""
    property string monoFamily: "Menlo"
    property int fontSize: 15     // prose
    property int stepSize: 14     // step rows and labels
    property int codeSize: 12     // commands, paths, output, diffs
    property int radius: 8
    property int columnWidth: 760
    readonly property alias list: list
    // Follow new items while the reader sits at the end, as a terminal does.
    property bool following: true

    color: backgroundColor

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    ListView {
        id: list
        anchors.fill: parent
        model: root.model
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        cacheBuffer: 1600
        header: Item { height: 20 }
        footer: Item { height: 28 }
        onMovementEnded: root.following = atYEnd
        onCountChanged: if (root.following) Qt.callLater(positionViewAtEnd)

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

            readonly property int gapAbove: index === 0 ? 0
                                          : afterStep ? 2
                                          : kind === "prompt" ? 32
                                          : kind === "turn" ? 10 : 16
            width: ListView.view.width
            height: loader.height + gapAbove
            Loader {
                id: loader
                y: row.gapAbove
                width: Math.min(root.columnWidth, row.width - 48)
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

    component Code: Rectangle {
        property alias text: codeText.text
        property alias textColor: codeText.color
        property int maxLines: 0
        height: codeText.height + 16
        color: root.codeColor
        radius: root.radius - 2
        Text {
            id: codeText
            x: 12; y: 8
            width: parent.width - 24
            wrapMode: Text.WrapAnywhere
            maximumLineCount: parent.maxLines > 0 ? parent.maxLines : 100000
            elide: Text.ElideRight
            color: root.textColor
            font.family: root.monoFamily
            font.pixelSize: root.codeSize
            lineHeight: 1.25
        }
    }

    Component {
        id: promptBlock
        Item {
            readonly property var entry: parent ? parent.entry : null
            height: bubble.height
            Rectangle {
                id: bubble
                anchors.right: parent.right
                width: Math.min(promptText.implicitWidth + 32, parent.width * 0.8)
                height: promptText.height + 20
                radius: 18
                color: root.raisedColor
                Text {
                    id: promptText
                    x: 16; y: 10
                    width: parent.width - 32
                    wrapMode: Text.Wrap
                    text: entry ? entry.body : ""
                    textFormat: Text.PlainText
                    maximumLineCount: 18
                    elide: Text.ElideRight
                    color: root.textColor
                    font.family: root.sansFamily
                    font.pixelSize: root.fontSize
                    lineHeight: 1.3
                }
            }
        }
    }

    Component {
        id: replyBlock
        Text {
            readonly property var entry: parent ? parent.entry : null
            wrapMode: Text.Wrap
            textFormat: Text.MarkdownText
            text: entry ? entry.body : ""
            color: root.textColor
            linkColor: root.accentColor
            font.family: root.sansFamily
            font.pixelSize: root.fontSize
            lineHeight: 1.4
            onLinkActivated: link => Qt.openUrlExternally(link)
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
            readonly property bool running: entry && !thinking && !entry.answered
            readonly property bool expandable: entry && (thinking ? entry.body.length > 0
                                               : entry.result.length > 0 || entry.detail.length > 0
                                                 || entry.body.indexOf("\n") >= 0)
            property bool open: false
            spacing: 6

            Item {
                width: parent.width
                height: Math.max(verb.implicitHeight, 22)
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
                        text: entry ? entry.title : ""
                        color: entry && entry.failed ? root.faultColor
                             : hover.containsMouse ? root.textColor : root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                    }
                    Text {
                        id: target
                        visible: text.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - verb.width - counts.width - chevron.width - 30)
                        text: entry && !step.thinking ? entry.body.split("\n")[0] : ""
                        elide: Text.ElideRight
                        color: entry && entry.failed ? root.tint(root.faultColor, 0.85) : root.tint(root.textColor, 0.82)
                        font.family: root.monoFamily
                        font.pixelSize: root.codeSize
                    }
                    Row {
                        id: counts
                        visible: step.edit
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text { text: entry ? "+" + entry.added : ""; color: root.addedColor; font.pixelSize: root.codeSize; font.family: root.monoFamily; visible: entry && entry.added > 0 }
                        Text { text: entry ? "−" + entry.removed : ""; color: root.removedColor; font.pixelSize: root.codeSize; font.family: root.monoFamily; visible: entry && entry.removed > 0 }
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
                        model: entry && step.edit ? entry.diff.slice(0, step.open ? entry.diff.length : 8) : []
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

            Text {
                visible: step.open && entry && entry.detail.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: entry ? entry.detail : ""
                textFormat: Text.PlainText
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
            }
            Code {
                visible: step.open && entry && !step.thinking && !step.edit && entry.body.indexOf("\n") >= 0
                width: parent.width
                text: entry ? entry.body : ""
            }
            Code {
                visible: step.open && entry && entry.result.length > 0
                width: parent.width
                text: entry ? entry.result : ""
                textColor: entry && entry.failed ? root.faultColor : root.tint(root.textColor, 0.75)
            }
            Text {
                visible: step.open && step.thinking
                width: parent.width
                wrapMode: Text.Wrap
                text: entry ? entry.body : ""
                textFormat: Text.PlainText
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize
                lineHeight: 1.35
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
        Text {
            readonly property var entry: parent ? parent.entry : null
            text: entry ? entry.title : ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: entry && entry.failed ? root.faultColor : root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.stepSize
        }
    }

    Component {
        id: dividerBlock
        Item {
            readonly property var entry: parent ? parent.entry : null
            height: 24
            Rectangle { y: 12; width: parent.width; height: 1; color: root.borderColor }
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
                maxLines: 3
                text: entry ? entry.detail : ""
                textColor: root.mutedTextColor
            }
        }
    }

    // Back to the newest item; the view follows from there.
    Rectangle {
        visible: !root.following && list.count > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: 34; height: 34; radius: 17
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
