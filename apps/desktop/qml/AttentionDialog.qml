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
    property var answerModes: ({})
    property string feedback: ""
    property bool responseQueued: false
    property string lastDraftSessionId: ""
    property string lastDraftToken: ""
    property var draftCache: ({})
    readonly property int maximumDrafts: 64
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
    readonly property bool terminalOnly: draft !== null &&
                                         draft.details.responseLocation === "terminal"
    readonly property bool answersComplete: {
        const questions = draft ? (draft.details.questions || []) : []
        for (const question of questions) {
            if (!answers[question.id] || !answers[question.id].answers[0].length)
                return false
        }
        return true
    }

    function draftKey(sessionId, token) {
        return JSON.stringify([sessionId, token])
    }

    function rememberDraft() {
        if (!session || !draft || !requestExists(session, draft.token))
            return
        const key = draftKey(session.sessionId, draft.token)
        const retained = {}
        retained[key] = {
            sessionId: session.sessionId,
            token: draft.token,
            draft: draft,
            answers: answers,
            answerModes: answerModes,
            feedback: feedback,
            responseQueued: responseQueued
        }
        for (const existingKey of Object.keys(draftCache)) {
            if (Object.keys(retained).length >= maximumDrafts)
                break
            if (existingKey !== key)
                retained[existingKey] = draftCache[existingKey]
        }
        draftCache = retained
        lastDraftSessionId = session.sessionId
        lastDraftToken = draft.token
    }

    function restoreDraft(sessionId, token) {
        const cached = draftCache[draftKey(sessionId, token)]
        if (!cached)
            return false
        draft = cached.draft
        answers = cached.answers
        answerModes = cached.answerModes
        feedback = cached.feedback
        responseQueued = cached.responseQueued
        lastDraftSessionId = sessionId
        lastDraftToken = token
        return true
    }

    function requestExists(value, token) {
        if (!value)
            return false
        for (const request of value.attentionRequests) {
            if (request.token === token)
                return true
        }
        return false
    }

    function pruneDrafts(activeSessions) {
        const retained = {}
        for (const key of Object.keys(draftCache)) {
            const cached = draftCache[key]
            let alive = false
            for (const value of activeSessions) {
                if (value && value.sessionId === cached.sessionId &&
                        requestExists(value, cached.token)) {
                    alive = true
                    break
                }
            }
            if (alive)
                retained[key] = cached
        }
        draftCache = retained
        // A changed request disables its open form without destroying typed text.
        // Removing the entire session invalidates the dialog owner instead.
        if (session && !activeSessions.some(value => value === session))
            close()
    }

    function showSession(value, preferredToken) {
        rememberDraft()
        const restoreSessionId = value && lastDraftSessionId === value.sessionId ?
                    lastDraftSessionId : ""
        const restoreToken = restoreSessionId ? lastDraftToken : ""
        session = value
        draft = null
        answers = ({})
        answerModes = ({})
        feedback = ""
        responseQueued = false
        open()
        const targetToken = preferredToken ? preferredToken : restoreToken
        if (targetToken)
            selectExactRequest(targetToken)
    }
    function selectRequest(value) {
        if (!session || !value)
            return
        rememberDraft()
        if (!restoreDraft(session.sessionId, value.token)) {
            draft = value
            answers = ({})
            answerModes = ({})
            feedback = ""
            responseQueued = false
        }
        rememberDraft()
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
    function setAnswer(id, value) {
        updateAnswer(id, value, false)
    }
    function updateAnswer(id, value, fromOption) {
        const updated = Object.assign({}, answers)
        const modes = Object.assign({}, answerModes)
        modes[id] = fromOption === true
        answerModes = modes
        updated[id] = {answers: [value]}
        answers = updated
        rememberDraft()
    }
    function respond(choice) {
        if (!canRespond || !answersComplete)
            return
        responseQueued = session.respondAttention(draft.token, {choice: choice, answers: answers})
        feedback = responseQueued ? ""
            : qsTr("Request changed or the connection is unavailable. Select the current request.")
        rememberDraft()
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
    onClosed: {
        rememberDraft()
        if (draft && session) {
            lastDraftSessionId = session.sessionId
            lastDraftToken = draft.token
        }
        draft = null
        answers = ({})
        answerModes = ({})
        responseQueued = false
        session = null
    }
    onRequestsChanged: {
        if (typeof workspace !== "undefined")
            pruneDrafts(workspace.sessions)
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
                    text: modelData.reason + (modelData.responding ? qsTr(" · response sent") : "")
                    checked: dialog.draft !== null && modelData.token === dialog.draft.token
                    onClicked: dialog.selectRequest(modelData)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: dialog.draft !== null
                enabled: dialog.canRespond || dialog.terminalOnly
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
                Label {
                    Layout.fillWidth: true
                    objectName: "terminalOnlyNotice"
                    visible: dialog.terminalOnly
                    text: qsTr("Answer in the originating terminal.")
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: dialog.draft ? (dialog.draft.details.questions || []) : []
                    delegate: ColumnLayout {
                        id: questionForm
                        required property var modelData
                        readonly property var options: modelData.options || []
                        readonly property string savedAnswer: dialog.answers[modelData.id]
                            ? dialog.answers[modelData.id].answers[0] : ""
                        readonly property bool optionSelected: dialog.answerModes[modelData.id] === true
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
                            currentIndex: questionForm.optionSelected
                                ? questionForm.options.findIndex(option => option.label === questionForm.savedAnswer) : -1
                            displayText: currentIndex < 0 ? qsTr("Choose an answer") : currentText
                            onActivated: {
                                dialog.updateAnswer(questionForm.modelData.id, currentText, true)
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
                            text: questionForm.optionSelected ? "" : questionForm.savedAnswer
                            onTextEdited: {
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
