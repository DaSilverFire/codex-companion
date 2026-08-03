#pragma once

#include "codex/appserver/TaskCreator.h"
#include "codex/attachments/AttachmentStore.h"
#include "codex/chat/ChatService.h"
#include "codex/chat/WindowsOnDeviceChatBackend.h"
#include "codex/commands/ApprovalService.h"
#include "codex/commands/GoalService.h"
#include "codex/commands/TaskCommandService.h"
#include "codex/commands/UsageService.h"
#include "codex/runtime/CodexRuntimeLifetime.h"
#include "core/CredentialStore.h"

#include <functional>
#include <memory>

namespace companion {

using RuntimeTaskCreatePerformer =
    std::function<Result<QString>(
        const CreateTaskRequest&)>;

struct CodexRuntimeProductionServices final {
    std::shared_ptr<AttachmentStore>
        attachmentStore;
    std::shared_ptr<TaskCommandService>
        taskCommandService;
    std::shared_ptr<ApprovalService>
        approvalService;
    std::shared_ptr<TaskCreator> taskCreator;
    RuntimeTaskCreatePerformer
        taskCreatePerformer;
    std::shared_ptr<CredentialStore>
        credentialStore;
    std::shared_ptr<ChatHttpTransport>
        openAITransport;
    std::shared_ptr<ChatHttpTransport>
        lumoTransport;
    std::shared_ptr<
        WindowsOnDeviceChatBackend>
        onDeviceBackend;
    std::shared_ptr<GoalService> goalService;
    std::shared_ptr<UsageService> usageService;
};

struct CodexRuntimeProductionBundle final {
    CodexRuntimeDependencies dependencies;
    std::shared_ptr<
        WindowsOnDeviceChatBackend>
        onDeviceBackend;
};

class CodexRuntimeDependencyFactory final {
public:
    static Result<
        CodexRuntimeProductionBundle>
    build(
        const CodexEnvironment& environment,
        CodexRuntimeProductionServices services,
        RuntimeExecutor executor = {},
        RuntimeNowProvider nowProvider = {});

    static Result<
        std::unique_ptr<CodexRuntimeLifetime>>
    create(
        const CodexEnvironment& environment,
        CodexRuntimeProductionServices services,
        RuntimeExecutor executor = {},
        RuntimeNowProvider nowProvider = {},
        CodexRuntimeCadence cadence = {});
};

} // namespace companion
