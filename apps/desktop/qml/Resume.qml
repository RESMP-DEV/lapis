import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Cmd+O: resume a past Claude or Codex conversation on this Mac, newest
// first. Typing narrows by title, folder or CLI; up and down move, Return
// resumes it as a new agent in the current category.
Dialog {
    id: resumePopup
    objectName: "resumeDialog"
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
    property var pending: null
    signal chosen(var conversation)
    modal: true
    focus: true
    title: qsTr("Resume a conversation")
    anchors.centerIn: parent
    width: Math.min(700, parent.width - 32)
    height: Math.min(560, parent.height - 32)
    padding: 16
    font.pixelSize: uiFont
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: resumePopup.surfaceColor
        border.color: resumePopup.accentColor
        radius: resumePopup.chromeRadius
    }

    function run() {
        results = engine ? engine.recent(query.text, 60) : []
        list.currentIndex = results.length ? 0 : -1
    }
    function choose(index) {
        const hit = results[index]
        if (!hit)
            return
        pending = hit
        close()
    }
    function step(delta) {
        if (!results.length)
            return
        list.currentIndex = (list.currentIndex + delta + results.length) % results.length
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }
    onOpened: {
        pending = null
        query.text = ""
        if (engine)
            engine.refresh()
        run()
        query.forceActiveFocus()
    }
    onClosed: {
        const selected = pending
        pending = null
        if (selected)
            chosen(selected)
    }
    Connections {
        target: resumePopup.engine
        // A scan that finishes while this is open fills it in.
        function onChanged() { if (resumePopup.visible) resumePopup.run() }
    }

    contentItem: ColumnLayout {
        spacing: 8
        TextField {
            id: query
            objectName: "resumeSearchField"
            Layout.fillWidth: true
            placeholderText: qsTr("Title, folder or CLI…")
            color: resumePopup.textColor
            placeholderTextColor: resumePopup.mutedColor
            selectByMouse: true
            maximumLength: 160
            background: Rectangle {
                color: "transparent"
                radius: resumePopup.chromeRadius
                border.color: query.activeFocus ? resumePopup.accentColor : resumePopup.borderColor
            }
            Keys.onDownPressed: resumePopup.step(1)
            Keys.onUpPressed: resumePopup.step(-1)
            onTextChanged: resumePopup.run()
            onAccepted: resumePopup.choose(list.currentIndex)
        }
        ListView {
            id: list
            objectName: "resumeResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: resumePopup.results
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                readonly property bool selected: index === list.currentIndex
                objectName: "resumeResult_" + modelData.id
                width: list.width
                height: Math.max(44, details.implicitHeight + 12)
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.name: modelData.title + ", " + modelData.place + ", " + modelData.when
                onClicked: {
                    list.currentIndex = index
                    resumePopup.choose(index)
                }
                background: Rectangle {
                    radius: resumePopup.chromeRadius
                    color: row.selected ? resumePopup.selectionColor :
                           row.hovered ? resumePopup.hoverColor : resumePopup.surfaceColor
                    Behavior on color {
                        enabled: resumePopup.motionEnabled
                        ColorAnimation { duration: resumePopup.motionDuration; easing.type: Easing.OutCubic }
                    }
                    Rectangle {
                        visible: row.selected
                        width: 2
                        height: parent.height
                        color: resumePopup.accentColor
                    }
                }
                contentItem: RowLayout {
                    spacing: 10
                    AgentMark {
                        harnessId: row.modelData.harness
                        ink: resumePopup.textColor
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
                            color: resumePopup.textColor
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.place
                            textFormat: Text.PlainText
                            color: resumePopup.mutedColor
                            font.family: resumePopup.monoFamily
                            font.pixelSize: resumePopup.readoutFont
                            elide: Text.ElideMiddle
                        }
                    }
                    Label {
                        text: row.modelData.when
                        textFormat: Text.PlainText
                        color: resumePopup.mutedColor
                        font.family: resumePopup.monoFamily
                        font.pixelSize: resumePopup.readoutFont
                        Layout.alignment: Qt.AlignTop
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: resumePopup.engine && !resumePopup.engine.ready ? qsTr("Reading conversations…") :
                      query.text.length > 0 ? qsTr("No conversation matches") :
                                              qsTr("No Claude or Codex conversations on this Mac yet")
                color: resumePopup.mutedColor
            }
        }
    }
}
