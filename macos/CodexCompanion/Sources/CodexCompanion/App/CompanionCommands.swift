import SwiftUI

struct CompanionCommands: Commands {
    @ObservedObject var model: CompanionAppModel
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("New Handoff...") {
                openWindow(id: "handoff")
            }
            .keyboardShortcut("n")
        }

        CommandMenu("Companion") {
            Button("Show Handoff") {
                openWindow(id: "handoff")
            }
            .keyboardShortcut("h", modifiers: [.command, .shift])

            Button("Show Rate Limits") {
                model.rateLimitStore.refresh()
                openWindow(id: "rate-limits")
            }
            .keyboardShortcut("l", modifiers: [.command, .shift])

            Button("Refresh Rate Limits") {
                model.rateLimitStore.refresh()
            }

            Text(model.rateLimitStore.menuSummary)

            Divider()

            Button("Open Codex") {
                model.continueCodex()
            }

            Button("Local Chat") {
                model.showChatGPT()
            }

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
            .keyboardShortcut("p", modifiers: [.command, .option])
        }

        CommandMenu("Pet") {
            Menu("Choose Pet") {
                ForEach(model.petStore.pets) { pet in
                    Button(pet.displayName) {
                        model.petStore.selectedPetID = pet.id
                    }
                }
            }

            Menu("Animation") {
                ForEach(PetAnimationState.allCases) { state in
                    Button(state.title) {
                        model.setAnimation(state)
                    }
                }
            }

            Button("Reload Pets") {
                model.petStore.reload()
            }

            Divider()

            Button("Slower Animations") {
                model.animationSpeedScale = min(2.5, model.animationSpeedScale + 0.15)
            }

            Button("Faster Animations") {
                model.animationSpeedScale = max(0.75, model.animationSpeedScale - 0.15)
            }
        }
    }
}
