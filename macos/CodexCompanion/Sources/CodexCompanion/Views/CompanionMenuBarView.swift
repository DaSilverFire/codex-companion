import SwiftUI

struct CompanionMenuBarView: View {
    @ObservedObject var model: CompanionAppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Button(model.isPetVisible ? "Hide Pet" : "Show Pet") {
            if model.isPetVisible {
                model.hidePet()
            } else {
                model.showPet()
            }
        }

        Button(model.isQuickBarOpen ? "Hide Pet Quick Bar" : "Show Pet Quick Bar") {
            if model.isQuickBarOpen {
                model.hideQuickBar()
            } else {
                model.showQuickBar()
            }
        }

        Button("Handoff...") {
            openWindow(id: "handoff")
        }

        Button("Codex Rate Limits...") {
            model.rateLimitStore.refresh()
            openWindow(id: "rate-limits")
        }

        Button("Refresh Rate Limits") {
            model.rateLimitStore.refresh()
        }

        Text(model.rateLimitStore.menuSummary)

        Button("Refresh Goals") {
            model.processStore.refreshGoals()
        }

        Divider()

        Button("Open Codex") {
            model.continueCodex()
        }

        Button("Local Chat") {
            model.showChatGPT()
        }

        Divider()

        Menu("Pet") {
            ForEach(model.petStore.pets) { pet in
                Button(pet.displayName) {
                    model.petStore.selectedPetID = pet.id
                }
            }

            Divider()

            Button("Reload Pets") {
                model.petStore.reload()
            }
        }

        Menu("Animation") {
            ForEach(PetAnimationState.allCases) { state in
                Button(state.title) {
                    model.setAnimation(state)
                }
            }
        }

        SettingsLink {
            Label("Settings", systemImage: "gearshape")
        }

        Divider()

        Button("Quit Codex Companion") {
            NSApp.terminate(nil)
        }
    }
}
