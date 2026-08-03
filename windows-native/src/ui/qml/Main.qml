import QtQml
import CodexCompanion

QtObject {
    property var petWindow: PetWindow {
        visible: false
        petModel: typeof petViewModel === "undefined"
            ? null
            : petViewModel
        shellModel: typeof companionShell === "undefined"
            ? null
            : companionShell
        reactionModel:
            typeof petProcessReactions === "undefined"
            ? null
            : petProcessReactions
        movementController:
            typeof petWindowController === "undefined"
            ? null
            : petWindowController
    }

    property var settingsWindow: SettingsWindow {
        objectName: "settingsWindow"
        visible: false
        settingsModel: typeof settingsViewModel === "undefined"
            ? null
            : settingsViewModel
        routingModel: typeof companionShell === "undefined"
            ? null
            : companionShell
        petModel: typeof petViewModel === "undefined"
            ? null
            : petViewModel
        updateModel: typeof updateViewModel === "undefined"
            ? null
            : updateViewModel
    }

    property var companionMenuWindow: CompanionMenuWindow {
        visible: false
        shellModel: typeof companionShell === "undefined"
            ? null
            : companionShell
        attentionModel:
            typeof petProcessReactions === "undefined"
            ? null
            : petProcessReactions
        settingsModel: typeof settingsViewModel === "undefined"
            ? null
            : settingsViewModel
        backdropState: typeof windowBackdropState === "undefined"
            ? null
            : windowBackdropState
        placementController:
            typeof petWindowController === "undefined"
            ? null
            : petWindowController
    }

    property var usageWindow: UsageWindow {
        visible: false
        shellModel: typeof companionShell === "undefined"
            ? null
            : companionShell
        settingsModel: typeof settingsViewModel === "undefined"
            ? null
            : settingsViewModel
        backdropState: typeof windowBackdropState === "undefined"
            ? null
            : windowBackdropState
    }

    property var attentionWindow: AttentionWindow {
        visible: false
        attentionModel:
            typeof petProcessReactions === "undefined"
            ? null
            : petProcessReactions
        settingsModel: typeof settingsViewModel === "undefined"
            ? null
            : settingsViewModel
        backdropState: typeof windowBackdropState === "undefined"
            ? null
            : windowBackdropState
    }
}
