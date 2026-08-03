#include "codex/ipc/FollowerEndpointDiscovery.h"
#include "codex/ipc/FollowerFrameCodec.h"
#include "codex/ipc/FollowerClient.h"
#include "codex/ipc/FollowerRequestFactory.h"

#include <QFile>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace companion;
using namespace std::chrono_literals;

namespace {

class FakeEndpointEvidenceSource final
    : public IFollowerEndpointEvidenceSource {
public:
    QString environmentValue(QStringView name) const override
    {
        return environment.value(name.toString());
    }

    QVector<QString>
    protectedProgramFilesDirectories() const override
    {
        return protectedProgramFiles;
    }

    QVector<QString>
    installedCodexPackageExecutables() const override
    {
        return packageExecutables;
    }

    QVector<FollowerProcessEvidence>
    runningCodexProcesses() const override
    {
        return processes;
    }

    QVector<QString> installedRuntimeMetadata(
        const CodexEnvironment&) const override
    {
        return metadata;
    }

    QHash<QString, QString> environment;
    QVector<QString> protectedProgramFiles{
        QStringLiteral("C:/Program Files"),
    };
    QVector<QString> packageExecutables;
    QVector<FollowerProcessEvidence> processes;
    QVector<QString> metadata;
};

QJsonObject fixture(const QString& filename)
{
    QFile file(
        QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral("/codex-v034/")
        + filename);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll());
    return document.object();
}

QByteArray frameWithPayload(const QByteArray& payload)
{
    QByteArray frame(sizeof(quint32), Qt::Uninitialized);
    qToLittleEndian<quint32>(
        static_cast<quint32>(payload.size()),
        reinterpret_cast<uchar*>(frame.data()));
    frame.append(payload);
    return frame;
}

QJsonObject objectWithEncodedSize(qsizetype targetSize)
{
    const QByteArray emptyPayload =
        QJsonDocument(QJsonObject{
            {QStringLiteral("payload"), QString()},
        }).toJson(QJsonDocument::Compact);
    const qsizetype stringSize = targetSize - emptyPayload.size();
    return {
        {
            QStringLiteral("payload"),
            QString(stringSize, QLatin1Char('x')),
        },
    };
}

QVector<StagedAttachment> attachments()
{
    return {
        {
            QUuid(
                QStringLiteral(
                    "00000000-0000-0000-0000-000000000001")),
            AttachmentKind::File,
            QStringLiteral("notes.txt"),
            QStringLiteral("C:/staged/notes.txt"),
            QStringLiteral("C:/staged/notes.txt"),
            QStringLiteral("text/plain"),
        },
        {
            QUuid(
                QStringLiteral(
                    "00000000-0000-0000-0000-000000000002")),
            AttachmentKind::Image,
            QStringLiteral("reference.png"),
            QStringLiteral("C:/staged/reference.png"),
            QStringLiteral("C:/staged/reference.png"),
            QStringLiteral("image/png"),
        },
    };
}

QJsonObject queuedMessage(
    const QString& id,
    const QString& text,
    qint64 createdAt)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("text"), text},
        {QStringLiteral("createdAt"), createdAt},
    };
}

QString uniquePipeEndpoint()
{
    return QStringLiteral(R"(\\.\pipe\codex-companion-test-)")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString currentExecutablePath()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0
        || length
            >= static_cast<DWORD>(buffer.size())) {
        throw std::runtime_error(
            "GetModuleFileNameW failed.");
    }
    return QDir::fromNativeSeparators(
        QString::fromWCharArray(
            buffer.data(),
            static_cast<qsizetype>(length)));
}

std::runtime_error pipeFailure(const char* operation)
{
    return std::runtime_error(
        std::string(operation)
        + " failed with Win32 error "
        + std::to_string(GetLastError()));
}

void readExactly(
    HANDLE pipe,
    char* destination,
    qsizetype byteCount)
{
    qsizetype offset = 0;
    while (offset < byteCount) {
        DWORD read = 0;
        const DWORD requested = static_cast<DWORD>(
            qMin<qsizetype>(
                byteCount - offset,
                std::numeric_limits<DWORD>::max()));
        if (!ReadFile(
                pipe,
                destination + offset,
                requested,
                &read,
                nullptr)
            || read == 0) {
            throw pipeFailure("ReadFile");
        }
        offset += static_cast<qsizetype>(read);
    }
}

void writeExactly(
    HANDLE pipe,
    const char* source,
    qsizetype byteCount)
{
    qsizetype offset = 0;
    while (offset < byteCount) {
        DWORD written = 0;
        const DWORD requested = static_cast<DWORD>(
            qMin<qsizetype>(
                byteCount - offset,
                std::numeric_limits<DWORD>::max()));
        if (!WriteFile(
                pipe,
                source + offset,
                requested,
                &written,
                nullptr)
            || written == 0) {
            throw pipeFailure("WriteFile");
        }
        offset += static_cast<qsizetype>(written);
    }
}

QJsonObject readPipeMessage(HANDLE pipe)
{
    QByteArray header(sizeof(quint32), Qt::Uninitialized);
    readExactly(pipe, header.data(), header.size());
    const quint32 length = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData()));
    if (length > kFollowerMaximumParsedFrameBytes) {
        throw std::runtime_error(
            "Test server received an unexpectedly large frame.");
    }
    QByteArray payload(
        static_cast<qsizetype>(length),
        Qt::Uninitialized);
    readExactly(pipe, payload.data(), payload.size());
    const QJsonDocument document =
        QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        throw std::runtime_error(
            "Test server received invalid JSON.");
    }
    return document.object();
}

void writePipeMessage(
    HANDLE pipe,
    const QJsonObject& message)
{
    const auto encoded = FollowerFrameCodec::encode(message);
    if (!encoded.hasValue()) {
        throw std::runtime_error(
            "Test server could not encode a frame.");
    }
    writeExactly(
        pipe,
        encoded.value().constData(),
        encoded.value().size());
    FlushFileBuffers(pipe);
}

void writeRawFrame(
    HANDLE pipe,
    quint32 length,
    bool includeBody)
{
    QByteArray header(sizeof(quint32), Qt::Uninitialized);
    qToLittleEndian<quint32>(
        length,
        reinterpret_cast<uchar*>(header.data()));
    writeExactly(pipe, header.constData(), header.size());
    if (includeBody) {
        QByteArray chunk(64 * 1024, 'x');
        quint64 remaining = length;
        while (remaining > 0) {
            const qsizetype count = static_cast<qsizetype>(
                qMin<quint64>(
                    remaining,
                    static_cast<quint64>(chunk.size())));
            writeExactly(pipe, chunk.constData(), count);
            remaining -= static_cast<quint64>(count);
        }
    }
    FlushFileBuffers(pipe);
}

QJsonObject successResponse(
    const QString& requestId,
    const QString& clientId = {})
{
    QJsonObject result;
    if (!clientId.isEmpty()) {
        result.insert(QStringLiteral("clientId"), clientId);
    }
    return {
        {
            QStringLiteral("type"),
            QStringLiteral("response"),
        },
        {QStringLiteral("requestId"), requestId},
        {
            QStringLiteral("resultType"),
            QStringLiteral("success"),
        },
        {QStringLiteral("result"), result},
    };
}

QJsonObject errorResponse(
    const QString& requestId,
    QJsonValue error)
{
    return {
        {
            QStringLiteral("type"),
            QStringLiteral("response"),
        },
        {QStringLiteral("requestId"), requestId},
        {
            QStringLiteral("resultType"),
            QStringLiteral("error"),
        },
        {QStringLiteral("error"), std::move(error)},
    };
}

class TestPipeServer final {
public:
    using Script = std::function<void(HANDLE)>;

    TestPipeServer(QString endpoint, Script script)
        : endpoint_(std::move(endpoint)),
          pipe_(CreateNamedPipeW(
              reinterpret_cast<LPCWSTR>(
                  endpoint_.utf16()),
              PIPE_ACCESS_DUPLEX,
              PIPE_TYPE_BYTE
                  | PIPE_READMODE_BYTE
                  | PIPE_WAIT,
              1,
              64 * 1024,
              64 * 1024,
              0,
              nullptr))
    {
        if (pipe_ == INVALID_HANDLE_VALUE) {
            throw pipeFailure("CreateNamedPipeW");
        }

        thread_ = std::thread(
            [this, script = std::move(script)] {
                try {
                    const BOOL connected =
                        ConnectNamedPipe(pipe_, nullptr);
                    if (!connected) {
                        const DWORD connectError =
                            GetLastError();
                        // A verifier can connect and reject the pipe before
                        // this worker observes it. The script still needs to
                        // validate that no protocol bytes were sent.
                        if (connectError
                                != ERROR_PIPE_CONNECTED
                            && connectError
                                != ERROR_NO_DATA) {
                            throw pipeFailure(
                                "ConnectNamedPipe");
                        }
                    }
                    script(pipe_);
                    FlushFileBuffers(pipe_);
                    DisconnectNamedPipe(pipe_);
                } catch (...) {
                    error_ = std::current_exception();
                }
            });
    }

    ~TestPipeServer()
    {
        if (thread_.joinable()) {
            CancelSynchronousIo(
                static_cast<HANDLE>(
                    thread_.native_handle()));
            thread_.join();
        }
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
        }
    }

    TestPipeServer(const TestPipeServer&) = delete;
    TestPipeServer& operator=(const TestPipeServer&) = delete;

    void finish()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    QString endpoint_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::exception_ptr error_;
};

class TestNativeHandle final {
public:
    explicit TestNativeHandle(
        HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~TestNativeHandle()
    {
        reset();
    }

    TestNativeHandle(const TestNativeHandle&) = delete;
    TestNativeHandle& operator=(const TestNativeHandle&) = delete;

    TestNativeHandle(TestNativeHandle&& other) noexcept
        : handle_(std::exchange(
              other.handle_,
              INVALID_HANDLE_VALUE))
    {
    }

    TestNativeHandle& operator=(
        TestNativeHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(
                other.handle_,
                INVALID_HANDLE_VALUE));
        }
        return *this;
    }

    HANDLE get() const noexcept
    {
        return handle_;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE)
    {
        if (handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

TestNativeHandle occupyPipe(
    const QString& endpoint)
{
    if (!WaitNamedPipeW(
            reinterpret_cast<LPCWSTR>(
                endpoint.utf16()),
            2000)) {
        throw pipeFailure("WaitNamedPipeW");
    }
    TestNativeHandle client(CreateFileW(
        reinterpret_cast<LPCWSTR>(
            endpoint.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        SECURITY_SQOS_PRESENT
            | SECURITY_IDENTIFICATION,
        nullptr));
    if (client.get() == INVALID_HANDLE_VALUE) {
        throw pipeFailure("CreateFileW");
    }
    return client;
}

FollowerServerVerifier currentProcessServerVerifier()
{
    return [](quint32 processId) {
        return processId == GetCurrentProcessId();
    };
}

template <typename T>
T finished(QFuture<T> future)
{
    future.waitForFinished();
    return future.result();
}

} // namespace

class FollowerProtocolTests final : public QObject {
    Q_OBJECT

private slots:
    void endpointDiscoveryUsesEvidenceBeforeVerifiedWindowsEndpoint()
    {
        FakeEndpointEvidenceSource source;
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            QStringLiteral("C:/Program Files"));
        source.processes = {
            {
                QStringLiteral(
                    "C:/Program Files/WindowsApps/"
                    "OpenAI.Codex_26.715.7063.0_x64__"
                    "2p2nqsd0c76g0/"
                    "app/resources/codex.exe"),
                QStringLiteral(
                    R"(codex.exe app-server --follower-pipe="\\.\pipe\codex-current")"),
            },
        };
        source.metadata = {
            QStringLiteral(
                R"({"followerPipe":"\\\\.\\pipe\\codex-runtime"})"),
        };
        FollowerEndpointDiscovery discovery(source);

        const QVector<QString> candidates =
            discovery.candidates(CodexEnvironment{});

        QCOMPARE(
            candidates,
            QVector<QString>({
                QStringLiteral(R"(\\.\pipe\codex-current)"),
                QStringLiteral(R"(\\.\pipe\codex-runtime)"),
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void endpointDiscoveryReadsExplicitEnvironmentAndDeduplicates()
    {
        FakeEndpointEvidenceSource source;
        source.environment.insert(
            QStringLiteral("CODEX_COMPANION_FOLLOWER_PIPE"),
            QStringLiteral("codex-explicit"));
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            QStringLiteral("C:/Program Files"));
        source.processes = {
            {
                QStringLiteral(
                    "C:/Program Files/WindowsApps/"
                    "OpenAI.Codex_26.715.7063.0_x64__"
                    "2p2nqsd0c76g0/"
                    "app/resources/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-endpoint=\\.\pipe\codex-explicit)"),
            },
        };
        FollowerEndpointDiscovery discovery(source);

        const QVector<QString> candidates =
            discovery.candidates(CodexEnvironment{});

        QCOMPARE(
            candidates,
            QVector<QString>({
                QStringLiteral(R"(\\.\pipe\codex-explicit)"),
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void endpointDiscoveryRejectsUntrustedOrUnrelatedPipeText()
    {
        FakeEndpointEvidenceSource source;
        source.environment.insert(
            QStringLiteral("CODEX_COMPANION_FOLLOWER_PIPE"),
            QStringLiteral(R"(\\server\pipe\remote)"));
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            QStringLiteral("C:/Program Files"));
        source.processes = {
            {
                QStringLiteral(
                    "C:/Program Files/WindowsApps/"
                    "OpenAI.Codex_26.715.7063.0_x64__"
                    "2p2nqsd0c76g0/"
                    "app/resources/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\..\unsafe)"),
            },
            {
                QStringLiteral(
                    "C:/Program Files/WindowsApps/"
                    "OpenAI.Codex_26.715.7063.0_x64__"
                    "2p2nqsd0c76g0/"
                    "app/resources/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --browser-pipe=\\.\pipe\codex-browser-use-1)"),
            },
        };
        FollowerEndpointDiscovery discovery(source);

        QCOMPARE(
            discovery.candidates(CodexEnvironment{}),
            QVector<QString>({
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void endpointDiscoveryRejectsLookalikeProcessPaths()
    {
        FakeEndpointEvidenceSource source;
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            QStringLiteral("C:/Program Files"));
        source.processes = {
            {
                QStringLiteral("C:/Users/test/Downloads/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\hijacked)"),
            },
        };
        CodexEnvironment environment;
        environment.localAppData =
            QStringLiteral("C:/Users/test/AppData/Local");
        environment.codexBinRoot =
            QStringLiteral(
                "C:/Users/test/AppData/Local/OpenAI/Codex/bin");
        FollowerEndpointDiscovery discovery(source);

        QCOMPARE(
            discovery.candidates(environment),
            QVector<QString>({
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void endpointDiscoveryIgnoresSpoofedProgramFilesRoot()
    {
        FakeEndpointEvidenceSource source;
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            QStringLiteral("D:/Attacker"));
        source.environment.insert(
            QStringLiteral("ProgramW6432"),
            QStringLiteral("D:/Attacker"));
        source.processes = {
            {
                QStringLiteral(
                    "D:/Attacker/WindowsApps/"
                    "OpenAI.Codex_26.715.7063.0_x64__"
                    "2p2nqsd0c76g0/"
                    "app/resources/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\hijacked)"),
            },
        };
        FollowerEndpointDiscovery discovery(source);

        QCOMPARE(
            discovery.candidates(CodexEnvironment{}),
            QVector<QString>({
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void endpointDiscoveryTrustsPackageApiPathOnSecondaryVolume()
    {
        FakeEndpointEvidenceSource source;
        const QString package = QStringLiteral(
            "E:/WindowsApps/"
            "OpenAI.Codex_26.715.7063.0_x64__"
            "2p2nqsd0c76g0/"
            "app/resources/codex.exe");
        source.packageExecutables = {package};
        source.processes = {
            {
                package,
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\secondary-volume)"),
            },
        };
        FollowerEndpointDiscovery discovery(source);

        QCOMPARE(
            discovery.candidates(CodexEnvironment{}),
            QVector<QString>({
                QStringLiteral(
                    R"(\\.\pipe\secondary-volume)"),
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void officialPackageIdentityAcceptsUpdatedSecondaryPath()
    {
        const QString updatedPackage = QStringLiteral(
            "E:/WindowsApps/"
            "OpenAI.Codex_26.722.1000.0_x64__"
            "2p2nqsd0c76g0/"
            "app/resources/codex.exe");

        QVERIFY(
            FollowerEndpointDiscovery::
                isOfficialWindowsPackageProcess(
                    updatedPackage,
                    QStringLiteral(
                        "OpenAI.Codex_2p2nqsd0c76g0")));
        QVERIFY(
            !FollowerEndpointDiscovery::
                isOfficialWindowsPackageProcess(
                    updatedPackage,
                    QStringLiteral(
                        "OpenAI.Codex_lookalike")));
    }

    void endpointDiscoveryTrustsConfiguredAndLocalCodexPaths()
    {
        FakeEndpointEvidenceSource source;
        source.processes = {
            {
                QStringLiteral("D:/Tools/Codex/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\configured)"),
            },
            {
                QStringLiteral(
                    "C:/Users/test/AppData/Local/OpenAI/Codex/"
                    "bin/build/codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\local-bin)"),
            },
            {
                QStringLiteral(
                    "C:/Users/test/AppData/Local/OpenAI/Codex-Evil/"
                    "codex.exe"),
                QStringLiteral(
                    R"(codex.exe --follower-pipe=\\.\pipe\sibling)"),
            },
        };
        CodexEnvironment environment;
        environment.localAppData =
            QStringLiteral("C:/Users/test/AppData/Local");
        environment.codexBinRoot =
            QStringLiteral(
                "C:/Users/test/AppData/Local/OpenAI/Codex/bin");
        environment.configuredExecutable =
            QStringLiteral("D:/Tools/Codex/codex.exe");
        FollowerEndpointDiscovery discovery(source);

        QCOMPARE(
            discovery.candidates(environment),
            QVector<QString>({
                QStringLiteral(R"(\\.\pipe\configured)"),
                QStringLiteral(R"(\\.\pipe\local-bin)"),
                QStringLiteral(R"(\\.\pipe\codex-ipc)"),
            }));
    }

    void frameEncodeUsesLittleEndianLengthPrefix()
    {
        const QJsonObject message{
            {QStringLiteral("type"), QStringLiteral("request")},
            {QStringLiteral("method"), QStringLiteral("initialize")},
        };

        const Result<QByteArray> result =
            FollowerFrameCodec::encode(message);

        QVERIFY(result.hasValue());
        const QByteArray frame = result.value();
        QVERIFY(frame.size() > static_cast<qsizetype>(sizeof(quint32)));
        const quint32 payloadSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(frame.constData()));
        QCOMPARE(
            payloadSize,
            static_cast<quint32>(
                frame.size() - sizeof(quint32)));
        QCOMPARE(
            QJsonDocument::fromJson(
                frame.sliced(sizeof(quint32))).object(),
            message);
    }

    void frameDecoderHandlesSplitHeaderAndPayload()
    {
        const QJsonObject message{
            {QStringLiteral("type"), QStringLiteral("response")},
            {QStringLiteral("requestId"), QStringLiteral("request-id")},
        };
        const QByteArray frame =
            FollowerFrameCodec::encode(message).value();
        FollowerFrameCodec codec;

        QCOMPARE(codec.append(QByteArrayView(frame).first(2)).value().size(), 0);
        QCOMPARE(
            codec.append(QByteArrayView(frame).sliced(2, 3)).value().size(),
            0);
        const auto completed =
            codec.append(QByteArrayView(frame).sliced(5));

        QVERIFY(completed.hasValue());
        QCOMPARE(completed.value(), QVector<QJsonObject>({message}));
    }

    void frameDecoderParsesPayloadAtParsedLimit()
    {
        const QJsonObject message =
            objectWithEncodedSize(kFollowerMaximumParsedFrameBytes);
        const QByteArray payload =
            QJsonDocument(message).toJson(QJsonDocument::Compact);
        QCOMPARE(payload.size(), kFollowerMaximumParsedFrameBytes);
        FollowerFrameCodec codec;

        const auto completed =
            codec.append(frameWithPayload(payload));

        QVERIFY(completed.hasValue());
        QCOMPARE(completed.value(), QVector<QJsonObject>({message}));
    }

    void frameDecoderDrainsLegalOversizedPayloadAndContinues()
    {
        FollowerFrameCodec codec;
        const qsizetype oversizedLength =
            kFollowerMaximumParsedFrameBytes + 1;
        QByteArray header(sizeof(quint32), Qt::Uninitialized);
        qToLittleEndian<quint32>(
            static_cast<quint32>(oversizedLength),
            reinterpret_cast<uchar*>(header.data()));
        QVERIFY(codec.append(header).hasValue());

        QByteArray chunk(64 * 1024, 'x');
        qsizetype remaining = oversizedLength;
        while (remaining > 0) {
            const qsizetype count =
                qMin(remaining, chunk.size());
            const auto drained =
                codec.append(QByteArrayView(chunk).first(count));
            QVERIFY(drained.hasValue());
            QVERIFY(drained.value().isEmpty());
            remaining -= count;
        }

        const QJsonObject next{
            {QStringLiteral("type"), QStringLiteral("response")},
            {QStringLiteral("requestId"), QStringLiteral("next")},
        };
        const auto completed =
            codec.append(FollowerFrameCodec::encode(next).value());

        QVERIFY(completed.hasValue());
        QCOMPARE(completed.value(), QVector<QJsonObject>({next}));
    }

    void frameDecoderRejectsWireLengthAboveLimitFromHeader()
    {
        FollowerFrameCodec codec;
        QByteArray header(sizeof(quint32), Qt::Uninitialized);
        qToLittleEndian<quint32>(
            static_cast<quint32>(
                kFollowerMaximumWireFrameBytes + 1),
            reinterpret_cast<uchar*>(header.data()));

        const auto result = codec.append(header);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("follower.frame_too_large"));
    }

    void malformedJsonDoesNotPoisonFollowingFrame()
    {
        FollowerFrameCodec codec;
        const QJsonObject next{
            {QStringLiteral("type"), QStringLiteral("response")},
            {QStringLiteral("requestId"), QStringLiteral("next")},
        };
        QByteArray input = frameWithPayload(QByteArray("{invalid"));
        input.append(FollowerFrameCodec::encode(next).value());

        const auto completed = codec.append(input);

        QVERIFY(completed.hasValue());
        QCOMPARE(completed.value(), QVector<QJsonObject>({next}));
    }

    void initializeRequestMatchesMacOSV034()
    {
        QCOMPARE(
            FollowerRequestFactory::initialize(
                QStringLiteral("init-id")),
            QJsonObject({
                {
                    QStringLiteral("type"),
                    QStringLiteral("request"),
                },
                {
                    QStringLiteral("requestId"),
                    QStringLiteral("init-id"),
                },
                {
                    QStringLiteral("sourceClientId"),
                    QStringLiteral("initializing-client"),
                },
                {QStringLiteral("version"), 0},
                {
                    QStringLiteral("method"),
                    QStringLiteral("initialize"),
                },
                {
                    QStringLiteral("params"),
                    QJsonObject({
                        {
                            QStringLiteral("clientType"),
                            QStringLiteral("codex-companion"),
                        },
                    }),
                },
            }));
    }

    void settingsRequestMatchesFixture()
    {
        QCOMPARE(
            FollowerRequestFactory::threadSettings(
                QStringLiteral("settings-id"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral(" gpt-5.6 "),
                QStringLiteral(" high ")),
            fixture(QStringLiteral("follower-settings.json")));
    }

    void blankSettingsProduceEmptyObject()
    {
        const QJsonObject request =
            FollowerRequestFactory::threadSettings(
                QStringLiteral("settings-id"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral(" \r\n"),
                QStringLiteral("\t"));

        QCOMPARE(
            request.value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("threadSettings"))
                .toObject(),
            QJsonObject{});
    }

    void directReplyMatchesFixtureWithAllAttachmentForms()
    {
        const auto result =
            FollowerRequestFactory::action(
                QStringLiteral("reply-id"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral("Use these references"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                QStringLiteral("C:/work/repo"),
                attachments(),
                1234);

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value(),
            fixture(QStringLiteral("follower-reply.json")));
    }

    void steerMatchesFixtureWithAllAttachmentForms()
    {
        const auto result =
            FollowerRequestFactory::action(
                QStringLiteral("steer-id"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral("Use these references"),
                SendAction::Steer,
                QStringLiteral("message-id"),
                QStringLiteral(" C:/work/repo "),
                attachments(),
                1234);

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value(),
            fixture(QStringLiteral("follower-steer.json")));
    }

    void queuedReplyPreservesGlobalStateAndDeduplicatesMessageId()
    {
        const QJsonObject initialState{
            {
                QStringLiteral("other-thread"),
                QJsonArray({
                    queuedMessage(
                        QStringLiteral("existing"),
                        QStringLiteral("Keep me"),
                        1),
                }),
            },
            {
                QStringLiteral("thread-id"),
                QJsonArray({
                    queuedMessage(
                        QStringLiteral("older"),
                        QStringLiteral("Already queued"),
                        2),
                }),
            },
        };
        const auto first =
            FollowerRequestFactory::queuedReply(
                QStringLiteral("queue-id"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral("Follow up later"),
                QStringLiteral("new-message"),
                QStringLiteral("C:/work/repo"),
                initialState,
                attachments(),
                3);

        QVERIFY(first.hasValue());
        const QJsonObject firstState =
            first.value()
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("state"))
                .toObject();
        QCOMPARE(
            firstState.value(QStringLiteral("other-thread"))
                .toArray()
                .size(),
            1);
        QCOMPARE(
            firstState.value(QStringLiteral("thread-id"))
                .toArray()
                .size(),
            2);

        const auto duplicate =
            FollowerRequestFactory::queuedReply(
                QStringLiteral("queue-id-2"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral("Follow up later"),
                QStringLiteral("new-message"),
                QStringLiteral("C:/work/repo"),
                firstState,
                attachments(),
                4);

        QVERIFY(duplicate.hasValue());
        QCOMPARE(
            duplicate.value()
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("state"))
                .toObject()
                .value(QStringLiteral("thread-id"))
                .toArray()
                .size(),
            2);
    }

    void queuedReplyRejectsMalformedGlobalState()
    {
        const auto result =
            FollowerRequestFactory::queuedReply(
                QStringLiteral("queue-id"),
                QStringLiteral("client-id"),
                QStringLiteral("thread-id"),
                QStringLiteral("Follow up later"),
                QStringLiteral("new-message"),
                QString(),
                QJsonObject({
                    {
                        QStringLiteral("thread-id"),
                        QStringLiteral("not-an-array"),
                    },
                }),
                {},
                3);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("follower.queued_state_invalid"));
    }

    void queuedStateLoaderReadsOnlyCompletePersistedQueue()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral("global-state.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject({
            {
                QStringLiteral("unrelated-setting"),
                true,
            },
            {
                QStringLiteral("queued-follow-ups"),
                QJsonObject({
                    {
                        QStringLiteral("thread-id"),
                        QJsonArray({
                            queuedMessage(
                                QStringLiteral("message-id"),
                                QStringLiteral("Keep me"),
                                1),
                        }),
                    },
                }),
            },
        })).toJson(QJsonDocument::Compact));
        file.close();

        const auto result =
            FollowerRequestFactory::
                loadQueuedFollowUpState(path);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().size(), 1);
        QCOMPARE(
            result.value()
                .value(QStringLiteral("thread-id"))
                .toArray()
                .size(),
            1);
        QVERIFY(!result.value().contains(
            QStringLiteral("unrelated-setting")));
    }

    void queuedStateLoaderReturnsEmptyWhenFileIsMissing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const auto result =
            FollowerRequestFactory::
                loadQueuedFollowUpState(
                    directory.filePath(
                        QStringLiteral("missing.json")));

        QVERIFY(result.hasValue());
        QVERIFY(result.value().isEmpty());
    }

    void queuedStateLoaderRejectsNullPersistedQueue()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral("global-state.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject({
            {
                QStringLiteral("queued-follow-ups"),
                QJsonValue(QJsonValue::Null),
            },
        })).toJson(QJsonDocument::Compact));
        file.close();

        const auto result =
            FollowerRequestFactory::
                loadQueuedFollowUpState(path);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "follower.queued_state_invalid"));
    }

    void queuedStateLoaderRejectsMalformedPersistedQueue()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral("global-state.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject({
            {
                QStringLiteral("queued-follow-ups"),
                QJsonObject({
                    {
                        QStringLiteral("thread-id"),
                        QStringLiteral("not-an-array"),
                    },
                }),
            },
        })).toJson(QJsonDocument::Compact));
        file.close();

        const auto result =
            FollowerRequestFactory::
                loadQueuedFollowUpState(path);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "follower.queued_state_invalid"));
    }

    void approvalRequestsMatchNativeDecisionShapes()
    {
        const PendingApproval command{
            QStringLiteral("thread-id"),
            42,
            PendingApprovalMethod::CommandExecution,
            QVector<QString>({
                QStringLiteral("git"),
                QStringLiteral("status"),
            }),
        };
        const QJsonObject commandRequest =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                command,
                ApprovalDecision::ApproveSimilar);
        QCOMPARE(
            commandRequest.value(QStringLiteral("method")).toString(),
            QStringLiteral(
                "thread-follower-command-approval-decision"));
        QCOMPARE(
            commandRequest.value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"))
                .toObject()
                .value(QStringLiteral(
                    "acceptWithExecpolicyAmendment"))
                .toObject()
                .value(QStringLiteral("execpolicy_amendment"))
                .toArray(),
            QJsonArray({
                QStringLiteral("git"),
                QStringLiteral("status"),
            }));

        const PendingApproval file{
            QStringLiteral("thread-id"),
            17,
            PendingApprovalMethod::FileChange,
            std::nullopt,
        };
        const QJsonObject fileRequest =
            FollowerRequestFactory::approval(
                QStringLiteral("approval-id"),
                QStringLiteral("client-id"),
                file,
                ApprovalDecision::ApproveOnce);
        QCOMPARE(
            fileRequest.value(QStringLiteral("method")).toString(),
            QStringLiteral(
                "thread-follower-file-approval-decision"));
        QCOMPARE(
            fileRequest.value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("decision"))
                .toString(),
            QStringLiteral("accept"));
    }

    void outcomeMappingsMatchMacOSV034()
    {
        QCOMPARE(
            FollowerRequestFactory::sendOutcomeForError(
                QStringLiteral("no-client-found")),
            FollowerSendOutcome::ThreadNotLoaded);
        QCOMPARE(
            FollowerRequestFactory::sendOutcomeForError(
                QStringLiteral(
                    "Cannot steer without an active turn")),
            FollowerSendOutcome::NoActiveTurn);
        QCOMPARE(
            FollowerRequestFactory::sendOutcomeForError(
                QStringLiteral("request timed out")),
            FollowerSendOutcome::TimedOut);
        QCOMPARE(
            FollowerRequestFactory::approvalOutcomeForError(
                QStringLiteral("no pending approval")),
            FollowerApprovalOutcome::RequestNotFound);
    }

    void clientDefaultsMatchFollowerContract()
    {
        QCOMPARE(
            FollowerClient::defaultConnectTimeout(),
            500ms);
        QCOMPARE(
            FollowerClient::defaultResponseTimeout(),
            45s);
    }

    void clientRejectsUnverifiedPipeServerBeforeInitialize()
    {
        const QString endpoint = uniquePipeEndpoint();
        std::atomic_bool verifierCalled = false;
        std::atomic_bool receivedBytes = false;
        std::atomic<quint32> observedProcessId = 0;
        QSemaphore verificationFinished;
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                if (!verificationFinished.tryAcquire(
                        1,
                        2000)) {
                    throw std::runtime_error(
                        "Server verification did not run.");
                }
                std::this_thread::sleep_for(75ms);
                DWORD available = 0;
                if (PeekNamedPipe(
                        pipe,
                        nullptr,
                        0,
                        nullptr,
                        &available,
                        nullptr)) {
                    receivedBytes.store(
                        available > 0);
                    return;
                }
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE
                    && error
                        != ERROR_PIPE_NOT_CONNECTED) {
                    throw pipeFailure("PeekNamedPipe");
                }
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            [&](quint32 processId) {
                observedProcessId.store(processId);
                verifierCalled.store(true);
                verificationFinished.release();
                return false;
            });

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));
        server.finish();

        QCOMPARE(
            outcome,
            FollowerSendOutcome::
                SharedDaemonUnavailable);
        QVERIFY(verifierCalled.load());
        QCOMPARE(
            observedProcessId.load(),
            static_cast<quint32>(
                GetCurrentProcessId()));
        QVERIFY(!receivedBytes.load());
    }

    void productionClientTrustsExplicitConfiguredExecutable()
    {
        const QString endpoint = uniquePipeEndpoint();
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                const QJsonObject action =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        action.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        CodexEnvironment environment;
        environment.configuredExecutable =
            currentExecutablePath();
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            FollowerClient::trustedServerVerifier(
                environment));

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));

        QCOMPARE(outcome, FollowerSendOutcome::Sent);
        server.finish();
    }

    void clientSharesOneConnectDeadlineAcrossCandidates()
    {
        const QString first = uniquePipeEndpoint();
        const QString second = uniquePipeEndpoint();
        QSemaphore releaseFirst;
        QSemaphore releaseSecond;
        TestPipeServer firstServer(
            first,
            [&](HANDLE) {
                releaseFirst.acquire();
            });
        TestNativeHandle firstOccupant =
            occupyPipe(first);
        TestPipeServer secondServer(
            second,
            [&](HANDLE) {
                releaseSecond.acquire();
            });
        TestNativeHandle secondOccupant =
            occupyPipe(second);
        FollowerClient client(
            [first, second] {
                return QVector<QString>({first, second});
            },
            300ms,
            2s,
            currentProcessServerVerifier());

        const auto started =
            std::chrono::steady_clock::now();
        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));
        const auto elapsed =
            std::chrono::steady_clock::now() - started;

        firstOccupant.reset();
        secondOccupant.reset();
        releaseFirst.release();
        releaseSecond.release();
        firstServer.finish();
        secondServer.finish();

        QCOMPARE(
            outcome,
            FollowerSendOutcome::
                SharedDaemonUnavailable);
        QVERIFY2(
            elapsed < 500ms,
            "The connect timeout restarted for a later candidate.");
    }

    void clientTriesNextCandidateOnlyWhenEarlierEndpointIsUnavailable()
    {
        const QString current = uniquePipeEndpoint();
        const QString legacy = uniquePipeEndpoint();
        QVector<QJsonObject> received;
        TestPipeServer server(
            legacy,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                received.push_back(initialize);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));

                const QJsonObject action =
                    readPipeMessage(pipe);
                received.push_back(action);
                writePipeMessage(
                    pipe,
                    successResponse(
                        action.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        FollowerClient client(
            [current, legacy] {
                return QVector<QString>({current, legacy});
            },
            25ms,
            2s,
            currentProcessServerVerifier());

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Keep working"),
                QStringLiteral("thread-id"),
                SendAction::Steer,
                QStringLiteral("message-id"),
                QStringLiteral("C:/work/repo"),
                {}));
        server.finish();

        QCOMPARE(outcome, FollowerSendOutcome::Sent);
        QCOMPARE(received.size(), 2);
        QCOMPARE(
            received.at(0)
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("initialize"));
        QCOMPARE(
            received.at(1)
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral(
                "thread-follower-steer-turn"));
    }

    void clientReturnsUnavailableWhenNoCandidateConnects()
    {
        const QString endpoint = uniquePipeEndpoint();
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            25ms,
            2s,
            currentProcessServerVerifier());

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));

        QCOMPARE(
            outcome,
            FollowerSendOutcome::
                SharedDaemonUnavailable);
    }

    void clientIgnoresNotificationsAndOtherRequestIds()
    {
        const QString endpoint = uniquePipeEndpoint();
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    {
                        {
                            QStringLiteral("type"),
                            QStringLiteral("notification"),
                        },
                        {
                            QStringLiteral("method"),
                            QStringLiteral("thread-updated"),
                        },
                    });
                writePipeMessage(
                    pipe,
                    successResponse(
                        QStringLiteral("other-initialize"),
                        QStringLiteral("wrong-client")));
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));

                const QJsonObject action =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        QStringLiteral("other-action")));
                writePipeMessage(
                    pipe,
                    successResponse(
                        action.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            currentProcessServerVerifier());

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));
        server.finish();

        QCOMPARE(outcome, FollowerSendOutcome::Sent);
    }

    void clientSendsThreadSettingsAsOneFollowerOperation()
    {
        const QString endpoint = uniquePipeEndpoint();
        QVector<QJsonObject> received;
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                received.push_back(initialize);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                const QJsonObject settings =
                    readPipeMessage(pipe);
                received.push_back(settings);
                writePipeMessage(
                    pipe,
                    successResponse(
                        settings.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            currentProcessServerVerifier());

        const FollowerSendOutcome outcome = finished(
            client.updateThreadSettings(
                QStringLiteral("thread-id"),
                QStringLiteral(" gpt-5.6 "),
                QStringLiteral(" high ")));
        server.finish();

        QCOMPARE(outcome, FollowerSendOutcome::Sent);
        QCOMPARE(received.size(), 2);
        QCOMPARE(
            received.at(1)
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral(
                "thread-follower-update-thread-settings"));
        const QJsonObject settings =
            received.at(1)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("threadSettings"))
                .toObject();
        QCOMPARE(
            settings.value(QStringLiteral("model")).toString(),
            QStringLiteral("gpt-5.6"));
        QCOMPARE(
            settings.value(QStringLiteral("effort")).toString(),
            QStringLiteral("high"));
    }

    void clientQueuesReplyUsingCompletePersistedState()
    {
        const QString endpoint = uniquePipeEndpoint();
        QVector<QJsonObject> received;
        const QJsonObject queuedState{
            {
                QStringLiteral("other-thread"),
                QJsonArray({
                    queuedMessage(
                        QStringLiteral("existing"),
                        QStringLiteral("Keep me"),
                        1),
                }),
            },
        };
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                const QJsonObject action =
                    readPipeMessage(pipe);
                received.push_back(action);
                writePipeMessage(
                    pipe,
                    successResponse(
                        action.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            currentProcessServerVerifier(),
            [queuedState] {
                return Result<QJsonObject>::success(
                    queuedState);
            });

        const FollowerSendOutcome outcome = finished(
            client.queueReply(
                QStringLiteral("Follow up later"),
                QStringLiteral("thread-id"),
                QStringLiteral("message-id"),
                QStringLiteral("C:/work/repo"),
                attachments()));
        server.finish();

        QCOMPARE(outcome, FollowerSendOutcome::Sent);
        QCOMPARE(received.size(), 1);
        QCOMPARE(
            received.at(0)
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral(
                "thread-follower-set-queued-follow-ups-state"));
        const QJsonObject state =
            received.at(0)
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("state"))
                .toObject();
        QCOMPARE(
            state.value(QStringLiteral("other-thread"))
                .toArray()
                .size(),
            1);
        QCOMPARE(
            state.value(QStringLiteral("thread-id"))
                .toArray()
                .size(),
            1);
    }

    void queuedStateLoaderExceptionFailsClosedBeforeConnecting()
    {
        const QString endpoint = uniquePipeEndpoint();
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            25ms,
            2s,
            currentProcessServerVerifier(),
            []() -> Result<QJsonObject> {
                throw std::runtime_error(
                    "loader failed");
            });

        const FollowerSendOutcome outcome = finished(
            client.queueReply(
                QStringLiteral("Follow up later"),
                QStringLiteral("thread-id"),
                QStringLiteral("message-id"),
                {},
                {}));

        QCOMPARE(outcome, FollowerSendOutcome::Failed);
    }

    void clientRespondsToApprovalWithOriginalRequestIdentity()
    {
        const QString endpoint = uniquePipeEndpoint();
        QJsonObject received;
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                received = readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        received.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            currentProcessServerVerifier());
        const PendingApproval approval{
            QStringLiteral("thread-id"),
            42,
            PendingApprovalMethod::CommandExecution,
            QVector<QString>({
                QStringLiteral("git"),
                QStringLiteral("status"),
            }),
        };

        const FollowerApprovalOutcome outcome = finished(
            client.respondToApproval(
                approval,
                ApprovalDecision::ApproveSimilar));
        server.finish();

        QCOMPARE(
            outcome,
            FollowerApprovalOutcome::Approved);
        QCOMPARE(
            received.value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("requestId"))
                .toInteger(),
            42);
    }

    void clientMapsMissingApprovalRequest()
    {
        const QString endpoint = uniquePipeEndpoint();
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                const QJsonObject approval =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    errorResponse(
                        approval.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral(
                            "no pending approval")));
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            2s,
            currentProcessServerVerifier());

        const FollowerApprovalOutcome outcome = finished(
            client.respondToApproval(
                {
                    QStringLiteral("thread-id"),
                    42,
                    PendingApprovalMethod::FileChange,
                    std::nullopt,
                },
                ApprovalDecision::Decline));
        server.finish();

        QCOMPARE(
            outcome,
            FollowerApprovalOutcome::RequestNotFound);
    }

    void clientDrainsOversizedParsedFrameBeforeMatchingResponse()
    {
        const QString endpoint = uniquePipeEndpoint();
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writeRawFrame(
                    pipe,
                    static_cast<quint32>(
                        kFollowerMaximumParsedFrameBytes + 1),
                    true);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                const QJsonObject action =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        action.value(
                            QStringLiteral("requestId"))
                            .toString()));
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            5s,
            currentProcessServerVerifier());

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));
        server.finish();

        QCOMPARE(outcome, FollowerSendOutcome::Sent);
    }

    void clientMapsSharedResponseDeadlineToTimedOut()
    {
        const QString endpoint = uniquePipeEndpoint();
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                static_cast<void>(readPipeMessage(pipe));
                std::this_thread::sleep_for(100ms);
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            25ms,
            currentProcessServerVerifier());

        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));
        server.finish();

        QCOMPARE(outcome, FollowerSendOutcome::TimedOut);
    }

    void clientCancellationStopsPendingRead()
    {
        const QString endpoint = uniquePipeEndpoint();
        QSemaphore initializeRead;
        TestPipeServer server(
            endpoint,
            [&](HANDLE pipe) {
                static_cast<void>(readPipeMessage(pipe));
                initializeRead.release();
                std::this_thread::sleep_for(100ms);
            });
        FollowerClient client(
            [endpoint] {
                return QVector<QString>({endpoint});
            },
            500ms,
            10s,
            currentProcessServerVerifier());
        QFuture<FollowerSendOutcome> future =
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {});
        QVERIFY(initializeRead.tryAcquire(1, 2000));

        future.cancel();
        future.waitForFinished();
        server.finish();

        QVERIFY(future.isCanceled());
        QVERIFY(future.isFinished());
    }

    void clientDoesNotRerouteAfterFollowerErrorResponse()
    {
        const QString first = uniquePipeEndpoint();
        const QString second = uniquePipeEndpoint();
        TestPipeServer server(
            first,
            [&](HANDLE pipe) {
                const QJsonObject initialize =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    successResponse(
                        initialize.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("client-id")));
                const QJsonObject action =
                    readPipeMessage(pipe);
                writePipeMessage(
                    pipe,
                    errorResponse(
                        action.value(
                            QStringLiteral("requestId"))
                            .toString(),
                        QStringLiteral("no-client-found")));
            });
        FollowerClient client(
            [first, second] {
                return QVector<QString>({first, second});
            },
            250ms,
            2s,
            currentProcessServerVerifier());

        const auto started =
            std::chrono::steady_clock::now();
        const FollowerSendOutcome outcome = finished(
            client.submit(
                QStringLiteral("Follow up"),
                QStringLiteral("thread-id"),
                SendAction::Reply,
                QStringLiteral("message-id"),
                {},
                {}));
        const auto elapsed =
            std::chrono::steady_clock::now() - started;
        server.finish();

        QCOMPARE(
            outcome,
            FollowerSendOutcome::ThreadNotLoaded);
        QVERIFY(elapsed < 200ms);
    }
};

QTEST_APPLESS_MAIN(FollowerProtocolTests)

#include "FollowerProtocolTests.moc"
