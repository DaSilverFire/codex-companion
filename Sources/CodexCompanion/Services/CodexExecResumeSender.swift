import AppKit
import Foundation

final class CodexExecResumeSender {
    func submit(prompt: String, threadID: String, cwd: String?) -> Bool {
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedPrompt.isEmpty, !trimmedThreadID.isEmpty else { return false }

        guard let executableURL = Self.runningCodexExecutableURL() ?? WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        }) else {
            Self.log("missing codex executable")
            return false
        }

        let session = CodexExecResumeSession(
            executableURL: executableURL,
            prompt: trimmedPrompt,
            threadID: trimmedThreadID,
            cwd: cwd?.trimmingCharacters(in: .whitespacesAndNewlines).nilIfEmpty
        )
        return session.start()
    }

    private static func runningCodexExecutableURL() -> URL? {
        NSRunningApplication.runningApplications(withBundleIdentifier: "com.openai.codex")
            .compactMap { app -> URL? in
                guard let bundleURL = app.bundleURL else { return nil }
                let executableURL = bundleURL.appendingPathComponent("Contents/Resources/codex")
                guard FileManager.default.isExecutableFile(atPath: executableURL.path) else { return nil }
                return executableURL
            }
            .first
    }

    private static func log(_ message: String) {
        NSLog("CodexExecResumeSender: %@", message)
    }
}

private final class CodexExecResumeSession {
    private let executableURL: URL
    private let prompt: String
    private let threadID: String
    private let cwd: String?
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let lock = NSLock()
    private let readySemaphore = DispatchSemaphore(value: 0)
    private let logURL: URL
    private var stdoutBuffer = Data()
    private var hasFinishedStartup = false
    private var didAcceptSend = false

    init(executableURL: URL, prompt: String, threadID: String, cwd: String?) {
        self.executableURL = executableURL
        self.prompt = prompt
        self.threadID = threadID
        self.cwd = cwd

        let directory = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Logs", isDirectory: true)
            .appendingPathComponent("CodexCompanion", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        logURL = directory.appendingPathComponent("resume-\(UUID().uuidString).jsonl")
    }

    func start() -> Bool {
        process.executableURL = executableURL
        process.arguments = [
            "exec",
            "resume",
            "--json",
            "--skip-git-repo-check",
            "--all",
            threadID,
            "-",
        ]

        if let cwdURL = workingDirectoryURL {
            process.currentDirectoryURL = cwdURL
        }

        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.readStdout(handle.availableData)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.readStderr(handle.availableData)
        }
        process.terminationHandler = { [weak self] _ in
            self?.finishStartup(success: false)
            self?.cleanup()
        }

        appendLog("launch \(executableURL.path) \(process.arguments?.joined(separator: " ") ?? "")\n")

        do {
            try process.run()
        } catch {
            appendLog("launch failed: \(error.localizedDescription)\n")
            cleanup()
            return false
        }

        CodexExecResumeSessionRegistry.shared.retain(self)
        writePromptAndCloseStdin()

        let result = readySemaphore.wait(timeout: .now() + .seconds(3))
        if result == .timedOut, process.isRunning {
            appendLog("startup wait timed out; accepting running resume process\n")
            finishStartup(success: true)
        }

        return didAcceptSend
    }

    private var workingDirectoryURL: URL? {
        guard let cwd else { return nil }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: cwd, isDirectory: &isDirectory), isDirectory.boolValue else {
            return nil
        }
        return URL(fileURLWithPath: cwd, isDirectory: true)
    }

    private func writePromptAndCloseStdin() {
        guard let data = "\(prompt)\n".data(using: .utf8) else { return }
        try? stdinPipe.fileHandleForWriting.write(contentsOf: data)
        try? stdinPipe.fileHandleForWriting.close()
    }

    private func readStdout(_ data: Data) {
        guard !data.isEmpty else { return }
        appendLog(data)
        stdoutBuffer.append(data)
        let newline = Data([0x0A])

        while let range = stdoutBuffer.firstRange(of: newline) {
            let lineData = stdoutBuffer.subdata(in: stdoutBuffer.startIndex..<range.lowerBound)
            stdoutBuffer.removeSubrange(stdoutBuffer.startIndex..<range.upperBound)
            guard !lineData.isEmpty else { continue }
            handleLine(lineData)
        }
    }

    private func readStderr(_ data: Data) {
        guard !data.isEmpty else { return }
        appendLog(data)
    }

    private func handleLine(_ data: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: data),
            let message = object as? [String: Any]
        else {
            return
        }

        if message["error"] != nil {
            finishStartup(success: false)
            terminate()
            return
        }

        guard let type = message["type"] as? String else { return }
        if type == "error" || type.hasSuffix(".error") {
            finishStartup(success: false)
            terminate()
            return
        }

        if type == "thread.started"
            || type == "turn.started"
            || type == "turn.completed"
            || type == "agent_message" {
            finishStartup(success: true)
        }
    }

    private func finishStartup(success: Bool) {
        lock.lock()
        defer { lock.unlock() }
        guard !hasFinishedStartup else { return }
        hasFinishedStartup = true
        didAcceptSend = success
        readySemaphore.signal()
    }

    private func terminate() {
        cleanup()
        if process.isRunning {
            process.terminate()
        }
    }

    private func cleanup() {
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        CodexExecResumeSessionRegistry.shared.release(self)
    }

    private func appendLog(_ data: Data) {
        if FileManager.default.fileExists(atPath: logURL.path),
           let handle = try? FileHandle(forWritingTo: logURL) {
            defer {
                try? handle.close()
            }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
            return
        }

        try? data.write(to: logURL, options: .atomic)
    }

    private func appendLog(_ text: String) {
        guard let data = text.data(using: .utf8) else { return }
        appendLog(data)
    }
}

private final class CodexExecResumeSessionRegistry {
    static let shared = CodexExecResumeSessionRegistry()

    private let lock = NSLock()
    private var sessions: [ObjectIdentifier: CodexExecResumeSession] = [:]

    func retain(_ session: CodexExecResumeSession) {
        lock.lock()
        sessions[ObjectIdentifier(session)] = session
        lock.unlock()
    }

    func release(_ session: CodexExecResumeSession) {
        lock.lock()
        sessions.removeValue(forKey: ObjectIdentifier(session))
        lock.unlock()
    }
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }
}
