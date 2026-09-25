import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: commandsPopup
    objectName: "commandsDialog"
    property var commands: []
    property color surfaceColor: "#0e1822"
    property color textColor: "#e4eef1"
    property color mutedColor: "#94aab7"
    property color accentColor: "#70d8c5"
    property color selectionColor: "#193638"
    property color hoverColor: "#1b303e"
    property color borderColor: "#263c48"
    property string monoFamily: "monospace"
    property int uiFont: 13
    property int readoutFont: 12
    property int chromeRadius: 2
    property int motionDuration: 100
    property bool motionEnabled: false
    property string pendingCommand: ""
    property string selectedCommandId: ""
    readonly property var filteredCommands: {
        const terms = search.text.toLowerCase().trim().split(/\s+/).filter(t => t.length)
        return commands.filter(command => terms.every(term =>
            (command.label + " " + command.shortcut).toLowerCase().includes(term)))
    }
    signal requested(string commandId)
    modal: true
    focus: true
    title: qsTr("Commands")
    anchors.centerIn: parent
    width: Math.min(620, parent.width - 32)
    height: Math.min(520, parent.height - 32)
    padding: 16
    font.pixelSize: uiFont
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: commandsPopup.surfaceColor
        border.color: commandsPopup.accentColor
        radius: commandsPopup.chromeRadius
    }

    function choose(index) {
        const command = filteredCommands[index]
        if (!command || !command.enabled)
            return
        pendingCommand = command.id
        close()
    }
    function step(delta) {
        if (!filteredCommands.length)
            return
        results.currentIndex = (results.currentIndex + delta + filteredCommands.length) % filteredCommands.length
        selectedCommandId = filteredCommands[results.currentIndex].id
        results.positionViewAtIndex(results.currentIndex, ListView.Contain)
    }
    onOpened: {
        pendingCommand = ""
        selectedCommandId = ""
        search.text = ""
        results.currentIndex = 0
        search.forceActiveFocus()
    }
    onFilteredCommandsChanged: {
        if (!results) return
        const retained = filteredCommands.findIndex(command => command.id === selectedCommandId)
        results.currentIndex = retained >= 0 ? retained : (filteredCommands.length ? 0 : -1)
    }
    onClosed: {
        const selected = pendingCommand
        pendingCommand = ""
        if (selected.length)
            requested(selected)
    }

    // One cap per configured sequence, in the shared fixed-width face.
    component Keycap: Rectangle {
        id: cap
        property alias text: capText.text
        implicitWidth: capText.implicitWidth + 10
        implicitHeight: capText.implicitHeight + 4
        radius: Math.min(3, commandsPopup.chromeRadius + 1)
        color: "transparent"
        border.width: 1
        border.color: commandsPopup.borderColor
        Text {
            id: capText
            anchors.centerIn: parent
            textFormat: Text.PlainText
            color: commandsPopup.mutedColor
            font.family: commandsPopup.monoFamily
            font.pixelSize: commandsPopup.readoutFont
        }
    }

    contentItem: ColumnLayout {
        spacing: 8
        TextField {
            id: search
            objectName: "commandSearch"
            Layout.fillWidth: true
            placeholderText: qsTr("Search commands or browse below…")
            color: commandsPopup.textColor
            placeholderTextColor: commandsPopup.mutedColor
            selectByMouse: true
            maximumLength: 160
            background: Rectangle {
                color: "transparent"
                radius: commandsPopup.chromeRadius
                border.color: search.activeFocus ? commandsPopup.accentColor : commandsPopup.borderColor
            }
            Keys.onDownPressed: commandsPopup.step(1)
            Keys.onUpPressed: commandsPopup.step(-1)
            onTextChanged: {
                commandsPopup.selectedCommandId = ""
                results.currentIndex = commandsPopup.filteredCommands.length ? 0 : -1
            }
            onAccepted: commandsPopup.choose(results.currentIndex)
        }
        ListView {
            id: results
            objectName: "commandResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: commandsPopup.filteredCommands
            currentIndex: 0
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            // Keyboard selection is a filled row with a leading accent edge.
            // Pointer hover is a lighter wash, so the two never look alike.
            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                readonly property bool selected: index === results.currentIndex
                objectName: "command_" + modelData.id
                width: results.width
                height: Math.max(40, details.implicitHeight + 12)
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.name: modelData.label + ", " + (modelData.enabled ? modelData.shortcut : modelData.reason)
                onClicked: {
                    commandsPopup.selectedCommandId = modelData.id
                    results.currentIndex = index
                    commandsPopup.choose(index)
                }
                background: Rectangle {
                    radius: commandsPopup.chromeRadius
                    color: row.selected ? commandsPopup.selectionColor :
                           row.hovered && row.modelData.enabled ? commandsPopup.hoverColor : commandsPopup.surfaceColor
                    Behavior on color {
                        enabled: commandsPopup.motionEnabled
                        ColorAnimation { duration: commandsPopup.motionDuration; easing.type: Easing.OutCubic }
                    }
                    Rectangle {
                        objectName: "commandSelectionEdge"
                        visible: row.selected
                        width: 2
                        height: parent.height
                        color: commandsPopup.accentColor
                    }
                }
                contentItem: RowLayout {
                    spacing: 12
                    ColumnLayout {
                        id: details
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.label
                            textFormat: Text.PlainText
                            color: row.modelData.enabled ? commandsPopup.textColor : commandsPopup.mutedColor
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: !row.modelData.enabled
                            text: row.modelData.reason
                            textFormat: Text.PlainText
                            color: commandsPopup.mutedColor
                            wrapMode: Text.WordWrap
                            font.pixelSize: Math.max(11, commandsPopup.uiFont - 2)
                        }
                    }
                    Row {
                        spacing: 4
                        Layout.maximumWidth: results.width * 0.45
                        clip: true
                        Repeater {
                            model: row.modelData.shortcut.length > 0 ? row.modelData.shortcut.split(" / ") : []
                            delegate: Keycap {
                                required property string modelData
                                text: modelData
                            }
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: results.count === 0
                text: qsTr("No matching commands")
                color: commandsPopup.mutedColor
            }
        }
        Row {
            spacing: 6
            Keycap { text: "↑↓" }
            Label { text: qsTr("browse"); color: commandsPopup.mutedColor; font.pixelSize: commandsPopup.readoutFont; anchors.verticalCenter: parent.verticalCenter }
            Item { width: 6; height: 1 }
            Keycap { text: qsTr("Return") }
            Label { text: qsTr("run"); color: commandsPopup.mutedColor; font.pixelSize: commandsPopup.readoutFont; anchors.verticalCenter: parent.verticalCenter }
            Item { width: 6; height: 1 }
            Keycap { text: qsTr("Esc") }
            Label { text: qsTr("close"); color: commandsPopup.mutedColor; font.pixelSize: commandsPopup.readoutFont; anchors.verticalCenter: parent.verticalCenter }
        }
    }
}
