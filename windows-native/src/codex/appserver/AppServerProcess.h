#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "core/Result.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <memory>
#include <optional>
#include <stop_token>

namespace companion::appserver_detail {

enum class Transport {
    StdioJsonLines,
    ProxyWebSocket,
};

class ProcessSessionTestAccess;

class ReapableProcess final : public QProcess {
public:
    using QProcess::QProcess;

    void releaseToNativeReaper()
    {
        disconnect();
        closeWriteChannel();
        closeReadChannel(QProcess::StandardOutput);
        closeReadChannel(QProcess::StandardError);
        setProcessState(QProcess::NotRunning);
    }
};

class ProcessSession final {
public:
    static Result<std::unique_ptr<ProcessSession>> start(
        const QVector<QString>& executableCandidates,
        const QStringList& arguments,
        const QProcessEnvironment& environment,
        Transport transport,
        int timeoutMilliseconds,
        std::stop_token stopToken = {});

    ~ProcessSession();

    ProcessSession(const ProcessSession&) = delete;
    ProcessSession& operator=(const ProcessSession&) = delete;

    Result<void> send(const QJsonObject& message);
    Result<RpcResponse> response(int expectedId);
    Result<void> drainAvailable();
    Result<QVector<QJsonObject>>
    pendingServerRequests();
    void consumeServerRequests(qsizetype count);
    Result<void> sendResponse(
        const QJsonValue& id,
        const QJsonValue& result);
    void resetDeadline(int timeoutMilliseconds);
    bool isRunning() const;
    void close();

private:
    friend class ProcessSessionTestAccess;

    static constexpr qsizetype
        kMaximumStderrBytes = 1024;
    static constexpr qsizetype
        kMaximumStderrDrainBytesPerCall =
            64 * 1024;

    explicit ProcessSession(
        QProcessEnvironment environment,
        Transport transport,
        int timeoutMilliseconds,
        std::stop_token stopToken);

    template <
        typename StateProvider,
        typename WaitForStarted>
    Result<bool> waitForStartup(
        StateProvider&& stateProvider,
        WaitForStarted&& waitForStarted)
    {
        while (true) {
            const Result<void> notCanceled =
                checkCancellation();
            if (!notCanceled.hasValue()) {
                return Result<bool>::failure(
                    notCanceled.error());
            }
            if (stateProvider()
                == QProcess::Running) {
                return Result<bool>::success(true);
            }
            const int waitMilliseconds =
                waitSliceMilliseconds();
            if (waitMilliseconds <= 0) {
                return Result<bool>::failure(
                    timedOutError());
            }
            if (waitForStarted(waitMilliseconds)
                || stateProvider()
                    == QProcess::Running) {
                return Result<bool>::success(true);
            }
            if (stateProvider()
                == QProcess::NotRunning) {
                return Result<bool>::success(false);
            }
        }
    }

    template <
        typename BytesAvailable,
        typename ReadBytes>
    void drainStandardErrorSource(
        BytesAvailable&& bytesAvailable,
        ReadBytes&& readBytes)
    {
        qsizetype remaining =
            kMaximumStderrDrainBytesPerCall;
        while (remaining > 0
               && bytesAvailable() > 0) {
            const qint64 requested =
                static_cast<qint64>(remaining);
            const QByteArray available =
                readBytes(requested);
            if (available.isEmpty()) {
                break;
            }
            if (stderrBuffer_.size()
                < kMaximumStderrBytes) {
                const qsizetype retained =
                    std::min(
                        kMaximumStderrBytes
                            - stderrBuffer_.size(),
                        available.size());
                stderrBuffer_.append(
                    available.constData(),
                    retained);
            }
            remaining -= std::min(
                remaining,
                available.size());
        }
    }

    Result<void> launch(
        const QVector<QString>& executableCandidates,
        const QStringList& arguments);
    Result<void> upgradeWebSocket();
    Result<QJsonObject> nextMessage();
    Result<QJsonObject> nextStdioMessage();
    Result<QJsonObject> nextWebSocketMessage();
    Result<QByteArray> nextWebSocketPayload();
    Result<std::optional<QJsonObject>>
    takeBufferedMessage();
    Result<std::optional<QJsonObject>>
    takeBufferedStdioMessage();
    Result<std::optional<QJsonObject>>
    takeBufferedWebSocketMessage();
    Result<void> readMoreStandardOutput();
    Result<void> collectStandardOutput();
    Result<void> writeBytes(const QByteArray& bytes);
    Result<void> sendWebSocketFrame(
        quint8 opcode,
        const QByteArray& payload);
    Result<void> processWebSocketFrame(
        QJsonObject* message,
        bool* hasMessage);
    Result<RpcResponse> parseResponse(
        const QJsonObject& message) const;
    Result<bool> captureServerMessage(
        const QJsonObject& message);
    CompanionError processUnavailableError(
        const QString& message);
    CompanionError timedOutError() const;
    CompanionError invalidResponseError(
        const QString& message) const;
    CompanionError operationCanceledError() const;
    Result<void> checkCancellation();
    void drainStandardError();
    QString stderrDetail();
    qsizetype maximumStandardOutputBufferBytes() const;
    int remainingMilliseconds() const;
    int waitSliceMilliseconds() const;
    void cancelProcess();
    void terminateProcess();

    ReapableProcess process_;
    QProcessEnvironment environment_;
    Transport transport_ = Transport::StdioJsonLines;
    QDeadlineTimer deadline_;
    std::stop_token stopToken_;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QByteArray fragmentedPayload_;
    bool fragmentedText_ = false;
    bool webSocketUpgraded_ = false;
    QHash<int, QJsonObject> bufferedResponses_;
    QVector<QJsonObject> serverRequests_;
    std::optional<CompanionError> backgroundError_;
    quintptr nativeProcessHandle_ = 0;
    bool closed_ = false;
};

int retryAttemptTimeoutMilliseconds(
    const QDeadlineTimer& deadline,
    qsizetype attemptsRemaining);

QJsonObject initializeRequest(
    const QString& clientName,
    const QString& clientTitle,
    const QString& version);

QString windowsClientVersion(
    QString applicationVersion);

QJsonObject initializedNotification();

Result<int> numericResponseId(const QJsonObject& message);

} // namespace companion::appserver_detail
