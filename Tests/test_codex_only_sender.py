#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "Sources" / "CodexCompanion" / "Stores" / "CompanionAppModel.swift"
APP_SERVER = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexAppServerSender.swift"
FOLLOWER_IPC = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexFollowerIPCTransport.swift"
SEND_ACTION = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexSendAction.swift"
APPROVAL_SENDER = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexAppServerApprovalSender.swift"
MOBILE_BRIDGE = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexCompanionMobileBridgeServer.swift"
COMMANDS = ROOT / "Sources" / "CodexCompanion" / "App" / "CompanionCommands.swift"
MENU_BAR = ROOT / "Sources" / "CodexCompanion" / "Views" / "CompanionMenuBarView.swift"
CONTENT_VIEW = ROOT / "Sources" / "CodexCompanion" / "Views" / "ContentView.swift"
LEGACY_PROMPT_ROUTER = ROOT / "Sources" / "CodexCompanion" / "Services" / "PromptRouter.swift"
LEGACY_VISIBLE_SENDER = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexVisibleReplySender.swift"
LEGACY_VISIBLE_APPROVAL_SENDER = ROOT / "Sources" / "CodexCompanion" / "Services" / "CodexVisibleApprovalSender.swift"


def function_body(source: str, name: str) -> str:
    marker = f"private func {name}"
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"Could not parse function body for {name}")


def test_codex_target_sends_use_direct_app_server_not_visible_ui():
    source = MODEL.read_text()
    entry_body = function_body(source, "sendCodexPromptAsync")
    direct_body = function_body(source, "startDirectCodexSend")
    approval_body = function_body(source, "startApprovalFeedbackSend")

    assert "startDirectCodexSend(" in entry_body
    assert "startApprovalFeedbackSend(" in entry_body
    assert "Task(priority:" in direct_body
    assert "sendTimeout" in direct_body
    assert "expectedTurnID" in direct_body
    assert "await submitter(" in direct_body
    assert "await approvalSubmitter(threadID, .decline)" in approval_body
    assert "startDirectCodexSend(" in approval_body
    assert "CodexVisibleReplySender().submit" not in entry_body + direct_body + approval_body
    assert "open codex://threads" not in entry_body + direct_body + approval_body


def test_app_server_sender_uses_chatgpt_follower_ipc_without_a_second_app_server():
    source = APP_SERVER.read_text()
    ipc_source = FOLLOWER_IPC.read_text()

    submit_start = source.index("    func submit(")
    submit_end = source.index("\n    static var sharedDaemonSocketURL", submit_start)
    submit_body = source[submit_start:submit_end]

    assert "CodexFollowerIPCTransport().submit" in source
    assert "CodexFollowerIPCTransport().queueReply" in source
    assert "CodexBackgroundThreadLoader" not in source
    assert "threadLoader" not in submit_body
    assert "codex://threads" not in source
    assert "NSWorkspace.shared.open" not in source
    assert "sharedDaemonSocketURL" not in submit_body
    assert "Process()" not in submit_body
    assert "app-server" not in submit_body
    assert "CodexVisibleReplySender" not in submit_body
    assert '"thread-follower-start-turn"' in ipc_source
    assert '"thread-follower-steer-turn"' in ipc_source
    assert '"thread-follower-set-queued-follow-ups-state"' in ipc_source
    assert '"initializing-client"' in ipc_source
    assert '"codex-companion"' in ipc_source
    assert 'appendingPathComponent(".codex"' in ipc_source
    assert 'appendingPathComponent("ipc.sock")' in ipc_source
    assert 'appendingPathComponent("codex-ipc"' in ipc_source
    assert "for socketURL in CodexFollowerIPCProtocol.socketURLs" in ipc_source
    assert "maximumParsedFrameBytes" in ipc_source
    assert "drain(byteCount:" in ipc_source
    assert "Restart ChatGPT" not in ipc_source


def test_send_action_contract_is_transport_neutral():
    source = SEND_ACTION.read_text()

    assert "enum CodexSendAction" in source
    assert "case reply" in source
    assert "case steer" in source
    assert "import AppKit" not in source
    assert "ApplicationServices" not in source
    assert "NSWorkspace" not in source
    assert "AXIsProcessTrusted" not in source
    assert "CGEvent" not in source


def test_approval_sender_uses_native_follower_ipc_without_accessibility():
    source = APPROVAL_SENDER.read_text()
    respond_start = source.index("    func respond(")
    respond_end = source.index("\n    func approve(", respond_start)
    respond_body = source[respond_start:respond_end]

    assert "CodexFollowerIPCTransport().respond" in respond_body
    assert "CodexVisibleApprovalSender" not in respond_body
    assert "respondThroughSharedDaemon" not in respond_body
    assert "sharedDaemonSocketURL" not in respond_body


def test_active_companion_surfaces_do_not_request_accessibility_for_native_transport():
    model_source = MODEL.read_text()
    bridge_source = MOBILE_BRIDGE.read_text()
    notice_start = model_source.index("    var shouldShowCodexAccessibilityNotice")
    notice_end = model_source.index("\n    var shouldShowChatGPTModelPicker", notice_start)
    approval_start = model_source.index("    private func submitApproval(")
    approval_end = model_source.index("\n    func clearProcessTarget", approval_start)

    assert "return false" in model_source[notice_start:notice_end]
    assert "Accessibility" not in model_source[approval_start:approval_end]
    assert 'code: "accessibility_required"' not in bridge_source
    assert "Mac Accessibility settings" not in bridge_source


def test_desktop_codex_open_paths_stay_inside_companion_without_accessibility():
    model_source = MODEL.read_text()
    surface_sources = "\n".join(
        path.read_text()
        for path in (COMMANDS, MENU_BAR, CONTENT_VIEW)
    )

    assert "CodexVisibleReplySender" not in model_source
    assert "private let router = PromptRouter()" not in model_source
    assert "router.route(" not in model_source
    assert "router.openCodexThread" not in model_source
    assert "router.continueCodex" not in model_source
    assert "model.continueCodex()" not in surface_sources
    assert surface_sources.count("model.showCodexProcesses()") >= 3
    assert not LEGACY_PROMPT_ROUTER.exists()
    assert not LEGACY_VISIBLE_SENDER.exists()
    assert not LEGACY_VISIBLE_APPROVAL_SENDER.exists()


if __name__ == "__main__":
    test_codex_target_sends_use_direct_app_server_not_visible_ui()
    test_app_server_sender_uses_chatgpt_follower_ipc_without_a_second_app_server()
    test_send_action_contract_is_transport_neutral()
    test_approval_sender_uses_native_follower_ipc_without_accessibility()
    test_active_companion_surfaces_do_not_request_accessibility_for_native_transport()
    test_desktop_codex_open_paths_stay_inside_companion_without_accessibility()
    print("codex-only sender regression passed")
