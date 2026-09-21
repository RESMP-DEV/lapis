import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "attentionDialog"
    property var session: null
    // Freeze the selected form while the user types. Incoming snapshots may
    // invalidate its token, but must never replace a draft or an IME composition.
    property var draft: null
    property var answers: ({})
    property string feedback: ""
    property bool responseQueued: false
    readonly property var requests: session ? session.attentionRequests : []
    readonly property var current: {
        if (draft) {
            for (const request of requests) {
                if (request.token === draft.token)
                    return request
            }
        }
        return null
    }
    readonly property bool canRespond: current !== null && current.enabled
    readonly property bool answersComplete: {
        const questions = draft ? (draft.details.questions || []) : []
        for (const question of questions) {
            if (!answers[question.id] || !answers[question.id].answers[0].length)
                return false
        }
        return true
    }

    function showSession(value) {
        session = value
        draft = null
        answers = ({})
        feedback = ""
        responseQueued = false
        open()
    }
    function selectRequest(value) {
        draft = value
        answers = ({})
        feedback = ""
        responseQueued = false
    }
    function setAnswer(id, value) {
        const updated = Object.assign({}, answers)
        updated[id] = {answers: [value]}
        answers = updated
    }
    function respond(choice) {
        if (!canRespond || !answersComplete)
            return
        responseQueued = session.respondAttention(draft.token, {choice: choice, answers: answers})
        feedback = responseQueued ? ""
            : qsTr("Request changed or the connection is unavailable. Select the current request.")
    }
    function choiceLabel(choice) {
        if (choice === "accept") return qsTr("Approve command")
        if (choice === "decline") return qsTr("Decline command")
        if (choice === "cancel") return qsTr("Cancel request")
        return qsTr("Send answers")
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
    onClosed: { draft = null; answers = ({}); session = null; responseQueued = false }

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
                    text: modelData.reason + (modelData.responding ? qsTr(" · response sent") : "")
                    checked: dialog.draft !== null && modelData.token === dialog.draft.token
                    onClicked: dialog.selectRequest(modelData)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.draft !== null
                enabled: dialog.canRespond
                Label {
                    Layout.fillWidth: true
                    text: dialog.draft ? dialog.draft.summary : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.draft ? (dialog.draft.details.command || "") : ""
                    visible: text.length > 0
                    font.family: "monospace"
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.draft ? (dialog.draft.details.cwd || "") : ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.draft ? (dialog.draft.details.reason || "") : ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: dialog.draft ? (dialog.draft.details.questions || []) : []
                    delegate: ColumnLayout {
                        id: questionForm
                        required property var modelData
                        readonly property var options: modelData.options || []
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: questionForm.modelData.question
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        ComboBox {
                            id: optionChoice
                            objectName: "options-" + questionForm.modelData.id
                            Layout.fillWidth: true
                            visible: questionForm.options.length > 0
                            model: questionForm.options
                            textRole: "label"
                            currentIndex: -1
                            displayText: currentIndex < 0 ? qsTr("Choose an answer") : currentText
                            onActivated: {
                                freeAnswer.text = ""
                                dialog.setAnswer(questionForm.modelData.id, currentText)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: optionChoice.currentIndex >= 0 ? (questionForm.options[optionChoice.currentIndex].description || "") : ""
                            visible: text.length > 0
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        TextField {
                            id: freeAnswer
                            objectName: "answer-" + questionForm.modelData.id
                            Layout.fillWidth: true
                            visible: questionForm.options.length === 0 || questionForm.modelData.isOther === true
                            placeholderText: qsTr("Type an answer")
                            maximumLength: 8192
                            echoMode: questionForm.modelData.isSecret === true ? TextInput.Password : TextInput.Normal
                            onTextEdited: {
                                optionChoice.currentIndex = -1
                                dialog.setAnswer(questionForm.modelData.id, text)
                            }
                        }
                    }
                }
                RowLayout {
                    Repeater {
                        model: dialog.draft ? dialog.draft.choices : []
                        delegate: Button {
                            required property string modelData
                            objectName: "respond-" + modelData
                            text: dialog.choiceLabel(modelData)
                            enabled: dialog.canRespond && dialog.answersComplete
                            onClicked: dialog.respond(modelData)
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: dialog.draft !== null && !dialog.canRespond
                text: dialog.current === null ? qsTr("This request is no longer current.") :
                      dialog.current.responding ? qsTr("Waiting for the agent to resolve this request.") :
                      dialog.current.choices.length === 0 ? qsTr("Respond in the originating terminal.") :
                      qsTr("Reconnect and select the current request before responding.")
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: !dialog.responseQueued ? dialog.feedback :
                      dialog.current === null ? qsTr("This request is no longer pending.") :
                      dialog.current.responding ? qsTr("Response sent. Waiting for the agent.") :
                      dialog.canRespond ? qsTr("Response rejected. Review your answers and try again.") :
                      qsTr("Response status is uncertain. Reconnect before responding.")
                visible: text.length > 0
                wrapMode: Text.Wrap
            }
        }
    }
}
