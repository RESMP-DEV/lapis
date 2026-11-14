import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings: a few plainly named sections on the left, one at a time on the
// right, each a column of rows (a label, one control, a line of help where
// it needs one). Search filters rows across every section. Each change
// applies at once and is saved to lapis.json; a row that differs from the
// built-in value offers Reset. Nothing here deletes or ends anything.
Dialog {
    id: settings

    required property var themeModel
    required property var densityModel
    required property string currentTheme
    required property string currentDensity
    required property string configPath
    required property string configDiagnostic
    required property string shortcutHint
    required property var fontFamilies
    // Configured family; empty selects the platform's fixed-width font.
    required property string fontFamily
    required property string resolvedFontFamily
    required property int fontSize
    required property int fontSizeMinimum
    required property int fontSizeMaximum
    required property int fontSizeDefault
    required property int motionDuration
    required property bool motionEnabled
    required property bool finishSound
    required property bool keepAwake
    // The downloaded app's login item and updates (absent in a developer build).
    property bool loginAvailable: false
    property bool launchAtLogin: false
    property bool updatesAvailable: false
    // Every action that has a key: {label, keys}.
    property var shortcutRows: []
    // The visual view: its themes with their colors, reading and code fonts
    // ({name, label, family}), the saved choices and the size now shown.
    property var visualThemes: []
    property var visualFonts: []
    property var visualCodeFonts: []
    // Editors on this Mac, first the default: [{name, label, family}].
    property var visualEditors: []
    property var visual: ({})
    property int visualSize: 15
    property bool sidebarVisible: true
    property bool previewsVisible: true
    property bool scrollToBottomOnTyping: true
    // Config.settings (new agents, guesses, the log, Tab) and Config.defaults().
    property var values: ({})
    property var defaults: ({})
    // The CLIs lapis offers: [{id, name}].
    property var harnesses: []
    property bool fileBacked: true

    signal visualChosen(string key, var value)
    signal themeChosen(string name)
    signal densityChosen(string name)
    signal fontFamilyChosen(string name)
    signal fontSizeChosen(int pixels)
    signal finishSoundChosen(bool on)
    signal keepAwakeChosen(bool on)
    signal launchAtLoginChosen(bool on)
    signal checkUpdates()
    signal chimePlayed(bool needsYou)
    signal appearanceChosen(string mode)
    signal sidebarChosen(bool on)
    signal previewsChosen(bool on)
    signal scrollChosen(bool on)
    // A Config.setSetting path and its value.
    signal settingChosen(string path, var value)
    signal reloadRequested()
    signal openConfigRequested()
    signal openDocsRequested()

    // The section shown when nothing is searched, and the search.
    property string currentSection: "general"
    readonly property string query: search.text.trim().toLowerCase()
    readonly property var sections: [
        { id: "general", label: qsTr("General") },
        { id: "appearance", label: qsTr("Appearance") },
        { id: "reading", label: qsTr("Reading view") },
        { id: "terminal", label: qsTr("Terminal") },
        { id: "agents", label: qsTr("New agents") },
        { id: "keys", label: qsTr("Keys") },
        { id: "advanced", label: qsTr("Advanced") }
    ]
    function sectionLabel(id) {
        const found = sections.find(s => s.id === id)
        return found ? found.label : ""
    }
    // Every word searched appears in the row's label, help or keywords.
    function matches(text) {
        const haystack = text.toLowerCase()
        return query.split(/\s+/).every(word => haystack.indexOf(word) >= 0)
    }
    // Opens a row's section (for a search hit, or a caller looking for a row).
    function showSection(id) {
        search.text = ""
        currentSection = id
        sectionList.currentIndex = Math.max(0, sections.findIndex(section => section.id === id))
        settingsFlick.contentY = 0
    }
    function resetSection() {
        for (const row of body.children)
            if (row.isSettingRow && row.section === currentSection && row.available && row.modified)
                row.reset()
    }

    readonly property int uiFont: {
        if (font.pixelSize > 0)
            return Math.max(12, font.pixelSize)
        if (font.pointSize > 0)
            return Math.max(12, Math.round(font.pointSize * 96 / 72))
        return 13
    }
    readonly property int helpFont: Math.max(11, uiFont - 2)

    readonly property var currentAppearance: themeByName(currentTheme)
    readonly property int chromeRadius: currentAppearance ? currentAppearance.cornerRadius : 2
    readonly property int readoutFont: Math.max(11, uiFont - 2)
    font.family: currentAppearance && currentAppearance.monoChrome ? resolvedFontFamily : Qt.application.font.family

    objectName: "settingsDialog"
    title: qsTr("Settings")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(800, Math.max(320, (parent ? parent.width : 640) - 24))
    height: Math.min(660, Math.max(300, (parent ? parent.height : 480) - 24))
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        search.text = ""
        search.forceActiveFocus()
    }

    property color paletteWindow: "#070b10"
    property color paletteSurface: "#101820"
    property color paletteBorder: "#243140"
    property color paletteText: "#e7eef4"
    property color paletteMuted: "#8ea0b3"
    property color paletteAccent: "#7ddec8"
    property color paletteSelected: "#193638"
    property color paletteHover: "#1b303e"
    property color paletteFault: "#ee7a8a"

    function themeByName(name) {
        for (let i = 0; i < themeModel.length; ++i) {
            if (themeModel[i].name === name)
                return themeModel[i]
        }
        return null
    }

    function applyTheme(name) {
        const theme = themeByName(name)
        if (!theme)
            return
        paletteWindow = theme.surface
        paletteSurface = theme.card
        paletteBorder = theme.border
        paletteText = theme.text
        paletteMuted = theme.mutedText
        paletteAccent = theme.focusedBorder
        paletteSelected = theme.focused
        paletteHover = theme.hoveredCard
        paletteFault = theme.fault
    }

    Component.onCompleted: applyTheme(currentTheme)
    onCurrentThemeChanged: applyTheme(currentTheme)

    background: Rectangle {
        color: settings.paletteWindow
        radius: settings.chromeRadius
        border.width: 1
        border.color: settings.paletteBorder
    }

    header: Item {
        implicitHeight: 0
    }

    component PlainLabel: Label { textFormat: Text.PlainText }

    // Shared control feedback: hover and press wash, selection fill with a
    // focus edge. Color changes only, within the theme's motion duration.
    component ChoiceSurface: Rectangle {
        id: surface
        property bool marked: false
        property bool hovered: false
        property bool pressed: false
        property bool focused: false
        radius: settings.chromeRadius
        color: marked || pressed ? settings.paletteSelected :
               hovered ? settings.paletteHover : settings.paletteWindow
        border.width: marked || focused ? 2 : 1
        border.color: marked || focused ? settings.paletteAccent :
                      hovered ? settings.paletteMuted : settings.paletteBorder
        Behavior on color {
            enabled: settings.motionEnabled
            ColorAnimation { duration: settings.motionDuration; easing.type: Easing.OutCubic }
        }
        Behavior on border.color {
            enabled: settings.motionEnabled
            ColorAnimation { duration: settings.motionDuration; easing.type: Easing.OutCubic }
        }
    }

    // A button reachable with Tab whose label and edge follow the palette.
    component ChoiceButton: Button {
        id: choiceButton
        property bool marked: false
        focusPolicy: Qt.TabFocus
        hoverEnabled: true
        font.pixelSize: Math.max(12, settings.uiFont - 1)
        implicitWidth: Math.max(36, implicitContentWidth + 20)
        implicitHeight: Math.max(30, settings.uiFont + 16)
        Accessible.name: text
        contentItem: PlainLabel {
            text: choiceButton.text
            font: choiceButton.font
            color: choiceButton.enabled ? settings.paletteText : settings.paletteMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: ChoiceSurface {
            marked: choiceButton.marked
            hovered: choiceButton.hovered && choiceButton.enabled
            pressed: choiceButton.down
            focused: choiceButton.visualFocus
        }
    }

    // On or off.
    component Toggle: ChoiceButton {
        id: toggle
        property bool on: false
        signal switched(bool on)
        text: on ? qsTr("On") : qsTr("Off")
        marked: on
        implicitWidth: Math.max(56, implicitContentWidth + 24)
        Accessible.role: Accessible.CheckBox
        Accessible.checked: on
        onClicked: switched(!on)
    }

    // One of a few choices side by side: [{id, label}].
    component Segmented: Row {
        id: segmented
        property var choices: []
        property string selected: ""
        // objectName of each button: prefix + its id.
        property string prefix: "choice-"
        signal picked(string id)
        spacing: 4
        Repeater {
            model: segmented.choices
            delegate: ChoiceButton {
                required property var modelData
                objectName: segmented.prefix + modelData.id
                text: modelData.label
                font.family: modelData.family ? modelData.family : settings.font.family
                marked: modelData.id === segmented.selected
                onClicked: segmented.picked(modelData.id)
            }
        }
    }

    // A number with minus and plus, as a readout between them.
    component Stepper: RowLayout {
        id: stepper
        property real value: 0
        property real minimum: 0
        property real maximum: 100
        property real step: 1
        property string unit: ""
        property string prefix: "step"
        signal stepped(real value)
        spacing: 4
        ChoiceButton {
            objectName: stepper.prefix + "Smaller"
            text: "−"
            Accessible.name: qsTr("Less")
            enabled: stepper.value > stepper.minimum
            onClicked: stepper.stepped(Math.max(stepper.minimum, stepper.value - stepper.step))
        }
        PlainLabel {
            objectName: stepper.prefix + "Value"
            text: stepper.value + (stepper.unit.length > 0 ? " " + stepper.unit : "")
            color: settings.paletteText
            font.family: settings.resolvedFontFamily
            font.pixelSize: settings.readoutFont
            horizontalAlignment: Text.AlignHCenter
            Layout.minimumWidth: settings.uiFont * 3
        }
        ChoiceButton {
            objectName: stepper.prefix + "Bigger"
            text: "+"
            Accessible.name: qsTr("More")
            enabled: stepper.value < stepper.maximum
            onClicked: stepper.stepped(Math.min(stepper.maximum, stepper.value + stepper.step))
        }
    }

    // One setting: its label and a line of help on the left, its control on
    // the right (or under it when `wide`), and Reset while it differs from
    // the built-in value. Shown in its section, or anywhere while a search
    // matches it.
    component SettingRow: ColumnLayout {
        id: row
        readonly property bool isSettingRow: true
        required property string section
        property string label: ""
        property string help: ""
        property string keywords: ""
        // False where the platform or build lacks it; never shown then.
        property bool available: true
        property bool modified: false
        // Controls under the label rather than beside it.
        property bool wide: false
        default property alias control: holder.data
        signal reset()
        readonly property bool shown: available && (settings.query.length === 0
                                                    ? section === settings.currentSection
                                                    : settings.matches(label + " " + help + " " + keywords))
        visible: shown
        Layout.fillWidth: true
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: 2
                RowLayout {
                    spacing: 8
                    PlainLabel {
                        text: row.label
                        color: settings.paletteText
                        font.pixelSize: settings.uiFont
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    ChoiceButton {
                        objectName: "reset-" + row.label
                        visible: row.modified
                        text: qsTr("Reset")
                        font.pixelSize: settings.helpFont
                        implicitHeight: Math.max(22, settings.helpFont + 10)
                        Accessible.name: qsTr("Reset %1").arg(row.label)
                        onClicked: row.reset()
                    }
                }
                PlainLabel {
                    Layout.fillWidth: true
                    visible: row.help.length > 0
                    text: row.help
                    color: settings.paletteMuted
                    font.pixelSize: settings.helpFont
                    wrapMode: Text.WordWrap
                }
            }
            RowLayout {
                id: holder
                visible: !row.wide
                Layout.alignment: Qt.AlignRight | Qt.AlignTop
                spacing: 6
            }
        }
        // A wide row's control: the holder's children moved under the label.
        Component.onCompleted: {
            if (!wide)
                return
            while (holder.children.length > 0)
                holder.children[0].parent = under
        }
        ColumnLayout {
            id: under
            visible: row.wide
            Layout.fillWidth: true
            spacing: 6
        }
        Item { implicitHeight: 6 }
    }

    // A search hit's section, over its rows.
    component SectionHeading: PlainLabel {
        required property string section
        readonly property bool any: {
            for (const item of body.children)
                if (item.isSettingRow && item.section === section && item.shown)
                    return true
            return false
        }
        visible: settings.query.length > 0 && any
        text: settings.sectionLabel(section)
        color: settings.paletteMuted
        font.pixelSize: settings.helpFont
        font.bold: true
        font.capitalization: Font.AllUppercase
        Layout.topMargin: 8
    }

    // Window or thread themes as small swatches.
    component ThemeGrid: GridLayout {
        id: grid
        property var choices: []
        property string selected: ""
        property string prefix: "theme-"
        // Thread themes carry background/raised/text/accent; window themes
        // surface/card/text/focusedBorder.
        property bool thread: false
        signal picked(string name)
        columns: Math.max(2, Math.floor(width / 120))
        columnSpacing: 6
        rowSpacing: 6
        Layout.fillWidth: true
        Repeater {
            model: grid.choices
            delegate: Button {
                id: swatch
                required property var modelData
                readonly property bool marked: modelData.name === grid.selected
                objectName: grid.prefix + modelData.name
                Layout.fillWidth: true
                Layout.maximumWidth: 180
                implicitHeight: Math.max(54, settings.uiFont * 4)
                focusPolicy: Qt.TabFocus
                padding: 0
                hoverEnabled: true
                Accessible.name: modelData.label
                onClicked: grid.picked(modelData.name)
                background: Rectangle {
                    radius: grid.thread ? 8 : settings.chromeRadius
                    color: grid.thread ? swatch.modelData.background
                                       : swatch.hovered ? swatch.modelData.hoveredCard : swatch.modelData.card
                    border.width: swatch.marked || swatch.visualFocus ? 2 : 1
                    border.color: swatch.marked || swatch.visualFocus ? settings.paletteAccent
                                  : swatch.hovered ? settings.paletteMuted
                                  : grid.thread ? swatch.modelData.border : swatch.modelData.border
                    Rectangle {
                        x: parent.width - width - 8; y: 8
                        width: parent.width * 0.4; height: 8; radius: 4
                        color: grid.thread ? swatch.modelData.raised : swatch.modelData.surface
                    }
                    Rectangle {
                        x: 8; y: 20
                        width: parent.width * 0.55; height: 3; radius: 1.5
                        color: swatch.modelData.text
                        opacity: 0.85
                    }
                    Rectangle {
                        x: 8; y: 27
                        width: parent.width * 0.3; height: 3; radius: 1.5
                        color: grid.thread ? swatch.modelData.accent : swatch.modelData.focusedBorder
                    }
                    PlainLabel {
                        x: 8
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 5
                        width: parent.width - 16
                        text: swatch.modelData.label
                        color: swatch.modelData.text
                        font.pixelSize: Math.max(11, settings.uiFont - 2)
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    readonly property var modeChoices: [
        { id: "light", label: qsTr("Light") },
        { id: "dark", label: qsTr("Dark") },
        { id: "system", label: qsTr("Match macOS") }
    ]
    readonly property bool lightShown: visual.light === true
    // Themes of the kind light mode shows now; the other kind's are a click
    // away and switch the mode with them.
    readonly property var windowThemes: themeModel.filter(t => (t.light === true) === lightShown)
    readonly property var threadThemes: visualThemes.filter(t => (t.dark === false) === lightShown)
    readonly property var otherWindowThemes: themeModel.filter(t => (t.light === true) !== lightShown)
    readonly property var otherThreadThemes: visualThemes.filter(t => (t.dark === false) !== lightShown)
    readonly property var previewColors: visualThemes.find(t => t.name === visual.theme) || visualThemes[0] || ({})
    function familyOf(list, name) {
        const found = list.find(f => f.name === name)
        return found ? found.family : (list.length > 0 ? list[0].family : "")
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Title, search and the key that opens this window.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(52, settings.uiFont + 36)
            color: settings.paletteSurface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                PlainLabel {
                    text: qsTr("Settings")
                    color: settings.paletteText
                    font.pixelSize: settings.uiFont + 2
                    font.bold: true
                }
                TextField {
                    id: search
                    objectName: "settingsSearch"
                    Layout.fillWidth: true
                    Layout.maximumWidth: 360
                    placeholderText: qsTr("Search settings")
                    placeholderTextColor: settings.paletteMuted
                    color: settings.paletteText
                    font.pixelSize: Math.max(12, settings.uiFont - 1)
                    selectByMouse: true
                    Accessible.name: qsTr("Search settings")
                    background: ChoiceSurface {
                        focused: search.activeFocus
                        hovered: search.hovered
                    }
                    Keys.onDownPressed: sectionList.forceActiveFocus()
                }
                Item { Layout.fillWidth: true }
                PlainLabel {
                    text: settings.shortcutHint
                    color: settings.paletteMuted
                    font.family: settings.resolvedFontFamily
                    font.pixelSize: settings.readoutFont
                    elide: Text.ElideRight
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The sections; arrows move between them.
            ListView {
                id: sectionList
                objectName: "settingsSections"
                Layout.preferredWidth: Math.max(150, settings.uiFont * 12)
                Layout.fillHeight: true
                Layout.topMargin: 10
                model: settings.sections
                activeFocusOnTab: true
                keyNavigationEnabled: true
                interactive: false
                currentIndex: Math.max(0, settings.sections.findIndex(s => s.id === settings.currentSection))
                onCurrentIndexChanged: if (activeFocus && currentIndex >= 0)
                                           settings.showSection(settings.sections[currentIndex].id)
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Sections")
                delegate: ItemDelegate {
                    id: sectionItem
                    required property var modelData
                    required property int index
                    objectName: "section-" + modelData.id
                    readonly property bool current: settings.query.length === 0
                                                    && modelData.id === settings.currentSection
                    width: ListView.view.width
                    height: Math.max(32, settings.uiFont + 18)
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    Accessible.name: modelData.label
                    onClicked: settings.showSection(modelData.id)
                    contentItem: PlainLabel {
                        leftPadding: 10
                        text: sectionItem.modelData.label
                        color: sectionItem.current ? settings.paletteText : settings.paletteMuted
                        font.pixelSize: settings.uiFont
                        font.weight: sectionItem.current ? Font.DemiBold : Font.Normal
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: sectionItem.current ? settings.paletteSelected
                               : sectionItem.hovered ? settings.paletteHover : "transparent"
                        Rectangle {
                            width: 2
                            height: parent.height
                            visible: sectionItem.current
                            color: settings.paletteAccent
                        }
                        Rectangle {
                            anchors.fill: parent
                            color: "transparent"
                            border.width: sectionList.activeFocus && sectionItem.ListView.isCurrentItem ? 1 : 0
                            border.color: settings.paletteAccent
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: settings.paletteBorder
            }

            Flickable {
                id: settingsFlick
                objectName: "settingsScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 16
                contentWidth: width
                contentHeight: body.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                ColumnLayout {
                    id: body
                    width: settingsFlick.width - 12
                    spacing: 10

                    PlainLabel {
                        objectName: "settingsNoMatch"
                        visible: settings.query.length > 0 && !generalHeading.any && !appearanceHeading.any
                                 && !readingHeading.any && !terminalHeading.any && !agentsHeading.any
                                 && !keysHeading.any && !advancedHeading.any
                        text: qsTr("No setting matches “%1”.").arg(search.text.trim())
                        color: settings.paletteMuted
                        font.pixelSize: settings.uiFont
                    }

                    // General ------------------------------------------------
                    SectionHeading { id: generalHeading; section: "general" }
                    SettingRow {
                        section: "general"
                        label: qsTr("Chime when a turn finishes")
                        help: qsTr("Once, quietly, when a Codex or Claude turn ends or an agent asks for something out of view.")
                        keywords: "sound alert notification"
                        modified: settings.finishSound !== settings.defaults["alerts.finished"]
                        onReset: settings.finishSoundChosen(settings.defaults["alerts.finished"])
                        ChoiceButton {
                            objectName: "play-chime"
                            text: qsTr("Play")
                            onClicked: settings.chimePlayed(false)
                        }
                        Toggle {
                            objectName: "switch-finishSound"
                            on: settings.finishSound
                            Accessible.name: qsTr("Chime when a turn finishes")
                            onSwitched: on => settings.finishSoundChosen(on)
                        }
                    }
                    SettingRow {
                        section: "general"
                        label: qsTr("Keep this Mac awake")
                        help: qsTr("While it is plugged in, so the phone can reach your agents. The display still sleeps; closing the lid still sleeps.")
                        keywords: "sleep power phone"
                        modified: settings.keepAwake !== settings.defaults.keepAwake
                        onReset: settings.keepAwakeChosen(settings.defaults.keepAwake)
                        Toggle {
                            objectName: "switch-keepAwake"
                            on: settings.keepAwake
                            Accessible.name: qsTr("Keep this Mac awake")
                            onSwitched: on => settings.keepAwakeChosen(on)
                        }
                    }
                    SettingRow {
                        section: "general"
                        label: qsTr("Show the category sidebar")
                        keywords: "rail categories"
                        modified: settings.sidebarVisible !== settings.defaults.sidebarVisible
                        onReset: settings.sidebarChosen(settings.defaults.sidebarVisible)
                        Toggle {
                            objectName: "switch-sidebar"
                            on: settings.sidebarVisible
                            Accessible.name: qsTr("Show the category sidebar")
                            onSwitched: on => settings.sidebarChosen(on)
                        }
                    }
                    SettingRow {
                        section: "general"
                        label: qsTr("Show agent previews under the stage")
                        keywords: "strip cards"
                        modified: settings.previewsVisible !== settings.defaults.previewsVisible
                        onReset: settings.previewsChosen(settings.defaults.previewsVisible)
                        Toggle {
                            objectName: "switch-previews"
                            on: settings.previewsVisible
                            Accessible.name: qsTr("Show agent previews under the stage")
                            onSwitched: on => settings.previewsChosen(on)
                        }
                    }
                    SettingRow {
                        section: "general"
                        label: qsTr("Guess my next prompt")
                        help: qsTr("Tab fills in a guess at what you will type. Each guess asks a model, at most 60 times an hour.")
                        keywords: "suggestion tab predict nextPrompt"
                        modified: settings.values["nextPrompt.auto"] !== settings.defaults["nextPrompt.auto"]
                        onReset: settings.settingChosen("nextPrompt.auto", settings.defaults["nextPrompt.auto"])
                        Toggle {
                            objectName: "switch-nextPrompt"
                            on: settings.values["nextPrompt.auto"] === true
                            Accessible.name: qsTr("Guess my next prompt")
                            onSwitched: on => settings.settingChosen("nextPrompt.auto", on)
                        }
                    }
                    SettingRow {
                        objectName: "launchAtLoginRow"
                        section: "general"
                        available: settings.loginAvailable
                        label: qsTr("Keep agents running at login")
                        help: qsTr("At login lapis restarts agents that were running, resuming their conversations, and keeps them until you open it.")
                        keywords: "startup"
                        Toggle {
                            on: settings.launchAtLogin
                            Accessible.name: qsTr("Keep agents running at login")
                            onSwitched: on => settings.launchAtLoginChosen(on)
                        }
                    }
                    SettingRow {
                        objectName: "updatesRow"
                        section: "general"
                        available: settings.updatesAvailable
                        label: qsTr("Updates")
                        help: qsTr("lapis checks for updates once a day.")
                        keywords: "version upgrade"
                        ChoiceButton {
                            objectName: "checkUpdates"
                            text: qsTr("Check now")
                            onClicked: settings.checkUpdates()
                        }
                    }

                    // Appearance ---------------------------------------------
                    SectionHeading { id: appearanceHeading; section: "appearance" }
                    SettingRow {
                        section: "appearance"
                        label: qsTr("Light or dark")
                        help: qsTr("The reading view and the window around it. Terminals keep their own colors.")
                        keywords: "appearance mode theme night day macOS system"
                        modified: (settings.visual.appearance || "dark") !== settings.defaults["visual.appearance"]
                        onReset: settings.appearanceChosen(settings.defaults["visual.appearance"])
                        Segmented {
                            prefix: "appearance-"
                            choices: settings.modeChoices
                            selected: settings.visual.appearance || "dark"
                            onPicked: id => settings.appearanceChosen(id)
                        }
                    }
                    SettingRow {
                        section: "appearance"
                        label: qsTr("Preview")
                        keywords: "sample example"
                        wide: true
                        SettingsPreview {
                            objectName: "settingsPreview"
                            Layout.fillWidth: true
                            colors: settings.previewColors
                            sansFamily: settings.familyOf(settings.visualFonts, settings.visual.font || "system")
                            monoFamily: settings.familyOf(settings.visualCodeFonts, settings.visual.codeFont || "system")
                            fontSize: Math.min(16, settings.visualSize)
                        }
                    }
                    SettingRow {
                        section: "appearance"
                        label: settings.lightShown ? qsTr("Window theme in light mode") : qsTr("Window theme in dark mode")
                        help: qsTr("The sidebar, the agent strip and dialogs. A theme from the other list switches to that mode.")
                        keywords: "chrome colors sidebar"
                        wide: true
                        modified: settings.currentTheme !== (settings.lightShown ? settings.defaults["visual.lightWindowTheme"]
                                                                                 : settings.defaults["visual.darkWindowTheme"])
                        onReset: settings.themeChosen(settings.lightShown ? settings.defaults["visual.lightWindowTheme"]
                                                                          : settings.defaults["visual.darkWindowTheme"])
                        ThemeGrid {
                            choices: settings.windowThemes
                            selected: settings.currentTheme
                            onPicked: name => settings.themeChosen(name)
                        }
                        PlainLabel {
                            text: settings.lightShown ? qsTr("Dark themes") : qsTr("Light themes")
                            color: settings.paletteMuted
                            font.pixelSize: settings.helpFont
                        }
                        ThemeGrid {
                            choices: settings.otherWindowThemes
                            selected: settings.currentTheme
                            onPicked: name => settings.themeChosen(name)
                        }
                    }
                    SettingRow {
                        section: "appearance"
                        label: qsTr("Density")
                        help: settings.currentDensity === "compact" ? qsTr("Tighter categories and agent cards. Terminal text is unchanged.")
                              : settings.currentDensity === "minimal" ? qsTr("Smallest chrome. The terminal stays the main surface.")
                              : qsTr("Wider category rail and agent cards. Terminal text is unchanged.")
                        keywords: "compact comfortable minimal spacing"
                        modified: settings.currentDensity !== settings.defaults.density
                        onReset: settings.densityChosen(settings.defaults.density)
                        Segmented {
                            choices: settings.densityModel.map(name => ({ id: name,
                                label: name === "comfortable" ? qsTr("Comfortable") : name === "compact" ? qsTr("Compact")
                                     : name === "minimal" ? qsTr("Minimal") : name }))
                            selected: settings.currentDensity
                            onPicked: id => settings.densityChosen(id)
                        }
                    }

                    // Reading view -------------------------------------------
                    SectionHeading { id: readingHeading; section: "reading" }
                    SettingRow {
                        section: "reading"
                        label: settings.lightShown ? qsTr("Thread theme in light mode") : qsTr("Thread theme in dark mode")
                        help: qsTr("How an agent's conversation reads as a thread. A theme from the other list switches to that mode.")
                        keywords: "visual view colors tokyo codex claude graphite black"
                        wide: true
                        modified: (settings.visual.theme || "terminal") !== (settings.lightShown ? settings.defaults["visual.lightTheme"]
                                                                                                  : settings.defaults["visual.darkTheme"])
                        onReset: settings.visualChosen("theme", settings.lightShown ? settings.defaults["visual.lightTheme"]
                                                                                    : settings.defaults["visual.darkTheme"])
                        ThemeGrid {
                            objectName: "visualThemes"
                            thread: true
                            prefix: "visualTheme-"
                            choices: settings.threadThemes
                            selected: settings.visual.theme || "terminal"
                            onPicked: name => settings.visualChosen("theme", name)
                        }
                        PlainLabel {
                            text: settings.lightShown ? qsTr("Dark themes") : qsTr("Light themes")
                            color: settings.paletteMuted
                            font.pixelSize: settings.helpFont
                        }
                        ThemeGrid {
                            thread: true
                            prefix: "visualTheme-"
                            choices: settings.otherThreadThemes
                            selected: settings.visual.theme || "terminal"
                            onPicked: name => settings.visualChosen("theme", name)
                        }
                    }
                    SettingRow {
                        section: "reading"
                        label: qsTr("Reading font")
                        keywords: "typeface serif"
                        wide: true
                        modified: (settings.visual.font || "system") !== settings.defaults["visual.font"]
                        onReset: settings.visualChosen("font", settings.defaults["visual.font"])
                        Segmented {
                            Layout.fillWidth: true
                            prefix: "visual-font-"
                            choices: settings.visualFonts.map(f => ({ id: f.name, label: f.label, family: f.family }))
                            selected: settings.visual.font || "system"
                            onPicked: id => settings.visualChosen("font", id)
                        }
                    }
                    SettingRow {
                        section: "reading"
                        label: qsTr("Code font")
                        keywords: "monospace typeface"
                        wide: true
                        modified: (settings.visual.codeFont || "system") !== settings.defaults["visual.codeFont"]
                        onReset: settings.visualChosen("codeFont", settings.defaults["visual.codeFont"])
                        Segmented {
                            Layout.fillWidth: true
                            prefix: "visual-codeFont-"
                            choices: settings.visualCodeFonts.map(f => ({ id: f.name, label: f.label, family: f.family }))
                            selected: settings.visual.codeFont || "system"
                            onPicked: id => settings.visualChosen("codeFont", id)
                        }
                    }
                    SettingRow {
                        section: "reading"
                        label: qsTr("Text size")
                        help: (settings.visual.size || 0) > 0 ? qsTr("%1 px").arg(settings.visualSize)
                              : qsTr("%1 px, following the terminal font").arg(settings.visualSize)
                        keywords: "bigger smaller"
                        modified: (settings.visual.size || 0) !== settings.defaults["visual.size"]
                        onReset: settings.visualChosen("size", settings.defaults["visual.size"])
                        Stepper {
                            prefix: "visual"
                            value: settings.visualSize
                            minimum: 10
                            maximum: 40
                            unit: qsTr("px")
                            onStepped: value => settings.visualChosen("size", value)
                        }
                    }
                    SettingRow {
                        section: "reading"
                        available: settings.visualEditors.length > 0
                        label: qsTr("Open files in")
                        help: qsTr("Command-click a file path in the thread to open it there.")
                        keywords: "editor vscode cursor zed textedit"
                        wide: true
                        modified: (settings.visual.editor || "auto") !== settings.defaults["visual.editor"]
                        onReset: settings.visualChosen("editor", settings.defaults["visual.editor"])
                        Segmented {
                            Layout.fillWidth: true
                            prefix: "visual-editor-"
                            choices: settings.visualEditors.map(e => ({ id: e.name, label: e.label }))
                            selected: (settings.visual.editor || "auto") !== "auto" ? settings.visual.editor
                                      : settings.visualEditors.length > 0 ? settings.visualEditors[0].name : ""
                            onPicked: id => settings.visualChosen("editor", id)
                        }
                    }
                    SettingRow {
                        section: "reading"
                        label: qsTr("Show every agent as a thread")
                        help: qsTr("Each Claude Code, Codex and Antigravity agent on this Mac opens in the reading view.")
                        keywords: "visual all agents"
                        modified: (settings.visual.all === true) !== settings.defaults["visual.all"]
                        onReset: settings.visualChosen("all", settings.defaults["visual.all"])
                        Toggle {
                            objectName: "switch-visualAll"
                            on: settings.visual.all === true
                            Accessible.name: qsTr("Show every agent as a thread")
                            onSwitched: on => settings.visualChosen("all", on)
                        }
                    }

                    // Terminal -----------------------------------------------
                    SectionHeading { id: terminalHeading; section: "terminal" }
                    SettingRow {
                        section: "terminal"
                        label: qsTr("Font")
                        help: qsTr("Also used for paths, shortcuts and states in the window.")
                        keywords: "typeface monospace family"
                        wide: true
                        modified: settings.fontFamily !== settings.defaults["terminalFont.family"]
                        onReset: settings.fontFamilyChosen(settings.defaults["terminalFont.family"])
                        ComboBox {
                            id: familyChoice
                            objectName: "fontFamilyChoice"
                            Layout.fillWidth: true
                            implicitHeight: Math.max(30, settings.uiFont + 16)
                            font.pixelSize: Math.max(12, settings.uiFont - 1)
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Terminal font")
                            // Index 0 is the platform default; the rest are installed
                            // fixed-pitch families. A configured name that is not
                            // installed is still listed so the selection stays visible.
                            model: {
                                const names = [""].concat(settings.fontFamilies)
                                if (settings.fontFamily.length > 0 && names.indexOf(settings.fontFamily) < 0)
                                    names.splice(1, 0, settings.fontFamily)
                                return names
                            }
                            currentIndex: Math.max(0, model.indexOf(settings.fontFamily))
                            displayText: currentIndex <= 0 ? qsTr("System default · %1").arg(settings.resolvedFontFamily) :
                                                             currentText
                            onActivated: function(index) {
                                settings.fontFamilyChosen(model[index])
                                // Activation assigns currentIndex; follow the saved value again.
                                currentIndex = Qt.binding(() => Math.max(0, model.indexOf(settings.fontFamily)))
                            }
                            contentItem: PlainLabel {
                                text: familyChoice.displayText
                                font.family: settings.resolvedFontFamily
                                font.pixelSize: familyChoice.font.pixelSize
                                color: settings.paletteText
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 10
                                rightPadding: 24
                            }
                            indicator: PlainLabel {
                                x: familyChoice.width - width - 8
                                y: (familyChoice.height - height) / 2
                                text: "▾"
                                color: settings.paletteMuted
                            }
                            background: ChoiceSurface {
                                hovered: familyChoice.hovered
                                pressed: familyChoice.pressed
                                focused: familyChoice.visualFocus
                            }
                            delegate: ItemDelegate {
                                id: familyOption
                                required property var modelData
                                required property int index
                                width: familyChoice.width
                                hoverEnabled: true
                                // Each family previews in its own face.
                                contentItem: PlainLabel {
                                    text: familyOption.modelData.length > 0 ? familyOption.modelData : qsTr("System default")
                                    font.family: familyOption.modelData.length > 0 ? familyOption.modelData :
                                                                                     settings.resolvedFontFamily
                                    font.pixelSize: familyChoice.font.pixelSize
                                    color: settings.paletteText
                                    elide: Text.ElideRight
                                    leftPadding: 8
                                }
                                background: Rectangle {
                                    color: familyOption.index === familyChoice.currentIndex ? settings.paletteSelected :
                                           familyOption.hovered ? settings.paletteHover : settings.paletteSurface
                                }
                            }
                            popup.background: Rectangle {
                                color: settings.paletteSurface
                                border.color: settings.paletteBorder
                            }
                            popup.onAboutToShow: {
                                const limit = Math.max(120, settings.height - 80)
                                familyChoice.popup.height = Math.min(familyChoice.popup.implicitHeight, limit)
                            }
                        }
                        PlainLabel {
                            objectName: "fontUnavailable"
                            Layout.fillWidth: true
                            // Font matching ignores case, so "menlo" resolves to Menlo.
                            visible: settings.fontFamily.length > 0 &&
                                     settings.fontFamily.toLowerCase() !== settings.resolvedFontFamily.toLowerCase()
                            text: qsTr("%1 is not an installed fixed-width font; using %2.")
                                  .arg(settings.fontFamily).arg(settings.resolvedFontFamily)
                            color: settings.paletteFault
                            wrapMode: Text.WordWrap
                            font.pixelSize: settings.helpFont
                        }
                    }
                    SettingRow {
                        section: "terminal"
                        label: qsTr("Size")
                        help: qsTr("%1 to %2 px. Command-plus, minus and zero change it too.")
                              .arg(settings.fontSizeMinimum).arg(settings.fontSizeMaximum)
                        keywords: "font text bigger smaller"
                        modified: settings.fontSize !== settings.defaults["terminalFont.size"]
                        onReset: settings.fontSizeChosen(settings.defaults["terminalFont.size"])
                        ChoiceButton {
                            objectName: "fontSmaller"
                            text: "−"
                            Accessible.name: qsTr("Smaller terminal text")
                            enabled: settings.fontSize > settings.fontSizeMinimum
                            onClicked: settings.fontSizeChosen(settings.fontSize - 1)
                        }
                        TextField {
                            id: fontSizeField
                            objectName: "fontSizeValue"
                            text: settings.fontSize
                            color: settings.paletteText
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: Math.max(12, settings.uiFont - 1)
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: TextInput.AlignVCenter
                            Layout.preferredWidth: settings.uiFont * 3.5
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{1,3}/ }
                            maximumLength: 3
                            inputMethodHints: Qt.ImhDigitsOnly
                            selectByMouse: true
                            Accessible.name: qsTr("Terminal font size in pixels")
                            background: ChoiceSurface {
                                hovered: fontSizeField.hovered
                                focused: fontSizeField.activeFocus
                            }
                            function commitFontSize() {
                                const pixels = Number.parseInt(fontSizeField.text, 10)
                                if (Number.isInteger(pixels) &&
                                        pixels >= settings.fontSizeMinimum &&
                                        pixels <= settings.fontSizeMaximum &&
                                        pixels !== settings.fontSize)
                                    settings.fontSizeChosen(pixels)
                                fontSizeField.text = Qt.binding(() => settings.fontSize)
                            }
                            onEditingFinished: commitFontSize()
                            onActiveFocusChanged: if (!activeFocus) commitFontSize()
                        }
                        PlainLabel {
                            objectName: "fontSizeUnit"
                            text: qsTr("px")
                            color: settings.paletteMuted
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: settings.helpFont
                        }
                        ChoiceButton {
                            objectName: "fontLarger"
                            text: "+"
                            Accessible.name: qsTr("Larger terminal text")
                            enabled: settings.fontSize < settings.fontSizeMaximum
                            onClicked: settings.fontSizeChosen(settings.fontSize + 1)
                        }
                    }
                    SettingRow {
                        section: "terminal"
                        label: qsTr("Sample")
                        keywords: "preview"
                        wide: true
                        PlainLabel {
                            objectName: "fontPreview"
                            Layout.fillWidth: true
                            text: "~/lapis $ codex   0O 1lI {}[] ->"
                            color: settings.paletteText
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: settings.fontSize
                            elide: Text.ElideRight
                        }
                    }
                    SettingRow {
                        section: "terminal"
                        label: qsTr("Return to the bottom when typing")
                        help: qsTr("Typing over a full-screen agent you scrolled back brings it back to the bottom first.")
                        keywords: "scroll history"
                        modified: settings.scrollToBottomOnTyping !== settings.defaults.scrollToBottomOnTyping
                        onReset: settings.scrollChosen(settings.defaults.scrollToBottomOnTyping)
                        Toggle {
                            objectName: "switch-scrollToBottom"
                            on: settings.scrollToBottomOnTyping
                            Accessible.name: qsTr("Return to the bottom when typing")
                            onSwitched: on => settings.scrollChosen(on)
                        }
                    }

                    // New agents ---------------------------------------------
                    SectionHeading { id: agentsHeading; section: "agents" }
                    PlainLabel {
                        visible: settings.query.length === 0 && settings.currentSection === "agents"
                        Layout.fillWidth: true
                        text: qsTr("The new-agent form starts with your last choices; these fill in when there are none.")
                        color: settings.paletteMuted
                        font.pixelSize: settings.helpFont
                        wrapMode: Text.WordWrap
                    }
                    SettingRow {
                        section: "agents"
                        label: qsTr("CLI")
                        keywords: "harness claude codex default new agent"
                        wide: true
                        modified: (settings.values["newAgent.harness"] || "") !== settings.defaults["newAgent.harness"]
                        onReset: settings.settingChosen("newAgent.harness", settings.defaults["newAgent.harness"])
                        Segmented {
                            Layout.fillWidth: true
                            prefix: "agent-cli-"
                            choices: settings.harnesses.map(h => ({ id: h.id, label: h.name }))
                            selected: settings.values["newAgent.harness"] || ""
                            onPicked: id => settings.settingChosen("newAgent.harness", id)
                        }
                    }
                    SettingRow {
                        section: "agents"
                        label: qsTr("Starting folder")
                        help: qsTr("On this Mac. Start it with ~ for your home folder.")
                        keywords: "directory project path"
                        wide: true
                        modified: (settings.values["newAgent.folder"] || "") !== settings.defaults["newAgent.folder"]
                        onReset: settings.settingChosen("newAgent.folder", settings.defaults["newAgent.folder"])
                        TextField {
                            id: folderField
                            objectName: "agentFolder"
                            Layout.fillWidth: true
                            text: settings.values["newAgent.folder"] || ""
                            placeholderText: qsTr("The last folder used")
                            placeholderTextColor: settings.paletteMuted
                            color: settings.paletteText
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: Math.max(12, settings.uiFont - 1)
                            selectByMouse: true
                            Accessible.name: qsTr("Starting folder")
                            background: ChoiceSurface {
                                hovered: folderField.hovered
                                focused: folderField.activeFocus
                            }
                            function commit() {
                                if (text.trim() !== (settings.values["newAgent.folder"] || ""))
                                    settings.settingChosen("newAgent.folder", text.trim())
                                text = Qt.binding(() => settings.values["newAgent.folder"] || "")
                            }
                            onEditingFinished: commit()
                        }
                    }
                    SettingRow {
                        section: "agents"
                        label: qsTr("Approval mode")
                        help: qsTr("What an agent may do without asking. Full access when none is chosen.")
                        keywords: "permissions edits auto full"
                        modified: (settings.values["newAgent.mode"] || "") !== settings.defaults["newAgent.mode"]
                        onReset: settings.settingChosen("newAgent.mode", settings.defaults["newAgent.mode"])
                        Segmented {
                            prefix: "agent-mode-"
                            choices: [{ id: "edits", label: qsTr("Accept edits") }, { id: "auto", label: qsTr("Auto") },
                                      { id: "full", label: qsTr("Full access") }]
                            selected: settings.values["newAgent.mode"] || "full"
                            onPicked: id => settings.settingChosen("newAgent.mode", id)
                        }
                    }
                    Repeater {
                        model: settings.harnesses
                        delegate: SettingRow {
                            id: updateRow
                            required property var modelData
                            readonly property bool updating: (settings.values.harnessUpdatesOff || []).indexOf(modelData.id) < 0
                            section: "agents"
                            label: qsTr("Keep %1 up to date").arg(modelData.name)
                            help: qsTr("lapis updates it before starting a new agent. Off keeps the version you have.")
                            keywords: "cli update version upgrade harnessUpdates"
                            modified: !updating
                            onReset: settings.settingChosen("harnessUpdates." + modelData.id, true)
                            Toggle {
                                objectName: "switch-update-" + updateRow.modelData.id
                                on: updateRow.updating
                                Accessible.name: updateRow.label
                                onSwitched: on => settings.settingChosen("harnessUpdates." + updateRow.modelData.id, on)
                            }
                        }
                    }

                    // Keys ---------------------------------------------------
                    SectionHeading { id: keysHeading; section: "keys" }
                    PlainLabel {
                        visible: settings.query.length === 0 && settings.currentSection === "keys"
                        Layout.fillWidth: true
                        text: qsTr("To change a key, add the action's name and up to four keys under \"keybindings\" in lapis.json (Advanced), for example \"toggleTerminal\": [\"Meta+`\"].")
                        color: settings.paletteMuted
                        font.pixelSize: settings.helpFont
                        wrapMode: Text.WordWrap
                    }
                    // One row per action with a key, searchable like the rest.
                    Repeater {
                        objectName: "shortcutList"
                        model: settings.shortcutRows
                        delegate: RowLayout {
                            id: keyRow
                            required property var modelData
                            readonly property bool isSettingRow: true
                            readonly property string section: "keys"
                            readonly property bool shown: settings.query.length === 0
                                                          ? settings.currentSection === "keys"
                                                          : settings.matches(modelData.label + " " + modelData.keys + " shortcut key")
                            readonly property bool available: true
                            readonly property bool modified: false
                            function reset() {}
                            objectName: "key-" + modelData.label
                            visible: shown
                            Layout.fillWidth: true
                            spacing: 12
                            PlainLabel {
                                Layout.fillWidth: true
                                text: keyRow.modelData.label
                                color: settings.paletteText
                                elide: Text.ElideRight
                            }
                            PlainLabel {
                                text: keyRow.modelData.keys
                                color: settings.paletteMuted
                                font.family: settings.resolvedFontFamily
                                font.pixelSize: settings.readoutFont
                            }
                        }
                    }

                    // Advanced -----------------------------------------------
                    SectionHeading { id: advancedHeading; section: "advanced" }
                    PlainLabel {
                        visible: settings.query.length === 0 && settings.currentSection === "advanced"
                        Layout.fillWidth: true
                        text: qsTr("These change how lapis records and ranks your work. Read the help before changing one.")
                        color: settings.paletteFault
                        font.pixelSize: settings.helpFont
                        wrapMode: Text.WordWrap
                    }
                    SettingRow {
                        section: "advanced"
                        label: qsTr("Record my input for study")
                        help: qsTr("Keeps a private log of keys, clicks and pastes in lapis's windows (passwords left out) under the lapis folder. It holds what you type; do not share it.")
                        keywords: "interaction log privacy record interactionLog"
                        modified: settings.values["interactionLog.enabled"] !== settings.defaults["interactionLog.enabled"]
                        onReset: settings.settingChosen("interactionLog.enabled", settings.defaults["interactionLog.enabled"])
                        Toggle {
                            objectName: "switch-interactionLog"
                            on: settings.values["interactionLog.enabled"] === true
                            Accessible.name: qsTr("Record my input for study")
                            onSwitched: on => settings.settingChosen("interactionLog.enabled", on)
                        }
                    }
                    SettingRow {
                        section: "advanced"
                        label: qsTr("Tab goes first to agents waiting longer than")
                        help: qsTr("An agent that needs you this long comes first, oldest first. 0 turns this off.")
                        keywords: "tab away overdue hours tabAway"
                        modified: settings.values["tabAway.overdueHours"] !== settings.defaults["tabAway.overdueHours"]
                        onReset: settings.settingChosen("tabAway.overdueHours", settings.defaults["tabAway.overdueHours"])
                        Stepper {
                            prefix: "overdue"
                            value: settings.values["tabAway.overdueHours"] || 0
                            minimum: 0
                            maximum: 48
                            unit: qsTr("h")
                            onStepped: value => settings.settingChosen("tabAway.overdueHours", value)
                        }
                    }
                    SettingRow {
                        section: "advanced"
                        label: qsTr("Configuration file")
                        help: qsTr("Every change here is saved to it at once, and edits to it (by you or an agent) apply at once. Key bindings, models per CLI, folders per machine, extra CLI flags, chime sound files and the next-prompt model live only there.")
                        keywords: "lapis.json config file reload json keybindings models"
                        wide: true
                        PlainLabel {
                            Layout.fillWidth: true
                            text: settings.configPath
                            color: settings.paletteMuted
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: settings.readoutFont
                            elide: Text.ElideMiddle
                        }
                        PlainLabel {
                            Layout.fillWidth: true
                            visible: settings.configDiagnostic.length > 0
                            text: settings.configDiagnostic
                            color: settings.paletteFault
                            wrapMode: Text.WordWrap
                            font.pixelSize: settings.helpFont
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            ChoiceButton {
                                objectName: "openConfig"
                                text: qsTr("Open lapis.json")
                                enabled: settings.fileBacked
                                onClicked: settings.openConfigRequested()
                            }
                            ChoiceButton {
                                objectName: "openConfigDocs"
                                text: qsTr("Open config docs")
                                onClicked: settings.openDocsRequested()
                            }
                            ChoiceButton {
                                objectName: "reloadConfig"
                                text: qsTr("Reload")
                                enabled: settings.fileBacked
                                onClicked: settings.reloadRequested()
                            }
                        }
                    }

                    // Reset every changed row of the section shown.
                    ChoiceButton {
                        objectName: "resetSection"
                        visible: settings.query.length === 0 && settings.currentSection !== "keys"
                                 && settings.currentSection !== "advanced"
                        enabled: {
                            for (const row of body.children)
                                if (row.isSettingRow && row.section === settings.currentSection && row.available && row.modified)
                                    return true
                            return false
                        }
                        Layout.topMargin: 8
                        text: qsTr("Reset %1").arg(settings.sectionLabel(settings.currentSection))
                        onClicked: settings.resetSection()
                    }
                }
            }
        }
    }
}
