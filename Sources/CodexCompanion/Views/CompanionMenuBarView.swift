import SwiftUI

struct CompanionMenuBarView: View {
    @ObservedObject var model: CompanionAppModel

    var body: some View {
        Button {
            model.isPetVisible ? model.hidePet() : model.showPet()
        } label: {
            Label(model.isPetVisible ? "Hide Pet" : "Show Pet", systemImage: "pawprint")
        }

        Button {
            model.isQuickBarOpen ? model.hideQuickBar() : model.showQuickBar()
        } label: {
            Label(
                model.isQuickBarOpen ? "Hide Companion Menu" : "Show Companion Menu",
                systemImage: model.isQuickBarOpen ? "chevron.down.circle" : "chevron.up.circle"
            )
        }

        Divider()

        Button {
            model.showCodexProcesses()
        } label: {
            Label("Codex Processes", systemImage: "terminal")
        }

        Button {
            model.showChatGPT()
        } label: {
            Label("Local Chat", systemImage: "bubble.left.and.bubble.right")
        }

        SettingsLink {
            Label("Settings", systemImage: "gearshape")
        }

        Divider()

        Button {
            NSApp.terminate(nil)
        } label: {
            Label("Quit Codex Companion", systemImage: "power")
        }
    }
}
