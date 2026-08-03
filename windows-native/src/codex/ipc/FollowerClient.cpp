#include "codex/ipc/FollowerClient.h"

#include "codex/discovery/CodexDiscoverySource.h"
#include "codex/ipc/FollowerEndpointDiscovery.h"
#include "codex/ipc/FollowerFrameCodec.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPromise>
#include <QThreadPool>
#include <QUuid>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <appmodel.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace companion {

namespace {

using SteadyClock = std::chrono::steady_clock;
using Deadline = SteadyClock::time_point;
using CancellationCheck = std::function<bool()>;
using RequestBuilder =
    std::function<Result<QJsonObject>(
        const QString& requestId,
        const QString& clientId)>;

constexpr auto kOfficialCodexPackageFamily =
    L"OpenAI.Codex_2p2nqsd0c76g0";

enum class SessionResultKind {
    Success,
    ConnectionUnavailable,
    TimedOut,
    Cancelled,
    Error,
    Failed,
};

struct SessionResult final {
    SessionResultKind kind = SessionResultKind::Failed;
    QString error;
};

enum class SessionExceptionKind {
    TimedOut,
    Cancelled,
    Transport,
    InvalidFrame,
};

class SessionException final : public std::exception {
public:
    explicit SessionException(SessionExceptionKind kind)
        : kind_(kind)
    {
    }

    SessionExceptionKind kind() const noexcept
    {
        return kind_;
    }

private:
    SessionExceptionKind kind_;
};

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~NativeHandle() { reset(); }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    NativeHandle(NativeHandle&& other) noexcept
        : handle_(std::exchange(
              other.handle_,
              INVALID_HANDLE_VALUE))
    {
    }

    NativeHandle& operator=(NativeHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(
                other.handle_,
                INVALID_HANDLE_VALUE));
        }
        return *this;
    }

    bool valid() const noexcept
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const noexcept { return handle_; }

    HANDLE release() noexcept
    {
        return std::exchange(
            handle_,
            INVALID_HANDLE_VALUE);
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE)
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

QString processImagePath(HANDLE process)
{
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(
            process,
            0,
            buffer.data(),
            &size)
        || size == 0) {
        return {};
    }
    return QDir::fromNativeSeparators(
        QString::fromWCharArray(
            buffer.data(),
            static_cast<qsizetype>(size)));
}

PSID processUserSid(
    HANDLE process,
    NativeHandle& token,
    QByteArray& storage)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(
            process,
            TOKEN_QUERY,
            &rawToken)) {
        return nullptr;
    }
    token.reset(rawToken);

    DWORD required = 0;
    GetTokenInformation(
        token.get(),
        TokenUser,
        nullptr,
        0,
        &required);
    if (required < sizeof(TOKEN_USER)
        || GetLastError()
            != ERROR_INSUFFICIENT_BUFFER) {
        return nullptr;
    }

    storage.resize(
        static_cast<qsizetype>(required));
    if (!GetTokenInformation(
            token.get(),
            TokenUser,
            storage.data(),
            required,
            &required)) {
        return nullptr;
    }

    const auto* user =
        reinterpret_cast<const TOKEN_USER*>(
            storage.constData());
    if (user->User.Sid == nullptr
        || !IsValidSid(user->User.Sid)) {
        return nullptr;
    }
    return user->User.Sid;
}

bool serverRunsAsCurrentUser(HANDLE serverProcess)
{
    NativeHandle currentToken;
    NativeHandle serverToken;
    QByteArray currentStorage;
    QByteArray serverStorage;
    const PSID currentSid = processUserSid(
        GetCurrentProcess(),
        currentToken,
        currentStorage);
    const PSID serverSid = processUserSid(
        serverProcess,
        serverToken,
        serverStorage);
    return currentSid != nullptr
        && serverSid != nullptr
        && EqualSid(currentSid, serverSid);
}

bool serverRunsInCurrentSession(DWORD processId)
{
    DWORD currentSession = 0;
    DWORD serverSession = 0;
    return ProcessIdToSessionId(
               GetCurrentProcessId(),
               &currentSession)
        && ProcessIdToSessionId(
            processId,
            &serverSession)
        && currentSession == serverSession;
}

QString packageFamilyName(HANDLE process)
{
    UINT32 required = 0;
    const LONG sizeResult =
        GetPackageFamilyName(
            process,
            &required,
            nullptr);
    if (sizeResult != ERROR_INSUFFICIENT_BUFFER
        || required == 0
        || required > 1024) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    if (GetPackageFamilyName(
            process,
            &required,
            buffer.data())
        != ERROR_SUCCESS) {
        return {};
    }
    return QString::fromWCharArray(buffer.data());
}

bool verifyFollowerServerProcess(
    quint32 processId,
    const CodexEnvironment& environment,
    const QVector<QString>&
        protectedProgramFilesDirectories,
    const QVector<QString>&
        installedCodexPackageExecutables)
{
    if (processId == 0
        || !serverRunsInCurrentSession(processId)) {
        return false;
    }

    NativeHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        processId));
    if (!process.valid()
        || !serverRunsAsCurrentUser(process.get())) {
        return false;
    }

    const QString imagePath =
        processImagePath(process.get());
    const QString familyName =
        packageFamilyName(process.get());
    if (FollowerEndpointDiscovery::
            isOfficialWindowsPackageProcess(
                imagePath,
                familyName)) {
        return true;
    }

    const FollowerExecutableTrust trust =
        FollowerEndpointDiscovery::executableTrust(
            imagePath,
            environment,
            protectedProgramFilesDirectories,
            installedCodexPackageExecutables);
    if (trust == FollowerExecutableTrust::Untrusted) {
        return false;
    }
    if (trust
        == FollowerExecutableTrust::WindowsPackage) {
        return familyName.compare(
                   QString::fromWCharArray(
                       kOfficialCodexPackageFamily),
                   Qt::CaseInsensitive)
            == 0;
    }
    return true;
}

FollowerServerVerifier productionServerVerifier(
    const CodexEnvironment& environment)
{
    const QVector<QString> protectedProgramFiles =
        systemProtectedProgramFilesDirectories();
    return [
               environment,
               protectedProgramFiles](
               quint32 processId) {
        const QVector<QString> packageExecutables =
            systemCodexDiscoverySource()
                .installedCodexPackageExecutables();
        return verifyFollowerServerProcess(
            processId,
            environment,
            protectedProgramFiles,
            packageExecutables);
    };
}

DWORD boundedWaitMilliseconds(
    Deadline deadline,
    std::chrono::milliseconds maximumSlice)
{
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - SteadyClock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    const auto slice = std::min(remaining, maximumSlice);
    return static_cast<DWORD>(
        std::clamp<qint64>(
            slice.count(),
            1,
            MAXDWORD - 1));
}

void checkCancellation(
    const CancellationCheck& cancelled)
{
    if (cancelled && cancelled()) {
        throw SessionException(
            SessionExceptionKind::Cancelled);
    }
}

bool isUnavailablePipeError(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND
        || error == ERROR_PATH_NOT_FOUND
        || error == ERROR_PIPE_BUSY
        || error == ERROR_SEM_TIMEOUT
        || error == ERROR_BAD_PATHNAME
        || error == ERROR_ACCESS_DENIED;
}

NativeHandle connectEndpoint(
    const QString& endpoint,
    Deadline deadline,
    const FollowerServerVerifier& serverVerifier,
    const CancellationCheck& cancelled)
{
    while (true) {
        checkCancellation(cancelled);
        if (SteadyClock::now() >= deadline) {
            return NativeHandle();
        }
        NativeHandle pipe(CreateFileW(
            reinterpret_cast<LPCWSTR>(
                endpoint.utf16()),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED
                | SECURITY_SQOS_PRESENT
                | SECURITY_IDENTIFICATION,
            nullptr));
        if (pipe.valid()) {
            ULONG serverProcessId = 0;
            if (!GetNamedPipeServerProcessId(
                    pipe.get(),
                    &serverProcessId)
                || !serverVerifier
                || !serverVerifier(
                    static_cast<quint32>(
                        serverProcessId))) {
                return NativeHandle();
            }
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(
                pipe.get(),
                &mode,
                nullptr,
                nullptr);
            return pipe;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY) {
            return NativeHandle();
        }

        const DWORD waitMilliseconds =
            boundedWaitMilliseconds(
                deadline,
                std::chrono::milliseconds(25));
        if (waitMilliseconds == 0) {
            return NativeHandle();
        }
        if (!WaitNamedPipeW(
                reinterpret_cast<LPCWSTR>(
                    endpoint.utf16()),
                waitMilliseconds)) {
            const DWORD waitError = GetLastError();
            if (!isUnavailablePipeError(waitError)) {
                return NativeHandle();
            }
        }
    }
}

NativeHandle connectCandidates(
    const QVector<QString>& candidates,
    std::chrono::milliseconds timeout,
    const FollowerServerVerifier& serverVerifier,
    const CancellationCheck& cancelled)
{
    const Deadline deadline =
        SteadyClock::now() + timeout;
    for (const QString& endpoint : candidates) {
        NativeHandle pipe =
            connectEndpoint(
                endpoint,
                deadline,
                serverVerifier,
                cancelled);
        if (pipe.valid()) {
            return pipe;
        }
    }
    return NativeHandle();
}

void cancelAndCompleteOverlapped(
    HANDLE pipe,
    OVERLAPPED& operation) noexcept
{
    CancelIoEx(pipe, &operation);
    DWORD ignored = 0;
    GetOverlappedResult(
        pipe,
        &operation,
        &ignored,
        TRUE);
}

DWORD completeOverlapped(
    HANDLE pipe,
    OVERLAPPED& operation,
    Deadline deadline,
    const CancellationCheck& cancelled)
{
    while (true) {
        if (cancelled && cancelled()) {
            cancelAndCompleteOverlapped(
                pipe,
                operation);
            throw SessionException(
                SessionExceptionKind::Cancelled);
        }

        const DWORD waitMilliseconds =
            boundedWaitMilliseconds(
                deadline,
                std::chrono::milliseconds(25));
        if (waitMilliseconds == 0) {
            cancelAndCompleteOverlapped(
                pipe,
                operation);
            throw SessionException(
                SessionExceptionKind::TimedOut);
        }

        const DWORD waitResult =
            WaitForSingleObject(
                operation.hEvent,
                waitMilliseconds);
        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }
        if (waitResult != WAIT_OBJECT_0) {
            cancelAndCompleteOverlapped(
                pipe,
                operation);
            throw SessionException(
                SessionExceptionKind::Transport);
        }

        DWORD transferred = 0;
        if (!GetOverlappedResult(
                pipe,
                &operation,
                &transferred,
                FALSE)) {
            const DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED
                && cancelled
                && cancelled()) {
                throw SessionException(
                    SessionExceptionKind::Cancelled);
            }
            throw SessionException(
                SessionExceptionKind::Transport);
        }
        return transferred;
    }
}

DWORD overlappedWrite(
    HANDLE pipe,
    const char* data,
    DWORD byteCount,
    Deadline deadline,
    const CancellationCheck& cancelled)
{
    NativeHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.valid()) {
        throw SessionException(
            SessionExceptionKind::Transport);
    }

    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL completed = WriteFile(
        pipe,
        data,
        byteCount,
        &transferred,
        &operation);
    if (completed) {
        return transferred;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        throw SessionException(
            SessionExceptionKind::Transport);
    }
    return completeOverlapped(
        pipe,
        operation,
        deadline,
        cancelled);
}

DWORD overlappedRead(
    HANDLE pipe,
    char* data,
    DWORD byteCount,
    Deadline deadline,
    const CancellationCheck& cancelled)
{
    NativeHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.valid()) {
        throw SessionException(
            SessionExceptionKind::Transport);
    }

    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL completed = ReadFile(
        pipe,
        data,
        byteCount,
        &transferred,
        &operation);
    if (completed) {
        return transferred;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        throw SessionException(
            SessionExceptionKind::Transport);
    }
    return completeOverlapped(
        pipe,
        operation,
        deadline,
        cancelled);
}

void writeMessage(
    HANDLE pipe,
    const QJsonObject& message,
    Deadline deadline,
    const CancellationCheck& cancelled)
{
    const Result<QByteArray> encoded =
        FollowerFrameCodec::encode(message);
    if (!encoded.hasValue()) {
        throw SessionException(
            SessionExceptionKind::InvalidFrame);
    }

    const QByteArray& frame = encoded.value();
    qsizetype offset = 0;
    while (offset < frame.size()) {
        checkCancellation(cancelled);
        const DWORD requested = static_cast<DWORD>(
            qMin<qsizetype>(
                frame.size() - offset,
                std::numeric_limits<DWORD>::max()));
        const DWORD written = overlappedWrite(
            pipe,
            frame.constData() + offset,
            requested,
            deadline,
            cancelled);
        if (written == 0) {
            throw SessionException(
                SessionExceptionKind::Transport);
        }
        offset += static_cast<qsizetype>(written);
    }
}

QJsonObject waitForResponse(
    HANDLE pipe,
    const QString& requestId,
    Deadline deadline,
    const CancellationCheck& cancelled)
{
    FollowerFrameCodec codec;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);

    while (true) {
        checkCancellation(cancelled);
        const DWORD read = overlappedRead(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            deadline,
            cancelled);
        if (read == 0) {
            throw SessionException(
                SessionExceptionKind::Transport);
        }

        const Result<QVector<QJsonObject>> decoded =
            codec.append(QByteArrayView(
                buffer.constData(),
                static_cast<qsizetype>(read)));
        if (!decoded.hasValue()) {
            throw SessionException(
                SessionExceptionKind::InvalidFrame);
        }
        for (const QJsonObject& message :
             decoded.value()) {
            if (message.value(QStringLiteral("type"))
                    .toString()
                    != QStringLiteral("response")) {
                continue;
            }
            if (message.value(QStringLiteral("requestId"))
                    .toString()
                    != requestId) {
                continue;
            }
            return message;
        }
    }
}

bool responseSucceeded(const QJsonObject& response)
{
    return response.value(QStringLiteral("resultType"))
               .toString()
        == QStringLiteral("success");
}

QString responseErrorText(
    const QJsonObject& response)
{
    const QJsonValue error =
        response.value(QStringLiteral("error"));
    if (error.isString()) {
        return error.toString();
    }
    if (error.isObject()) {
        const QJsonObject object = error.toObject();
        const QString message =
            object.value(QStringLiteral("message"))
                .toString();
        if (!message.isEmpty()) {
            return message;
        }
        const QString code =
            object.value(QStringLiteral("code"))
                .toString();
        if (!code.isEmpty()) {
            return code;
        }
        return QString::fromUtf8(
            QJsonDocument(object)
                .toJson(QJsonDocument::Compact));
    }
    if (!error.isUndefined() && !error.isNull()) {
        return QString::fromUtf8(
            QJsonDocument(QJsonArray{error})
                .toJson(QJsonDocument::Compact))
            .sliced(1)
            .chopped(1);
    }
    return {};
}

SessionResult responseResult(
    const QJsonObject& response)
{
    if (responseSucceeded(response)) {
        return {SessionResultKind::Success, {}};
    }
    const QString error = responseErrorText(response);
    if (!error.isEmpty()) {
        return {SessionResultKind::Error, error};
    }
    return {SessionResultKind::Failed, {}};
}

SessionResult runSession(
    const FollowerCandidateProvider& candidateProvider,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds responseTimeout,
    const FollowerServerVerifier& serverVerifier,
    const RequestBuilder& requestBuilder,
    const CancellationCheck& cancelled)
{
    try {
        checkCancellation(cancelled);
        const QVector<QString> candidates =
            candidateProvider
            ? candidateProvider()
            : QVector<QString>{};
        NativeHandle pipe = connectCandidates(
            candidates,
            connectTimeout,
            serverVerifier,
            cancelled);
        if (!pipe.valid()) {
            return {
                SessionResultKind::ConnectionUnavailable,
                {},
            };
        }

        const Deadline deadline =
            SteadyClock::now() + responseTimeout;
        const QString initializeId =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        writeMessage(
            pipe.get(),
            FollowerRequestFactory::initialize(
                initializeId),
            deadline,
            cancelled);
        const QJsonObject initializeResponse =
            waitForResponse(
                pipe.get(),
                initializeId,
                deadline,
                cancelled);
        const SessionResult initialized =
            responseResult(initializeResponse);
        if (initialized.kind
            != SessionResultKind::Success) {
            return initialized;
        }

        const QString clientId =
            initializeResponse
                .value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("clientId"))
                .toString();
        if (clientId.isEmpty()) {
            return {SessionResultKind::Failed, {}};
        }

        const QString requestId =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        const Result<QJsonObject> request =
            requestBuilder(requestId, clientId);
        if (!request.hasValue()) {
            return {
                SessionResultKind::Failed,
                request.error().message,
            };
        }
        writeMessage(
            pipe.get(),
            request.value(),
            deadline,
            cancelled);
        return responseResult(waitForResponse(
            pipe.get(),
            requestId,
            deadline,
            cancelled));
    } catch (const SessionException& error) {
        switch (error.kind()) {
        case SessionExceptionKind::TimedOut:
            return {SessionResultKind::TimedOut, {}};
        case SessionExceptionKind::Cancelled:
            return {SessionResultKind::Cancelled, {}};
        case SessionExceptionKind::Transport:
        case SessionExceptionKind::InvalidFrame:
            return {SessionResultKind::Failed, {}};
        }
    } catch (...) {
        return {SessionResultKind::Failed, {}};
    }
    return {SessionResultKind::Failed, {}};
}

FollowerSendOutcome sendOutcome(
    const SessionResult& result)
{
    switch (result.kind) {
    case SessionResultKind::Success:
        return FollowerSendOutcome::Sent;
    case SessionResultKind::ConnectionUnavailable:
        return FollowerSendOutcome::
            SharedDaemonUnavailable;
    case SessionResultKind::TimedOut:
        return FollowerSendOutcome::TimedOut;
    case SessionResultKind::Error:
        return FollowerRequestFactory::
            sendOutcomeForError(result.error);
    case SessionResultKind::Cancelled:
    case SessionResultKind::Failed:
        return FollowerSendOutcome::Failed;
    }
    return FollowerSendOutcome::Failed;
}

FollowerApprovalOutcome approvalOutcome(
    const SessionResult& result,
    ApprovalDecision decision)
{
    switch (result.kind) {
    case SessionResultKind::Success:
        return decision == ApprovalDecision::Decline
            ? FollowerApprovalOutcome::Declined
            : FollowerApprovalOutcome::Approved;
    case SessionResultKind::ConnectionUnavailable:
        return FollowerApprovalOutcome::
            SharedDaemonUnavailable;
    case SessionResultKind::TimedOut:
        return FollowerApprovalOutcome::TimedOut;
    case SessionResultKind::Error:
        return FollowerRequestFactory::
            approvalOutcomeForError(result.error);
    case SessionResultKind::Cancelled:
    case SessionResultKind::Failed:
        return FollowerApprovalOutcome::Failed;
    }
    return FollowerApprovalOutcome::Failed;
}

template <typename T, typename Operation>
QFuture<T> launch(
    T failureOutcome,
    Operation operation)
{
    auto promise = std::make_shared<QPromise<T>>();
    promise->start();
    QFuture<T> future = promise->future();
    QThreadPool::globalInstance()->start(
        [promise,
         failureOutcome,
         operation = std::move(operation)]() mutable {
            try {
                const T result = operation(
                    [promise] {
                        return promise->isCanceled();
                    });
                promise->addResult(result);
            } catch (...) {
                promise->addResult(failureOutcome);
            }
            promise->finish();
        });
    return future;
}

} // namespace

struct FollowerClient::State final {
    FollowerCandidateProvider candidateProvider;
    std::chrono::milliseconds connectTimeout;
    std::chrono::milliseconds responseTimeout;
    FollowerServerVerifier serverVerifier;
    FollowerQueuedStateLoader queuedStateLoader;
};

FollowerClient::FollowerClient(
    const CodexEnvironment& environment)
    : FollowerClient(
          [environment] {
              return FollowerEndpointDiscovery()
                  .candidates(environment);
          },
          defaultConnectTimeout(),
          defaultResponseTimeout(),
          trustedServerVerifier(environment),
          [path = QDir(environment.codexHome)
                      .filePath(QStringLiteral(
                          ".codex-global-state.json"))] {
              return FollowerRequestFactory::
                  loadQueuedFollowUpState(path);
          })
{
}

FollowerClient::FollowerClient(
    FollowerCandidateProvider candidateProvider,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds responseTimeout,
    FollowerServerVerifier serverVerifier,
    FollowerQueuedStateLoader queuedStateLoader)
    : state_(std::make_shared<State>(
          State{
              std::move(candidateProvider),
              std::max(
                  connectTimeout,
                  std::chrono::milliseconds(1)),
              std::max(
                  responseTimeout,
                  std::chrono::milliseconds(1)),
              std::move(serverVerifier),
              std::move(queuedStateLoader),
          }))
{
}

FollowerServerVerifier
FollowerClient::trustedServerVerifier(
    const CodexEnvironment& environment)
{
    return productionServerVerifier(environment);
}

FollowerClient::~FollowerClient() = default;

QFuture<FollowerSendOutcome>
FollowerClient::updateThreadSettings(
    QString threadId,
    QString model,
    QString reasoningEffort) const
{
    const auto state = state_;
    return launch<FollowerSendOutcome>(
        FollowerSendOutcome::Failed,
        [state,
         threadId = std::move(threadId),
         model = std::move(model),
         reasoningEffort =
             std::move(reasoningEffort)](
            const CancellationCheck& cancelled) {
            return sendOutcome(runSession(
                state->candidateProvider,
                state->connectTimeout,
                state->responseTimeout,
                state->serverVerifier,
                [threadId,
                 model,
                 reasoningEffort](
                    const QString& requestId,
                    const QString& clientId) {
                    return Result<QJsonObject>::success(
                        FollowerRequestFactory::
                            threadSettings(
                                requestId,
                                clientId,
                                threadId,
                                model,
                                reasoningEffort));
                },
                cancelled));
        });
}

QFuture<FollowerSendOutcome> FollowerClient::submit(
    QString prompt,
    QString threadId,
    SendAction action,
    QString clientMessageId,
    QString cwd,
    QVector<StagedAttachment> attachments) const
{
    const auto state = state_;
    return launch<FollowerSendOutcome>(
        FollowerSendOutcome::Failed,
        [state,
         prompt = std::move(prompt),
         threadId = std::move(threadId),
         action,
         clientMessageId =
             std::move(clientMessageId),
         cwd = std::move(cwd),
         attachments = std::move(attachments)](
            const CancellationCheck& cancelled) {
            return sendOutcome(runSession(
                state->candidateProvider,
                state->connectTimeout,
                state->responseTimeout,
                state->serverVerifier,
                [prompt,
                 threadId,
                 action,
                 clientMessageId,
                 cwd,
                 attachments](
                    const QString& requestId,
                    const QString& clientId) {
                    return FollowerRequestFactory::action(
                        requestId,
                        clientId,
                        threadId,
                        prompt,
                        action,
                        clientMessageId,
                        cwd,
                        attachments,
                        QDateTime::
                            currentMSecsSinceEpoch());
                },
                cancelled));
        });
}

QFuture<FollowerSendOutcome> FollowerClient::queueReply(
    QString prompt,
    QString threadId,
    QString clientMessageId,
    QString cwd,
    QVector<StagedAttachment> attachments) const
{
    const auto state = state_;
    return launch<FollowerSendOutcome>(
        FollowerSendOutcome::Failed,
        [state,
         prompt = std::move(prompt),
         threadId = std::move(threadId),
         clientMessageId =
             std::move(clientMessageId),
         cwd = std::move(cwd),
         attachments = std::move(attachments)](
            const CancellationCheck& cancelled) {
            if (!state->queuedStateLoader) {
                return FollowerSendOutcome::Failed;
            }
            const Result<QJsonObject> queuedState =
                state->queuedStateLoader();
            if (!queuedState.hasValue()) {
                return FollowerSendOutcome::Failed;
            }
            return sendOutcome(runSession(
                state->candidateProvider,
                state->connectTimeout,
                state->responseTimeout,
                state->serverVerifier,
                [prompt,
                 threadId,
                 clientMessageId,
                 cwd,
                 attachments,
                 state = queuedState.value()](
                    const QString& requestId,
                    const QString& clientId) {
                    return FollowerRequestFactory::
                        queuedReply(
                            requestId,
                            clientId,
                            threadId,
                            prompt,
                            clientMessageId,
                            cwd,
                            state,
                            attachments,
                            QDateTime::
                                currentMSecsSinceEpoch());
                },
                cancelled));
        });
}

QFuture<FollowerApprovalOutcome>
FollowerClient::respondToApproval(
    PendingApproval request,
    ApprovalDecision decision) const
{
    const auto state = state_;
    return launch<FollowerApprovalOutcome>(
        FollowerApprovalOutcome::Failed,
        [state,
         request = std::move(request),
         decision](
            const CancellationCheck& cancelled) {
            return approvalOutcome(
                runSession(
                    state->candidateProvider,
                    state->connectTimeout,
                    state->responseTimeout,
                    state->serverVerifier,
                    [request,
                     decision](
                        const QString& requestId,
                        const QString& clientId) {
                        return Result<QJsonObject>::success(
                            FollowerRequestFactory::
                                approval(
                                    requestId,
                                    clientId,
                                    request,
                                    decision));
                    },
                    cancelled),
                decision);
        });
}

} // namespace companion
