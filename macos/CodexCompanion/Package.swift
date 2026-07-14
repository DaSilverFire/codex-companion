// swift-tools-version: 5.10

import Foundation
import PackageDescription

var targets: [Target] = [
    .executableTarget(name: "CodexCompanion")
]

if FileManager.default.fileExists(atPath: "Tests/CodexCompanionTests") {
    targets.append(
        .testTarget(
            name: "CodexCompanionTests",
            dependencies: ["CodexCompanion"],
            path: "Tests/CodexCompanionTests"
        )
    )
}

let package = Package(
    name: "CodexCompanion",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "CodexCompanion", targets: ["CodexCompanion"])
    ],
    targets: targets
)
