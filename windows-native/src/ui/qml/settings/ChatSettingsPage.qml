import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var settingsModel: null
    property var routingModel: null
    property alias defaultModeSelector: defaultModeSelector
    property alias chatDeliverySelector: chatDeliverySelector
    property alias chatDeliveryDescription: chatDeliveryDescription
    property alias openAISectionLabel: openAISectionLabel
    property alias openAIModelSelector: openAIModelSelector
    property alias openAIKeyField: openAIKeyField
    property alias openAISaveButton: openAISaveButton
    property alias openAIRemoveButton: openAIRemoveButton
    property alias lumoSectionLabel: lumoSectionLabel
    property alias lumoModelSelector: lumoModelSelector
    property alias lumoKeyField: lumoKeyField
    property alias lumoSaveButton: lumoSaveButton
    property alias lumoRemoveButton: lumoRemoveButton

    spacing: 0

    component SettingsSelector: ComboBox {
        id: control

        textRole: "text"
        valueRole: "value"
        implicitHeight: 28
        implicitWidth: 168

        delegate: ItemDelegate {
            required property int index
            width: control.width
            height: 28
            highlighted:
                control.highlightedIndex === index

            contentItem: Label {
                text: control.textAt(parent.index)
                color: parent.highlighted
                    ? CompanionTheme.accentText
                    : CompanionTheme.textPrimary
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                radius: 4
                color: parent.highlighted
                    ? CompanionTheme.accent
                    : parent.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.surfaceRaised
            }
        }

        contentItem: Label {
            leftPadding: 10
            rightPadding: 28
            text: control.displayText
            color: CompanionTheme.textPrimary
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        indicator: Canvas {
            x: control.width - width - 10
            y: (control.height - height) / 2
            width: 9
            height: 6
            contextType: "2d"

            onPaint: {
                context.reset()
                context.strokeStyle =
                    CompanionTheme.textSecondary
                context.lineWidth = 1.5
                context.lineCap = "round"
                context.lineJoin = "round"
                context.beginPath()
                context.moveTo(1, 1)
                context.lineTo(
                    width / 2,
                    height - 1)
                context.lineTo(width - 1, 1)
                context.stroke()
            }
        }

        background: Rectangle {
            radius: CompanionTheme.radius
            color: control.down
                ? CompanionTheme.controlPressed
                : control.hovered
                    ? CompanionTheme.controlHover
                    : CompanionTheme.control
            border.color: control.activeFocus
                ? CompanionTheme.accent
                : CompanionTheme.border
        }

        popup: Popup {
            y: control.height + 4
            width: control.width
            implicitHeight:
                contentItem.implicitHeight + 2
            padding: 1

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: control.popup.visible
                    ? control.delegateModel
                    : null
                currentIndex:
                    control.highlightedIndex
            }

            background: Rectangle {
                radius: CompanionTheme.radius
                color: CompanionTheme.surfaceRaised
                border.color: CompanionTheme.border
            }
        }
    }

    function modelValue(name, fallbackValue) {
        if (settingsModel === null || settingsModel === undefined) {
            return fallbackValue
        }
        const value = settingsModel[name]
        return value === undefined ? fallbackValue : value
    }

    function routeModeValue() {
        if (routingModel === null
                || routingModel === undefined
                || routingModel.routeMode === undefined) {
            return "local-chat"
        }
        return routingModel.routeMode
    }

    function syncDefaultModeSelector() {
        defaultModeSelector.currentIndex =
            defaultModeSelector.indexOfValue(
                routeModeValue())
    }

    function selectedChatModelId() {
        if (routingModel === null
                || routingModel === undefined
                || routingModel.selectedChatModelId === undefined) {
            return "on-device"
        }
        return routingModel.selectedChatModelId
    }

    function selectedChatDelivery() {
        const modelId = selectedChatModelId()
        if (modelId.indexOf("openai:") === 0) {
            return "openai"
        }
        if (modelId.indexOf("lumo:") === 0) {
            return "lumo"
        }
        return "on-device"
    }

    function chatDeliveryDescriptionText() {
        const delivery = selectedChatDelivery()
        if (delivery === "openai") {
            return "Answers inside Companion through your OpenAI API key."
        }
        if (delivery === "lumo") {
            return "Answers inside Companion through an API key included with Lumo+."
        }
        return "Reasons on this PC without an API key or Codex usage; "
            + "live tools contact their data sources when needed."
    }

    function modelsForGroup(group) {
        if (routingModel === null
                || routingModel === undefined
                || routingModel.chatModels === undefined) {
            return []
        }
        const result = []
        const models = routingModel.chatModels
        for (let index = 0; index < models.length; ++index) {
            const model = models[index]
            if (model.group !== group) {
                continue
            }
            let note = model.detail === undefined
                ? ""
                : model.detail
            const separator = note.indexOf(" - ")
            if (separator >= 0) {
                note = note.substring(separator + 3)
            }
            result.push({
                text: model.title
                    + (note.length > 0
                        ? " \u00b7 " + note
                        : ""),
                value: model.id
            })
        }
        return result
    }

    function chooseChatModel(value) {
        if (routingModel === null
                || routingModel === undefined) {
            return false
        }
        if (routingModel.chooseChatModel !== undefined) {
            return routingModel.chooseChatModel(value)
        }
        if (routingModel.selectedChatModelId !== undefined) {
            routingModel.selectedChatModelId = value
            return true
        }
        return false
    }

    function firstModelId(group) {
        const models = modelsForGroup(group)
        return models.length > 0
            ? models[0].value
            : ""
    }

    function syncChatSelectors() {
        chatDeliverySelector.currentIndex =
            chatDeliverySelector.indexOfValue(
                selectedChatDelivery())
        openAIModelSelector.currentIndex =
            openAIModelSelector.indexOfValue(
                selectedChatModelId())
        lumoModelSelector.currentIndex =
            lumoModelSelector.indexOfValue(
                selectedChatModelId())
    }

    function selectChatDelivery(value) {
        let modelId = "on-device"
        if (value === "openai") {
            modelId = firstModelId("openai")
        } else if (value === "lumo") {
            modelId = firstModelId("lumo")
        }
        if (modelId.length === 0
                || !chooseChatModel(modelId)) {
            syncChatSelectors()
        }
    }

    function selectProviderModel(value) {
        if (!chooseChatModel(value)) {
            syncChatSelectors()
        }
    }

    function selectRouteMode(value) {
        if (routingModel === null
                || routingModel === undefined) {
            return
        }
        if (value === "processes"
                && routingModel.showProcesses !== undefined) {
            routingModel.showProcesses()
        } else if (value === "local-chat"
                   && routingModel.showLocalChat !== undefined) {
            routingModel.showLocalChat()
        }
        syncDefaultModeSelector()
    }

    function saveOpenAIKey() {
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel.saveOpenAIAPIKey === undefined) {
            return
        }
        if (settingsModel.saveOpenAIAPIKey(openAIKeyField.text)) {
            openAIKeyField.clear()
        }
    }

    function removeOpenAIKey() {
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel.removeOpenAIAPIKey === undefined) {
            return
        }
        if (settingsModel.removeOpenAIAPIKey()) {
            openAIKeyField.clear()
        }
    }

    function saveLumoKey() {
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel.saveLumoAPIKey === undefined) {
            return
        }
        if (settingsModel.saveLumoAPIKey(lumoKeyField.text)) {
            lumoKeyField.clear()
        }
    }

    function removeLumoKey() {
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel.removeLumoAPIKey === undefined) {
            return
        }
        if (settingsModel.removeLumoAPIKey()) {
            lumoKeyField.clear()
        }
    }

    onRoutingModelChanged: {
        Qt.callLater(syncDefaultModeSelector)
        Qt.callLater(syncChatSelectors)
    }

    Connections {
        target: root.routingModel
        ignoreUnknownSignals: true

        function onSelectedChatModelIdChanged() {
            root.syncChatSelectors()
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 8
        text: "Routing"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Default mode"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        ComboBox {
            id: defaultModeSelector
            property string interactionId:
                "settings.route.default"
            Accessible.name: "Default mode"
            textRole: "text"
            valueRole: "value"
            implicitHeight: 28
            implicitWidth: 168
            model: [
                { text: "Chat", value: "local-chat" },
                { text: "Codex", value: "processes" }
            ]
            Component.onCompleted:
                root.syncDefaultModeSelector()
            onActivated:
                root.selectRouteMode(currentValue)

            Connections {
                target: root.routingModel
                ignoreUnknownSignals: true

                function onRouteModeChanged() {
                    root.syncDefaultModeSelector()
                }
            }

            delegate: ItemDelegate {
                required property int index
                width: defaultModeSelector.width
                height: 28
                highlighted:
                    defaultModeSelector.highlightedIndex
                    === index

                contentItem: Label {
                    text: defaultModeSelector.textAt(
                        parent.index)
                    color: parent.highlighted
                        ? CompanionTheme.accentText
                        : CompanionTheme.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: 4
                    color: parent.highlighted
                        ? CompanionTheme.accent
                        : parent.hovered
                            ? CompanionTheme.controlHover
                            : CompanionTheme.surfaceRaised
                }
            }

            contentItem: Label {
                leftPadding: 10
                rightPadding: 28
                text: defaultModeSelector.displayText
                color: CompanionTheme.textPrimary
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            indicator: Canvas {
                x: defaultModeSelector.width - width - 10
                y: (defaultModeSelector.height - height) / 2
                width: 9
                height: 6
                contextType: "2d"

                onPaint: {
                    context.reset()
                    context.strokeStyle =
                        CompanionTheme.textSecondary
                    context.lineWidth = 1.5
                    context.lineCap = "round"
                    context.lineJoin = "round"
                    context.beginPath()
                    context.moveTo(1, 1)
                    context.lineTo(
                        width / 2,
                        height - 1)
                    context.lineTo(width - 1, 1)
                    context.stroke()
                }
            }

            background: Rectangle {
                radius: CompanionTheme.radius
                color: defaultModeSelector.down
                    ? CompanionTheme.controlPressed
                    : defaultModeSelector.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
                border.color:
                    defaultModeSelector.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
            }

            popup: Popup {
                y: defaultModeSelector.height + 4
                width: defaultModeSelector.width
                implicitHeight:
                    contentItem.implicitHeight + 2
                padding: 1

                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model:
                        defaultModeSelector.popup.visible
                        ? defaultModeSelector.delegateModel
                        : null
                    currentIndex:
                        defaultModeSelector.highlightedIndex
                }

                background: Rectangle {
                    radius: CompanionTheme.radius
                    color: CompanionTheme.surfaceRaised
                    border.color: CompanionTheme.border
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Chat delivery"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        SettingsSelector {
            id: chatDeliverySelector
            property string interactionId:
                "settings.chat.delivery"
            Accessible.name: "Chat delivery"
            model: [
                {
                    text: "On-device Windows model",
                    value: "on-device"
                },
                {
                    text: "OpenAI API",
                    value: "openai"
                },
                {
                    text: "Lumo API",
                    value: "lumo"
                }
            ]
            Component.onCompleted:
                root.syncChatSelectors()
            onActivated:
                root.selectChatDelivery(currentValue)
        }
    }

    Label {
        id: chatDeliveryDescription
        Layout.fillWidth: true
        Layout.bottomMargin: 12
        text: root.chatDeliveryDescriptionText()
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: CompanionTheme.separator
    }

    Label {
        id: openAISectionLabel
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        text: "OpenAI API"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
        visible:
            root.selectedChatDelivery() === "openai"
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12
        visible:
            root.selectedChatDelivery() === "openai"

        Label {
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Model"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        SettingsSelector {
            id: openAIModelSelector
            property string interactionId:
                "settings.openai.model"
            Accessible.name: "OpenAI model"
            model: root.modelsForGroup("openai")
            Component.onCompleted:
                root.syncChatSelectors()
            onActivated:
                root.selectProviderModel(
                    currentValue)
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        text: "API key"
        color: CompanionTheme.textPrimary
        font.pixelSize: 13
        visible:
            root.selectedChatDelivery() === "openai"
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        radius: CompanionTheme.radius
        color: openAIKeyField.activeFocus
            ? CompanionTheme.controlHover
            : CompanionTheme.control
        border.color: openAIKeyField.activeFocus
            ? CompanionTheme.accent
            : CompanionTheme.border
        visible:
            root.selectedChatDelivery() === "openai"

        TextInput {
            id: openAIKeyField
            property string accessibleName: "OpenAI API key"
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            echoMode: TextInput.Password
            color: CompanionTheme.textPrimary
            selectionColor: CompanionTheme.accent
            selectedTextColor: CompanionTheme.accentText
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: accessibleName
        }

        Label {
            anchors.fill: openAIKeyField
            text: "OpenAI API key"
            color: CompanionTheme.textMuted
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            visible: openAIKeyField.text.length === 0
                && !openAIKeyField.activeFocus
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 8
        visible:
            root.selectedChatDelivery() === "openai"

        Button {
            id: openAISaveButton
            property string interactionId:
                "settings.openai.key.save"
            implicitHeight: 30
            text: root.modelValue(
                "hasOpenAIAPIKey",
                false)
                ? "Replace Key"
                : "Save Key"
            enabled: openAIKeyField.text.trim().length > 0
            Accessible.name: text + " for OpenAI"
            onClicked: root.saveOpenAIKey()

            background: Rectangle {
                radius: CompanionTheme.radius
                color: openAISaveButton.down
                    ? CompanionTheme.accentPressed
                    : openAISaveButton.hovered
                        ? CompanionTheme.accentHover
                        : CompanionTheme.accent
                opacity: openAISaveButton.enabled ? 1 : 0.45
            }
            contentItem: Label {
                text: openAISaveButton.text
                color: CompanionTheme.accentText
                font.pixelSize: 12
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        Button {
            id: openAIRemoveButton
            property string interactionId:
                "settings.openai.key.remove"
            implicitHeight: 30
            text: "Remove Key"
            enabled: root.modelValue(
                "hasOpenAIAPIKey",
                false)
            Accessible.name: "Remove OpenAI API key"
            onClicked: root.removeOpenAIKey()

            background: Rectangle {
                radius: CompanionTheme.radius
                color: openAIRemoveButton.down
                    ? CompanionTheme.controlPressed
                    : openAIRemoveButton.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
                border.color: openAIRemoveButton.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
                opacity: openAIRemoveButton.enabled ? 1 : 0.45
            }
            contentItem: Label {
                text: openAIRemoveButton.text
                color: CompanionTheme.textPrimary
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 7
        Layout.bottomMargin: 14
        text: root.modelValue(
            "openAIAPIKeyStatus",
            "No OpenAI API key saved.")
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
        visible:
            root.selectedChatDelivery() === "openai"
    }

    Label {
        id: lumoSectionLabel
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        text: "Lumo API"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
        visible:
            root.selectedChatDelivery() === "lumo"
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12
        visible:
            root.selectedChatDelivery() === "lumo"

        Label {
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Model"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        SettingsSelector {
            id: lumoModelSelector
            property string interactionId:
                "settings.lumo.model"
            Accessible.name: "Lumo model"
            model: root.modelsForGroup("lumo")
            Component.onCompleted:
                root.syncChatSelectors()
            onActivated:
                root.selectProviderModel(
                    currentValue)
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        text: "API key"
        color: CompanionTheme.textPrimary
        font.pixelSize: 13
        visible:
            root.selectedChatDelivery() === "lumo"
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        radius: CompanionTheme.radius
        color: lumoKeyField.activeFocus
            ? CompanionTheme.controlHover
            : CompanionTheme.control
        border.color: lumoKeyField.activeFocus
            ? CompanionTheme.accent
            : CompanionTheme.border
        visible:
            root.selectedChatDelivery() === "lumo"

        TextInput {
            id: lumoKeyField
            property string accessibleName: "Lumo API key"
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            echoMode: TextInput.Password
            color: CompanionTheme.textPrimary
            selectionColor: CompanionTheme.accent
            selectedTextColor: CompanionTheme.accentText
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: accessibleName
        }

        Label {
            anchors.fill: lumoKeyField
            text: "Lumo API key"
            color: CompanionTheme.textMuted
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            visible: lumoKeyField.text.length === 0
                && !lumoKeyField.activeFocus
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 8
        visible:
            root.selectedChatDelivery() === "lumo"

        Button {
            id: lumoSaveButton
            property string interactionId:
                "settings.lumo.key.save"
            implicitHeight: 30
            text: root.modelValue(
                "hasLumoAPIKey",
                false)
                ? "Replace Key"
                : "Save Key"
            enabled: lumoKeyField.text.trim().length > 0
            Accessible.name: text + " for Lumo"
            onClicked: root.saveLumoKey()

            background: Rectangle {
                radius: CompanionTheme.radius
                color: lumoSaveButton.down
                    ? CompanionTheme.accentPressed
                    : lumoSaveButton.hovered
                        ? CompanionTheme.accentHover
                        : CompanionTheme.accent
                opacity: lumoSaveButton.enabled ? 1 : 0.45
            }
            contentItem: Label {
                text: lumoSaveButton.text
                color: CompanionTheme.accentText
                font.pixelSize: 12
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        Button {
            id: lumoRemoveButton
            property string interactionId:
                "settings.lumo.key.remove"
            implicitHeight: 30
            text: "Remove Key"
            enabled: root.modelValue(
                "hasLumoAPIKey",
                false)
            Accessible.name: "Remove Lumo API key"
            onClicked: root.removeLumoKey()

            background: Rectangle {
                radius: CompanionTheme.radius
                color: lumoRemoveButton.down
                    ? CompanionTheme.controlPressed
                    : lumoRemoveButton.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
                border.color: lumoRemoveButton.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
                opacity: lumoRemoveButton.enabled ? 1 : 0.45
            }
            contentItem: Label {
                text: lumoRemoveButton.text
                color: CompanionTheme.textPrimary
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 7
        Layout.bottomMargin: 4
        text: root.modelValue(
            "lumoAPIKeyStatus",
            "No Lumo API key saved.")
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
        visible:
            root.selectedChatDelivery() === "lumo"
    }

    Item {
        Layout.fillHeight: true
        Layout.fillWidth: true
    }
}
