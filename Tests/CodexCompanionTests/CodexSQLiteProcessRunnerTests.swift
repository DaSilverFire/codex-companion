import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexSQLiteProcessRunnerTests {
    @Test
    func readArgumentsUseReadonlyModeAndWaitForTransientWriterLocks() {
        let databaseURL = URL(fileURLWithPath: "/tmp/state.sqlite")

        let arguments = CodexSQLiteProcessRunner.readArguments(
            databaseURL: databaseURL,
            query: "select id from threads;",
            columnSeparator: "\u{1f}",
            rowSeparator: "\u{1e}"
        )

        #expect(arguments == [
            "-readonly",
            "-cmd", ".timeout 3000",
            "-separator", "\u{1f}",
            "-newline", "\u{1e}",
            databaseURL.path,
            "select id from threads;",
        ])
    }

    @Test
    func drainsLargeStandardOutputAndErrorWithoutDeadlocking() throws {
        let command = """
        dd if=/dev/zero bs=1048576 count=1 2>/dev/null &
        dd if=/dev/zero bs=1048576 count=1 1>&2 2>/dev/null &
        wait
        """

        let result = try CodexSQLiteProcessRunner.run(
            executableURL: URL(fileURLWithPath: "/bin/sh"),
            arguments: ["-c", command]
        )

        #expect(result.terminationStatus == 0)
        #expect(result.standardOutput.count == 1_048_576)
        #expect(result.standardError.count == 1_048_576)
    }

    @Test
    func transientReadLocksAreRetriedBeforeReturningSuccess() throws {
        var attempts = 0
        var delays: [TimeInterval] = []

        let result = CodexSQLiteProcessRunner.retryingTransientRead(
            retryDelays: [0.1, 0.25],
            sleep: { delays.append($0) }
        ) {
            attempts += 1
            if attempts < 3 {
                return CodexSQLiteProcessResult(
                    terminationStatus: 5,
                    standardOutput: Data(),
                    standardError: Data(
                        "Parse error in command line argument: database is locked (5)"
                            .utf8
                    )
                )
            }
            return CodexSQLiteProcessResult(
                terminationStatus: 0,
                standardOutput: Data("ready".utf8),
                standardError: Data()
            )
        }

        #expect(attempts == 3)
        #expect(delays == [0.1, 0.25])
        #expect(result.terminationStatus == 0)
        #expect(String(decoding: result.standardOutput, as: UTF8.self) == "ready")
    }

    @Test
    func permanentReadFailuresAreNotRetried() throws {
        var attempts = 0

        let result = CodexSQLiteProcessRunner.retryingTransientRead(
            retryDelays: [0.1, 0.25],
            sleep: { _ in Issue.record("A permanent failure must not sleep") }
        ) {
            attempts += 1
            return CodexSQLiteProcessResult(
                terminationStatus: 1,
                standardOutput: Data(),
                standardError: Data("no such table: threads".utf8)
            )
        }

        #expect(attempts == 1)
        #expect(result.terminationStatus == 1)
    }
}
