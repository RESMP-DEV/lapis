import QtQuick

// The selected agent's conversation as messages and tool cards: a read-only
// view beside its terminal, which stays where input goes. Kinds the reader
// does not know show as plain blocks so new CLI features never vanish.
Rectangle {
    id: root

    property var model: null
    property color backgroundColor: "#000000"
    property color cardColor: "#000000"
    property color borderColor: "#222222"
    property color accentColor: "#62d6c2"
    property color textColor: "#e6e6e6"
    property color mutedTextColor: "#8e8e8e"
    property color activityColor: "#6aa6ff"
    property color faultColor: "#ff6f86"
    property color plentyColor: "#6fdc8c"
    property string monoFamily: "Menlo"
    property int fontSize: 14
    property int radius: 2
    // The reading column's widest extent; wider stages center it.
    property int columnWidth: 860
    readonly property alias list: list
    readonly property int monoSize: Math.max(10, fontSize - 2)
    readonly property int smallSize: Math.max(10, fontSize - 3)

    color: backgroundColor

    // Follow new items while the reader sits at the end, as a terminal does.
    property bool following: true

    ListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        model: root.model
        spacing: 10
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        cacheBuffer: 1200
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
            required property int resultLines
            required property bool failed
            required property bool answered

            width: ListView.view.width
            height: loader.height
            Loader {
                id: loader
                width: Math.min(root.columnWidth, row.width - 32)
                x: (row.width - width) / 2
                sourceComponent: row.kind === "prompt" ? promptBlock
                               : row.kind === "reply" ? replyBlock
                               : row.kind === "thinking" ? thinkingBlock
                               : row.kind === "tool" ? toolBlock
                               : row.kind === "command" ? commandBlock
                               : row.kind === "notice" ? noticeBlock
                               : row.kind === "divider" ? dividerBlock
                               : otherBlock
                property var entry: row
            }
        }
    }

    component Label: Text {
        color: root.mutedTextColor
        font.pixelSize: root.smallSize
        font.letterSpacing: 0.6
        font.capitalization: Font.AllUppercase
    }

    component Copyable: TextEdit {
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        color: root.textColor
        selectionColor: Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.35)
        selectedTextColor: root.textColor
        font.pixelSize: root.fontSize
    }

    Component {
        id: promptBlock
        Rectangle {
            readonly property var entry: parent ? parent.entry : null
            height: promptText.implicitHeight + 24
            color: Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.07)
            radius: root.radius
            Rectangle { width: 2; height: parent.height; color: root.accentColor }
            Copyable {
                id: promptText
                x: 16; y: 12
                width: parent.width - 32
                text: entry ? entry.body : ""
            }
        }
    }

    Component {
        id: replyBlock
        Copyable {
            readonly property var entry: parent ? parent.entry : null
            leftPadding: 2
            textFormat: TextEdit.MarkdownText
            text: entry ? entry.body : ""
            onLinkActivated: link => Qt.openUrlExternally(link)
        }
    }

    Component {
        id: thinkingBlock
        Column {
            readonly property var entry: parent ? parent.entry : null
            property bool open: false
            spacing: 4
            Text {
                text: (parent.open ? "▾ " : "▸ ") + "Thought" +
                      (entry && entry.body.length > 0 && !parent.open
                           ? "  ·  " + entry.body.split("\n")[0] : "")
                width: parent.width
                elide: Text.ElideRight
                color: root.mutedTextColor
                font.pixelSize: root.smallSize + 1
                font.italic: true
                MouseArea {
                    anchors.fill: parent
                    enabled: entry && entry.body.length > 0
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: parent.parent.open = !parent.parent.open
                }
            }
            Copyable {
                visible: parent.open
                width: parent.width
                leftPadding: 14
                color: root.mutedTextColor
                font.pixelSize: root.smallSize + 1
                font.italic: true
                text: entry ? entry.body : ""
            }
        }
    }

    Component {
        id: toolBlock
        Rectangle {
            id: card
            readonly property var entry: parent ? parent.entry : null
            readonly property color tone: !entry || !entry.answered ? root.activityColor
                                        : entry.failed ? root.faultColor : root.plentyColor
            property bool open: false
            height: toolColumn.implicitHeight + 20
            color: root.cardColor
            border.color: entry && entry.failed ? Qt.rgba(root.faultColor.r, root.faultColor.g, root.faultColor.b, 0.6)
                                                : root.borderColor
            border.width: 1
            radius: root.radius

            Column {
                id: toolColumn
                x: 12; y: 10
                width: parent.width - 24
                spacing: 8

                Row {
                    spacing: 8
                    width: parent.width
                    Rectangle {
                        width: 7; height: 7; radius: 3.5
                        anchors.verticalCenter: parent.verticalCenter
                        color: card.tone
                    }
                    Text {
                        id: toolName
                        text: entry ? entry.title : ""
                        color: root.textColor
                        font.family: root.monoFamily
                        font.pixelSize: root.monoSize
                        font.bold: true
                    }
                    Text {
                        text: entry ? entry.detail : ""
                        width: parent.width - toolName.width - 30
                        elide: Text.ElideRight
                        color: root.mutedTextColor
                        font.pixelSize: root.smallSize + 1
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Copyable {
                    visible: text.length > 0
                    width: parent.width
                    text: entry ? entry.body : ""
                    font.family: root.monoFamily
                    font.pixelSize: root.monoSize
                    wrapMode: TextEdit.WrapAnywhere
                }

                Column {
                    visible: entry && entry.diff.length > 0
                    width: parent.width
                    Repeater {
                        model: entry ? entry.diff : []
                        Rectangle {
                            required property var modelData
                            width: parent.width
                            height: diffText.implicitHeight + 2
                            color: modelData.sign === "+" ? Qt.rgba(root.plentyColor.r, root.plentyColor.g, root.plentyColor.b, 0.10)
                                 : modelData.sign === "-" ? Qt.rgba(root.faultColor.r, root.faultColor.g, root.faultColor.b, 0.10)
                                 : "transparent"
                            Text {
                                id: diffText
                                x: 6
                                width: parent.width - 12
                                text: modelData.sign + " " + modelData.text
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 4
                                color: modelData.sign === "+" ? root.plentyColor
                                     : modelData.sign === "-" ? root.faultColor : root.mutedTextColor
                                font.family: root.monoFamily
                                font.pixelSize: root.monoSize
                            }
                        }
                    }
                }

                Item {
                    visible: entry && entry.result.length > 0
                    width: parent.width
                    height: visible ? resultBox.height : 0
                    Rectangle {
                        id: resultBox
                        width: parent.width
                        height: (card.open ? resultText.implicitHeight : Math.min(resultText.implicitHeight, 5 * resultText.font.pixelSize * 1.3)) + 14
                        color: Qt.rgba(1, 1, 1, 0.03)
                        radius: root.radius
                        clip: true
                        Text {
                            id: resultText
                            x: 8; y: 7
                            width: parent.width - 16
                            text: entry ? entry.result : ""
                            wrapMode: Text.WrapAnywhere
                            color: entry && entry.failed ? root.faultColor : root.mutedTextColor
                            font.family: root.monoFamily
                            font.pixelSize: root.monoSize - 1
                        }
                    }
                }

                Text {
                    readonly property int hidden: entry ? Math.max(0, entry.resultLines - 5) : 0
                    visible: hidden > 0 && entry.result.length > 0
                    text: card.open ? "Show less"
                                    : "Show more (" + entry.resultLines + " lines)"
                    color: root.accentColor
                    font.pixelSize: root.smallSize
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.open = !card.open
                    }
                }
            }
        }
    }

    Component {
        id: commandBlock
        Row {
            readonly property var entry: parent ? parent.entry : null
            spacing: 8
            Rectangle {
                width: commandText.implicitWidth + 16
                height: commandText.implicitHeight + 8
                radius: root.radius
                color: "transparent"
                border.color: root.borderColor
                Text {
                    id: commandText
                    anchors.centerIn: parent
                    text: entry ? (entry.title + (entry.body.length > 0 ? " " + entry.body : "")) : ""
                    color: root.mutedTextColor
                    font.family: root.monoFamily
                    font.pixelSize: root.monoSize
                }
            }
        }
    }

    Component {
        id: noticeBlock
        Text {
            readonly property var entry: parent ? parent.entry : null
            text: "◆  " + (entry ? entry.title : "")
            wrapMode: Text.Wrap
            color: root.mutedTextColor
            font.pixelSize: root.smallSize + 1
        }
    }

    Component {
        id: dividerBlock
        Item {
            readonly property var entry: parent ? parent.entry : null
            height: 28
            Rectangle { y: 14; width: parent.width; height: 1; color: root.borderColor }
            Rectangle {
                anchors.centerIn: parent
                width: dividerText.implicitWidth + 20
                height: dividerText.implicitHeight + 4
                color: root.backgroundColor
                Label { id: dividerText; anchors.centerIn: parent; text: entry ? entry.title : "" }
            }
        }
    }

    Component {
        id: otherBlock
        Rectangle {
            readonly property var entry: parent ? parent.entry : null
            height: otherColumn.implicitHeight + 16
            color: "transparent"
            border.color: root.borderColor
            radius: root.radius
            Column {
                id: otherColumn
                x: 10; y: 8
                width: parent.width - 20
                spacing: 4
                Label { text: entry ? entry.title : "" }
                Text {
                    width: parent.width
                    text: entry ? entry.detail : ""
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 4
                    elide: Text.ElideRight
                    color: root.mutedTextColor
                    font.family: root.monoFamily
                    font.pixelSize: root.monoSize - 1
                }
            }
        }
    }

    // Back to the newest item; the view follows from there.
    Rectangle {
        visible: !root.following && list.count > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        width: jumpText.implicitWidth + 20
        height: jumpText.implicitHeight + 10
        radius: root.radius
        color: root.cardColor
        border.color: root.accentColor
        Text { id: jumpText; anchors.centerIn: parent; text: "Latest ↓"; color: root.accentColor; font.pixelSize: root.smallSize + 1 }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.following = true; list.positionViewAtEnd() }
        }
    }
}
