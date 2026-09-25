import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Plan limits as each CLI reports them, with where the current pace ends,
// and tokens counted from this Mac's Codex and Claude transcripts.
Dialog {
    id: usagePopup
    objectName: "usageDialog"
    property var engine: null
    readonly property var providers: engine ? engine.providers : []
    property color surfaceColor: "#0e1822"
    property color cardColor: "#15202b"
    property color textColor: "#e4eef1"
    property color mutedColor: "#94aab7"
    property color accentColor: "#70d8c5"
    property color faultColor: "#ee7a8a"
    property color borderColor: "#263c48"
    property string monoFamily: "monospace"
    property int uiFont: 13
    property int readoutFont: 12
    property int chromeRadius: 2
    property int motionDuration: 100
    property bool motionEnabled: false
    modal: true
    focus: true
    title: qsTr("Usage")
    anchors.centerIn: parent
    width: Math.min(620, parent.width - 32)
    height: Math.min(680, parent.height - 32)
    padding: 16
    font.pixelSize: uiFont
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: usagePopup.surfaceColor
        border.color: usagePopup.borderColor
        radius: usagePopup.chromeRadius
    }
    onOpened: if (engine) engine.refresh()

    function tokens(count) {
        const n = Number(count) || 0
        if (n >= 1e9) return (n / 1e9).toFixed(n >= 1e10 ? 1 : 2) + "B"
        if (n >= 1e6) return (n / 1e6).toFixed(n >= 1e7 ? 0 : 1) + "M"
        if (n >= 1e3) return (n / 1e3).toFixed(0) + "K"
        return String(n)
    }
    function resetText(when) {
        if (!when || isNaN(when.getTime()))
            return ""
        const hours = (when.getTime() - Date.now()) / 3600000
        if (hours <= 0)
            return qsTr("resets now")
        return hours < 20 ? qsTr("resets %1").arg(Qt.formatTime(when, "h:mm ap"))
                          : qsTr("resets %1").arg(Qt.formatDateTime(when, "ddd h:mm ap"))
    }
    function barColor(percent) {
        return percent >= 90 ? faultColor : accentColor
    }

    component Readout: Label {
        textFormat: Text.PlainText
        color: usagePopup.mutedColor
        font.family: usagePopup.monoFamily
        font.pixelSize: usagePopup.readoutFont
    }

    contentItem: ColumnLayout {
        spacing: 12
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    visible: usagePopup.providers.length === 0
                    text: usagePopup.engine && usagePopup.engine.counting ? qsTr("Checking…")
                                                                          : qsTr("Neither Codex nor Claude is installed here, and no transcripts were found.")
                    color: usagePopup.mutedColor
                    wrapMode: Text.WordWrap
                }
                Repeater {
                    model: usagePopup.providers
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        objectName: "usage_" + modelData.id
                        Layout.fillWidth: true
                        implicitHeight: body.implicitHeight + 24
                        color: usagePopup.cardColor
                        border.color: usagePopup.borderColor
                        radius: usagePopup.chromeRadius
                        readonly property var chart: modelData.days
                        readonly property real chartPeak: Math.max(1, ...modelData.days)

                        ColumnLayout {
                            id: body
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                AgentMark {
                                    harnessId: card.modelData.id
                                    ink: usagePopup.textColor
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                }
                                Label {
                                    text: card.modelData.name
                                    textFormat: Text.PlainText
                                    color: usagePopup.textColor
                                    font.bold: true
                                }
                                Readout {
                                    visible: card.modelData.plan.length > 0
                                    text: card.modelData.plan.charAt(0).toUpperCase() + card.modelData.plan.slice(1)
                                }
                                Item { Layout.fillWidth: true }
                                Readout {
                                    visible: card.modelData.checked && !isNaN(card.modelData.checked.getTime())
                                    text: visible ? qsTr("checked %1").arg(Qt.formatTime(card.modelData.checked, "h:mm ap")) : ""
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: [card.modelData.note,
                                       card.modelData.resetCredits > 0 ?
                                           (card.modelData.resetCredits === 1 ? qsTr("1 free reset on the account")
                                                                              : qsTr("%1 free resets on the account").arg(card.modelData.resetCredits)) : ""]
                                      .filter(part => part.length > 0).join("  ·  ")
                                textFormat: Text.PlainText
                                color: usagePopup.textColor
                                wrapMode: Text.WordWrap
                            }
                            Repeater {
                                model: card.modelData.windows
                                delegate: ColumnLayout {
                                    id: windowRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 10
                                        Label {
                                            text: windowRow.modelData.label
                                            textFormat: Text.PlainText
                                            color: usagePopup.textColor
                                            Layout.preferredWidth: 120
                                            elide: Text.ElideRight
                                        }
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 6
                                            radius: 3
                                            color: usagePopup.borderColor
                                            Rectangle {
                                                width: parent.width * Math.min(100, windowRow.modelData.percent) / 100
                                                height: parent.height
                                                radius: parent.radius
                                                color: usagePopup.barColor(windowRow.modelData.percent)
                                                Behavior on width {
                                                    enabled: usagePopup.motionEnabled
                                                    NumberAnimation { duration: usagePopup.motionDuration; easing.type: Easing.OutCubic }
                                                }
                                            }
                                        }
                                        Readout {
                                            text: Math.round(windowRow.modelData.percent) + "%"
                                            color: usagePopup.textColor
                                            horizontalAlignment: Text.AlignRight
                                            Layout.preferredWidth: 44
                                        }
                                    }
                                    Readout {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 130
                                        text: [usagePopup.resetText(windowRow.modelData.resets),
                                               windowRow.modelData.pace >= 0 && windowRow.modelData.percent < 100 ?
                                                   qsTr("at this pace %1% by then").arg(Math.round(windowRow.modelData.pace)) : ""]
                                              .filter(part => part.length > 0).join("  ·  ")
                                        color: windowRow.modelData.pace > 100 && windowRow.modelData.percent < 100 ?
                                                   usagePopup.faultColor : usagePopup.mutedColor
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 16
                                ColumnLayout {
                                    spacing: 2
                                    Readout { text: qsTr("Today") }
                                    Label {
                                        objectName: "usageToday_" + card.modelData.id
                                        text: usagePopup.tokens(card.modelData.today.total)
                                        textFormat: Text.PlainText
                                        color: usagePopup.textColor
                                        font.pixelSize: usagePopup.uiFont + 3
                                    }
                                }
                                ColumnLayout {
                                    spacing: 2
                                    Readout { text: qsTr("This month") }
                                    Label {
                                        objectName: "usageMonth_" + card.modelData.id
                                        text: usagePopup.tokens(card.modelData.month.total)
                                        textFormat: Text.PlainText
                                        color: usagePopup.textColor
                                        font.pixelSize: usagePopup.uiFont + 3
                                    }
                                }
                                // Tokens per day over 30 days; today on the right.
                                Item {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 36
                                    Row {
                                        anchors.fill: parent
                                        spacing: 2
                                        Repeater {
                                            model: card.chart
                                            delegate: Item {
                                                required property var modelData
                                                required property int index
                                                width: (parent.width - 2 * 29) / 30
                                                height: parent.height
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(1, parent.height * Number(parent.modelData) / card.chartPeak)
                                                    color: usagePopup.accentColor
                                                    opacity: parent.index === 29 ? 1 : 0.45
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            Readout {
                                Layout.fillWidth: true
                                text: qsTr("input %1  ·  cache read %2  ·  cache write %3  ·  output %4 this month")
                                        .arg(usagePopup.tokens(card.modelData.month.input))
                                        .arg(usagePopup.tokens(card.modelData.month.cacheRead))
                                        .arg(usagePopup.tokens(card.modelData.month.cacheWrite))
                                        .arg(usagePopup.tokens(card.modelData.month.output))
                                wrapMode: Text.WordWrap
                            }
                            Readout {
                                Layout.fillWidth: true
                                visible: card.modelData.models.length > 0
                                text: card.modelData.models.slice(0, 4)
                                        .map(model => model.name + " " + usagePopup.tokens(model.total)).join("  ·  ")
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: usagePopup.engine && usagePopup.engine.counting ?
                          qsTr("Counting tokens in this Mac's transcripts…") :
                          qsTr("Limits as each CLI reports them, checked every five minutes. Tokens from this Mac's transcripts, without prices.")
                textFormat: Text.PlainText
                color: usagePopup.mutedColor
                font.pixelSize: usagePopup.readoutFont
                wrapMode: Text.WordWrap
            }
            Button {
                objectName: "usageRefresh"
                text: qsTr("Refresh")
                focusPolicy: Qt.NoFocus
                enabled: usagePopup.engine !== null
                onClicked: usagePopup.engine.refresh()
            }
        }
    }
}
