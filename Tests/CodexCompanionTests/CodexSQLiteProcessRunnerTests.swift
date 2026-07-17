import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexSQLiteProcessRunnerTests {
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
}
