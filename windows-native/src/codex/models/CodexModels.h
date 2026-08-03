#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

namespace companion {

struct CodexThreadRecord final {
    QString id;
    QString title;
    QString workingDirectory;
    QString firstUserMessage;
    QString rolloutPath;
    QString preview;
    std::optional<QString> model;
    std::optional<QString> reasoningEffort;
    QDateTime updatedAt;
    QDateTime recencyAt;

    friend bool operator==(
        const CodexThreadRecord&,
        const CodexThreadRecord&) = default;
};

struct CodexJobRecord final {
    QString id;
    QString name;
    QString status;
    QString instruction;
    std::optional<QString> error;
    std::optional<QString> threadId;
    QDateTime updatedAt;
    std::optional<QDateTime> startedAt;

    friend bool operator==(
        const CodexJobRecord&,
        const CodexJobRecord&) = default;
};

struct CodexStateSnapshot final {
    QVector<CodexThreadRecord> threads;
    QVector<CodexJobRecord> jobs;

    friend bool operator==(
        const CodexStateSnapshot&,
        const CodexStateSnapshot&) = default;
};

enum class LifecycleState {
    Active,
    Completed,
    Failed,
};

struct TaskLifecycle final {
    LifecycleState state = LifecycleState::Completed;
    std::optional<QString> turnId;

    bool isActive() const noexcept
    {
        return state == LifecycleState::Active;
    }

    friend bool operator==(
        const TaskLifecycle&,
        const TaskLifecycle&) = default;
};

enum class CodexMessageRole {
    User,
    Assistant,
};

struct RolloutMessage final {
    CodexMessageRole role = CodexMessageRole::User;
    QString text;
    std::optional<QString> turnId;
    std::optional<QDateTime> createdAt;

    friend bool operator==(
        const RolloutMessage&,
        const RolloutMessage&) = default;
};

struct RolloutSnapshot final {
    std::optional<RolloutMessage> latestUserMessage;
    std::optional<RolloutMessage> latestAssistantMessage;
    std::optional<TaskLifecycle> lifecycle;

    friend bool operator==(
        const RolloutSnapshot&,
        const RolloutSnapshot&) = default;
};

} // namespace companion
