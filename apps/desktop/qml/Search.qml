import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Cmd+K: find an agent by its name, folder, category, CLI, machine or a line
// on its screen. The index is read once when this opens; each keystroke only
// searches it.
Dialog {
    id: searchPopup
    objectName: "searchDialog"
    property var engine: null
    property var results: []
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
    property string pendingAgent: ""
    signal chosen(string sessionId)
    modal: true
    focus: true
    title: qsTr("Find an agent")
    anchors.centerIn: parent
    width: Math.min(660, parent.width - 32)
    height: Math.min(540, parent.height - 32)
    padding: 16
    font.pixelSize: uiFont
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: searchPopup.surfaceColor
        border.color: searchPopup.accentColor
        radius: searchPopup.chromeRadius
    }

    function run() {
        results = engine ? engine.search(query.text, 40) : []
        list.currentIndex = results.length ? 0 : -1
    }
    function choose(index) {
        const hit = results[index]
        if (!hit)
            return
        pendingAgent = hit.sessionId
        close()
    }
    function step(delta) {
        if (!results.length)
            return
        list.currentIndex = (list.currentIndex + delta + results.length) % results.length
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }
    onOpened: {
        pendingAgent = ""
        query.text = ""
        if (engine)
            engine.refresh()
        run()
        query.forceActiveFocus()
    }
    onClosed: {
        const selected = pendingAgent
        pendingAgent = ""
        if (selected.length)
            chosen(selected)
    }

    contentItem: ColumnLayout {
        spacing: 8
        TextField {
            id: query
            objectName: "agentSearchField"
            Layout.fillWidth: true
            placeholderText: qsTr("Name, folder, category, machine, or text on its screen…")
            color: searchPopup.textColor
            placeholderTextColor: searchPopup.mutedColor
            selectByMouse: true
            maximumLength: 160
            background: Rectangle {
                color: "transparent"
                radius: searchPopup.chromeRadius
                border.color: query.activeFocus ? searchPopup.accentColor : searchPopup.borderColor
            }
            Keys.onDownPressed: searchPopup.step(1)
            Keys.onUpPressed: searchPopup.step(-1)
            onTextChanged: searchPopup.run()
            onAccepted: searchPopup.choose(list.currentIndex)
        }
        ListView {
            id: list
            objectName: "agentSearchResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: searchPopup.results
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                readonly property bool selected: index === list.currentIndex
                objectName: "agentResult_" + modelData.sessionId
                width: list.width
                height: Math.max(44, details.implicitHeight + 12)
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.name: modelData.title + ", " + modelData.place
                onClicked: {
                    list.currentIndex = index
                    searchPopup.choose(index)
                }
                background: Rectangle {
                    radius: searchPopup.chromeRadius
                    color: row.selected ? searchPopup.selectionColor :
                           row.hovered ? searchPopup.hoverColor : searchPopup.surfaceColor
                    Behavior on color {
                        enabled: searchPopup.motionEnabled
                        ColorAnimation { duration: searchPopup.motionDuration; easing.type: Easing.OutCubic }
                    }
                    Rectangle {
                        visible: row.selected
                        width: 2
                        height: parent.height
                        color: searchPopup.accentColor
                    }
                }
                contentItem: RowLayout {
                    spacing: 10
                    AgentMark {
                        harnessId: row.modelData.harnessId
                        ink: searchPopup.textColor
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        Layout.alignment: Qt.AlignTop
                    }
                    ColumnLayout {
                        id: details
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.title
                            textFormat: Text.PlainText
                            color: searchPopup.textColor
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.category + "  ·  " + row.modelData.place
                            textFormat: Text.PlainText
                            color: searchPopup.mutedColor
                            font.family: searchPopup.monoFamily
                            font.pixelSize: searchPopup.readoutFont
                            elide: Text.ElideMiddle
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: row.modelData.snippet.length > 0
                            text: row.modelData.snippet
                            textFormat: Text.PlainText
                            color: searchPopup.accentColor
                            font.family: searchPopup.monoFamily
                            font.pixelSize: searchPopup.readoutFont
                            elide: Text.ElideRight
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: qsTr("No agent matches")
                color: searchPopup.mutedColor
            }
        }
    }
}
