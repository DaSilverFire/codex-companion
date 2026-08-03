#include "codex/appserver/AppServerRpcClient.h"
#include "codex/appserver/TaskCreator.h"
#include "codex/attachments/AttachmentStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "codex/appserver/AppServerProcess.h"

using namespace companion;

namespace companion::appserver_detail {

struct StderrDrainObservation final {
    qsizetype readBytes = 0;
    qsizetype retainedBytes = 0;
};

class ProcessSessionTestAccess final {
public:
    static Result<bool> waitForStartup(
        std::stop_token stopToken,
        const std::function<QProcess::ProcessState()>&
            stateProvider,
        const std::function<bool(int)>& waitForStarted)
    {
        ProcessSession session(
            QProcessEnvironment::systemEnvironment(),
            Transport::StdioJsonLines,
            5000,
            stopToken);
        return session.waitForStartup(
            stateProvider,
            waitForStarted);
    }

    static Result<bool> waitForExpiredStartup()
    {
        ProcessSession session(
            QProcessEnvironment::systemEnvironment(),
            Transport::StdioJsonLines,
            5000,
            {});
        session.deadline_ = QDeadlineTimer(0);
        return session.waitForStartup(
            [] {
                return QProcess::Starting;
            },
            [](int) {
                return false;
            });
    }

    static StderrDrainObservation
    drainContinuousStandardError()
    {
        ProcessSession session(
            QProcessEnvironment::systemEnvironment(),
            Transport::StdioJsonLines,
            5000,
            {});
        StderrDrainObservation observation;
        session.drainStandardErrorSource(
            [&observation] {
                return observation.readBytes
                        < 2 * 1024 * 1024
                    ? static_cast<qint64>(
                          64 * 1024)
                    : static_cast<qint64>(0);
            },
            [&observation](qint64 maximumBytes) {
                observation.readBytes +=
                    static_cast<qsizetype>(
                        maximumBytes);
                return QByteArray(
                    maximumBytes,
                    'e');
            });
        observation.retainedBytes =
            session.stderrBuffer_.size();
        return observation;
    }
};

} // namespace companion::appserver_detail

namespace {

constexpr auto kModeVariable =
    "COMPANION_TASK_CREATOR_HELPER_MODE";
constexpr auto kTranscriptVariable =
    "COMPANION_TASK_CREATOR_TRANSCRIPT";
constexpr auto kLaunchesVariable =
    "COMPANION_TASK_CREATOR_LAUNCHES";
constexpr auto kPidVariable =
    "COMPANION_TASK_CREATOR_PID";
constexpr auto kDebugVariable =
    "COMPANION_TASK_CREATOR_DEBUG";
constexpr qsizetype kOversizedHandshakeBytes =
    32 * 1024 + 1024;
constexpr qsizetype kOversizedMessageBytes =
    4 * 1024 * 1024 + 1024;

struct ClientFrame final {
    quint8 opcode = 0;
    bool final = false;
    QByteArray payload;
};

QString currentExecutablePath()
{
    return QDir::fromNativeSeparators(
        QCoreApplication::applicationFilePath());
}

bool writeAll(QFile& file, const QByteArray& data)
{
    qsizetype offset = 0;
    while (offset < data.size()) {
        const qint64 written =
            file.write(data.constData() + offset, data.size() - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    return file.flush();
}

bool writeAll(HANDLE handle, const QByteArray& data)
{
    qsizetype offset = 0;
    while (offset < data.size()) {
        const DWORD requested = static_cast<DWORD>(
            qMin<qsizetype>(
                data.size() - offset,
                std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                handle,
                data.constData() + offset,
                requested,
                &written,
                nullptr)
            || written == 0) {
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    return true;
}

DWORD WINAPI writeContinuousStandardError(void*)
{
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error == nullptr
        || error == INVALID_HANDLE_VALUE) {
        return 1;
    }

    const QByteArray chunk(64 * 1024, 'e');
    while (writeAll(error, chunk)) {
    }
    return 0;
}

bool startContinuousStandardError()
{
    const HANDLE thread = CreateThread(
        nullptr,
        0,
        &writeContinuousStandardError,
        nullptr,
        0,
        nullptr);
    if (thread == nullptr) {
        return false;
    }
    CloseHandle(thread);
    return true;
}

std::optional<QByteArray> readExactly(
    HANDLE handle,
    qsizetype size)
{
    QByteArray result(size, Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < size) {
        const DWORD requested = static_cast<DWORD>(
            qMin<qsizetype>(
                size - offset,
                std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(
                handle,
                result.data() + offset,
                requested,
                &read,
                nullptr)
            || read == 0) {
            return std::nullopt;
        }
        offset += static_cast<qsizetype>(read);
    }
    return result;
}

std::optional<QByteArray> readThrough(
    HANDLE handle,
    const QByteArray& terminator,
    qsizetype maximumBytes)
{
    QByteArray result;
    while (!result.endsWith(terminator)) {
        const auto byte = readExactly(handle, 1);
        if (!byte.has_value()) {
            return std::nullopt;
        }
        result.append(*byte);
        if (result.size() > maximumBytes) {
            return std::nullopt;
        }
    }
    return result;
}

QByteArray headerValue(
    const QByteArray& header,
    const QByteArray& name)
{
    const QByteArray expected =
        name.trimmed().toLower();
    for (QByteArray line : header.split('\n')) {
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

QByteArray webSocketAccept(const QByteArray& key)
{
    return QCryptographicHash::hash(
               key
                   + QByteArray(
                       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
               QCryptographicHash::Sha1)
        .toBase64();
}

QByteArray serverFrame(
    quint8 opcode,
    const QByteArray& payload,
    bool final = true)
{
    QByteArray frame;
    frame.append(static_cast<char>(
        (final ? 0x80 : 0x00) | opcode));
    if (payload.size() < 126) {
        frame.append(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>(
            (payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.append(static_cast<char>(127));
        const quint64 length =
            static_cast<quint64>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.append(static_cast<char>(
                (length >> shift) & 0xff));
        }
    }
    frame.append(payload);
    return frame;
}

QByteArray maskedServerFrame(
    quint8 opcode,
    const QByteArray& payload)
{
    constexpr std::array<char, 4> mask{
        '\x11', '\x22', '\x33', '\x44'};
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | opcode));
    frame.append(static_cast<char>(
        0x80 | payload.size()));
    frame.append(mask.data(), mask.size());
    for (qsizetype index = 0;
         index < payload.size();
         ++index) {
        frame.append(static_cast<char>(
            static_cast<quint8>(payload.at(index))
            ^ static_cast<quint8>(
                mask.at(
                    static_cast<std::size_t>(
                        index % mask.size())))));
    }
    return frame;
}

std::optional<ClientFrame> readClientFrame(HANDLE input)
{
    const auto header = readExactly(input, 2);
    if (!header.has_value()) {
        return std::nullopt;
    }

    const quint8 first =
        static_cast<quint8>(header->at(0));
    const quint8 second =
        static_cast<quint8>(header->at(1));
    quint64 payloadLength = second & 0x7f;
    if (payloadLength == 126) {
        const auto extended = readExactly(input, 2);
        if (!extended.has_value()) {
            return std::nullopt;
        }
        payloadLength =
            (static_cast<quint64>(
                 static_cast<quint8>(extended->at(0)))
             << 8)
            | static_cast<quint8>(extended->at(1));
    } else if (payloadLength == 127) {
        const auto extended = readExactly(input, 8);
        if (!extended.has_value()) {
            return std::nullopt;
        }
        payloadLength = 0;
        for (const char byte : *extended) {
            payloadLength =
                (payloadLength << 8)
                | static_cast<quint8>(byte);
        }
    }
    if ((second & 0x80) == 0
        || payloadLength
            > static_cast<quint64>(4 * 1024 * 1024)) {
        return std::nullopt;
    }

    const auto mask = readExactly(input, 4);
    const auto encoded = readExactly(
        input, static_cast<qsizetype>(payloadLength));
    if (!mask.has_value() || !encoded.has_value()) {
        return std::nullopt;
    }

    QByteArray payload = *encoded;
    for (qsizetype index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(
            static_cast<quint8>(payload.at(index))
            ^ static_cast<quint8>(
                mask->at(index % mask->size())));
    }
    return ClientFrame{
        static_cast<quint8>(first & 0x0f),
        (first & 0x80) != 0,
        std::move(payload),
    };
}

bool appendLine(const QString& path, const QByteArray& line)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly
                   | QIODevice::Append
                   | QIODevice::Text)) {
        return false;
    }
    return writeAll(file, line + '\n');
}

int recordLaunch(const QString& transport)
{
    const QString path =
        qEnvironmentVariable(kLaunchesVariable);
    if (path.isEmpty()) {
        return 0;
    }

    QFile file(path);
    int launchIndex = 0;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            if (!file.readLine().trimmed().isEmpty()) {
                ++launchIndex;
            }
        }
    }
    return appendLine(path, transport.toUtf8())
        ? launchIndex
        : -1;
}

bool recordPid()
{
    const QString path = qEnvironmentVariable(kPidVariable);
    return path.isEmpty()
        || appendLine(
            path,
            QByteArray::number(
                QCoreApplication::applicationPid()));
}

bool recordStage(const QByteArray& stage)
{
    const QString path =
        qEnvironmentVariable(kDebugVariable);
    return path.isEmpty() || appendLine(path, stage);
}

bool recordMessage(const QJsonObject& message)
{
    const QString path =
        qEnvironmentVariable(kTranscriptVariable);
    if (path.isEmpty()) {
        return true;
    }
    return appendLine(
        path,
        QJsonDocument(message).toJson(QJsonDocument::Compact));
}

std::optional<QJsonObject> parseObject(const QByteArray& data)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError
        || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

int numericId(const QJsonObject& object)
{
    const QJsonValue value =
        object.value(QStringLiteral("id"));
    if (value.isDouble()) {
        return value.toInt(-1);
    }
    if (value.isString()) {
        bool ok = false;
        const int id = value.toString().toInt(&ok);
        return ok ? id : -1;
    }
    return -1;
}

bool writeProxyResponse(
    HANDLE output,
    const QJsonObject& response,
    bool fragmented = false)
{
    const QByteArray payload =
        QJsonDocument(response).toJson(QJsonDocument::Compact);
    if (!fragmented || payload.size() < 4) {
        return writeAll(output, serverFrame(0x1, payload));
    }

    const qsizetype split = payload.size() / 2;
    return writeAll(
               output,
               serverFrame(0x1, payload.first(split), false))
        && writeAll(
            output,
            serverFrame(0x0, payload.sliced(split), true));
}

bool writeStdioResponse(
    QFile& output,
    const QJsonObject& response)
{
    return writeAll(
        output,
        QJsonDocument(response).toJson(QJsonDocument::Compact)
            + '\n');
}

QJsonObject taskResponse(
    const QJsonObject& request,
    const QString& mode)
{
    const int id = numericId(request);
    const QString method =
        request.value(QStringLiteral("method")).toString();
    if (id == 1) {
        if (mode
            == QStringLiteral(
                "invalid-response-id")) {
            return {
                {QStringLiteral("id"), 1.5},
                {QStringLiteral("result"), QJsonObject{}},
            };
        }
        return {
            {QStringLiteral("id"), QStringLiteral("1")},
            {QStringLiteral("result"), QJsonObject{}},
        };
    }
    if (method == QStringLiteral("thread/start")) {
        if (mode == QStringLiteral("malformed-thread")) {
            return {
                {QStringLiteral("id"), id},
                {
                    QStringLiteral("result"),
                    QJsonObject{
                        {
                            QStringLiteral("thread"),
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("   "),
                                },
                            },
                        },
                    },
                },
            };
        }
        return {
            {QStringLiteral("id"), id},
            {
                QStringLiteral("result"),
                QJsonObject{
                    {
                        QStringLiteral("thread"),
                        QJsonObject{
                            {
                                QStringLiteral("id"),
                                id == 2
                                    ? QStringLiteral(
                                          " thread-created ")
                                    : QStringLiteral(
                                          " thread-created-%1 ")
                                          .arg(id),
                            },
                        },
                    },
                },
            },
        };
    }
    if (method == QStringLiteral("turn/start")
        && mode == QStringLiteral("turn-error")) {
        return {
            {QStringLiteral("id"), id},
            {
                QStringLiteral("error"),
                QJsonObject{
                    {
                        QStringLiteral("message"),
                        QStringLiteral("turn rejected"),
                    },
                },
            },
        };
    }
    if (method == QStringLiteral("turn/start")
        && mode == QStringLiteral("null-turn-ack")) {
        return {
            {QStringLiteral("id"), id},
            {
                QStringLiteral("result"),
                QJsonValue(QJsonValue::Null),
            },
        };
    }
    return {
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("result"), QJsonObject{}},
    };
}

QJsonObject approvalRequest()
{
    return {
        {QStringLiteral("id"), 77},
        {
            QStringLiteral("method"),
            QStringLiteral(
                "item/commandExecution/requestApproval"),
        },
        {
            QStringLiteral("params"),
            QJsonObject{
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-created"),
                },
            },
        },
    };
}

int runProxyHelper(const QString& mode)
{
    const int launchIndex =
        recordLaunch(QStringLiteral("proxy"));
    if (launchIndex < 0
        || !recordPid()
        || !recordStage("launched")) {
        return 90;
    }
    if (mode == QStringLiteral("exit")
        || (mode
                == QStringLiteral(
                    "exit-first-then-success")
            && launchIndex == 0)
        || (mode
                == QStringLiteral(
                    "rpc-exit-first-then-success")
            && launchIndex == 0)) {
        return 23;
    }
    if (mode == QStringLiteral("hang")
        || mode
            == QStringLiteral(
                "proxy-hang-then-stdio-success")) {
        QThread::sleep(60);
        return 24;
    }

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == nullptr
        || input == INVALID_HANDLE_VALUE
        || output == nullptr
        || output == INVALID_HANDLE_VALUE
        || !recordStage("stdio-open")) {
        return 91;
    }

    const auto handshake =
        readThrough(input, QByteArray("\r\n\r\n"), 32 * 1024);
    if (!handshake.has_value()
        || !handshake->startsWith("GET /rpc HTTP/1.1\r\n")
        || !handshake->contains("Upgrade: websocket\r\n")) {
        recordStage(
            QByteArray("handshake-base64-")
            + (handshake.has_value()
                   ? handshake->toBase64()
                   : QByteArray("missing")));
        recordStage("handshake-invalid");
        return 92;
    }
    if (!recordStage("handshake-read")) {
        return 92;
    }
    const QByteArray requestKey =
        headerValue(
            *handshake,
            QByteArray("Sec-WebSocket-Key"));
    if (requestKey.isEmpty()) {
        return 92;
    }
    QByteArray handshakeResponse(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n");
    handshakeResponse += "Sec-WebSocket-Accept: ";
    handshakeResponse +=
        mode == QStringLiteral("invalid-websocket-accept")
        ? QByteArray("invalid")
        : webSocketAccept(requestKey);
    handshakeResponse += "\r\n";
    if (mode == QStringLiteral("oversized-handshake")) {
        handshakeResponse += "X-Padding: ";
        handshakeResponse += QByteArray(
            kOversizedHandshakeBytes, 'x');
        handshakeResponse += "\r\n";
    }
    handshakeResponse += "\r\n";
    if (!writeAll(output, handshakeResponse)
        || !writeAll(
            output,
            serverFrame(
                0x9,
                QByteArray("keepalive"),
                mode
                    != QStringLiteral(
                        "fragmented-control-frame")))) {
        recordStage("handshake-write-failed");
        return 93;
    }
    if (!recordStage("handshake-written")) {
        return 93;
    }

    bool initializeResponseSent = false;
    while (true) {
        if (!recordStage("frame-wait")) {
            return 94;
        }
        const auto frame = readClientFrame(input);
        if (!frame.has_value()) {
            recordStage("frame-eof");
            return 0;
        }
        if (!recordStage(
                QByteArray("frame-opcode-")
                + QByteArray::number(frame->opcode))) {
            return 94;
        }
        if (frame->opcode == 0xa) {
            continue;
        }
        if (frame->opcode == 0x8) {
            return 0;
        }
        if (frame->opcode != 0x1 || !frame->final) {
            return 94;
        }
        const auto request = parseObject(frame->payload);
        if (!request.has_value() || !recordMessage(*request)) {
            recordStage("frame-json-invalid");
            return 95;
        }
        if (!recordStage("message-recorded")) {
            return 95;
        }
        if (!request->contains(QStringLiteral("id"))) {
            continue;
        }
        const QJsonObject response =
            taskResponse(*request, mode);
        if (mode
                == QStringLiteral(
                    "malformed-websocket-json")
            && numericId(*request) == 1) {
            if (!writeAll(
                    output,
                    serverFrame(
                        0x1,
                        QByteArray("{not-json}")))) {
                return 96;
            }
            QThread::sleep(60);
            return 96;
        }
        const bool fragmented =
            numericId(*request) == 1
            && !std::exchange(initializeResponseSent, true);
        if (mode == QStringLiteral("masked-server-frame")) {
            const QByteArray payload =
                QJsonDocument(response).toJson(
                    QJsonDocument::Compact);
            if (!writeAll(
                    output,
                    maskedServerFrame(0x1, payload))) {
                return 96;
            }
        } else if (!writeProxyResponse(
                       output, response, fragmented)) {
            return 96;
        }
    }
}

int runStdioHelper(const QString& mode)
{
    const int launchIndex =
        recordLaunch(QStringLiteral("stdio"));
    if (launchIndex < 0 || !recordPid()) {
        return 97;
    }
    if (mode == QStringLiteral("exit")
        || (mode
                == QStringLiteral(
                    "exit-first-then-success")
            && launchIndex == 0)
        || (mode
                == QStringLiteral(
                    "rpc-exit-first-then-success")
            && launchIndex == 0)) {
        return 25;
    }
    if (mode == QStringLiteral("hang")
        || (mode
                == QStringLiteral(
                    "rpc-hang-first-then-success")
            && launchIndex == 0)) {
        QThread::sleep(60);
        return 26;
    }
    QFile input;
    QFile output;
    if (!input.open(stdin, QIODevice::ReadOnly)
        || !output.open(stdout, QIODevice::WriteOnly)) {
        return 98;
    }
    if (mode
            == QStringLiteral(
                "rpc-continuous-stderr")
        && (!startContinuousStandardError()
            || !recordStage(
                "stderr-continuous"))) {
        return 98;
    }
    if (mode == QStringLiteral("stderr-flood")) {
        const HANDLE error =
            GetStdHandle(STD_ERROR_HANDLE);
        if (error == nullptr
            || error == INVALID_HANDLE_VALUE
            || !writeAll(
                error,
                QByteArray(
                    8 * 1024 * 1024,
                    'e'))) {
            return 98;
        }
    }

    while (true) {
        const QByteArray line = input.readLine();
        if (line.isEmpty()) {
            return 0;
        }
        const auto request = parseObject(line.trimmed());
        if (!request.has_value() || !recordMessage(*request)) {
            return 99;
        }
        if (!request->contains(QStringLiteral("id"))) {
            continue;
        }
        const bool invalidFallbackResponse =
            mode == QStringLiteral(
                "oversized-handshake")
            || mode == QStringLiteral(
                "masked-server-frame")
            || mode == QStringLiteral(
                "fragmented-control-frame")
            || mode == QStringLiteral(
                "malformed-websocket-json");
        if ((mode
                 == QStringLiteral(
                     "malformed-json-line")
             || invalidFallbackResponse)
            && numericId(*request) == 1) {
            if (!writeAll(
                    output,
                    QByteArray("{not-json}\n"))) {
                return 102;
            }
            QThread::sleep(60);
            return 102;
        }
        const QString method =
            request
                ->value(QStringLiteral("method"))
                .toString();
        if (mode
                == QStringLiteral(
                    "transactional-approval-failure")
            && launchIndex == 0
            && method == QStringLiteral("turn/start")) {
            if (!writeStdioResponse(
                    output, approvalRequest())) {
                return 103;
            }
            QThread::msleep(1000);
            if (!writeAll(
                    output,
                    QByteArray("{not-json}\n"))) {
                return 103;
            }
            QThread::sleep(60);
            return 103;
        }
        if ((mode == QStringLiteral("turn-invalid")
             || (mode
                     == QStringLiteral(
                         "turn-invalid-first-then-success")
                 && launchIndex == 0))
            && method == QStringLiteral("turn/start")) {
            if (!writeAll(
                    output,
                    QByteArray("{not-json}\n"))) {
                return 103;
            }
            QThread::sleep(60);
            return 103;
        }
        if ((mode == QStringLiteral("approval-request")
             || mode
                 == QStringLiteral(
                     "approval-before-turn-failure-first-then-success"))
            && method.isEmpty()
            && numericId(*request) == 77) {
            continue;
        }
        if (mode
                == QStringLiteral(
                    "approval-before-turn-failure-first-then-success")
            && launchIndex == 0
            && method == QStringLiteral("turn/start")) {
            if (!writeStdioResponse(
                    output, approvalRequest())) {
                return 103;
            }
            if (!writeAll(
                    output,
                    QByteArray("{not-json}\n"))) {
                return 103;
            }
            const QByteArray responseLine =
                input.readLine();
            const auto response = parseObject(
                responseLine.trimmed());
            if (!response.has_value()
                || !recordMessage(*response)) {
                return 103;
            }
            QThread::sleep(60);
            return 103;
        }
        if ((mode == QStringLiteral("thread-timeout")
             || (mode
                     == QStringLiteral(
                         "thread-timeout-after-first-success")
                 && numericId(*request) >= 4))
            && method == QStringLiteral("thread/start")) {
            QThread::sleep(60);
            return 103;
        }
        if ((mode == QStringLiteral("turn-timeout")
             || (mode
                     == QStringLiteral(
                         "turn-timeout-first-then-success")
                 && launchIndex == 0)
             || (mode
                     == QStringLiteral(
                         "turn-timeout-after-first-success")
                 && numericId(*request) >= 5))
            && method == QStringLiteral("turn/start")) {
            QThread::sleep(60);
            return 103;
        }
        if (mode == QStringLiteral("oversized-json-line")
            && numericId(*request) == 1) {
            if (!writeStdioResponse(
                    output,
                    {
                        {QStringLiteral("id"), 1},
                        {
                            QStringLiteral("result"),
                            QJsonObject{
                                {
                                    QStringLiteral("padding"),
                                    QString(
                                        kOversizedMessageBytes,
                                        QLatin1Char('x')),
                                },
                            },
                        },
                    })) {
                return 102;
            }
            continue;
        }

        if (mode == QStringLiteral("rpc")
            || mode
                == QStringLiteral(
                    "rpc-numeric-server-request")
            || mode
                == QStringLiteral(
                    "rpc-response-flood")
            || mode
                == QStringLiteral(
                    "rpc-exit-first-then-success")
            || mode
                == QStringLiteral(
                    "rpc-hang-first-then-success")
            || mode
                == QStringLiteral(
                    "rpc-read-block")
            || mode
                == QStringLiteral(
                    "rpc-write-block")
            || mode
                == QStringLiteral(
                    "rpc-continuous-stderr")) {
            const int id = numericId(*request);
            if (id == 1) {
                if (!writeStdioResponse(
                        output,
                        {
                            {
                                QStringLiteral("id"),
                                QStringLiteral("1"),
                            },
                            {
                                QStringLiteral("result"),
                                QJsonObject{},
                            },
                        })) {
                    return 100;
                }
                if (mode
                    == QStringLiteral(
                        "rpc-write-block")) {
                    if (!recordStage(
                            "write-block")) {
                        return 100;
                    }
                    QThread::sleep(60);
                    return 100;
                }
                continue;
            }
            if (mode
                == QStringLiteral(
                    "rpc-continuous-stderr")) {
                if (!recordStage(
                        "stderr-read-block")) {
                    return 101;
                }
                QThread::sleep(60);
                return 101;
            }
            if (mode
                == QStringLiteral(
                    "rpc-read-block")) {
                if (!recordStage("read-block")) {
                    return 101;
                }
                QThread::sleep(60);
                return 101;
            }
            if (mode
                == QStringLiteral(
                    "rpc-numeric-server-request")
                && !writeStdioResponse(
                    output,
                    {
                        {QStringLiteral("id"), id},
                        {
                            QStringLiteral("method"),
                            QStringLiteral(
                                "server/request"),
                        },
                        {
                            QStringLiteral("params"),
                            QJsonObject{},
                        },
                    })) {
                return 101;
            }
            if (mode
                == QStringLiteral(
                    "rpc-response-flood")) {
                for (int unexpectedId = 1000;
                     unexpectedId < 1129;
                     ++unexpectedId) {
                    if (!writeStdioResponse(
                            output,
                            {
                                {
                                    QStringLiteral("id"),
                                    unexpectedId,
                                },
                                {
                                    QStringLiteral("result"),
                                    QJsonObject{},
                                },
                            })) {
                        return 101;
                    }
                }
            }
            if (!writeStdioResponse(
                    output,
                    {
                        {
                            QStringLiteral("method"),
                            QStringLiteral("server/notice"),
                        },
                    })
                || !writeStdioResponse(
                    output,
                    {
                        {
                            QStringLiteral("id"),
                            QString::number(id),
                        },
                        {
                            QStringLiteral("result"),
                            QJsonObject{
                                {
                                    QStringLiteral("method"),
                                    request->value(
                                        QStringLiteral("method")),
                                },
                            },
                        },
                    })) {
                return 101;
            }
            continue;
        }

        if (!writeStdioResponse(
                output,
                taskResponse(*request, mode))) {
            return 102;
        }
        if (mode == QStringLiteral("approval-request")
            && method == QStringLiteral("turn/start")
            && !writeStdioResponse(
                output, approvalRequest())) {
            return 103;
        }
        if (mode
                == QStringLiteral(
                    "transactional-approval-failure")
            && launchIndex > 0
            && method == QStringLiteral("turn/start")
            && !writeStdioResponse(
                output, approvalRequest())) {
            return 103;
        }
    }
}

int runHelper(const QStringList& arguments)
{
    if (arguments.size() < 3
        || arguments.at(1) != QStringLiteral("app-server")) {
        return -1;
    }
    const QString mode = qEnvironmentVariable(
        kModeVariable, QStringLiteral("success"));
    if (arguments.at(2) == QStringLiteral("proxy")) {
        return runProxyHelper(mode);
    }
    if (arguments.contains(QStringLiteral("--listen"))
        && arguments.contains(QStringLiteral("stdio://"))) {
        return runStdioHelper(mode);
    }
    return 89;
}

QProcessEnvironment helperEnvironment(
    const QString& mode,
    const QString& transcript,
    const QString& launches,
    const QString& pid = {})
{
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QString::fromLatin1(kModeVariable), mode);
    environment.insert(
        QString::fromLatin1(kTranscriptVariable), transcript);
    environment.insert(
        QString::fromLatin1(kLaunchesVariable), launches);
    environment.insert(
        QString::fromLatin1(kDebugVariable),
        transcript + QStringLiteral(".debug"));
    if (!pid.isEmpty()) {
        environment.insert(
            QString::fromLatin1(kPidVariable), pid);
    }
    return environment;
}

QVector<QJsonObject> readJsonLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QVector<QJsonObject> result;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const auto object = parseObject(line);
        if (!object.has_value()) {
            return {};
        }
        result.append(*object);
    }
    return result;
}

QStringList readTextLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QStringList result;
    while (!file.atEnd()) {
        const QString line =
            QString::fromUtf8(file.readLine()).trimmed();
        if (!line.isEmpty()) {
            result.append(line);
        }
    }
    return result;
}

QVector<QJsonObject> taskFixture()
{
    return readJsonLines(
        QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral(
            "/codex-v034/app-server-task-transcript.jsonl"));
}

QVector<StagedAttachment> taskAttachments()
{
    return {
        {
            QUuid(
                QStringLiteral(
                    "00000000-0000-0000-0000-000000000001")),
            AttachmentKind::Image,
            QStringLiteral("reference.png"),
            QStringLiteral("C:\\staged\\reference.png"),
            QStringLiteral("C:\\staged\\reference.png"),
            QStringLiteral("image/png"),
        },
        {
            QUuid(
                QStringLiteral(
                    "00000000-0000-0000-0000-000000000002")),
            AttachmentKind::File,
            QStringLiteral("design.md"),
            QStringLiteral("C:\\staged\\design.md"),
            QStringLiteral("C:\\staged\\design.md"),
            QStringLiteral("text/markdown"),
        },
    };
}

CreateTaskRequest fullTaskRequest()
{
    return {
        QStringLiteral("  Build the Windows bridge  "),
        QStringLiteral("  C:\\repo  "),
        QStringLiteral("  gpt-5.6  "),
        QStringLiteral("  high  "),
        QStringLiteral("  design-and-build  "),
        QStringLiteral(
            "  C:\\skills\\design-and-build\\SKILL.md  "),
        taskAttachments(),
        QStringLiteral("message-stable"),
    };
}

TaskCreator creator(
    const QVector<QString>& candidates,
    bool sharedDaemonAvailable,
    const QProcessEnvironment& environment,
    int timeoutMilliseconds =
        TaskCreator::kDefaultTimeoutMilliseconds)
{
    return TaskCreator(
        [candidates] { return candidates; },
        [sharedDaemonAvailable] {
            return sharedDaemonAvailable;
        },
        environment,
        timeoutMilliseconds);
}

bool processIsAlive(quint32 processId)
{
    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        processId);
    if (process == nullptr) {
        return false;
    }
    DWORD exitCode = 0;
    const bool alive =
        GetExitCodeProcess(process, &exitCode)
        && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

bool terminateProcessForTest(quint32 processId)
{
    const HANDLE process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        processId);
    if (process == nullptr) {
        return false;
    }
    const bool terminated =
        TerminateProcess(
            process,
            ERROR_PROCESS_ABORTED)
        != FALSE;
    const DWORD waitResult =
        WaitForSingleObject(process, 3000);
    CloseHandle(process);
    return terminated
        && waitResult == WAIT_OBJECT_0;
}

bool waitForCompletion(
    const std::atomic_bool& completed,
    int timeoutMilliseconds)
{
    QDeadlineTimer deadline(timeoutMilliseconds);
    while (!completed.load(
        std::memory_order_acquire)
        && deadline.remainingTime() > 0) {
        QThread::msleep(5);
    }
    return completed.load(
        std::memory_order_acquire);
}

} // namespace

class AppServerTaskCreatorTests final : public QObject {
    Q_OBJECT

private slots:
    void windowsClientVersionUsesApplicationVersion()
    {
        QCOMPARE(
            appserver_detail::
                windowsClientVersion(
                    QStringLiteral(
                        "0.3.5")),
            QStringLiteral(
                "0.3.5-windows"));
        QCOMPARE(
            appserver_detail::
                windowsClientVersion({}),
            QStringLiteral(
                "0-windows"));
    }

    void proxySessionMatchesV034Transcript()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches));

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QString diagnostic;
        if (!result.hasValue()) {
            diagnostic =
                result.error().code
                + QStringLiteral(": ")
                + result.error().message
                + QStringLiteral(" context=")
                + QString::fromUtf8(
                    QJsonDocument::fromVariant(
                        result.error().context)
                        .toJson(QJsonDocument::Compact))
                + QStringLiteral(" launches=")
                + readTextLines(launches).join(
                    QLatin1Char(','))
                + QStringLiteral(" messages=")
                + QString::number(
                    readJsonLines(transcript).size())
                + QStringLiteral(" debug=")
                + readTextLines(
                      transcript
                      + QStringLiteral(".debug"))
                      .join(QLatin1Char(','));
        }
        QVERIFY2(
            result.hasValue(),
            qPrintable(diagnostic));
        QCOMPARE(result.value(), QStringLiteral("thread-created"));
        QCOMPARE(readJsonLines(transcript), taskFixture());
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("proxy")});
    }

    void missingSharedDaemonUsesOneWindowsStdioSession()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(QStringLiteral("pid.txt"));
        quint32 processId = 0;
        {
            TaskCreator taskCreator = creator(
                {currentExecutablePath()},
                false,
                helperEnvironment(
                    QStringLiteral("success"),
                    transcript,
                    launches,
                    pid));

            const Result<QString> result =
                taskCreator.create(fullTaskRequest());

            QVERIFY(result.hasValue());
            QCOMPARE(
                result.value(),
                QStringLiteral("thread-created"));
            QCOMPARE(readJsonLines(transcript), taskFixture());
            QCOMPARE(
                readTextLines(launches),
                QStringList{QStringLiteral("stdio")});
            const QStringList pids = readTextLines(pid);
            QCOMPARE(pids.size(), 1);
            bool ok = false;
            processId = pids.first().toUInt(&ok);
            QVERIFY(ok);
            QVERIFY(processIsAlive(processId));
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            !processIsAlive(processId), 3000);
    }

    void consecutiveTasksReuseProcessAndAdvanceRequestIds()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(QStringLiteral("pid.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches,
                pid));

        const Result<QString> first =
            taskCreator.create(fullTaskRequest());
        const Result<QString> second =
            taskCreator.create(fullTaskRequest());

        QVERIFY(first.hasValue());
        QCOMPARE(
            first.value(),
            QStringLiteral("thread-created"));
        QVERIFY(second.hasValue());
        QCOMPARE(
            second.value(),
            QStringLiteral("thread-created-4"));
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
        QCOMPARE(readTextLines(pid).size(), 1);

        const QVector<QJsonObject> messages =
            readJsonLines(transcript);
        QCOMPARE(messages.size(), 6);
        QCOMPARE(numericId(messages.at(0)), 1);
        QVERIFY(!messages.at(1).contains(QStringLiteral("id")));
        QCOMPARE(numericId(messages.at(2)), 2);
        QCOMPARE(numericId(messages.at(3)), 3);
        QCOMPARE(numericId(messages.at(4)), 4);
        QCOMPARE(numericId(messages.at(5)), 5);
    }

    void blankOptionalsAndPartialSkillAreOmitted()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches));
        CreateTaskRequest request;
        request.prompt = QStringLiteral("  Minimal task  ");
        request.cwd = QStringLiteral("  ");
        request.model = QStringLiteral("\r\n");
        request.reasoningEffort = QStringLiteral(" ");
        request.skillName = QStringLiteral("design-and-build");
        request.skillPath = QStringLiteral(" ");
        request.clientMessageId =
            QStringLiteral("stable-minimal");

        const Result<QString> result =
            taskCreator.create(request);

        QVERIFY(result.hasValue());
        const QVector<QJsonObject> messages =
            readJsonLines(transcript);
        QCOMPARE(messages.size(), 4);
        const QJsonObject threadParams =
            messages.at(2)
                .value(QStringLiteral("params"))
                .toObject();
        QVERIFY(!threadParams.contains(QStringLiteral("cwd")));
        QVERIFY(!threadParams.contains(QStringLiteral("model")));
        const QJsonObject turnParams =
            messages.at(3)
                .value(QStringLiteral("params"))
                .toObject();
        QVERIFY(!turnParams.contains(QStringLiteral("model")));
        QVERIFY(!turnParams.contains(QStringLiteral("effort")));
        const QJsonArray input =
            turnParams.value(QStringLiteral("input")).toArray();
        QCOMPARE(input.size(), 1);
        QCOMPARE(
            input.first()
                .toObject()
                .value(QStringLiteral("text"))
                .toString(),
            QStringLiteral("Minimal task"));
    }

    void malformedThreadResponseIsAmbiguousBeforeTurnStart()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral("malformed-thread"),
                transcript,
                launches));

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.task_create_ambiguous"));
        QVERIFY(!result.error().retryable);
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("phase"))
                .toString(),
            QStringLiteral("thread/start"));
        QCOMPARE(
            result.error()
                .context.value(
                    QStringLiteral("clientMessageId"))
                .toString(),
            QStringLiteral("message-stable"));
        const QVector<QJsonObject> messages =
            readJsonLines(transcript);
        QCOMPARE(messages.size(), 3);
        QCOMPARE(
            messages.last()
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("thread/start"));
    }

    void threadAcknowledgementTimeoutReturnsAmbiguousContext()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {
                currentExecutablePath(),
                currentExecutablePath(),
            },
            false,
            helperEnvironment(
                QStringLiteral(
                    "thread-timeout-after-first-success"),
                transcript,
                launches),
            2000);

        const Result<QString> warmup =
            taskCreator.create(fullTaskRequest());
        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(warmup.hasValue());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.task_create_ambiguous"));
        QVERIFY(!result.error().retryable);
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("phase"))
                .toString(),
            QStringLiteral("thread/start"));
        QCOMPARE(
            result.error()
                .context.value(
                    QStringLiteral("clientMessageId"))
                .toString(),
            QStringLiteral("message-stable"));
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("causeCode"))
                .toString(),
            QStringLiteral(
                "codex.app_server_timed_out"));
        QVERIFY(
            !result.error()
                 .context.contains(
                     QStringLiteral("threadId")));
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void turnErrorFailsWithoutReturningCreatedThread()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {
                currentExecutablePath(),
                currentExecutablePath(),
            },
            false,
            helperEnvironment(
                QStringLiteral("turn-error"),
                transcript,
                launches));

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.task_create_failed"));
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("serverMessage"))
                .toString(),
            QStringLiteral("turn rejected"));
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void nullTurnAcknowledgementMatchesMacParity()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("null-turn-ack"),
                transcript,
                launches));

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            result.value(),
            QStringLiteral("thread-created"));
    }

    void turnAcknowledgementTimeoutReturnsAmbiguousContext()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {
                currentExecutablePath(),
                currentExecutablePath(),
            },
            false,
            helperEnvironment(
                QStringLiteral(
                    "turn-timeout-after-first-success"),
                transcript,
                launches),
            2000);

        const Result<QString> warmup =
            taskCreator.create(fullTaskRequest());
        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(warmup.hasValue());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.task_create_ambiguous"));
        QVERIFY(!result.error().retryable);
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-created-4"));
        QCOMPARE(
            result.error()
                .context.value(
                    QStringLiteral("clientMessageId"))
                .toString(),
            QStringLiteral("message-stable"));
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("phase"))
                .toString(),
            QStringLiteral("turn/start"));
        QCOMPARE(
            result.error()
                .context.value(QStringLiteral("causeCode"))
                .toString(),
            QStringLiteral(
                "codex.app_server_timed_out"));
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void ambiguousHostIsQuarantinedBeforeNextTask()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(QStringLiteral("pid.txt"));
        QVector<quint32> processIds;
        {
            TaskCreator taskCreator = creator(
                {currentExecutablePath()},
                false,
                helperEnvironment(
                    QStringLiteral(
                        "turn-invalid-first-then-success"),
                    transcript,
                    launches,
                    pid),
                2000);

            const Result<QString> first =
                taskCreator.create(fullTaskRequest());
            const Result<QString> second =
                taskCreator.create(fullTaskRequest());

            QVERIFY(!first.hasValue());
            QCOMPARE(
                first.error().code,
                QStringLiteral(
                    "codex.task_create_ambiguous"));
            QVERIFY(second.hasValue());
            QCOMPARE(
                second.value(),
                QStringLiteral("thread-created-4"));
            const QStringList expectedLaunches{
                QStringLiteral("stdio"),
                QStringLiteral("stdio"),
            };
            QCOMPARE(
                readTextLines(launches),
                expectedLaunches);
            const QStringList pids = readTextLines(pid);
            QCOMPARE(pids.size(), 2);
            for (const QString& text : pids) {
                bool ok = false;
                const quint32 processId =
                    text.toUInt(&ok);
                QVERIFY(ok);
                QVERIFY(processIsAlive(processId));
                processIds.append(processId);
            }
        }
        for (const quint32 processId : processIds) {
            QTRY_VERIFY_WITH_TIMEOUT(
                !processIsAlive(processId), 3000);
        }
    }

    void activeHostPreservesAndAcceptsServerRequest()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("approval-request"),
                transcript,
                launches));

        const Result<QString> created =
            taskCreator.create(fullTaskRequest());
        QVERIFY(created.hasValue());

        const auto pending =
            taskCreator.takePendingServerRequests();
        QVERIFY(pending.hasValue());
        QCOMPARE(pending.value().size(), 1);
        const PendingAppServerRequest request =
            pending.value().first();
        QVERIFY(request.hostId > 0);
        QCOMPARE(request.requestId.toInt(), 77);
        QCOMPARE(
            request.method,
            QStringLiteral(
                "item/commandExecution/requestApproval"));
        QCOMPARE(
            request.params
                .value(QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-created"));

        const Result<void> responded =
            taskCreator.respondToServerRequest(
                request.hostId,
                request.requestId,
                QJsonObject{
                    {
                        QStringLiteral("decision"),
                        QStringLiteral("accept"),
                    },
                });
        QVERIFY(responded.hasValue());
        QTRY_VERIFY_WITH_TIMEOUT(
            readJsonLines(transcript).size() >= 5,
            2000);
        const QJsonObject response =
            readJsonLines(transcript).last();
        QCOMPARE(numericId(response), 77);
        QCOMPARE(
            response
                .value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("decision"))
                .toString(),
            QStringLiteral("accept"));
    }

    void quarantinedHostReceivesItsServerResponse()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral(
                    "approval-before-turn-failure-first-then-success"),
                transcript,
                launches),
            2000);

        const Result<QString> first =
            taskCreator.create(fullTaskRequest());
        const Result<QString> second =
            taskCreator.create(fullTaskRequest());
        QVERIFY(!first.hasValue());
        QCOMPARE(
            first.error().code,
            QStringLiteral(
                "codex.task_create_ambiguous"));
        QVERIFY(second.hasValue());

        const auto pending =
            taskCreator.takePendingServerRequests();
        QVERIFY(pending.hasValue());
        QCOMPARE(pending.value().size(), 1);
        const PendingAppServerRequest request =
            pending.value().first();
        const Result<void> responded =
            taskCreator.respondToServerRequest(
                request.hostId,
                request.requestId,
                QJsonObject{
                    {
                        QStringLiteral("decision"),
                        QStringLiteral("decline"),
                    },
                });
        QVERIFY(responded.hasValue());
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const QVector<QJsonObject> messages =
                    readJsonLines(transcript);
                return std::any_of(
                    messages.cbegin(),
                    messages.cend(),
                    [](const QJsonObject& message) {
                        return numericId(message) == 77
                            && message
                                   .value(
                                       QStringLiteral(
                                           "result"))
                                   .toObject()
                                   .value(
                                       QStringLiteral(
                                           "decision"))
                                   .toString()
                                == QStringLiteral(
                                    "decline");
                    });
            }(),
            2000);
    }

    void pendingRequestCollectionIsTransactionalAcrossHosts()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(QStringLiteral("pid.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral(
                    "transactional-approval-failure"),
                transcript,
                launches,
                pid),
            500);

        const Result<QString> first =
            taskCreator.create(fullTaskRequest());
        const Result<QString> second =
            taskCreator.create(fullTaskRequest());
        QVERIFY(!first.hasValue());
        QCOMPARE(
            first.error().code,
            QStringLiteral(
                "codex.task_create_ambiguous"));
        QVERIFY(second.hasValue());

        QTRY_COMPARE_WITH_TIMEOUT(
            readTextLines(pid).size(), 2, 2000);
        const QStringList pids = readTextLines(pid);
        bool firstPidOk = false;
        const quint32 firstProcessId =
            pids.first().toUInt(&firstPidOk);
        QVERIFY(firstPidOk);
        QTRY_VERIFY_WITH_TIMEOUT(
            !processIsAlive(firstProcessId), 2000);

        const auto pending =
            taskCreator.takePendingServerRequests();
        QVERIFY(pending.hasValue());
        QCOMPARE(pending.value().size(), 1);
        QCOMPARE(
            pending.value().first().method,
            QStringLiteral(
                "item/commandExecution/requestApproval"));

        const auto drained =
            taskCreator.takePendingServerRequests();
        QVERIFY(drained.hasValue());
        QVERIFY(drained.value().isEmpty());
    }

    void quarantineLimitBoundsUncertainTaskHosts()
    {
        constexpr int maximumQuarantinedHosts = 8;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("turn-invalid"),
                transcript,
                launches),
            2000);

        for (int index = 0;
             index < maximumQuarantinedHosts;
             ++index) {
            const Result<QString> ambiguous =
                taskCreator.create(fullTaskRequest());
            QVERIFY(!ambiguous.hasValue());
            QCOMPARE(
                ambiguous.error().code,
                QStringLiteral(
                    "codex.task_create_ambiguous"));
        }

        const Result<QString> bounded =
            taskCreator.create(fullTaskRequest());
        QVERIFY(!bounded.hasValue());
        QCOMPARE(
            bounded.error().code,
            QStringLiteral(
                "codex.app_server_quarantine_limit"));
        QCOMPARE(
            readTextLines(launches).size(),
            maximumQuarantinedHosts);
    }

    void completeOversizedProxyHandshakeIsRejected()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral("oversized-handshake"),
                transcript,
                launches),
            2000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
    }

    void invalidProxyUpgradeFallsBackToStdio()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral(
                    "invalid-websocket-accept"),
                transcript,
                launches),
            2000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            result.value(),
            QStringLiteral("thread-created"));
        QCOMPARE(
            readTextLines(launches),
            QStringList({
                QStringLiteral("proxy"),
                QStringLiteral("stdio"),
            }));
    }

    void hangingProxyPreservesStdioFallbackBudget()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral(
                    "proxy-hang-then-stdio-success"),
                transcript,
                launches),
            3000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            result.value(),
            QStringLiteral("thread-created"));
        QCOMPARE(
            readTextLines(launches),
            QStringList({
                QStringLiteral("proxy"),
                QStringLiteral("stdio"),
            }));
    }

    void maskedServerFrameIsRejected()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral("masked-server-frame"),
                transcript,
                launches),
            2000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
    }

    void fragmentedControlFrameIsRejected()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral(
                    "fragmented-control-frame"),
                transcript,
                launches),
            2000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
    }

    void completeOversizedJsonLineIsRejected()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("oversized-json-line"),
                transcript,
                launches),
            5000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
        const qlonglong retainedBytes =
            result.error()
                .context.value(
                    QStringLiteral("bufferedBytes"))
                .toLongLong();
        const qlonglong bufferLimitBytes =
            result.error()
                .context.value(
                    QStringLiteral("bufferLimitBytes"))
                .toLongLong();
        QVERIFY(bufferLimitBytes > 0);
        QVERIFY(retainedBytes > 0);
        QVERIFY(retainedBytes <= bufferLimitBytes);
    }

    void largeStderrDoesNotBlockTaskCreation()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("stderr-flood"),
                transcript,
                launches),
            5000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            result.value(),
            QStringLiteral("thread-created"));
    }

    void unlaunchableCandidateFallsThroughBeforeAnyChildStarts()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {
                temporary.filePath(
                    QStringLiteral("missing-codex.exe")),
                currentExecutablePath(),
            },
            false,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches));

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(result.hasValue());
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void initializationExitTriesNextExecutable()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {
                currentExecutablePath(),
                currentExecutablePath(),
            },
            true,
            helperEnvironment(
                QStringLiteral(
                    "exit-first-then-success"),
                transcript,
                launches));

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            result.value(),
            QStringLiteral("thread-created"));
        const QStringList expectedLaunches{
            QStringLiteral("proxy"),
            QStringLiteral("proxy"),
        };
        QCOMPARE(
            readTextLines(launches),
            expectedLaunches);
    }

    void timeoutTerminatesTheChild()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(QStringLiteral("pid.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("hang"),
                transcript,
                launches,
                pid),
            500);
        QElapsedTimer elapsed;
        elapsed.start();

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.app_server_timed_out"));
        QVERIFY(elapsed.elapsed() < 5000);
        const QStringList pids = readTextLines(pid);
        QCOMPARE(pids.size(), 1);
        bool ok = false;
        const quint32 processId = pids.first().toUInt(&ok);
        QVERIFY(ok);
        QTRY_VERIFY_WITH_TIMEOUT(
            !processIsAlive(processId), 3000);
    }

    void forcedCleanupConfirmsEveryChildExit()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(QStringLiteral("pid.txt"));

        for (int index = 0; index < 4; ++index) {
            {
                TaskCreator taskCreator = creator(
                    {currentExecutablePath()},
                    false,
                    helperEnvironment(
                        QStringLiteral("turn-invalid"),
                        transcript,
                        launches,
                        pid),
                    2000);
                const Result<QString> result =
                    taskCreator.create(fullTaskRequest());
                QVERIFY(!result.hasValue());
                QCOMPARE(
                    result.error().code,
                    QStringLiteral(
                        "codex.task_create_ambiguous"));
            }

            const QStringList pids = readTextLines(pid);
            QCOMPARE(pids.size(), index + 1);
            bool ok = false;
            const quint32 processId =
                pids.last().toUInt(&ok);
            QVERIFY(ok);
            QVERIFY2(
                !processIsAlive(processId),
                "TaskCreator destroyed a session before its child exited.");
        }
    }

    void emptyPromptFailsWithoutLaunching()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches));
        CreateTaskRequest request;
        request.prompt = QStringLiteral(" \r\n ");

        const Result<QString> result =
            taskCreator.create(request);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.task_prompt_empty"));
        QVERIFY(readTextLines(launches).isEmpty());
    }

    void oversizedThreadRequestIsKnownUnsentAndHostRemainsUsable()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches),
            2000);
        CreateTaskRequest oversized = fullTaskRequest();
        oversized.cwd = QString(
            kOversizedMessageBytes,
            QLatin1Char('x'));

        const Result<QString> rejected =
            taskCreator.create(oversized);
        const Result<QString> recovered =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!rejected.hasValue());
        QCOMPARE(
            rejected.error().code,
            QStringLiteral(
                "codex.app_server_invalid_request"));
        QVERIFY(recovered.hasValue());
        QCOMPARE(
            recovered.value(),
            QStringLiteral("thread-created-4"));
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void oversizedTurnRequestIsKnownUnsentAndHostRemainsUsable()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("success"),
                transcript,
                launches),
            2000);
        CreateTaskRequest oversized = fullTaskRequest();
        oversized.prompt = QString(
            kOversizedMessageBytes,
            QLatin1Char('x'));

        const Result<QString> rejected =
            taskCreator.create(oversized);
        const Result<QString> recovered =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!rejected.hasValue());
        QCOMPARE(
            rejected.error().code,
            QStringLiteral(
                "codex.app_server_invalid_request"));
        QCOMPARE(
            rejected.error()
                .context.value(QStringLiteral("threadId"))
                .toString(),
            QStringLiteral("thread-created"));
        QVERIFY(recovered.hasValue());
        QCOMPARE(
            recovered.value(),
            QStringLiteral("thread-created-4"));
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void rpcClientInitializesOnceAndMatchesExactResponseIds()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral("rpc"),
                transcript,
                launches),
            2000);
        const QVector<RpcRequest> requests{
            {
                2,
                QStringLiteral("thread/goal/get"),
                {
                    {
                        QStringLiteral("threadId"),
                        QStringLiteral("thread-a"),
                    },
                },
            },
            {
                9,
                QStringLiteral("account/rateLimits/read"),
                {},
            },
        };

        const Result<QHash<int, RpcResponse>> result =
            client.perform(requests);

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(result.value().size(), 2);
        QVERIFY(result.value().contains(2));
        QVERIFY(result.value().contains(9));
        QCOMPARE(
            result.value()
                .value(2)
                .result
                .toObject()
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("thread/goal/get"));
        QCOMPARE(
            result.value()
                .value(9)
                .result
                .toObject()
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("account/rateLimits/read"));

        const QVector<QJsonObject> messages =
            readJsonLines(transcript);
        QCOMPARE(messages.size(), 4);
        QCOMPARE(numericId(messages.at(0)), 1);
        QCOMPARE(
            messages.at(1)
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("initialized"));
        QCOMPARE(numericId(messages.at(2)), 2);
        QCOMPARE(numericId(messages.at(3)), 9);
        QCOMPARE(
            readTextLines(launches),
            QStringList{QStringLiteral("stdio")});
    }

    void rpcClientRetriesCandidateThroughInitialization()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral(
                    "rpc-exit-first-then-success"),
                transcript,
                launches),
            2000);

        const Result<QHash<int, RpcResponse>> result =
            client.perform({
                {
                    2,
                    QStringLiteral("thread/goal/get"),
                    {},
                },
            });

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        const QStringList expectedLaunches{
            QStringLiteral("stdio"),
            QStringLiteral("stdio"),
        };
        QCOMPARE(
            readTextLines(launches),
            expectedLaunches);
    }

    void startupWaitChecksCancellationWithinFiftyMilliseconds()
    {
        std::stop_source stopSource;
        std::latch firstWait(1);
        std::atomic_bool waitSignaled = false;
        std::atomic_int requestedWait = 0;
        std::optional<Result<bool>> result;
        std::jthread worker([&] {
            result = appserver_detail::
                ProcessSessionTestAccess::
                    waitForStartup(
                        stopSource.get_token(),
                        [] {
                            return QProcess::Starting;
                        },
                        [&](int waitMilliseconds) {
                            requestedWait.store(
                                waitMilliseconds,
                                std::memory_order_release);
                            if (!waitSignaled.exchange(
                                    true,
                                    std::memory_order_acq_rel)) {
                                firstWait.count_down();
                            }
                            QThread::msleep(
                                static_cast<unsigned long>(
                                    waitMilliseconds));
                            return false;
                        });
        });

        firstWait.wait();
        QElapsedTimer elapsed;
        elapsed.start();
        stopSource.request_stop();
        worker.join();

        QVERIFY(result.has_value());
        QVERIFY(!result->hasValue());
        QCOMPARE(
            result->error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        QCOMPARE(
            result->error().message,
            QStringLiteral(
                "The Codex operation was canceled."));
        QVERIFY(!result->error().retryable);
        QVERIFY(result->error().context.isEmpty());
        QVERIFY(
            requestedWait.load(
                std::memory_order_acquire)
            <= 50);
        QVERIFY2(
            elapsed.elapsed() < 250,
            qPrintable(
                QStringLiteral("elapsed=%1")
                    .arg(elapsed.elapsed())));
    }

    void continuousStderrDrainIsBoundedPerCall()
    {
        const appserver_detail::
            StderrDrainObservation observation =
                appserver_detail::
                    ProcessSessionTestAccess::
                        drainContinuousStandardError();

        QVERIFY(observation.readBytes > 0);
        QVERIFY2(
            observation.readBytes
                <= 64 * 1024,
            qPrintable(
                QStringLiteral("readBytes=%1")
                    .arg(observation.readBytes)));
        QCOMPARE(
            observation.retainedBytes,
            static_cast<qsizetype>(1024));
    }

    void rpcCancellationDuringReadWaitIsPrompt()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(
                QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(
                QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(
                QStringLiteral("pid.txt"));
        const QString debug =
            transcript + QStringLiteral(".debug");
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral(
                    "rpc-read-block"),
                transcript,
                launches,
                pid),
            5000);
        std::stop_source stopSource;
        std::optional<
            Result<QHash<int, RpcResponse>>>
            result;
        std::jthread worker([&] {
            result = client.perform(
                {
                    {
                        2,
                        QStringLiteral(
                            "model/list"),
                        {},
                    },
                },
                stopSource.get_token());
        });

        QTRY_VERIFY_WITH_TIMEOUT(
            readTextLines(debug).contains(
                QStringLiteral("read-block")),
            3000);
        QElapsedTimer elapsed;
        elapsed.start();
        stopSource.request_stop();
        worker.join();

        QVERIFY(result.has_value());
        QVERIFY(!result->hasValue());
        QCOMPARE(
            result->error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        QCOMPARE(
            result->error().message,
            QStringLiteral(
                "The Codex operation was canceled."));
        QVERIFY(!result->error().retryable);
        QVERIFY(result->error().context.isEmpty());
        QVERIFY2(
            elapsed.elapsed() < 1500,
            qPrintable(
                QStringLiteral("elapsed=%1")
                    .arg(elapsed.elapsed())));

        const QStringList pids =
            readTextLines(pid);
        QCOMPARE(pids.size(), 1);
        bool ok = false;
        const quint32 processId =
            pids.front().toUInt(&ok);
        QVERIFY(ok);
        QTRY_VERIFY_WITH_TIMEOUT(
            !processIsAlive(processId),
            3000);
    }

    void rpcCancellationDuringWriteWaitIsPrompt()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(
                QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(
                QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(
                QStringLiteral("pid.txt"));
        const QString debug =
            transcript + QStringLiteral(".debug");
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral(
                    "rpc-write-block"),
                transcript,
                launches,
                pid),
            5000);
        const QString payload(
            2 * 1024 * 1024,
            QLatin1Char('x'));
        std::stop_source stopSource;
        std::optional<
            Result<QHash<int, RpcResponse>>>
            result;
        std::jthread worker([&] {
            result = client.perform(
                {
                    {
                        2,
                        QStringLiteral(
                            "model/list"),
                        {
                            {
                                QStringLiteral(
                                    "payload"),
                                payload,
                            },
                        },
                    },
                },
                stopSource.get_token());
        });

        QTRY_VERIFY_WITH_TIMEOUT(
            readTextLines(debug).contains(
                QStringLiteral("write-block")),
            3000);
        QThread::msleep(200);
        QElapsedTimer elapsed;
        elapsed.start();
        stopSource.request_stop();
        worker.join();

        QVERIFY(result.has_value());
        QVERIFY(!result->hasValue());
        QCOMPARE(
            result->error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        QCOMPARE(
            result->error().message,
            QStringLiteral(
                "The Codex operation was canceled."));
        QVERIFY(!result->error().retryable);
        QVERIFY(result->error().context.isEmpty());
        QVERIFY2(
            elapsed.elapsed() < 1500,
            qPrintable(
                QStringLiteral("elapsed=%1")
                    .arg(elapsed.elapsed())));

        const QStringList pids =
            readTextLines(pid);
        QCOMPARE(pids.size(), 1);
        bool ok = false;
        const quint32 processId =
            pids.front().toUInt(&ok);
        QVERIFY(ok);
        QTRY_VERIFY_WITH_TIMEOUT(
            !processIsAlive(processId),
            3000);
    }

    void rpcCancellationWithContinuousStderrIsPrompt()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(
                QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(
                QStringLiteral("launches.txt"));
        const QString pid =
            temporary.filePath(
                QStringLiteral("pid.txt"));
        const QString debug =
            transcript + QStringLiteral(".debug");
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral(
                    "rpc-continuous-stderr"),
                transcript,
                launches,
                pid),
            5000);
        std::stop_source stopSource;
        std::optional<
            Result<QHash<int, RpcResponse>>>
            result;
        std::atomic_bool completed = false;
        std::jthread worker([&] {
            result = client.perform(
                {
                    {
                        2,
                        QStringLiteral(
                            "model/list"),
                        {},
                    },
                },
                stopSource.get_token());
            completed.store(
                true,
                std::memory_order_release);
        });

        QTRY_VERIFY_WITH_TIMEOUT(
            readTextLines(debug).contains(
                QStringLiteral(
                    "stderr-continuous")),
            3000);
        QThread::msleep(100);
        QElapsedTimer elapsed;
        elapsed.start();
        stopSource.request_stop();
        const bool completedPromptly =
            waitForCompletion(completed, 1000);

        const QStringList pids =
            readTextLines(pid);
        QCOMPARE(pids.size(), 1);
        bool ok = false;
        const quint32 processId =
            pids.front().toUInt(&ok);
        QVERIFY(ok);
        if (!completedPromptly) {
            QVERIFY(terminateProcessForTest(
                processId));
        }
        worker.join();

        QVERIFY2(
            completedPromptly,
            qPrintable(
                QStringLiteral("elapsed=%1")
                    .arg(elapsed.elapsed())));
        QVERIFY(result.has_value());
        QVERIFY(!result->hasValue());
        QCOMPARE(
            result->error().code,
            QStringLiteral(
                "codex.operation_canceled"));
        QCOMPARE(
            result->error().message,
            QStringLiteral(
                "The Codex operation was canceled."));
        QVERIFY(!result->error().retryable);
        QVERIFY(result->error().context.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            !processIsAlive(processId),
            3000);
    }

    void startupTimeoutReturnsRetryableTimeout()
    {
        const Result<bool> result =
            appserver_detail::
                ProcessSessionTestAccess::
                    waitForExpiredStartup();

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_timed_out"));
        QCOMPARE(
            result.error().message,
            QStringLiteral(
                "Codex app-server did not respond in time."));
        QVERIFY(result.error().retryable);
    }

    void rpcClientHangingFirstCandidatePreservesFallbackBudget()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral(
                    "rpc-hang-first-then-success"),
                transcript,
                launches),
            3000);

        const Result<QHash<int, RpcResponse>> result =
            client.perform({
                {
                    2,
                    QStringLiteral("thread/goal/get"),
                    {},
                },
            });

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            readTextLines(launches),
            QStringList({
                QStringLiteral("stdio"),
                QStringLiteral("stdio"),
            }));
    }

    void rpcClientCleanupStaysWithinSharedDeadline()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                    currentExecutablePath(),
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral("malformed-json-line"),
                transcript,
                launches),
            6000);
        QElapsedTimer elapsed;
        elapsed.start();

        const Result<QHash<int, RpcResponse>> result =
            client.perform({
                {
                    2,
                    QStringLiteral("thread/goal/get"),
                    {},
                },
            });

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
        QVERIFY2(
            elapsed.elapsed() < 5000,
            qPrintable(
                QStringLiteral("elapsed=%1")
                    .arg(elapsed.elapsed())));
    }

    void malformedStdioJsonFailsImmediately()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("malformed-json-line"),
                transcript,
                launches),
            1000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
    }

    void malformedWebSocketJsonFailsImmediately()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral(
                    "malformed-websocket-json"),
                transcript,
                launches),
            3000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
    }

    void malformedStdioResponseIdFailsImmediately()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            false,
            helperEnvironment(
                QStringLiteral("invalid-response-id"),
                transcript,
                launches),
            1000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response_id"));
    }

    void malformedWebSocketResponseIdFailsImmediately()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        TaskCreator taskCreator = creator(
            {currentExecutablePath()},
            true,
            helperEnvironment(
                QStringLiteral("invalid-response-id"),
                transcript,
                launches),
            1000);

        const Result<QString> result =
            taskCreator.create(fullTaskRequest());

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response_id"));
    }

    void rpcClientRejectsReservedOrDuplicateRequestIds()
    {
        AppServerRpcClient client(
            [] { return QVector<QString>{}; });

        const Result<QHash<int, RpcResponse>> reserved =
            client.perform({
                {1, QStringLiteral("reserved"), {}},
            });
        QVERIFY(!reserved.hasValue());
        QCOMPARE(
            reserved.error().code,
            QStringLiteral("codex.app_server_invalid_request"));

        const Result<QHash<int, RpcResponse>> duplicate =
            client.perform({
                {2, QStringLiteral("first"), {}},
                {2, QStringLiteral("second"), {}},
            });
        QVERIFY(!duplicate.hasValue());
        QCOMPARE(
            duplicate.error().code,
            QStringLiteral("codex.app_server_invalid_request"));
    }

    void rpcClientIgnoresNumericServerRequests()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral(
                    "rpc-numeric-server-request"),
                transcript,
                launches),
            2000);

        const Result<QHash<int, RpcResponse>> result =
            client.perform({
                {
                    2,
                    QStringLiteral("thread/goal/get"),
                    {},
                },
            });

        QVERIFY2(
            result.hasValue(),
            result.hasValue()
                ? ""
                : qPrintable(result.error().message));
        QCOMPARE(
            result.value()
                .value(2)
                .result
                .toObject()
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("thread/goal/get"));
    }

    void rpcClientBoundsUnexpectedResponseCache()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString transcript =
            temporary.filePath(QStringLiteral("transcript.jsonl"));
        const QString launches =
            temporary.filePath(QStringLiteral("launches.txt"));
        AppServerRpcClient client(
            [] {
                return QVector<QString>{
                    currentExecutablePath(),
                };
            },
            helperEnvironment(
                QStringLiteral("rpc-response-flood"),
                transcript,
                launches),
            2000);

        const Result<QHash<int, RpcResponse>> result =
            client.perform({
                {
                    2,
                    QStringLiteral("thread/goal/get"),
                    {},
                },
            });

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.app_server_invalid_response"));
    }

    void defaultTimeoutsMatchMacV034()
    {
        QCOMPARE(
            TaskCreator::kDefaultTimeoutMilliseconds,
            12'000);
        QCOMPARE(
            AppServerRpcClient::kDefaultTimeoutMilliseconds,
            20'000);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationVersion(
        QStringLiteral("0.3.4"));
    const int helperResult =
        runHelper(application.arguments());
    if (helperResult >= 0) {
        return helperResult;
    }

    AppServerTaskCreatorTests tests;
    return QTest::qExec(
        &tests, application.arguments());
}

#include "AppServerTaskCreatorTests.moc"
