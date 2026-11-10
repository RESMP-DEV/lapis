import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// lapis surfaces an agent's requests; the agent takes every answer in its own
// terminal (an older service's request too), so nothing here is editable.
Dialog {
    id: dialog
    objectName: "attentionDialog"
    property var session: null
    property string monoFamily: "monospace"
    // The selected request as it was when chosen. A later snapshot may retire
    // it; the dialog then says so instead of switching to another request.
    property var selected: null
    property string lastSessionId: ""
    property string lastToken: ""
    readonly property var requests: session ? session.attentionRequests : []
    readonly property var current: {
        if (selected) {
            for (const request of requests) {
                if (request.token === selected.token)
                    return request
            }
        }
        return null
    }

    function showSession(value, preferredToken) {
        const restoreToken = value && lastSessionId === value.sessionId ? lastToken : ""
        session = value
        selected = null
        open()
        const targetToken = preferredToken ? preferredToken : restoreToken
        if (targetToken)
            selectExactRequest(targetToken)
    }
    function selectRequest(value) {
        if (!session || !value)
            return
        selected = value
        lastSessionId = session.sessionId
        lastToken = value.token
    }
    function selectExactRequest(token) {
        if (!session)
            return false
        for (const request of requests) {
            if (request.token === token) {
                selectRequest(request)
                return true
            }
        }
        return false
    }
    function optionText(option) {
        return option.description ? option.label + " — " + option.description : option.label
    }

    title: qsTr("Agent requests")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 32)
    height: Math.min(620, parent.height - 32)
    background: Rectangle {
        color: dialog.palette.window
        radius: 10
        border.color: dialog.palette.mid
    }
    standardButtons: Dialog.Close
    closePolicy: Popup.CloseOnEscape
    onClosed: {
        selected = null
        session = null
    }
    onRequestsChanged: {
        // Removing the entire session invalidates the dialog owner.
        if (session && typeof workspace !== "undefined" &&
                !workspace.sessions.some(value => value === session))
            close()
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: dialog.session ? dialog.session.attentionDiagnostic : ""
                visible: text.length > 0
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                visible: dialog.requests.length === 0
                text: qsTr("No pending requests.")
            }
            Repeater {
                model: dialog.requests
                delegate: Button {
                    required property int index
                    objectName: "request-" + index
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.reason
                    checked: dialog.selected !== null && modelData.token === dialog.selected.token
                    onClicked: dialog.selectRequest(modelData)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.selected !== null
                Label {
                    Layout.fillWidth: true
                    text: dialog.selected ? dialog.selected.summary : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.selected ? (dialog.selected.details.command || "") : ""
                    visible: text.length > 0
                    font.family: dialog.monoFamily
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.selected ? (dialog.selected.details.cwd || "") : ""
                    visible: text.length > 0
                    font.family: dialog.monoFamily
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.selected ? (dialog.selected.details.reason || "") : ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                // A question and its offered answers, as text to read before
                // answering in the terminal.
                Repeater {
                    model: dialog.selected ? (dialog.selected.details.questions || []) : []
                    delegate: ColumnLayout {
                        id: question
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            objectName: "question-" + question.modelData.id
                            text: question.modelData.question || ""
                            font.weight: Font.Medium
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            model: question.modelData.options || []
                            delegate: Label {
                                required property var modelData
                                Layout.fillWidth: true
                                leftPadding: 12
                                text: "• " + dialog.optionText(modelData)
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                objectName: "requestNotice"
                visible: dialog.selected !== null
                text: dialog.current === null ? qsTr("This request is no longer current.")
                                              : qsTr("Answer in the originating terminal.")
                font.weight: Font.Medium
                wrapMode: Text.Wrap
            }
        }
    }
}
