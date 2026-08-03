#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "codex/attachments/AttachmentStore.h"
#include "codex/discovery/CodexEnvironment.h"
#include "core/Result.h"

#include <QProcessEnvironment>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace companion {

struct CreateTaskRequest final {
    QString prompt;
    QString cwd;
    QString model;
    QString reasoningEffort;
    QString skillName;
    QString skillPath;
    QVector<StagedAttachment> attachments;
    QString clientMessageId;

    friend bool operator==(
        const CreateTaskRequest&,
        const CreateTaskRequest&) = default;
};

struct PendingAppServerRequest final {
    quint64 hostId = 0;
    QJsonValue requestId;
    QString method;
    QJsonObject params;

    friend bool operator==(
        const PendingAppServerRequest&,
        const PendingAppServerRequest&) = default;
};

using SharedDaemonAvailabilityProbe =
    std::function<bool()>;

class TaskCreator final {
public:
    static constexpr int kDefaultTimeoutMilliseconds = 12'000;

    explicit TaskCreator(
        const CodexEnvironment& environment,
        QProcessEnvironment processEnvironment =
            QProcessEnvironment::systemEnvironment(),
        int timeoutMilliseconds =
            kDefaultTimeoutMilliseconds);

    explicit TaskCreator(
        CodexExecutableCandidateProvider candidateProvider,
        SharedDaemonAvailabilityProbe sharedDaemonProbe,
        QProcessEnvironment processEnvironment =
            QProcessEnvironment::systemEnvironment(),
        int timeoutMilliseconds =
            kDefaultTimeoutMilliseconds);

    ~TaskCreator();

    TaskCreator(const TaskCreator&) = delete;
    TaskCreator& operator=(const TaskCreator&) = delete;
    TaskCreator(TaskCreator&&) = delete;
    TaskCreator& operator=(TaskCreator&&) = delete;

    Result<QString> create(
        const CreateTaskRequest& request) const;
    Result<QVector<PendingAppServerRequest>>
    takePendingServerRequests() const;
    Result<void> respondToServerRequest(
        quint64 hostId,
        const QJsonValue& requestId,
        const QJsonValue& result) const;

private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace companion
