#include "codex/chat/ChatCatalog.h"
#include "codex/chat/ChatService.h"
#include "core/CredentialStore.h"
#include "platform/windows/DpapiCredentialStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Aclapi.h>
#include <Windows.h>
#include <dpapi.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace companion;

namespace {

constexpr auto kOpenAIService =
    "companion.openai-api-key";
constexpr auto kLumoService =
    "companion.lumo-api-key";

CompanionError missingCredential(
    const QString& service)
{
    return {
        QStringLiteral("credential.not_found"),
        QStringLiteral("The requested credential was not found."),
        false,
        {{QStringLiteral("service"), service}},
    };
}

class MemoryCredentialStore final : public CredentialStore {
public:
    Result<QByteArray> read(
        const QString& service) const override
    {
        const QMutexLocker lock(&mutex_);
        const auto value = values_.constFind(service);
        if (value == values_.constEnd()) {
            return Result<QByteArray>::failure(
                missingCredential(service));
        }
        return Result<QByteArray>::success(value.value());
    }

    Result<void> write(
        const QString& service,
        QByteArrayView secret) override
    {
        const QMutexLocker lock(&mutex_);
        values_.insert(
            service,
            QByteArray(secret.data(), secret.size()));
        return Result<void>::success();
    }

    Result<void> remove(
        const QString& service) override
    {
        const QMutexLocker lock(&mutex_);
        values_.remove(service);
        return Result<void>::success();
    }

    bool contains(
        const QString& service) const override
    {
        const QMutexLocker lock(&mutex_);
        return values_.contains(service);
    }

private:
    mutable QMutex mutex_;
    QHash<QString, QByteArray> values_;
};

class ScriptedCredentialStore final
    : public CredentialStore {
public:
    using Reader = std::function<
        Result<QByteArray>(const QString&)>;

    explicit ScriptedCredentialStore(
        Reader reader)
        : reader_(std::move(reader))
    {
    }

    Result<QByteArray> read(
        const QString& service) const override
    {
        reads_.append(service);
        return reader_(service);
    }

    Result<void> write(
        const QString&,
        QByteArrayView) override
    {
        return Result<void>::success();
    }

    Result<void> remove(
        const QString&) override
    {
        return Result<void>::success();
    }

    bool contains(
        const QString&) const override
    {
        return false;
    }

    QStringList reads() const
    {
        return reads_;
    }

private:
    Reader reader_;
    mutable QStringList reads_;
};

class InspectableCredentialStore final
    : public CredentialStore {
public:
    explicit InspectableCredentialStore(
        QByteArray bytes)
        : bytes_(std::move(bytes))
    {
    }

    Result<QByteArray> read(
        const QString&) const override
    {
        return Result<QByteArray>::success(
            QByteArray::fromRawData(
                bytes_.constData(),
                bytes_.size()));
    }

    Result<void> write(
        const QString&,
        QByteArrayView) override
    {
        return Result<void>::success();
    }

    Result<void> remove(
        const QString&) override
    {
        return Result<void>::success();
    }

    bool contains(
        const QString&) const override
    {
        return true;
    }

    QByteArray bytes() const
    {
        return bytes_;
    }

private:
    QByteArray bytes_;
};

class RecordingHttpTransport final
    : public ChatHttpTransport {
public:
    using Responder = std::function<
        Result<ChatHttpResponse>(
            const ChatHttpRequest&)>;

    explicit RecordingHttpTransport(
        Responder responder)
        : responder_(std::move(responder))
    {
    }

    Result<ChatHttpResponse> post(
        const ChatHttpRequest& request) override
    {
        {
            const QMutexLocker lock(&mutex_);
            requests_.append(request);
        }
        return responder_(request);
    }

    int callCount() const
    {
        const QMutexLocker lock(&mutex_);
        return requests_.size();
    }

    ChatHttpRequest lastRequest() const
    {
        const QMutexLocker lock(&mutex_);
        return requests_.last();
    }

private:
    Responder responder_;
    mutable QMutex mutex_;
    QVector<ChatHttpRequest> requests_;
};

Result<ChatResult> waitFor(
    QFuture<Result<ChatResult>> future)
{
    future.waitForFinished();
    return future.result();
}

Result<ChatHttpResponse> jsonResponse(
    int statusCode,
    QByteArray body)
{
    return Result<ChatHttpResponse>::success({
        statusCode,
        std::move(body),
    });
}

std::shared_ptr<RecordingHttpTransport>
successfulTransport(QByteArray body)
{
    return std::make_shared<RecordingHttpTransport>(
        [body = std::move(body)](
            const ChatHttpRequest&) {
            return jsonResponse(200, body);
        });
}

BridgeAttachment exampleAttachment()
{
    return {
        QUuid::createUuid(),
        AttachmentKind::File,
        QStringLiteral("notes.txt"),
        QStringLiteral("text/plain"),
        QByteArray("hello"),
    };
}

QJsonObject requestBody(
    const ChatHttpRequest& request)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            request.body, &error);
    if (error.error !=
            QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    return document.object();
}

QByteArray readOnlyCredentialFile(
    const QString& directory)
{
    const QStringList files =
        QDir(directory).entryList(
            QDir::Files | QDir::NoDotAndDotDot);
    if (files.size() != 1) {
        return {};
    }
    QFile file(
        QDir(directory).filePath(files.front()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString onlyCredentialPath(
    const QString& directory)
{
    const QStringList files =
        QDir(directory).entryList(
            QDir::Files | QDir::NoDotAndDotDot);
    return files.size() == 1
        ? QDir(directory).filePath(files.front())
        : QString();
}

bool isCurrentUserOnlyAcl(
    const QString& path,
    bool requireProtected)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &token)) {
        return false;
    }

    DWORD tokenBytes = 0;
    GetTokenInformation(
        token,
        TokenUser,
        nullptr,
        0,
        &tokenBytes);
    QByteArray tokenBuffer(
        static_cast<qsizetype>(tokenBytes),
        Qt::Uninitialized);
    const BOOL readToken = GetTokenInformation(
        token,
        TokenUser,
        tokenBuffer.data(),
        tokenBytes,
        &tokenBytes);
    CloseHandle(token);
    if (!readToken) {
        return false;
    }
    const auto* tokenUser =
        reinterpret_cast<const TOKEN_USER*>(
            tokenBuffer.constData());

    std::wstring nativePath = path.toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD securityStatus =
        GetNamedSecurityInfoW(
            nativePath.data(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            &dacl,
            nullptr,
            &descriptor);
    if (securityStatus != ERROR_SUCCESS
        || descriptor == nullptr
        || dacl == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool protectedDacl =
        GetSecurityDescriptorControl(
            descriptor,
            &control,
            &revision)
        && (control & SE_DACL_PROTECTED) != 0;

    bool currentUserAllowed = false;
    bool otherIdentityAllowed = false;
    for (DWORD index = 0;
         index < dacl->AceCount;
         ++index) {
        void* rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            otherIdentityAllowed = true;
            break;
        }
        const auto* header =
            static_cast<const ACE_HEADER*>(
                rawAce);
        if (header->AceType !=
            ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        const auto* ace =
            static_cast<const ACCESS_ALLOWED_ACE*>(
                rawAce);
        PSID sid = const_cast<DWORD*>(
            &ace->SidStart);
        if (EqualSid(
                sid,
                tokenUser->User.Sid)) {
            currentUserAllowed = true;
        } else {
            otherIdentityAllowed = true;
        }
    }

    LocalFree(descriptor);
    return (!requireProtected || protectedDacl)
        && currentUserAllowed
        && !otherIdentityAllowed;
}

class MessageCapture final {
public:
    MessageCapture()
    {
        QMutexLocker lock(&globalMutex());
        activeCapture() = this;
        previous_ = qInstallMessageHandler(
            &MessageCapture::handle);
    }

    ~MessageCapture()
    {
        QMutexLocker lock(&globalMutex());
        qInstallMessageHandler(previous_);
        activeCapture() = nullptr;
    }

    QString joined() const
    {
        const QMutexLocker lock(&mutex_);
        return messages_.join('\n');
    }

private:
    static QMutex& globalMutex()
    {
        static QMutex mutex;
        return mutex;
    }

    static MessageCapture*& activeCapture()
    {
        static MessageCapture* capture = nullptr;
        return capture;
    }

    static void handle(
        QtMsgType,
        const QMessageLogContext&,
        const QString& message)
    {
        QMutexLocker globalLock(&globalMutex());
        MessageCapture* capture =
            activeCapture();
        if (capture == nullptr) {
            return;
        }
        const QMutexLocker lock(
            &capture->mutex_);
        capture->messages_.append(message);
    }

    QtMessageHandler previous_ = nullptr;
    mutable QMutex mutex_;
    QStringList messages_;
};

} // namespace

class ChatCatalogTests final : public QObject {
    Q_OBJECT

private slots:
    void agentsMatchV034BuiltIns()
    {
        QCOMPARE(
            ChatCatalog::agents(),
            QVector<BridgeChatAgent>({
                {
                    QStringLiteral("general"),
                    QStringLiteral("General"),
                    QStringLiteral(
                        "Direct answers and everyday help"),
                    QStringLiteral("sparkles"),
                },
                {
                    QStringLiteral("explain"),
                    QStringLiteral("Explain"),
                    QStringLiteral(
                        "Clear explanations with useful context"),
                    QStringLiteral(
                        "text.book.closed"),
                },
                {
                    QStringLiteral("plan"),
                    QStringLiteral("Plan"),
                    QStringLiteral(
                        "Practical steps and tradeoffs"),
                    QStringLiteral("checklist"),
                },
                {
                    QStringLiteral("create"),
                    QStringLiteral("Create"),
                    QStringLiteral(
                        "Ideas, drafts, and alternatives"),
                    QStringLiteral(
                        "wand.and.stars"),
                },
            }));
    }

    void agentResolutionMatchesV034Prompts()
    {
        const auto general =
            ChatCatalog::resolveAgent(
                QStringLiteral("general"));
        QCOMPARE(
            general.agent.id,
            QStringLiteral("general"));
        QCOMPARE(
            general.agent.name,
            QStringLiteral("General"));
        QCOMPARE(
            general.promptInstruction,
            QStringLiteral(
                "Answer directly and concisely."));

        const auto explain =
            ChatCatalog::resolveAgent(
                QStringLiteral("explain"));
        QCOMPARE(
            explain.agent.name,
            QStringLiteral("Explain"));
        QCOMPARE(
            explain.promptInstruction,
            QStringLiteral(
                "Explain the answer clearly, define unfamiliar terms, and use a short example when useful."));

        const auto plan =
            ChatCatalog::resolveAgent(
                QStringLiteral("plan"));
        QCOMPARE(
            plan.promptInstruction,
            QStringLiteral(
                "Turn the request into a practical ordered plan. State important constraints and tradeoffs."));

        const auto create =
            ChatCatalog::resolveAgent(
                QStringLiteral("create"));
        QCOMPARE(
            create.promptInstruction,
            QStringLiteral(
                "Generate polished ideas or drafts. Offer distinct alternatives when there is more than one good direction."));

        for (const QString& fallbackId :
             {QString(), QStringLiteral("unknown")}) {
            const auto fallback =
                ChatCatalog::resolveAgent(
                    fallbackId);
            QCOMPARE(
                fallback.agent.id,
                QStringLiteral("general"));
            QCOMPARE(
                fallback.promptInstruction,
                QStringLiteral(
                    "Answer directly and concisely."));
        }
    }

    void catalogMatchesV034Fixture()
    {
        QFile fixture(QStringLiteral(
            COMPANION_FIXTURE_ROOT
            "/codex-v034/capabilities.json"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QJsonDocument expectedDocument =
            QJsonDocument::fromJson(
                fixture.readAll());
        QVERIFY(expectedDocument.isObject());
        const QJsonArray expected =
            expectedDocument.object()
                .value(QStringLiteral("chatModels"))
                .toArray();

        const QVector<BridgeChatModel> actual =
            ChatCatalog::capabilities({
                true,
                false,
                true,
                true,
            });

        QCOMPARE(actual.size(), expected.size());
        for (qsizetype index = 0;
             index < actual.size();
             ++index) {
            const QJsonObject row =
                expected.at(index).toObject();
            QCOMPARE(
                actual.at(index).id,
                row.value(QStringLiteral("id"))
                    .toString());
            QCOMPARE(
                actual.at(index).model,
                row.value(QStringLiteral("model"))
                    .toString());
            QCOMPARE(
                actual.at(index).displayName,
                row.value(
                       QStringLiteral("displayName"))
                    .toString());
            QCOMPARE(
                actual.at(index).description,
                row.value(
                       QStringLiteral("description"))
                    .toString());
            QCOMPARE(
                actual.at(index).isDefault,
                row.value(QStringLiteral("isDefault"))
                    .toBool());
            QCOMPARE(
                actual.at(index).isAvailable,
                row.value(
                       QStringLiteral("isAvailable"))
                    .toBool());
            QCOMPARE(
                actual.at(index).supportsAttachments,
                row.value(QStringLiteral(
                              "supportsAttachments"))
                    .toBool());
        }

        QCOMPARE(
            actual.at(0).provider,
            ChatProvider::OnDevice);
        QCOMPARE(
            actual.at(1).provider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            actual.at(4).provider,
            ChatProvider::LumoAPI);
    }

    void missingKeysKeepCloudModelsVisible()
    {
        const QVector<BridgeChatModel> models =
            ChatCatalog::capabilities({
                true,
                false,
                false,
                false,
            });

        QCOMPARE(models.size(), 7);
        QVERIFY(models.at(0).isAvailable);
        QVERIFY(models.at(0).isDefault);
        QVERIFY(!models.at(0).supportsAttachments);
        for (qsizetype index = 1;
             index < models.size();
             ++index) {
            QVERIFY(!models.at(index).isAvailable);
            QVERIFY(!models.at(index)
                         .supportsAttachments);
        }
        QCOMPARE(
            models.at(1).description,
            QStringLiteral(
                "Add an OpenAI API key on this PC"));
        QCOMPARE(
            models.at(4).description,
            QStringLiteral(
                "Add a Lumo API key on this PC"));
    }

    void typedAvailabilityControlsEveryProvider()
    {
        const QVector<BridgeChatModel> unavailable =
            ChatCatalog::capabilities({});
        QCOMPARE(unavailable.size(), 7);
        QVERIFY(!unavailable.at(0).isAvailable);
        QVERIFY(
            !unavailable.at(0)
                 .supportsAttachments);
        for (qsizetype index = 1;
             index < unavailable.size();
             ++index) {
            QVERIFY(
                !unavailable.at(index)
                     .isAvailable);
        }

        const QVector<BridgeChatModel> available =
            ChatCatalog::capabilities({
                true,
                true,
                true,
                true,
            });
        QVERIFY(available.at(0).isAvailable);
        QVERIFY(
            available.at(0)
                .supportsAttachments);
        for (qsizetype index = 1;
             index < available.size();
             ++index) {
            QVERIFY(available.at(index).isAvailable);
        }
    }

    void credentialAvailabilityUsesSharedServiceIdentifiers()
    {
        ScriptedCredentialStore store(
            [](const QString&) {
                return Result<QByteArray>::success(
                    QByteArray("secret"));
            });

        QVERIFY(
            ChatService::hasUsableCredential(
                store,
                ChatProvider::OpenAIAPI));
        QVERIFY(
            ChatService::hasUsableCredential(
                store,
                ChatProvider::LumoAPI));

        QCOMPARE(
            store.reads(),
            QStringList({
                QString::fromLatin1(
                    kOpenAIService),
                QString::fromLatin1(
                    kLumoService),
            }));
    }

    void missingCredentialIsUnavailable()
    {
        ScriptedCredentialStore store(
            [](const QString& service) {
                return Result<QByteArray>::failure(
                    missingCredential(service));
            });

        QVERIFY(
            !ChatService::hasUsableCredential(
                store,
                ChatProvider::OpenAIAPI));
    }

    void blankCredentialIsUnavailable()
    {
        ScriptedCredentialStore store(
            [](const QString&) {
                return Result<QByteArray>::success(
                    QByteArray(" \t\r\n "));
            });

        QVERIFY(
            !ChatService::hasUsableCredential(
                store,
                ChatProvider::OpenAIAPI));
    }

    void corruptCredentialIsUnavailable()
    {
        ScriptedCredentialStore store(
            [](const QString&) {
                return Result<QByteArray>::failure({
                    QStringLiteral(
                        "credential.decrypt_failed"),
                    QStringLiteral(
                        "SECRET_CORRUPT_CREDENTIAL"),
                    false,
                    {
                        {
                            QStringLiteral("service"),
                            QStringLiteral(
                                "companion.openai-api-key"),
                        },
                    },
                });
            });

        QVERIFY(
            !ChatService::hasUsableCredential(
                store,
                ChatProvider::OpenAIAPI));
    }

    void throwingCredentialStoreIsUnavailable()
    {
        ScriptedCredentialStore store(
            [](const QString&)
                -> Result<QByteArray> {
                throw std::runtime_error(
                    "SECRET_THROWN_CREDENTIAL");
            });

        QVERIFY(
            !ChatService::hasUsableCredential(
                store,
                ChatProvider::LumoAPI));
    }

    void unsupportedCredentialProviderIsUnavailable()
    {
        ScriptedCredentialStore store(
            [](const QString&) {
                return Result<QByteArray>::success(
                    QByteArray("secret"));
            });

        QVERIFY(
            !ChatService::hasUsableCredential(
                store,
                static_cast<ChatProvider>(99)));
        QVERIFY(store.reads().isEmpty());
    }

    void credentialAvailabilityClearsReadableBytes_data()
    {
        QTest::addColumn<QByteArray>("secret");
        QTest::addColumn<bool>("available");

        QTest::newRow("usable")
            << QByteArray("  readable-secret  ")
            << true;
        QTest::newRow("blank")
            << QByteArray(" \t\r\n ")
            << false;
    }

    void credentialAvailabilityClearsReadableBytes()
    {
        QFETCH(QByteArray, secret);
        QFETCH(bool, available);
        InspectableCredentialStore store(
            std::move(secret));

        QCOMPARE(
            ChatService::hasUsableCredential(
                store,
                ChatProvider::OpenAIAPI),
            available);

        const QByteArray cleared =
            store.bytes();
        QVERIFY(!cleared.isEmpty());
        for (const char byte : cleared) {
            QCOMPARE(byte, '\0');
        }
    }

    void openAIRequestUsesSelectedModelContract()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView(
                            "openai-secret"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray(
                R"({"output_text":"  The Moon is a satellite.  ","usage":{"input_tokens":8,"output_tokens":6}})"));
        auto lumo = successfulTransport(
            QByteArray(
                R"({"choices":[{"message":{"role":"assistant","content":"unused"}}]})"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Terra"),
                QStringLiteral("Explain the moon."),
                {},
            }));

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().text,
            QStringLiteral(
                "The Moon is a satellite."));
        QCOMPARE(
            result.value().inputTokens,
            std::optional<qint64>(8));
        QCOMPARE(
            result.value().outputTokens,
            std::optional<qint64>(6));
        QCOMPARE(openAI->callCount(), 1);
        QCOMPARE(lumo->callCount(), 0);

        const ChatHttpRequest request =
            openAI->lastRequest();
        QCOMPARE(
            request.endpoint,
            QUrl(QStringLiteral(
                "https://api.openai.com/v1/responses")));
        QCOMPARE(
            request.authorization,
            QByteArray("Bearer openai-secret"));
        QCOMPARE(
            request.contentType,
            QByteArray("application/json"));

        const QJsonObject body =
            requestBody(request);
        QCOMPARE(
            body.value(QStringLiteral("model"))
                .toString(),
            QStringLiteral("gpt-5.6-terra"));
        QCOMPARE(
            body.value(QStringLiteral("input"))
                .toString(),
            QStringLiteral("Explain the moon."));
        QCOMPARE(
            body.value(QStringLiteral("reasoning"))
                .toObject()
                .value(QStringLiteral("effort"))
                .toString(),
            QStringLiteral("high"));
        QCOMPARE(
            body.value(QStringLiteral("text"))
                .toObject()
                .value(QStringLiteral("verbosity"))
                .toString(),
            QStringLiteral("medium"));
        QCOMPARE(
            body.value(
                    QStringLiteral(
                        "max_output_tokens"))
                .toInt(),
            700);
    }

    void defaultHttpTransportCanBeOwnedByProductionRuntime()
    {
        const auto first =
            ChatService::
                createDefaultHttpTransport();
        const auto second =
            ChatService::
                createDefaultHttpTransport();

        QVERIFY(first != nullptr);
        QVERIFY(second != nullptr);
        QVERIFY(first != second);
    }

    void openAIProviderMappings_data()
    {
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("apiModel");
        QTest::addColumn<QString>("effort");
        QTest::addColumn<QString>("verbosity");

        QTest::newRow("luna")
            << QStringLiteral("gpt56Luna")
            << QStringLiteral("gpt-5.6-luna")
            << QStringLiteral("low")
            << QStringLiteral("low");
        QTest::newRow("terra")
            << QStringLiteral("gpt56Terra")
            << QStringLiteral("gpt-5.6-terra")
            << QStringLiteral("high")
            << QStringLiteral("medium");
        QTest::newRow("sol")
            << QStringLiteral("gpt56Sol")
            << QStringLiteral("gpt-5.6-sol")
            << QStringLiteral("xhigh")
            << QStringLiteral("medium");
        QTest::newRow("unknown")
            << QStringLiteral("future-model")
            << QStringLiteral("gpt-5.6-luna")
            << QStringLiteral("low")
            << QStringLiteral("low");
    }

    void openAIProviderMappings()
    {
        QFETCH(QString, selection);
        QFETCH(QString, apiModel);
        QFETCH(QString, effort);
        QFETCH(QString, verbosity);

        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("key"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray(
                R"({"output_text":"answer"})"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                selection,
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(result.hasValue());
        const QJsonObject body =
            requestBody(openAI->lastRequest());
        QCOMPARE(
            body.value(QStringLiteral("model"))
                .toString(),
            apiModel);
        QCOMPARE(
            body.value(QStringLiteral("reasoning"))
                .toObject()
                .value(QStringLiteral("effort"))
                .toString(),
            effort);
        QCOMPARE(
            body.value(QStringLiteral("text"))
                .toObject()
                .value(QStringLiteral("verbosity"))
                .toString(),
            verbosity);
    }

    void unknownOpenAISelectionUsesLuna()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("key"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray(
                R"({"output":[{"content":[{"type":"output_text","text":"First"},{"type":"output_text","text":"Second"}]}]})"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("future-model"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().text,
            QStringLiteral("First\nSecond"));
        const QJsonObject body =
            requestBody(openAI->lastRequest());
        QCOMPARE(
            body.value(QStringLiteral("model"))
                .toString(),
            QStringLiteral("gpt-5.6-luna"));
        QCOMPARE(
            body.value(QStringLiteral("reasoning"))
                .toObject()
                .value(QStringLiteral("effort"))
                .toString(),
            QStringLiteral("low"));
        QCOMPARE(
            body.value(QStringLiteral("text"))
                .toObject()
                .value(QStringLiteral("verbosity"))
                .toString(),
            QStringLiteral("low"));
    }

    void lumoRequestUsesAutomaticFallback()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kLumoService),
                        QByteArrayView("lumo-secret"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray(
                R"({"choices":[{"message":{"role":"assistant","content":"First"}},{"message":{"role":"assistant","content":"Second"}}],"usage":{"prompt_tokens":9,"completion_tokens":5}})"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                QStringLiteral("future-model"),
                QStringLiteral("Plan a trip."),
                {},
            }));

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().text,
            QStringLiteral("First\nSecond"));
        QCOMPARE(
            result.value().inputTokens,
            std::optional<qint64>(9));
        QCOMPARE(
            result.value().outputTokens,
            std::optional<qint64>(5));
        QCOMPARE(openAI->callCount(), 0);
        QCOMPARE(lumo->callCount(), 1);

        const ChatHttpRequest request =
            lumo->lastRequest();
        QCOMPARE(
            request.endpoint,
            QUrl(QStringLiteral(
                "https://lumo.proton.me/api/ai/v1/chat/completions")));
        QCOMPARE(
            request.authorization,
            QByteArray("Bearer lumo-secret"));
        QCOMPARE(
            request.contentType,
            QByteArray("application/json"));
        const QJsonObject body =
            requestBody(request);
        QCOMPARE(
            body.value(QStringLiteral("model"))
                .toString(),
            QStringLiteral("auto"));
        QCOMPARE(
            body.value(QStringLiteral("stream"))
                .toBool(),
            false);
        QCOMPARE(
            body.value(QStringLiteral("max_tokens"))
                .toInt(),
            700);
        const QJsonArray messages =
            body.value(QStringLiteral("messages"))
                .toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(
            messages.at(0)
                .toObject()
                .value(QStringLiteral("role"))
                .toString(),
            QStringLiteral("user"));
        QCOMPARE(
            messages.at(0)
                .toObject()
                .value(QStringLiteral("content"))
                .toString(),
            QStringLiteral("Plan a trip."));
    }

    void lumoProviderMappings_data()
    {
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("apiModel");

        QTest::newRow("automatic")
            << QStringLiteral("automatic")
            << QStringLiteral("auto");
        QTest::newRow("fast")
            << QStringLiteral("fast")
            << QStringLiteral("lumo-basic-v1");
        QTest::newRow("thinking")
            << QStringLiteral("thinking")
            << QStringLiteral("lumo-plus-v1");
        QTest::newRow("unknown")
            << QStringLiteral("future-model")
            << QStringLiteral("auto");
    }

    void lumoProviderMappings()
    {
        QFETCH(QString, selection);
        QFETCH(QString, apiModel);

        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kLumoService),
                        QByteArrayView("key"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray(
                R"({"choices":[{"message":{"role":"assistant","content":"answer"}}]})"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                selection,
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(result.hasValue());
        const QJsonObject body =
            requestBody(lumo->lastRequest());
        QCOMPARE(
            body.value(QStringLiteral("model"))
                .toString(),
            apiModel);
    }

    void missingOpenAIKeyDoesNotCallProviders()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.openai_key_missing"));
        QCOMPARE(openAI->callCount(), 0);
        QCOMPARE(lumo->callCount(), 0);
    }

    void missingLumoKeyDoesNotCallProviders()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                QStringLiteral("automatic"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.lumo_key_missing"));
        QCOMPARE(openAI->callCount(), 0);
        QCOMPARE(lumo->callCount(), 0);
    }

    void cloudAttachmentsReturnProviderSpecificErrors()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("openai"))
                    .hasValue());
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kLumoService),
                        QByteArrayView("lumo"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const QVector<BridgeAttachment> attachments{
            exampleAttachment(),
        };
        const Result<ChatResult> openAIResult =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                attachments,
            }));
        const Result<ChatResult> lumoResult =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                QStringLiteral("automatic"),
                QStringLiteral("Prompt"),
                attachments,
            }));

        QVERIFY(!openAIResult.hasValue());
        QCOMPARE(
            openAIResult.error().code,
            QStringLiteral(
                "chat.openai_attachments_unsupported"));
        QVERIFY(!lumoResult.hasValue());
        QCOMPARE(
            lumoResult.error().code,
            QStringLiteral(
                "chat.lumo_attachments_unsupported"));
        QCOMPARE(openAI->callCount(), 0);
        QCOMPARE(lumo->callCount(), 0);
    }

    void unknownProviderDoesNotCallProviders()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                static_cast<ChatProvider>(99),
                QString(),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.provider_unsupported"));
        QCOMPARE(openAI->callCount(), 0);
        QCOMPARE(lumo->callCount(), 0);
    }

    void providerFailureNeverFallsBack()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("openai"))
                    .hasValue());
        auto openAI =
            std::make_shared<RecordingHttpTransport>(
                [](const ChatHttpRequest&) {
                    return Result<ChatHttpResponse>::
                        failure({
                            QStringLiteral(
                                "chat.transport_failed"),
                            QStringLiteral(
                                "Network unavailable."),
                            true,
                            {},
                        });
                });
        auto lumo = successfulTransport(
            QByteArray(
                R"({"choices":[{"message":{"role":"assistant","content":"must not be used"}}]})"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(openAI->callCount(), 1);
        QCOMPARE(lumo->callCount(), 0);
    }

    void lumoProviderFailureNeverFallsBack()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kLumoService),
                        QByteArrayView("lumo"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray(
                R"({"output_text":"must not be used"})"));
        auto lumo =
            std::make_shared<RecordingHttpTransport>(
                [](const ChatHttpRequest&) {
                    return Result<ChatHttpResponse>::
                        failure({
                            QStringLiteral(
                                "chat.transport_failed"),
                            QStringLiteral(
                                "Network unavailable."),
                            true,
                            {},
                        });
                });
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                QStringLiteral("automatic"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(openAI->callCount(), 0);
        QCOMPARE(lumo->callCount(), 1);
    }

    void providerErrorEnvelopeIsSurfaced()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kLumoService),
                        QByteArrayView("lumo"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo =
            std::make_shared<RecordingHttpTransport>(
                [](const ChatHttpRequest&) {
                    return jsonResponse(
                        401,
                        QByteArray(
                            R"({"error":{"message":"Invalid API key"}})"));
                });
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                QStringLiteral("thinking"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.lumo_request_failed"));
        QCOMPARE(
            result.error().message,
            QStringLiteral("Invalid API key"));
    }

    void openAIErrorEnvelopeIsSurfaced()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("openai"))
                    .hasValue());
        auto openAI =
            std::make_shared<RecordingHttpTransport>(
                [](const ChatHttpRequest&) {
                    return jsonResponse(
                        401,
                        QByteArray(
                            R"({"error":{"message":"Invalid OpenAI key"}})"));
                });
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.openai_request_failed"));
        QCOMPARE(
            result.error().message,
            QStringLiteral(
                "Invalid OpenAI key"));
    }

    void responseDiagnosticsClipBodies()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("openai"))
                    .hasValue());
        const QByteArray oversizedBody(
            600, 'x');
        auto openAI =
            std::make_shared<RecordingHttpTransport>(
                [oversizedBody](
                    const ChatHttpRequest&) {
                    return jsonResponse(
                        500, oversizedBody);
                });
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().context
                .value(QStringLiteral("body"))
                .toString()
                .size(),
            360);
        QVERIFY(
            !result.error().message.contains(
                QString(361, QLatin1Char('x'))));
    }

    void blankProviderResponseFails()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        QByteArrayView("openai"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray(
                R"({"output_text":"   "})"));
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.openai_request_failed"));
    }

    void blankLumoResponseFails()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kLumoService),
                        QByteArrayView("lumo"))
                    .hasValue());
        auto openAI = successfulTransport(
            QByteArray("{}"));
        auto lumo = successfulTransport(
            QByteArray(
                R"({"choices":[{"message":{"role":"assistant","content":"   "}}]})"));
        ChatService service(
            credentials, openAI, lumo);

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::LumoAPI,
                QStringLiteral("automatic"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.lumo_request_failed"));
    }

    void networkRequestsNeverFollowRedirects()
    {
        const Result<QNetworkRequest> result =
            chat_detail::makeNetworkRequest({
                QUrl(QStringLiteral(
                    "https://api.openai.com/v1/responses")),
                QByteArray("Bearer secret"),
                QByteArray("application/json"),
                QByteArray("{}"),
            });

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value()
                .attribute(
                    QNetworkRequest::
                        RedirectPolicyAttribute)
                .toInt(),
            static_cast<int>(
                QNetworkRequest::
                    ManualRedirectPolicy));
    }

    void partialSuccessfulHttpResponseFails()
    {
        const Result<ChatHttpResponse> result =
            chat_detail::classifyNetworkResponse(
                QUrl(QStringLiteral(
                    "https://api.openai.com/v1/responses")),
                200,
                QNetworkReply::TimeoutError,
                QStringLiteral(
                    "Operation timed out"),
                QByteArray(
                    R"({"output_text":"partial"})"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "chat.transport_failed"));
    }

    void providerHttpErrorsKeepResponseBodies()
    {
        const QByteArray body(
            R"({"error":{"message":"Invalid key"}})");
        const Result<ChatHttpResponse> result =
            chat_detail::classifyNetworkResponse(
                QUrl(QStringLiteral(
                    "https://api.openai.com/v1/responses")),
                401,
                QNetworkReply::
                    ContentAccessDenied,
                QStringLiteral(
                    "Host requires authentication"),
                body);

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().statusCode,
            401);
        QCOMPARE(result.value().body, body);
    }

    void secretsNeverEnterQtLogs()
    {
        auto credentials =
            std::make_shared<MemoryCredentialStore>();
        const QByteArray secret(
            "private-openai-secret");
        QVERIFY(credentials
                    ->write(
                        QString::fromLatin1(
                            kOpenAIService),
                        secret)
                    .hasValue());
        auto openAI =
            std::make_shared<RecordingHttpTransport>(
                [](const ChatHttpRequest&) {
                    return jsonResponse(
                        200,
                        QByteArray("{not-json}"));
                });
        auto lumo = successfulTransport(
            QByteArray("{}"));
        ChatService service(
            credentials, openAI, lumo);
        MessageCapture capture;

        const Result<ChatResult> result =
            waitFor(service.send({
                ChatProvider::OpenAIAPI,
                QStringLiteral("gpt56Luna"),
                QStringLiteral("Prompt"),
                {},
            }));

        QVERIFY(!result.hasValue());
        const QString logs = capture.joined();
        QVERIFY(
            !logs.contains(
                QString::fromUtf8(secret)));
        QVERIFY(
            !logs.contains(
                QStringLiteral("Authorization"),
                Qt::CaseInsensitive));
    }

    void dpapiDefaultRootUsesLocalAppData()
    {
        const QString localAppData =
            qEnvironmentVariable("LOCALAPPDATA");
        QVERIFY(!localAppData.isEmpty());
        QCOMPARE(
            QDir::cleanPath(
                DpapiCredentialStore::
                    defaultRootDirectory()),
            QDir::cleanPath(
                QDir(localAppData).filePath(
                    QStringLiteral(
                        "Codex Companion/Credentials"))));
    }

    void dpapiUsesCurrentUserProtectionFlags()
    {
        const unsigned long flags =
            DpapiCredentialStore::
                protectionFlags();

        QCOMPARE(
            flags,
            static_cast<unsigned long>(
                CRYPTPROTECT_UI_FORBIDDEN));
        QCOMPARE(
            flags
                & static_cast<unsigned long>(
                    CRYPTPROTECT_LOCAL_MACHINE),
            0UL);
    }

    void dpapiRoundTripEncryptsAndRestrictsFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DpapiCredentialStore store(
            directory.path());
        const QString service =
            QString::fromLatin1(kOpenAIService);
        const QByteArray secret(
            "sk-test-super-secret-value");

        const Result<void> written =
            store.write(service, secret);

        if (!written.hasValue()) {
            QFAIL(qPrintable(
                written.error().message));
        }
        QVERIFY(store.contains(service));
        const QByteArray ciphertext =
            readOnlyCredentialFile(
                directory.path());
        QVERIFY(!ciphertext.isEmpty());
        QVERIFY(!ciphertext.contains(secret));
        const QString path =
            onlyCredentialPath(
                directory.path());
        QVERIFY(!path.isEmpty());
        QVERIFY(
            isCurrentUserOnlyAcl(
                directory.path(), true));
        QVERIFY(
            isCurrentUserOnlyAcl(
                path, false));

        const Result<QByteArray> read =
            store.read(service);
        if (!read.hasValue()) {
            QFAIL(qPrintable(
                read.error().message));
        }
        QCOMPARE(read.value(), secret);
    }

    void dpapiAclFailureDoesNotCommitCredential()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DpapiCredentialStore store(
            directory.path(),
            [](const QString&, bool) {
                return Result<void>::failure({
                    QStringLiteral(
                        "credential.acl_failed"),
                    QStringLiteral(
                        "ACL injection failure."),
                    false,
                    {},
                });
            });

        const Result<void> result =
            store.write(
                QString::fromLatin1(
                    kOpenAIService),
                QByteArrayView("secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "credential.acl_failed"));
        QCOMPARE(
            QDir(directory.path())
                .entryList(
                    QDir::Files
                        | QDir::NoDotAndDotDot)
                .size(),
            0);
    }

    void dpapiRejectsUnreadableSecretSize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DpapiCredentialStore store(
            directory.path());
        const QByteArray oversized(
            1024 * 1024, 'x');

        const Result<void> result =
            store.write(
                QString::fromLatin1(
                    kOpenAIService),
                oversized);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "credential.secret_too_large"));
        QCOMPARE(
            QDir(directory.path())
                .entryList(
                    QDir::Files
                        | QDir::NoDotAndDotDot)
                .size(),
            0);
    }

    void dpapiRemoveClearsCredentialFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DpapiCredentialStore store(
            directory.path());
        const QString service =
            QString::fromLatin1(kLumoService);
        QVERIFY(
            store.write(
                     service,
                     QByteArrayView("secret"))
                .hasValue());

        const Result<void> removed =
            store.remove(service);

        QVERIFY(removed.hasValue());
        QVERIFY(!store.contains(service));
        QCOMPARE(
            QDir(directory.path())
                .entryList(
                    QDir::Files
                        | QDir::NoDotAndDotDot)
                .size(),
            0);
        const Result<QByteArray> read =
            store.read(service);
        QVERIFY(!read.hasValue());
        QCOMPARE(
            read.error().code,
            QStringLiteral("credential.not_found"));
    }

    void dpapiRejectsUnsafeServiceNames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DpapiCredentialStore store(
            directory.path());

        const Result<void> result =
            store.write(
                QStringLiteral("../outside"),
                QByteArrayView("secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "credential.invalid_service"));
        QCOMPARE(
            QDir(directory.path())
                .entryList(
                    QDir::Files
                        | QDir::NoDotAndDotDot)
                .size(),
            0);
    }
};

QTEST_GUILESS_MAIN(ChatCatalogTests)
#include "ChatCatalogTests.moc"
