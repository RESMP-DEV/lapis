import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lapis 1.0

// One status row per canonical harness. The pane reports a probe's own answer
// or says that it is still unknown; it does not offer configuration or launch.
Dialog {
    id: toolsPopup
    objectName: "toolsDialog"

    property var engine: null
    readonly property var tools: engine ? engine.rows : []
    property color surfaceColor: "#0e1822"
    property color cardColor: "#15202b"
    property color textColor: "#e4eef1"
    property color mutedColor: "#94aab7"
    property color accentColor: "#70d8c5"
    property color faultColor: "#ee7a8a"
    property color plentyColor: "#6fdc8c"
    property color attentionColor: "#ffb454"
    property color borderColor: "#263c48"
    property color selectionColor: "#193638"
    property string monoFamily: "monospace"
    property int uiFont: 13
    property int readoutFont: 12
    property int chromeRadius: 2
    property int motionDuration: 100
    property bool motionEnabled: false

    modal: true
    focus: true
    title: qsTr("Tools")
    anchors.centerIn: parent
    width: Math.min(660, parent.width - 32)
    height: Math.min(720, parent.height - 32)
    padding: 16
    font.pixelSize: uiFont
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: toolsPopup.surfaceColor
        border.color: toolsPopup.borderColor
        radius: toolsPopup.chromeRadius
    }
    onOpened: if (engine) engine.refresh()

    function stateText(state) {
        if (state === "ok") return qsTr("working")
        if (state === "failed") return qsTr("failed")
        if (state === "timeout") return qsTr("no answer before timeout")
        if (state === "missing") return qsTr("not installed")
        if (state === "present") return qsTr("installed, nothing to probe")
        return qsTr("not checked yet")
    }

    component Readout: Label {
        textFormat: Text.PlainText
        color: toolsPopup.mutedColor
        font.family: toolsPopup.monoFamily
        font.pixelSize: toolsPopup.readoutFont
        elide: Text.ElideRight
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Known agent commands")
                color: toolsPopup.textColor
                font.bold: true
                Layout.fillWidth: true
            }
            Button {
                objectName: "toolsRefreshAll"
                text: qsTr("Check all")
                focusPolicy: Qt.NoFocus
                enabled: toolsPopup.engine !== null
                onClicked: if (toolsPopup.engine) toolsPopup.engine.refresh()
            }
        }

        ScrollView {
            id: scroll
            objectName: "toolsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                id: toolList
                objectName: "toolsList"
                model: toolsPopup.tools
                spacing: 10
                boundsBehavior: Flickable.StopAtBounds
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property string id: modelData.id
                    width: toolList.width
                    height: body.implicitHeight + 24
                    color: toolsPopup.cardColor
                    border.color: toolsPopup.borderColor
                    radius: toolsPopup.chromeRadius

                    ColumnLayout {
                        id: body
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 7

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            AgentMark {
                                harnessId: card.id
                                ink: toolsPopup.textColor
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                            }
                            Label {
                                text: card.modelData.label
                                color: toolsPopup.textColor
                                font.bold: true
                                Layout.fillWidth: true
                            }
                            Label {
                                objectName: "toolState_" + card.id
                                text: toolsPopup.stateText(card.modelData.state) +
                                      (card.modelData.state === "failed"
                                           ? qsTr(", exit %1").arg(card.modelData.exitCode) : "")
                                color: card.modelData.state === "ok" ? toolsPopup.plentyColor
                                    : card.modelData.state === "unknown" ? toolsPopup.mutedColor
                                    : toolsPopup.faultColor
                            }
                            Button {
                                objectName: "toolRerun_" + card.id
                                visible: card.modelData.probed
                                text: qsTr("Check")
                                focusPolicy: Qt.NoFocus
                                onClicked: if (toolsPopup.engine) toolsPopup.engine.refreshTool(card.id)
                            }
                        }

                        Readout {
                            Layout.fillWidth: true
                            text: card.modelData.command
                        }
                        Label {
                            objectName: "toolDetail_" + card.id
                            Layout.fillWidth: true
                            visible: card.modelData.detail.length > 0
                            text: card.modelData.detail
                            textFormat: Text.PlainText
                            color: toolsPopup.mutedColor
                            elide: Text.ElideRight
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Readout {
                                objectName: "toolChecked_" + card.id
                                text: card.modelData.checked.length > 0
                                          ? qsTr("checked %1").arg(
                                                Qt.formatDateTime(new Date(card.modelData.checked),
                                                                  "h:mm ap"))
                                          : qsTr("never checked")
                            }
                            Label {
                                objectName: "toolStale_" + card.id
                                visible: card.modelData.checked.length > 0 && !card.modelData.fresh
                                text: qsTr("stale")
                                color: toolsPopup.attentionColor
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                visible: !card.modelData.offered
                                text: qsTr("saved agents only")
                                color: toolsPopup.mutedColor
                            }
                        }
                    }
                }
            }
        }
    }
}
