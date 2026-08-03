#include "codex/chat/FoundryLocalDriverInternal.h"

#include <QQueue>
#include <QtTest>

#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

using namespace companion;

namespace {

CompanionError fakeFailure()
{
    return {
        QStringLiteral("fake.failed"),
        QStringLiteral("fake failed"),
        false,
        {},
    };
}

detail::FoundryChatCompletion textCompletion(
    QByteArray text,
    int statusCode = 200)
{
    return {
        true,
        statusCode,
        {
            {
                detail::FoundryChatFinishReason::
                    Stop,
                detail::FoundryChatMessage{
                    QStringLiteral("assistant"),
                    std::move(text),
                    std::nullopt,
                    {},
                },
            },
        },
    };
}

detail::FoundryChatCompletion toolCompletion(
    QString callId,
    QString functionName,
    QByteArray argumentsJson)
{
    return {
        true,
        200,
        {
            {
                detail::FoundryChatFinishReason::
                    ToolCalls,
                detail::FoundryChatMessage{
                    QStringLiteral("assistant"),
                    {},
                    std::nullopt,
                    {
                        {
                            std::move(callId),
                            QStringLiteral(
                                "function"),
                            detail::
                                FoundryFunctionCall{
                                    std::move(
                                        functionName),
                                    std::move(
                                        argumentsJson),
                                },
                        },
                    },
                },
            },
        },
    };
}

class FakeFoundryApi final
    : public detail::FoundryLocalApi {
public:
    bool isInitialized()
        const noexcept override
    {
        return initialized;
    }

    Result<void> create(
        const QString& applicationName,
        const QString& applicationDataDirectory,
        const QString& modelCacheDirectory) override
    {
        calls.append(QStringLiteral("create"));
        createdApplicationName =
            applicationName;
        createdApplicationDataDirectory =
            applicationDataDirectory;
        createdModelCacheDirectory =
            modelCacheDirectory;
        ++createCalls;
        if (failCreate) {
            return Result<void>::failure(
                fakeFailure());
        }
        initialized = true;
        return Result<void>::success();
    }

    Result<QVector<
        detail::
            FoundryExecutionProviderInfo>>
    discoverExecutionProviders() override
    {
        calls.append(
            QStringLiteral("discover"));
        const qsizetype index =
            discoverCalls++;
        if (failDiscovery
            || discoveries.isEmpty()) {
            return Result<QVector<
                detail::
                    FoundryExecutionProviderInfo>>::
                failure(fakeFailure());
        }
        return Result<QVector<
            detail::
                FoundryExecutionProviderInfo>>::
            success(
                discoveries.at(
                    std::min<qsizetype>(
                        index,
                        discoveries.size()
                            - 1)));
    }

    Result<void>
    downloadAndRegisterExecutionProviders(
        const QStringList& names,
        detail::FoundryProgressObserver
            progress,
        detail::FoundryCancellationProbe
            cancellation) override
    {
        calls.append(
            QStringLiteral("download-eps"));
        ++downloadExecutionProviderCalls;
        downloadedExecutionProviderNames =
            names;
        if (progress) {
            progress(42.0);
        }
        if ((cancellation
             && cancellation())
            || failExecutionProviderDownload) {
            return Result<void>::failure(
                fakeFailure());
        }
        return Result<void>::success();
    }

    Result<void>
    invalidateCatalog() override
    {
        calls.append(
            QStringLiteral(
                "invalidate-catalog"));
        ++invalidateCatalogCalls;
        return failCatalogInvalidation
            ? Result<void>::failure(
                  fakeFailure())
            : Result<void>::success();
    }

    Result<QVector<
        detail::FoundryModelVariantInfo>>
    resolveModelVariants(
        const QString& alias) override
    {
        calls.append(
            QStringLiteral("resolve-model"));
        resolvedAlias = alias;
        if (failModelResolution) {
            return Result<QVector<
                detail::
                    FoundryModelVariantInfo>>::
                failure(fakeFailure());
        }
        return Result<QVector<
            detail::
                FoundryModelVariantInfo>>::
            success(variants);
    }

    Result<void> selectVariant(
        const QString& variantId) override
    {
        calls.append(
            QStringLiteral("select-variant"));
        selectedVariantId = variantId;
        ++selectVariantCalls;
        return failVariantSelection
            ? Result<void>::failure(
                  fakeFailure())
            : Result<void>::success();
    }

    Result<void>
    downloadSelectedModel(
        detail::FoundryProgressObserver
            progress,
        detail::FoundryCancellationProbe
            cancellation) override
    {
        calls.append(
            QStringLiteral(
                "download-model"));
        ++downloadModelCalls;
        if (progress) {
            progress(73.0);
        }
        if ((cancellation
             && cancellation())
            || failModelDownload) {
            return Result<void>::failure(
                fakeFailure());
        }
        return Result<void>::success();
    }

    Result<void>
    loadSelectedModel() override
    {
        calls.append(
            QStringLiteral("load-model"));
        ++loadModelCalls;
        if (failModelLoad) {
            return Result<void>::failure(
                fakeFailure());
        }
        loaded = true;
        return Result<void>::success();
    }

    Result<detail::FoundryChatCompletion>
    completeChat(
        const QVector<
            detail::FoundryChatMessage>&
            messages,
        const QVector<
            detail::FoundryToolDefinition>&
            tools,
        int maximumTokens,
        int choiceCount) override
    {
        calls.append(
            QStringLiteral("complete-chat"));
        sentMessages.append(messages);
        sentTools.append(tools);
        sentMaximumTokens = maximumTokens;
        sentChoiceCount = choiceCount;
        ++completeChatCalls;
        if (failChat) {
            return Result<
                detail::
                    FoundryChatCompletion>::
                failure(fakeFailure());
        }
        if (!queuedCompletions.isEmpty()) {
            return Result<
                detail::
                    FoundryChatCompletion>::
                success(
                    queuedCompletions
                        .dequeue());
        }
        return Result<
            detail::
                FoundryChatCompletion>::
            success(completion);
    }

    void unloadSelectedModel()
        noexcept override
    {
        calls.append(
            QStringLiteral("unload-model"));
        ++unloadCalls;
        loaded = false;
    }

    void destroy() noexcept override
    {
        calls.append(
            QStringLiteral("destroy"));
        ++destroyCalls;
        initialized = false;
    }

    QVector<QVector<
        detail::
            FoundryExecutionProviderInfo>>
        discoveries;
    QVector<
        detail::FoundryModelVariantInfo>
        variants;
    detail::FoundryChatCompletion
        completion =
            textCompletion(
                QByteArray("answer"));
    QQueue<detail::FoundryChatCompletion>
        queuedCompletions;
    QVector<QVector<
        detail::FoundryChatMessage>>
        sentMessages;
    QVector<QVector<
        detail::FoundryToolDefinition>>
        sentTools;
    QStringList calls;
    QStringList
        downloadedExecutionProviderNames;
    QString createdApplicationName;
    QString createdApplicationDataDirectory;
    QString createdModelCacheDirectory;
    QString resolvedAlias;
    QString selectedVariantId;
    int sentMaximumTokens = 0;
    int sentChoiceCount = 0;
    int createCalls = 0;
    int discoverCalls = 0;
    int downloadExecutionProviderCalls = 0;
    int invalidateCatalogCalls = 0;
    int selectVariantCalls = 0;
    int downloadModelCalls = 0;
    int loadModelCalls = 0;
    int completeChatCalls = 0;
    int unloadCalls = 0;
    int destroyCalls = 0;
    bool initialized = false;
    bool loaded = false;
    bool failCreate = false;
    bool failDiscovery = false;
    bool failExecutionProviderDownload =
        false;
    bool failCatalogInvalidation = false;
    bool failModelResolution = false;
    bool failVariantSelection = false;
    bool failModelDownload = false;
    bool failModelLoad = false;
    bool failChat = false;
};

std::shared_ptr<
    detail::WindowsOnDeviceChatDriver>
createDriver(
    const std::shared_ptr<FakeFoundryApi>&
        api)
{
    auto created =
        detail::createFoundryLocalChatDriver(
            api,
            QStringLiteral(
                "C:/Users/test/AppData/Local/Codex Companion/Foundry Local"));
    if (!created.hasValue()) {
        return {};
    }
    return std::move(created.value());
}

QVector<
    detail::FoundryModelVariantInfo>
mixedVariants()
{
    return {
        {
            QStringLiteral("npu-missing"),
            detail::FoundryModelRuntimeInfo{
                detail::
                    FoundryModelDeviceType::
                        Npu,
                QStringLiteral(
                    "MissingExecutionProvider"),
            },
        },
        {
            QStringLiteral("gpu-registered"),
            detail::FoundryModelRuntimeInfo{
                detail::
                    FoundryModelDeviceType::
                        Gpu,
                QStringLiteral(
                    "RegisteredGpu"),
            },
        },
        {
            QStringLiteral("cpu-fallback"),
            detail::FoundryModelRuntimeInfo{
                detail::
                    FoundryModelDeviceType::
                        Cpu,
                {},
            },
        },
    };
}

} // namespace

class FoundryLocalDriverTests final
    : public QObject {
    Q_OBJECT

private slots:
    void preparationUsesPinnedSequenceAndVariantPolicy()
    {
        const auto api =
            std::make_shared<
                FakeFoundryApi>();
        api->discoveries = {
            {
                {
                    QStringLiteral(" "),
                    false,
                },
                {
                    QStringLiteral(
                        "RegisteredNpu"),
                    true,
                },
                {
                    QStringLiteral(
                        "RegisteredGpu"),
                    false,
                },
            },
            {
                {
                    QStringLiteral(
                        "RegisteredNpu"),
                    true,
                },
                {
                    QStringLiteral(
                        "RegisteredGpu"),
                    true,
                },
            },
        };
        api->variants = mixedVariants();
        const auto driver =
            createDriver(api);
        QVERIFY(driver != nullptr);

        QVector<QPair<
            WindowsOnDeviceChatPhase,
            double>>
            progress;
        const Result<void> prepared =
            driver->prepare(
                [&progress](
                    WindowsOnDeviceChatPhase
                        phase,
                    double percent) {
                    progress.append({
                        phase,
                        percent,
                    });
                },
                {});

        QVERIFY(prepared.hasValue());
        QCOMPARE(api->createCalls, 1);
        QCOMPARE(
            api->createdApplicationName,
            QStringLiteral(
                "Codex Companion"));
        QCOMPARE(
            api
                ->createdApplicationDataDirectory,
            QStringLiteral(
                "C:/Users/test/AppData/Local/Codex Companion/Foundry Local"));
        QCOMPARE(
            api->createdModelCacheDirectory,
            QStringLiteral(
                "C:/Users/test/AppData/Local/Codex Companion/Foundry Local/cache/models"));
        QCOMPARE(api->discoverCalls, 2);
        QCOMPARE(
            api->downloadExecutionProviderCalls,
            1);
        const QStringList expectedDownloads{
            QStringLiteral(
                "RegisteredGpu"),
        };
        QCOMPARE(
            api
                ->downloadedExecutionProviderNames,
            expectedDownloads);
        QCOMPARE(
            api->invalidateCatalogCalls,
            1);
        QCOMPARE(
            api->resolvedAlias,
            QStringLiteral(
                "qwen2.5-0.5b"));
        QCOMPARE(
            api->selectedVariantId,
            QStringLiteral(
                "gpu-registered"));
        QCOMPARE(api->downloadModelCalls, 1);
        QCOMPARE(api->loadModelCalls, 1);
        QVERIFY(api->loaded);
        const QStringList expectedCalls{
            QStringLiteral("create"),
            QStringLiteral("discover"),
            QStringLiteral("download-eps"),
            QStringLiteral("discover"),
            QStringLiteral(
                "invalidate-catalog"),
            QStringLiteral(
                "resolve-model"),
            QStringLiteral(
                "select-variant"),
            QStringLiteral(
                "download-model"),
            QStringLiteral("load-model"),
        };
        QCOMPARE(api->calls, expectedCalls);
        QVERIFY(progress.size() >= 7);

        driver->shutdown();
        QCOMPARE(api->unloadCalls, 1);
        QCOMPARE(api->destroyCalls, 1);
    }

    void emptyUnregisteredSetSkipsDownloadAndInvalidation()
    {
        const auto api =
            std::make_shared<
                FakeFoundryApi>();
        api->discoveries = {
            {
                {
                    QStringLiteral(
                        "RegisteredGpu"),
                    true,
                },
            },
            {
                {
                    QStringLiteral(
                        "RegisteredGpu"),
                    true,
                },
            },
        };
        api->variants = {
            {
                QStringLiteral(
                    "runtime-free"),
                std::nullopt,
            },
            {
                QStringLiteral("cpu"),
                detail::FoundryModelRuntimeInfo{
                    detail::
                        FoundryModelDeviceType::
                            Cpu,
                    {},
                },
            },
        };
        const auto driver =
            createDriver(api);
        QVERIFY(driver != nullptr);

        QVERIFY(
            driver
                ->prepare(
                    [](auto, auto) {},
                    {})
                .hasValue());
        QCOMPARE(api->discoverCalls, 2);
        QCOMPARE(
            api->downloadExecutionProviderCalls,
            0);
        QCOMPARE(
            api->invalidateCatalogCalls,
            0);
        QCOMPARE(
            api->selectedVariantId,
            QStringLiteral(
                "runtime-free"));
        driver->shutdown();
    }

    void failureCleansUpBeforeRetry()
    {
        const auto api =
            std::make_shared<
                FakeFoundryApi>();
        api->discoveries = {
            {},
            {},
        };
        api->variants = mixedVariants();
        api->failModelLoad = true;
        const auto driver =
            createDriver(api);
        QVERIFY(driver != nullptr);

        QVERIFY(
            !driver
                 ->prepare(
                     [](auto, auto) {},
                     {})
                 .hasValue());
        QCOMPARE(api->unloadCalls, 1);
        QCOMPARE(api->destroyCalls, 1);
        QVERIFY(!api->initialized);

        api->failModelLoad = false;
        QVERIFY(
            driver
                ->prepare(
                    [](auto, auto) {},
                    {})
                .hasValue());
        QCOMPARE(api->createCalls, 2);
        QCOMPARE(api->destroyCalls, 1);
        driver->shutdown();
        QCOMPARE(api->unloadCalls, 2);
        QCOMPARE(api->destroyCalls, 2);
    }

    void sendUsesCompanionInstructionsAndExactSettings()
    {
        const auto api =
            std::make_shared<
                FakeFoundryApi>();
        api->discoveries = {
            {},
            {},
        };
        api->variants = {
            {
                QStringLiteral("cpu"),
                detail::FoundryModelRuntimeInfo{
                    detail::
                        FoundryModelDeviceType::
                            Cpu,
                    {},
                },
            },
        };
        api->completion =
            textCompletion(
                QByteArray(
                    "  local answer  "),
                201);
        const auto driver =
            createDriver(api);
        QVERIFY(driver != nullptr);
        QVERIFY(
            driver
                ->prepare(
                    [](auto, auto) {},
                    {})
                .hasValue());

        const Result<ChatResult> sent =
            driver->send({
                ChatProvider::OnDevice,
                QStringLiteral(
                    "on-device"),
                QStringLiteral(
                    "hello \u2603"),
                {},
            });
        QVERIFY(sent.hasValue());
        QCOMPARE(
            sent.value().text,
            QStringLiteral(
                "local answer"));
        QVERIFY(
            !sent.value()
                 .inputTokens
                 .has_value());
        QVERIFY(
            !sent.value()
                 .outputTokens
                 .has_value());
        QCOMPARE(api->sentMessages.size(), 1);
        const auto& messages =
            api->sentMessages.constFirst();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(
            messages.at(0).role,
            QStringLiteral("system"));
        QVERIFY(
            messages.at(0)
                .content
                .contains(
                    "concise local assistant"));
        QCOMPARE(
            messages.at(1).role,
            QStringLiteral("user"));
        QCOMPARE(
            messages.at(1).content,
            QStringLiteral(
                "hello \u2603")
                .toUtf8());
        QCOMPARE(api->sentTools.size(), 1);
        QCOMPARE(
            api->sentTools.constFirst().size(),
            4);
        QCOMPARE(api->sentMaximumTokens, 700);
        QCOMPARE(api->sentChoiceCount, 1);
        QCOMPARE(api->completeChatCalls, 1);
        driver->shutdown();
    }

    void sendExecutesPortableToolAndContinuesConversation()
    {
        const auto api =
            std::make_shared<
                FakeFoundryApi>();
        api->discoveries = {
            {},
            {},
        };
        api->variants = {
            {
                QStringLiteral("cpu"),
                detail::FoundryModelRuntimeInfo{
                    detail::
                        FoundryModelDeviceType::
                            Cpu,
                    {},
                },
            },
        };
        api->queuedCompletions.enqueue(
            toolCompletion(
                QStringLiteral("call-1"),
                QStringLiteral("calculate"),
                QByteArray(
                    R"({"expression":"6 * 7"})")));
        api->queuedCompletions.enqueue(
            textCompletion(
                QByteArray("  42  ")));

        const auto driver =
            createDriver(api);
        QVERIFY(driver != nullptr);
        QVERIFY(
            driver
                ->prepare(
                    [](auto, auto) {},
                    {})
                .hasValue());

        const Result<ChatResult> sent =
            driver->send({
                ChatProvider::OnDevice,
                QStringLiteral(
                    "on-device"),
                QStringLiteral(
                    "What is 6 times 7?"),
                {},
            });

        QVERIFY(sent.hasValue());
        QCOMPARE(
            sent.value().text,
            QStringLiteral("42"));
        QCOMPARE(api->completeChatCalls, 2);
        QCOMPARE(api->sentMessages.size(), 2);
        QCOMPARE(api->sentTools.size(), 2);

        const auto& firstTools =
            api->sentTools.at(0);
        QStringList toolNames;
        for (const auto& tool : firstTools) {
            toolNames.append(tool.name);
        }
        QCOMPARE(
            toolNames,
            QStringList({
                QStringLiteral("calculate"),
                QStringLiteral(
                    "current_context"),
                QStringLiteral(
                    "current_weather"),
                QStringLiteral(
                    "web_reference_search"),
            }));

        const auto& continued =
            api->sentMessages.at(1);
        QCOMPARE(continued.size(), 4);
        QCOMPARE(
            continued.at(2).role,
            QStringLiteral("assistant"));
        QCOMPARE(
            continued.at(2)
                .toolCalls
                .size(),
            1);
        QCOMPARE(
            continued.at(2)
                .toolCalls
                .constFirst()
                .id,
            QStringLiteral("call-1"));
        QCOMPARE(
            continued.at(3).role,
            QStringLiteral("tool"));
        QCOMPARE(
            continued.at(3).toolCallId,
            std::optional<QString>(
                QStringLiteral("call-1")));
        QCOMPARE(
            continued.at(3).content,
            QByteArray(
                "Exact calculator result: 42"));
        driver->shutdown();
    }

    void malformedResponsesAreRejected()
    {
        const auto api =
            std::make_shared<
                FakeFoundryApi>();
        api->discoveries = {
            {},
            {},
        };
        api->variants = {
            {
                QStringLiteral("cpu"),
                detail::FoundryModelRuntimeInfo{
                    detail::
                        FoundryModelDeviceType::
                            Cpu,
                    {},
                },
            },
        };
        const auto driver =
            createDriver(api);
        QVERIFY(driver != nullptr);
        QVERIFY(
            driver
                ->prepare(
                    [](auto, auto) {},
                    {})
                .hasValue());

        const auto send = [&] {
            return driver->send({
                ChatProvider::OnDevice,
                QStringLiteral(
                    "on-device"),
                QStringLiteral("prompt"),
                {},
            });
        };
        api->completion =
            textCompletion(
                QByteArray("answer"));
        api->completion.successful = false;
        QVERIFY(!send().hasValue());
        api->completion =
            textCompletion(
                QByteArray("answer"),
                500);
        QVERIFY(!send().hasValue());
        api->completion = {
            true,
            200,
            {},
        };
        QVERIFY(!send().hasValue());
        api->completion = {
            true,
            200,
            {
                {
                    detail::
                        FoundryChatFinishReason::
                            Stop,
                    std::nullopt,
                },
            },
        };
        QVERIFY(!send().hasValue());
        api->completion =
            textCompletion(
                QByteArray("   "));
        QVERIFY(!send().hasValue());
        api->completion =
            textCompletion(
                QByteArray::fromHex(
                    "c328"));
        QVERIFY(!send().hasValue());
        driver->shutdown();
    }
};

QTEST_GUILESS_MAIN(FoundryLocalDriverTests)

#include "FoundryLocalDriverTests.moc"
