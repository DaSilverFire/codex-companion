#include "codex/appserver/AppServerProcess.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSet>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace companion::appserver_detail {

namespace {

constexpr qsizetype kMaximumMessageBytes =
    4 * 1024 * 1024;
constexpr qsizetype kMaximumHandshakeBytes =
    32 * 1024;
constexpr qsizetype kIoReadChunkBytes =
    64 * 1024;
constexpr qsizetype kMaximumStdioBufferBytes =
    kMaximumMessageBytes + 1;
constexpr qsizetype kMaximumWebSocketBufferBytes =
    kMaximumMessageBytes + kIoReadChunkBytes;
constexpr qsizetype kMaximumBufferedResponses = 128;
constexpr qsizetype kMaximumServerRequests = 128;
constexpr int kCancellationWaitSliceMilliseconds = 50;
constexpr int kTerminationGraceMilliseconds = 250;
constexpr int kForcedKillReapMilliseconds = 3000;
constexpr quint8 kContinuationOpcode = 0x0;
constexpr quint8 kTextOpcode = 0x1;
constexpr quint8 kBinaryOpcode = 0x2;
constexpr quint8 kCloseOpcode = 0x8;
constexpr quint8 kPingOpcode = 0x9;
constexpr quint8 kPongOpcode = 0xa;

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE handle = nullptr)
        : handle_(handle)
    {
    }

    ~NativeHandle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    bool valid() const noexcept
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

DWORD WINAPI reapNativeProcess(void* context)
{
    const HANDLE process =
        static_cast<HANDLE>(context);
    TerminateProcess(
        process, ERROR_PROCESS_ABORTED);
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    return 0;
}

bool startNativeProcessReaper(HANDLE process)
{
    const HANDLE thread = CreateThread(
        nullptr,
        0,
        &reapNativeProcess,
        process,
        0,
        nullptr);
    if (thread == nullptr) {
        return false;
    }
    CloseHandle(thread);
    return true;
}

QString transportName(Transport transport)
{
    return transport == Transport::ProxyWebSocket
        ? QStringLiteral("proxy")
        : QStringLiteral("stdio");
}

QString serverErrorText(const QJsonValue& value)
{
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        return text.isEmpty()
            ? QStringLiteral(
                  "Codex app-server request failed.")
            : text;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        const QString message =
            object.value(QStringLiteral("message"))
                .toString()
                .trimmed();
        if (!message.isEmpty()) {
            return message;
        }
        return QString::fromUtf8(
            QJsonDocument(object).toJson(
                QJsonDocument::Compact));
    }
    return QStringLiteral(
        "Codex app-server request failed.");
}

CompanionError withWriteCertainty(
    CompanionError error,
    bool mayHaveWritten)
{
    error.context.insert(
        QStringLiteral("writeMayHaveStarted"),
        mayHaveWritten);
    return error;
}

QByteArray randomWebSocketKey()
{
    QByteArray bytes(16, Qt::Uninitialized);
    for (qsizetype offset = 0;
         offset < bytes.size();
         offset += static_cast<qsizetype>(sizeof(quint32))) {
        const quint32 value =
            QRandomGenerator::system()->generate();
        const qsizetype copySize = std::min(
            static_cast<qsizetype>(sizeof(value)),
            bytes.size() - offset);
        std::memcpy(
            bytes.data() + offset,
            &value,
            static_cast<std::size_t>(copySize));
    }
    return bytes.toBase64();
}

QByteArray maskedClientFrame(
    quint8 opcode,
    const QByteArray& payload)
{
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.append(static_cast<char>(
            0x80 | payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.append(static_cast<char>(0x80 | 126));
        frame.append(static_cast<char>(
            (payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(
            payload.size() & 0xff));
    } else {
        frame.append(static_cast<char>(0x80 | 127));
        const quint64 length =
            static_cast<quint64>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.append(static_cast<char>(
                (length >> shift) & 0xff));
        }
    }

    const quint32 maskValue =
        QRandomGenerator::system()->generate();
    QByteArray mask(
        reinterpret_cast<const char*>(&maskValue),
        static_cast<qsizetype>(sizeof(maskValue)));
    frame.append(mask);
    for (qsizetype index = 0;
         index < payload.size();
         ++index) {
        frame.append(static_cast<char>(
            static_cast<quint8>(payload.at(index))
            ^ static_cast<quint8>(
                mask.at(index % mask.size()))));
    }
    return frame;
}

Result<QJsonObject> parseJsonObject(
    const QByteArray& payload)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<QJsonObject>::failure({
            QStringLiteral(
                "codex.app_server_invalid_response"),
            QStringLiteral(
                "Codex app-server returned unreadable JSON."),
            false,
            {
                {
                    QStringLiteral("parseError"),
                    parseError.errorString(),
                },
            },
        });
    }
    return Result<QJsonObject>::success(document.object());
}

QByteArray headerValue(
    const QByteArray& header,
    const QByteArray& name)
{
    const QByteArray expected =
        name.trimmed().toLower();
    const QList<QByteArray> lines = header.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        if (line.first(colon).trimmed().toLower()
            == expected) {
            return line.sliced(colon + 1).trimmed();
        }
    }
    return {};
}

bool headerContainsToken(
    const QByteArray& header,
    const QByteArray& name,
    const QByteArray& token)
{
    const QByteArray expectedToken =
        token.trimmed().toLower();
    for (QByteArray value :
         headerValue(header, name).split(',')) {
        if (value.trimmed().toLower()
            == expectedToken) {
            return true;
        }
    }
    return false;
}

QByteArray webSocketAccept(
    const QByteArray& key)
{
    return QCryptographicHash::hash(
               key
                   + QByteArray(
                       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
               QCryptographicHash::Sha1)
        .toBase64();
}

} // namespace

int retryAttemptTimeoutMilliseconds(
    const QDeadlineTimer& deadline,
    qsizetype attemptsRemaining)
{
    const qint64 remaining = deadline.remainingTime();
    if (remaining <= 0 || attemptsRemaining <= 0) {
        return 0;
    }
    const qint64 budget = attemptsRemaining == 1
        ? remaining
        : std::max<qint64>(
              1, remaining / attemptsRemaining);
    return static_cast<int>(
        std::min<qint64>(
            budget,
            std::numeric_limits<int>::max()));
}

Result<std::unique_ptr<ProcessSession>>
ProcessSession::start(
    const QVector<QString>& executableCandidates,
    const QStringList& arguments,
    const QProcessEnvironment& environment,
    Transport transport,
    int timeoutMilliseconds,
    std::stop_token stopToken)
{
    auto session = std::unique_ptr<ProcessSession>(
        new ProcessSession(
            environment,
            transport,
            timeoutMilliseconds,
            stopToken));
    const Result<void> launched =
        session->launch(executableCandidates, arguments);
    if (!launched.hasValue()) {
        return Result<std::unique_ptr<ProcessSession>>::failure(
            launched.error());
    }
    if (transport == Transport::ProxyWebSocket) {
        const Result<void> upgraded =
            session->upgradeWebSocket();
        if (!upgraded.hasValue()) {
            return Result<std::unique_ptr<ProcessSession>>::failure(
                upgraded.error());
        }
    }
    return Result<std::unique_ptr<ProcessSession>>::success(
        std::move(session));
}

ProcessSession::ProcessSession(
    QProcessEnvironment environment,
    Transport transport,
    int timeoutMilliseconds,
    std::stop_token stopToken)
    : environment_(std::move(environment)),
      transport_(transport),
      deadline_(std::max(1, timeoutMilliseconds)),
      stopToken_(stopToken)
{
    process_.setProcessChannelMode(
        QProcess::SeparateChannels);
    process_.setReadChannel(
        QProcess::StandardOutput);
    process_.setProcessEnvironment(environment_);
    QObject::connect(
        &process_,
        &QProcess::readyReadStandardError,
        &process_,
        [this] { drainStandardError(); },
        Qt::DirectConnection);
    QObject::connect(
        &process_,
        &QProcess::readyReadStandardOutput,
        &process_,
        [this] {
            if (closed_) {
                return;
            }
            const Result<void> drained =
                drainAvailable();
            if (!drained.hasValue()) {
                backgroundError_ = drained.error();
                terminateProcess();
            }
        },
        Qt::QueuedConnection);
}

ProcessSession::~ProcessSession()
{
    close();
}

Result<void> ProcessSession::launch(
    const QVector<QString>& executableCandidates,
    const QStringList& arguments)
{
    int attempted = 0;
    QString lastError;
    for (const QString& candidate : executableCandidates) {
        const Result<void> notCanceled =
            checkCancellation();
        if (!notCanceled.hasValue()) {
            return notCanceled;
        }
        const QString executable = candidate.trimmed();
        if (executable.isEmpty()) {
            continue;
        }
        ++attempted;
        process_.setProgram(executable);
        process_.setArguments(arguments);
        process_.start();

        const Result<bool> started =
            waitForStartup(
                [this] {
                    return process_.state();
                },
                [this](int waitMilliseconds) {
                    return process_.waitForStarted(
                        waitMilliseconds);
                });
        if (!started.hasValue()) {
            if (started.error().code
                == QStringLiteral(
                    "codex.app_server_timed_out")) {
                terminateProcess();
            }
            return Result<void>::failure(
                started.error());
        }
        if (started.value()) {
            nativeProcessHandle_ =
                reinterpret_cast<quintptr>(
                    OpenProcess(
                        PROCESS_TERMINATE
                            | SYNCHRONIZE,
                        FALSE,
                        static_cast<DWORD>(
                            process_.processId())));
            return Result<void>::success();
        }
        lastError = process_.errorString();
    }

    return Result<void>::failure({
        attempted == 0
            ? QStringLiteral("codex.executable_not_found")
            : QStringLiteral("codex.app_server_launch_failed"),
        attempted == 0
            ? QStringLiteral(
                  "Could not find an installed Codex executable.")
            : QStringLiteral(
                  "Codex app-server could not start."),
        false,
        {
            {QStringLiteral("candidateCount"), attempted},
            {QStringLiteral("detail"), lastError},
            {
                QStringLiteral("transport"),
                transportName(transport_),
            },
        },
    });
}

Result<void> ProcessSession::upgradeWebSocket()
{
    const QByteArray key = randomWebSocketKey();
    const QByteArray expectedAccept =
        webSocketAccept(key);
    const QByteArray request =
        QByteArray("GET /rpc HTTP/1.1\r\n")
        + "Host: localhost\r\n"
        + "Upgrade: websocket\r\n"
        + "Connection: Upgrade\r\n"
        + "Sec-WebSocket-Key: "
        + key
        + "\r\n"
        + "Sec-WebSocket-Version: 13\r\n"
        + "\r\n";
    const Result<void> written = writeBytes(request);
    if (!written.hasValue()) {
        return written;
    }

    const QByteArray terminator("\r\n\r\n");
    while (true) {
        const qsizetype headerEnd =
            stdoutBuffer_.indexOf(terminator);
        if (headerEnd >= 0) {
            if (headerEnd + terminator.size()
                > kMaximumHandshakeBytes) {
                return Result<void>::failure(
                    invalidResponseError(
                        QStringLiteral(
                            "Codex app-server returned an oversized WebSocket handshake.")));
            }
            const QByteArray header =
                stdoutBuffer_.first(headerEnd);
            stdoutBuffer_.remove(
                0, headerEnd + terminator.size());
            if (!header.startsWith("HTTP/1.1 101 ")
                || !headerContainsToken(
                    header,
                    QByteArray("Upgrade"),
                    QByteArray("websocket"))
                || !headerContainsToken(
                    header,
                    QByteArray("Connection"),
                    QByteArray("upgrade"))
                || headerValue(
                       header,
                       QByteArray(
                           "Sec-WebSocket-Accept"))
                    != expectedAccept) {
                return Result<void>::failure(
                    invalidResponseError(
                        QStringLiteral(
                            "Codex app-server rejected the WebSocket upgrade.")));
            }
            webSocketUpgraded_ = true;
            return Result<void>::success();
        }
        if (stdoutBuffer_.size() > kMaximumHandshakeBytes) {
            return Result<void>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server returned an oversized WebSocket handshake.")));
        }
        const Result<void> read =
            readMoreStandardOutput();
        if (!read.hasValue()) {
            return read;
        }
    }
}

Result<void> ProcessSession::send(
    const QJsonObject& message)
{
    const Result<void> notCanceled =
        checkCancellation();
    if (!notCanceled.hasValue()) {
        return notCanceled;
    }
    const QByteArray payload =
        QJsonDocument(message).toJson(
            QJsonDocument::Compact);
    if (payload.size() > kMaximumMessageBytes) {
        return Result<void>::failure({
            QStringLiteral(
                "codex.app_server_invalid_request"),
            QStringLiteral(
                "Codex app-server request is too large."),
            false,
            {
                {
                    QStringLiteral("payloadBytes"),
                    payload.size(),
                },
                {
                    QStringLiteral(
                        "writeMayHaveStarted"),
                    false,
                },
            },
        });
    }
    if (transport_ == Transport::ProxyWebSocket) {
        return sendWebSocketFrame(
            kTextOpcode, payload);
    }
    return writeBytes(payload + '\n');
}

Result<RpcResponse> ProcessSession::response(
    int expectedId)
{
    const Result<void> notCanceled =
        checkCancellation();
    if (!notCanceled.hasValue()) {
        return Result<RpcResponse>::failure(
            notCanceled.error());
    }
    if (backgroundError_.has_value()) {
        return Result<RpcResponse>::failure(
            *backgroundError_);
    }
    const auto buffered =
        bufferedResponses_.find(expectedId);
    if (buffered != bufferedResponses_.end()) {
        const QJsonObject message = buffered.value();
        bufferedResponses_.erase(buffered);
        return parseResponse(message);
    }

    while (true) {
        const Result<void> stillNotCanceled =
            checkCancellation();
        if (!stillNotCanceled.hasValue()) {
            return Result<RpcResponse>::failure(
                stillNotCanceled.error());
        }
        const Result<QJsonObject> next = nextMessage();
        if (!next.hasValue()) {
            return Result<RpcResponse>::failure(
                next.error());
        }
        const Result<bool> serverMessage =
            captureServerMessage(next.value());
        if (!serverMessage.hasValue()) {
            return Result<RpcResponse>::failure(
                serverMessage.error());
        }
        if (serverMessage.value()) {
            continue;
        }
        const Result<int> id =
            numericResponseId(next.value());
        if (!id.hasValue()) {
            return Result<RpcResponse>::failure(
                id.error());
        }
        if (id.value() == expectedId) {
            return parseResponse(next.value());
        }
        if (!bufferedResponses_.contains(id.value())
            && bufferedResponses_.size()
                >= kMaximumBufferedResponses) {
            return Result<RpcResponse>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server returned too many unmatched responses.")));
        }
        bufferedResponses_.insert(
            id.value(), next.value());
    }
}

Result<void> ProcessSession::drainAvailable()
{
    const Result<void> notCanceled =
        checkCancellation();
    if (!notCanceled.hasValue()) {
        return notCanceled;
    }
    drainStandardError();
    const Result<void> collected =
        collectStandardOutput();
    if (!collected.hasValue()) {
        return collected;
    }
    if (transport_ == Transport::ProxyWebSocket) {
        resetDeadline(1000);
    }

    while (true) {
        const auto next = takeBufferedMessage();
        if (!next.hasValue()) {
            return Result<void>::failure(
                next.error());
        }
        if (!next.value().has_value()) {
            return Result<void>::success();
        }
        const QJsonObject& message =
            *next.value();
        const Result<bool> serverMessage =
            captureServerMessage(message);
        if (!serverMessage.hasValue()) {
            return Result<void>::failure(
                serverMessage.error());
        }
        if (serverMessage.value()) {
            continue;
        }
        const Result<int> id =
            numericResponseId(message);
        if (!id.hasValue()) {
            return Result<void>::failure(
                id.error());
        }
        if (!bufferedResponses_.contains(id.value())
            && bufferedResponses_.size()
                >= kMaximumBufferedResponses) {
            return Result<void>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server returned too many unmatched responses.")));
        }
        bufferedResponses_.insert(
            id.value(), message);
    }
}

Result<QVector<QJsonObject>>
ProcessSession::pendingServerRequests()
{
    if (backgroundError_.has_value()) {
        return Result<
            QVector<QJsonObject>>::failure(
            *backgroundError_);
    }
    const Result<void> drained = drainAvailable();
    if (!drained.hasValue()) {
        return Result<
            QVector<QJsonObject>>::failure(
            drained.error());
    }
    return Result<QVector<QJsonObject>>::success(
        serverRequests_);
}

void ProcessSession::consumeServerRequests(
    qsizetype count)
{
    if (count <= 0) {
        return;
    }
    serverRequests_.remove(
        0, std::min(count, serverRequests_.size()));
}

Result<void> ProcessSession::sendResponse(
    const QJsonValue& id,
    const QJsonValue& result)
{
    if ((!id.isDouble() && !id.isString())
        || id.isNull() || id.isUndefined()) {
        return Result<void>::failure({
            QStringLiteral(
                "codex.app_server_invalid_request"),
            QStringLiteral(
                "Codex app-server response requires a numeric or string request ID."),
            false,
            {},
        });
    }
    return send({
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result},
    });
}

void ProcessSession::resetDeadline(
    int timeoutMilliseconds)
{
    deadline_ = QDeadlineTimer(
        std::max(1, timeoutMilliseconds));
}

bool ProcessSession::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

void ProcessSession::close()
{
    if (closed_) {
        return;
    }
    closed_ = true;
    process_.closeWriteChannel();
    terminateProcess();
}

Result<QJsonObject> ProcessSession::nextMessage()
{
    return transport_ == Transport::ProxyWebSocket
        ? nextWebSocketMessage()
        : nextStdioMessage();
}

Result<QJsonObject> ProcessSession::nextStdioMessage()
{
    while (true) {
        const auto buffered =
            takeBufferedStdioMessage();
        if (!buffered.hasValue()) {
            return Result<QJsonObject>::failure(
                buffered.error());
        }
        if (buffered.value().has_value()) {
            return Result<QJsonObject>::success(
                std::move(*buffered.value()));
        }
        const Result<void> read =
            readMoreStandardOutput();
        if (!read.hasValue()) {
            return Result<QJsonObject>::failure(
                read.error());
        }
    }
}

Result<QJsonObject>
ProcessSession::nextWebSocketMessage()
{
    while (true) {
        const Result<QByteArray> payload =
            nextWebSocketPayload();
        if (!payload.hasValue()) {
            return Result<QJsonObject>::failure(
                payload.error());
        }
        const Result<QJsonObject> parsed =
            parseJsonObject(payload.value());
        if (parsed.hasValue()) {
            return parsed;
        }
    }
}

Result<QByteArray>
ProcessSession::nextWebSocketPayload()
{
    while (true) {
        const auto buffered =
            takeBufferedWebSocketMessage();
        if (!buffered.hasValue()) {
            return Result<QByteArray>::failure(
                buffered.error());
        }
        if (buffered.value().has_value()) {
            return Result<QByteArray>::success(
                QJsonDocument(*buffered.value()).toJson(
                    QJsonDocument::Compact));
        }
        const Result<void> read =
            readMoreStandardOutput();
        if (!read.hasValue()) {
            return Result<QByteArray>::failure(
                read.error());
        }
    }
}

Result<std::optional<QJsonObject>>
ProcessSession::takeBufferedMessage()
{
    return transport_ == Transport::ProxyWebSocket
        ? takeBufferedWebSocketMessage()
        : takeBufferedStdioMessage();
}

Result<std::optional<QJsonObject>>
ProcessSession::takeBufferedStdioMessage()
{
    while (true) {
        const qsizetype newline =
            stdoutBuffer_.indexOf('\n');
        if (newline < 0) {
            if (stdoutBuffer_.size()
                > kMaximumMessageBytes) {
                return Result<
                    std::optional<QJsonObject>>::failure(
                    invalidResponseError(
                        QStringLiteral(
                            "Codex app-server returned an oversized JSON line.")));
            }
            return Result<
                std::optional<QJsonObject>>::success(
                std::nullopt);
        }
        if (newline > kMaximumMessageBytes) {
            return Result<
                std::optional<QJsonObject>>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server returned an oversized JSON line.")));
        }
        QByteArray line =
            stdoutBuffer_.first(newline);
        stdoutBuffer_.remove(0, newline + 1);
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const Result<QJsonObject> parsed =
            parseJsonObject(line);
        if (!parsed.hasValue()) {
            return Result<
                std::optional<QJsonObject>>::failure(
                parsed.error());
        }
        return Result<
            std::optional<QJsonObject>>::success(
            std::optional<QJsonObject>(
                parsed.value()));
    }
}

Result<std::optional<QJsonObject>>
ProcessSession::takeBufferedWebSocketMessage()
{
    while (true) {
        const qsizetype bufferedBefore =
            stdoutBuffer_.size();
        QJsonObject message;
        bool hasMessage = false;
        const Result<void> processed =
            processWebSocketFrame(
                &message, &hasMessage);
        if (!processed.hasValue()) {
            return Result<
                std::optional<QJsonObject>>::failure(
                processed.error());
        }
        if (hasMessage) {
            return Result<
                std::optional<QJsonObject>>::success(
                std::optional<QJsonObject>(
                    std::move(message)));
        }
        if (stdoutBuffer_.size()
            == bufferedBefore) {
            return Result<
                std::optional<QJsonObject>>::success(
                std::nullopt);
        }
    }
}

Result<void> ProcessSession::readMoreStandardOutput()
{
    while (true) {
        const Result<void> notCanceled =
            checkCancellation();
        if (!notCanceled.hasValue()) {
            return notCanceled;
        }
        drainStandardError();
        if (process_.bytesAvailable() > 0) {
            const qsizetype before =
                stdoutBuffer_.size();
            const Result<void> collected =
                collectStandardOutput();
            if (!collected.hasValue()) {
                return collected;
            }
            if (stdoutBuffer_.size() > before) {
                return Result<void>::success();
            }
        }

        const int waitMilliseconds =
            waitSliceMilliseconds();
        if (waitMilliseconds <= 0) {
            return Result<void>::failure(
                timedOutError());
        }
        if (process_.waitForReadyRead(
                waitMilliseconds)) {
            drainStandardError();
            const Result<void> collected =
                collectStandardOutput();
            if (!collected.hasValue()) {
                return collected;
            }
            return Result<void>::success();
        }

        const qsizetype before =
            stdoutBuffer_.size();
        const Result<void> collected =
            collectStandardOutput();
        if (!collected.hasValue()) {
            return collected;
        }
        drainStandardError();
        if (stdoutBuffer_.size() > before) {
            return Result<void>::success();
        }
        if (process_.state()
            == QProcess::NotRunning) {
            return Result<void>::failure(
                processUnavailableError(
                    QStringLiteral(
                        "Codex app-server exited before replying.")));
        }
    }
}

Result<void> ProcessSession::collectStandardOutput()
{
    const qsizetype bufferLimit =
        maximumStandardOutputBufferBytes();
    const qsizetype remaining =
        std::max<qsizetype>(
            0, bufferLimit - stdoutBuffer_.size());
    const qint64 requested = remaining > 0
        ? std::min<qsizetype>(
              remaining, kIoReadChunkBytes)
        : 1;
    const QByteArray available =
        process_.read(requested);
    if (available.isEmpty()) {
        return Result<void>::success();
    }
    if (remaining == 0) {
        return Result<void>::failure(
            invalidResponseError(
                transport_
                        == Transport::ProxyWebSocket
                    && !webSocketUpgraded_
                    ? QStringLiteral(
                          "Codex app-server returned an oversized WebSocket handshake.")
                    : QStringLiteral(
                          "Codex app-server output exceeded the bounded transport buffer.")));
    }
    stdoutBuffer_.append(available);
    return Result<void>::success();
}

Result<void> ProcessSession::writeBytes(
    const QByteArray& bytes)
{
    const Result<void> notCanceled =
        checkCancellation();
    if (!notCanceled.hasValue()) {
        return notCanceled;
    }
    if (process_.state() == QProcess::NotRunning) {
        return Result<void>::failure(
            withWriteCertainty(
                processUnavailableError(
                    QStringLiteral(
                        "Codex app-server is not running.")),
                false));
    }

    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const Result<void> stillNotCanceled =
            checkCancellation();
        if (!stillNotCanceled.hasValue()) {
            return stillNotCanceled;
        }
        drainStandardError();
        const qint64 written = process_.write(
            bytes.constData() + offset,
            bytes.size() - offset);
        if (written < 0) {
            return Result<void>::failure(
                withWriteCertainty(
                    processUnavailableError(
                        QStringLiteral(
                            "Could not write to Codex app-server.")),
                    offset > 0));
        }
        if (written == 0) {
            const int waitMilliseconds =
                waitSliceMilliseconds();
            if (waitMilliseconds <= 0) {
                return Result<void>::failure(
                    withWriteCertainty(
                        timedOutError(),
                        offset > 0));
            }
            if (!process_.waitForBytesWritten(
                    waitMilliseconds)
                && process_.state()
                    == QProcess::NotRunning) {
                return Result<void>::failure(
                    withWriteCertainty(
                        processUnavailableError(
                            QStringLiteral(
                                "Codex app-server stopped accepting input.")),
                        offset > 0));
            }
            continue;
        }
        offset += static_cast<qsizetype>(written);
    }
    while (process_.bytesToWrite() > 0) {
        const Result<void> stillNotCanceled =
            checkCancellation();
        if (!stillNotCanceled.hasValue()) {
            return stillNotCanceled;
        }
        drainStandardError();
        const int waitMilliseconds =
            waitSliceMilliseconds();
        if (waitMilliseconds <= 0) {
            return Result<void>::failure(
                withWriteCertainty(
                    timedOutError(),
                    true));
        }
        if (!process_.waitForBytesWritten(
                waitMilliseconds)
            && process_.state()
                == QProcess::NotRunning) {
            return Result<void>::failure(
                withWriteCertainty(
                    processUnavailableError(
                        QStringLiteral(
                            "Codex app-server stopped accepting input.")),
                    true));
        }
    }
    return Result<void>::success();
}

Result<void> ProcessSession::sendWebSocketFrame(
    quint8 opcode,
    const QByteArray& payload)
{
    return writeBytes(
        maskedClientFrame(opcode, payload));
}

Result<void> ProcessSession::processWebSocketFrame(
    QJsonObject* message,
    bool* hasMessage)
{
    *hasMessage = false;
    if (stdoutBuffer_.size() < 2) {
        return Result<void>::success();
    }

    const quint8 first =
        static_cast<quint8>(stdoutBuffer_.at(0));
    const quint8 second =
        static_cast<quint8>(stdoutBuffer_.at(1));
    const quint8 opcode = first & 0x0f;
    const bool final = (first & 0x80) != 0;
    if ((first & 0x70) != 0) {
        return Result<void>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server returned a WebSocket frame with unsupported reserved bits.")));
    }
    if ((second & 0x80) != 0) {
        return Result<void>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server returned an invalid masked WebSocket frame.")));
    }
    quint64 payloadLength = second & 0x7f;
    const bool controlFrame = opcode >= 0x8;
    if (controlFrame
        && (!final || payloadLength > 125)) {
        return Result<void>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server returned an invalid WebSocket control frame.")));
    }
    qsizetype cursor = 2;
    if (payloadLength == 126) {
        if (stdoutBuffer_.size() < cursor + 2) {
            return Result<void>::success();
        }
        payloadLength =
            (static_cast<quint64>(
                 static_cast<quint8>(
                     stdoutBuffer_.at(cursor)))
             << 8)
            | static_cast<quint8>(
                stdoutBuffer_.at(cursor + 1));
        cursor += 2;
    } else if (payloadLength == 127) {
        if (stdoutBuffer_.size() < cursor + 8) {
            return Result<void>::success();
        }
        payloadLength = 0;
        for (int index = 0; index < 8; ++index) {
            payloadLength =
                (payloadLength << 8)
                | static_cast<quint8>(
                    stdoutBuffer_.at(cursor + index));
        }
        cursor += 8;
    }
    if (payloadLength
        > static_cast<quint64>(kMaximumMessageBytes)) {
        return Result<void>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server WebSocket message is too large.")));
    }

    const quint64 required =
        static_cast<quint64>(cursor) + payloadLength;
    if (required
        > static_cast<quint64>(
            std::numeric_limits<qsizetype>::max())
        || stdoutBuffer_.size()
            < static_cast<qsizetype>(required)) {
        return Result<void>::success();
    }

    QByteArray payload = stdoutBuffer_.sliced(
        cursor, static_cast<qsizetype>(payloadLength));
    stdoutBuffer_.remove(
        0, static_cast<qsizetype>(required));

    if (opcode == kPingOpcode) {
        return sendWebSocketFrame(kPongOpcode, payload);
    }
    if (opcode == kPongOpcode) {
        return Result<void>::success();
    }
    if (opcode == kCloseOpcode) {
        if (payload.size() == 1) {
            return Result<void>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server returned an invalid WebSocket close frame.")));
        }
        return Result<void>::failure(
            processUnavailableError(
                QStringLiteral(
                    "Codex app-server closed the proxy connection.")));
    }
    if (opcode == kBinaryOpcode) {
        return Result<void>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server returned an unsupported binary WebSocket message.")));
    }
    if (opcode == kTextOpcode) {
        if (fragmentedText_) {
            return Result<void>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server started a nested fragmented WebSocket message.")));
        }
        if (final) {
            const Result<QJsonObject> parsed =
                parseJsonObject(payload);
            if (!parsed.hasValue()) {
                return Result<void>::failure(
                    parsed.error());
            }
            *message = parsed.value();
            *hasMessage = true;
            return Result<void>::success();
        }
        fragmentedText_ = true;
        fragmentedPayload_ = std::move(payload);
        return Result<void>::success();
    }
    if (opcode == kContinuationOpcode) {
        if (!fragmentedText_) {
            return Result<void>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server returned an unexpected WebSocket continuation.")));
        }
        fragmentedPayload_.append(payload);
        if (fragmentedPayload_.size()
            > kMaximumMessageBytes) {
            return Result<void>::failure(
                invalidResponseError(
                    QStringLiteral(
                        "Codex app-server fragmented WebSocket message is too large.")));
        }
        if (!final) {
            return Result<void>::success();
        }
        const Result<QJsonObject> parsed =
            parseJsonObject(fragmentedPayload_);
        fragmentedText_ = false;
        fragmentedPayload_.clear();
        if (!parsed.hasValue()) {
            return Result<void>::failure(
                parsed.error());
        }
        *message = parsed.value();
        *hasMessage = true;
        return Result<void>::success();
    }

    return Result<void>::failure(
        invalidResponseError(
            QStringLiteral(
                "Codex app-server returned an unsupported WebSocket frame.")));
}

Result<RpcResponse> ProcessSession::parseResponse(
    const QJsonObject& message) const
{
    if (message.contains(QStringLiteral("error"))) {
        return Result<RpcResponse>::success({
            {},
            serverErrorText(
                message.value(QStringLiteral("error"))),
            true,
        });
    }
    return Result<RpcResponse>::success({
        message.value(QStringLiteral("result")),
        {},
        false,
    });
}

Result<bool> ProcessSession::captureServerMessage(
    const QJsonObject& message)
{
    const QString method =
        message
            .value(QStringLiteral("method"))
            .toString()
            .trimmed();
    if (method.isEmpty()) {
        return Result<bool>::success(false);
    }

    const QJsonValue id =
        message.value(QStringLiteral("id"));
    if (id.isUndefined() || id.isNull()) {
        return Result<bool>::success(true);
    }
    if (!id.isDouble() && !id.isString()) {
        return Result<bool>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server request contained an invalid ID.")));
    }
    if (serverRequests_.size()
        >= kMaximumServerRequests) {
        return Result<bool>::failure(
            invalidResponseError(
                QStringLiteral(
                    "Codex app-server returned too many pending server requests.")));
    }
    serverRequests_.append(message);
    return Result<bool>::success(true);
}

CompanionError
ProcessSession::processUnavailableError(
    const QString& message)
{
    QVariantMap context{
        {
            QStringLiteral("transport"),
            transportName(transport_),
        },
    };
    if (process_.state() == QProcess::NotRunning) {
        context.insert(
            QStringLiteral("exitCode"),
            process_.exitCode());
    }
    const QString stderrText = stderrDetail();
    if (!stderrText.isEmpty()) {
        context.insert(
            QStringLiteral("stderr"), stderrText);
    }
    return {
        QStringLiteral(
            "codex.app_server_process_exited"),
        message,
        true,
        std::move(context),
    };
}

CompanionError ProcessSession::timedOutError() const
{
    return {
        QStringLiteral("codex.app_server_timed_out"),
        QStringLiteral(
            "Codex app-server did not respond in time."),
        true,
        {
            {
                QStringLiteral("transport"),
                transportName(transport_),
            },
        },
    };
}

CompanionError
ProcessSession::invalidResponseError(
    const QString& message) const
{
    return {
        QStringLiteral(
            "codex.app_server_invalid_response"),
        message,
        false,
        {
            {
                QStringLiteral("transport"),
                transportName(transport_),
            },
            {
                QStringLiteral("bufferedBytes"),
                static_cast<qlonglong>(
                    stdoutBuffer_.size()),
            },
            {
                QStringLiteral("bufferLimitBytes"),
                static_cast<qlonglong>(
                    maximumStandardOutputBufferBytes()),
            },
        },
    };
}

CompanionError
ProcessSession::operationCanceledError() const
{
    return {
        QStringLiteral(
            "codex.operation_canceled"),
        QStringLiteral(
            "The Codex operation was canceled."),
        false,
        {},
    };
}

Result<void> ProcessSession::checkCancellation()
{
    if (!stopToken_.stop_requested()) {
        return Result<void>::success();
    }
    process_.closeWriteChannel();
    cancelProcess();
    return Result<void>::failure(
        operationCanceledError());
}

QString ProcessSession::stderrDetail()
{
    drainStandardError();
    return QString::fromLocal8Bit(
               stderrBuffer_)
        .trimmed();
}

void ProcessSession::drainStandardError()
{
    const QProcess::ProcessChannel previous =
        process_.readChannel();
    process_.setReadChannel(
        QProcess::StandardError);
    drainStandardErrorSource(
        [this] {
            return process_.bytesAvailable();
        },
        [this](qint64 maximumBytes) {
            return process_.read(maximumBytes);
        });
    process_.setReadChannel(previous);
}

qsizetype
ProcessSession::maximumStandardOutputBufferBytes() const
{
    if (transport_ == Transport::ProxyWebSocket) {
        return webSocketUpgraded_
            ? kMaximumWebSocketBufferBytes
            : kMaximumHandshakeBytes;
    }
    return kMaximumStdioBufferBytes;
}

int ProcessSession::remainingMilliseconds() const
{
    const qint64 remaining =
        deadline_.remainingTime();
    if (remaining <= 0) {
        return 0;
    }
    return static_cast<int>(
        std::min<qint64>(
            remaining,
            std::numeric_limits<int>::max()));
}

int ProcessSession::waitSliceMilliseconds() const
{
    return std::min(
        remainingMilliseconds(),
        kCancellationWaitSliceMilliseconds);
}

void ProcessSession::cancelProcess()
{
    NativeHandle nativeProcess(
        reinterpret_cast<HANDLE>(
            std::exchange(
                nativeProcessHandle_,
                static_cast<quintptr>(0))));
    if (process_.state()
        == QProcess::NotRunning) {
        drainStandardError();
        return;
    }
    if (!nativeProcess.valid()) {
        nativeProcess.reset(OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE,
            static_cast<DWORD>(
                process_.processId())));
    }
    if (nativeProcess.valid()) {
        TerminateProcess(
            nativeProcess.get(),
            ERROR_PROCESS_ABORTED);
        drainStandardError();
        HANDLE reaperProcess =
            nativeProcess.release();
        if (startNativeProcessReaper(
                reaperProcess)) {
            process_.releaseToNativeReaper();
            return;
        }
        CloseHandle(reaperProcess);
    }
    process_.kill();
    drainStandardError();
    terminateProcess();
}

void ProcessSession::terminateProcess()
{
    drainStandardError();
    NativeHandle nativeProcess(
        reinterpret_cast<HANDLE>(
            std::exchange(
                nativeProcessHandle_,
                static_cast<quintptr>(0))));
    if (process_.state() == QProcess::NotRunning) {
        return;
    }
    if (!nativeProcess.valid()) {
        nativeProcess.reset(OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE,
            static_cast<DWORD>(
                process_.processId())));
    }
    process_.terminate();
    QDeadlineTimer graceDeadline(std::min(
        kTerminationGraceMilliseconds,
        remainingMilliseconds()));
    while (process_.state()
           != QProcess::NotRunning) {
        const qint64 remaining =
            graceDeadline.remainingTime();
        if (remaining <= 0) {
            break;
        }
        process_.waitForFinished(
            static_cast<int>(
                std::min<qint64>(
                    remaining,
                    kCancellationWaitSliceMilliseconds)));
        drainStandardError();
    }
    if (process_.state()
        == QProcess::NotRunning) {
        return;
    }
    process_.kill();
    QDeadlineTimer reapDeadline(
        kForcedKillReapMilliseconds);
    while (process_.state() != QProcess::NotRunning) {
        const qint64 remaining =
            reapDeadline.remainingTime();
        if (remaining <= 0) {
            break;
        }
        process_.waitForFinished(
            static_cast<int>(
                std::min<qint64>(
                    remaining,
                    kCancellationWaitSliceMilliseconds)));
        drainStandardError();
    }
    if (process_.state() != QProcess::NotRunning
        && nativeProcess.valid()) {
        HANDLE reaperProcess =
            nativeProcess.release();
        if (startNativeProcessReaper(
                reaperProcess)) {
            process_.releaseToNativeReaper();
        } else {
            CloseHandle(reaperProcess);
            while (process_.state()
                   != QProcess::NotRunning) {
                process_.kill();
                process_.waitForFinished(
                    kCancellationWaitSliceMilliseconds);
                drainStandardError();
            }
        }
    }
    drainStandardError();
}

QJsonObject initializeRequest(
    const QString& clientName,
    const QString& clientTitle,
    const QString& version)
{
    return {
        {QStringLiteral("id"), 1},
        {
            QStringLiteral("method"),
            QStringLiteral("initialize"),
        },
        {
            QStringLiteral("params"),
            QJsonObject{
                {
                    QStringLiteral("clientInfo"),
                    QJsonObject{
                        {
                            QStringLiteral("name"),
                            clientName,
                        },
                        {
                            QStringLiteral("title"),
                            clientTitle,
                        },
                        {
                            QStringLiteral("version"),
                            version,
                        },
                    },
                },
                {
                    QStringLiteral("capabilities"),
                    QJsonObject{
                        {
                            QStringLiteral(
                                "experimentalApi"),
                            true,
                        },
                        {
                            QStringLiteral(
                                "optOutNotificationMethods"),
                            QJsonArray{},
                        },
                    },
                },
            },
        },
    };
}

QString windowsClientVersion(
    QString applicationVersion)
{
    applicationVersion =
        applicationVersion.trimmed();
    if (applicationVersion.isEmpty()) {
        applicationVersion =
            QStringLiteral("0");
    }
    if (applicationVersion.endsWith(
            QStringLiteral("-windows"),
            Qt::CaseInsensitive)) {
        return applicationVersion;
    }
    return applicationVersion
        + QStringLiteral("-windows");
}

QJsonObject initializedNotification()
{
    return {
        {
            QStringLiteral("method"),
            QStringLiteral("initialized"),
        },
    };
}

Result<int> numericResponseId(
    const QJsonObject& message)
{
    const QJsonValue value =
        message.value(QStringLiteral("id"));
    if (value.isDouble()) {
        const double number = value.toDouble();
        const int id = value.toInt(
            std::numeric_limits<int>::min());
        if (number == static_cast<double>(id)) {
            return Result<int>::success(id);
        }
    } else if (value.isString()) {
        bool ok = false;
        const int id = value.toString().toInt(&ok);
        if (ok) {
            return Result<int>::success(id);
        }
    }
    return Result<int>::failure({
        QStringLiteral(
            "codex.app_server_invalid_response_id"),
        QStringLiteral(
            "Codex app-server response did not contain a numeric ID."),
        false,
        {},
    });
}

} // namespace companion::appserver_detail
