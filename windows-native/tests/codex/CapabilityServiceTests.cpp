#include "codex/chat/ChatCatalog.h"
#include "codex/runtime/CapabilityService.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <stdexcept>
#include <stop_token>
#include <utility>

using namespace companion;

namespace {

RpcResponse successResponse(QJsonObject result)
{
    return {
        std::move(result),
        {},
        false,
    };
}

QHash<int, RpcResponse> emptyResponses()
{
    return {
        {
            2,
            successResponse({
                {
                    QStringLiteral("data"),
                    QJsonArray{},
                },
            }),
        },
        {
            3,
            successResponse({
                {
                    QStringLiteral("data"),
                    QJsonArray{},
                },
            }),
        },
        {
            4,
            successResponse({
                {
                    QStringLiteral("marketplaces"),
                    QJsonArray{},
                },
            }),
        },
    };
}

Result<BridgeCapabilities> loadWith(
    QJsonObject models,
    QJsonObject skills,
    QJsonObject plugins,
    CapabilityCredentialProbe credentialProbe = {})
{
    QHash<int, RpcResponse> responses{
        {2, successResponse(std::move(models))},
        {3, successResponse(std::move(skills))},
        {4, successResponse(std::move(plugins))},
    };
    CapabilityService service(
        [responses = std::move(responses)](
            const QVector<RpcRequest>&,
            std::stop_token) {
            return Result<QHash<int, RpcResponse>>::
                success(responses);
        },
        std::move(credentialProbe));
    return service.load(QStringLiteral("C:\\repo"));
}

QString publicErrorText(
    const CompanionError& error)
{
    return error.code
        + QLatin1Char('\n')
        + error.message
        + QLatin1Char('\n')
        + QString::fromUtf8(
            QJsonDocument::fromVariant(
                error.context)
                .toJson(QJsonDocument::Compact));
}

void verifyCloudAvailability(
    const QVector<BridgeChatModel>& models,
    bool hasOpenAIKey,
    bool hasLumoKey)
{
    QCOMPARE(models.size(), 7);
    QCOMPARE(
        models.at(0).provider,
        ChatProvider::OnDevice);
    QVERIFY(!models.at(0).isAvailable);
    QVERIFY(
        !models.at(0).supportsAttachments);
    for (qsizetype index = 1;
         index <= 3;
         ++index) {
        QCOMPARE(
            models.at(index).provider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            models.at(index).isAvailable,
            hasOpenAIKey);
    }
    for (qsizetype index = 4;
         index <= 6;
         ++index) {
        QCOMPARE(
            models.at(index).provider,
            ChatProvider::LumoAPI);
        QCOMPARE(
            models.at(index).isAvailable,
            hasLumoKey);
    }
}

} // namespace

class CapabilityServiceTests final : public QObject {
    Q_OBJECT

private slots:
    void sendsExactRequestTranscript()
    {
        QVector<RpcRequest> captured;
        int calls = 0;
        std::stop_source stopSource;
        const std::stop_token expectedToken =
            stopSource.get_token();
        bool tokenMatched = false;
        CapabilityService service(
            [&](
                const QVector<RpcRequest>& requests,
                std::stop_token stopToken) {
                ++calls;
                captured = requests;
                tokenMatched =
                    stopToken == expectedToken;
                return Result<
                    QHash<int, RpcResponse>>::success(
                    emptyResponses());
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(
                QStringLiteral("  C:\\repo  "),
                expectedToken);

        QVERIFY(result.hasValue());
        QCOMPARE(calls, 1);
        QVERIFY(tokenMatched);
        QCOMPARE(captured.size(), 3);

        QCOMPARE(captured.at(0).id, 2);
        QCOMPARE(
            captured.at(0).method,
            QStringLiteral("model/list"));
        QCOMPARE(
            captured.at(0).params,
            QJsonObject({
                {QStringLiteral("limit"), 100},
                {
                    QStringLiteral("includeHidden"),
                    false,
                },
            }));

        QCOMPARE(captured.at(1).id, 3);
        QCOMPARE(
            captured.at(1).method,
            QStringLiteral("skills/list"));
        QCOMPARE(
            captured.at(1).params,
            QJsonObject({
                {
                    QStringLiteral("cwds"),
                    QJsonArray{
                        QStringLiteral("C:\\repo"),
                    },
                },
                {
                    QStringLiteral("forceReload"),
                    false,
                },
            }));

        QCOMPARE(captured.at(2).id, 4);
        QCOMPARE(
            captured.at(2).method,
            QStringLiteral("plugin/list"));
        QCOMPARE(
            captured.at(2).params,
            QJsonObject({
                {
                    QStringLiteral("cwds"),
                    QJsonArray{
                        QStringLiteral("C:\\repo"),
                    },
                },
                {
                    QStringLiteral(
                        "marketplaceKinds"),
                    QJsonArray{
                        QStringLiteral("local"),
                    },
                },
            }));
    }

    void blankCwdUsesHomeDirectory()
    {
        QVector<RpcRequest> captured;
        CapabilityService service(
            [&](
                const QVector<RpcRequest>& requests,
                std::stop_token) {
                captured = requests;
                return Result<
                    QHash<int, RpcResponse>>::success(
                    emptyResponses());
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(
                QStringLiteral(" \t\r\n "));

        QVERIFY(result.hasValue());
        QCOMPARE(captured.size(), 3);
        for (qsizetype index = 1;
             index <= 2;
             ++index) {
            QCOMPARE(
                captured.at(index)
                    .params
                    .value(QStringLiteral("cwds"))
                    .toArray(),
                QJsonArray{QDir::homePath()});
        }
    }

    void parsesModelsWithFilteringFallbacksAndOrder()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("hidden"),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral("hidden-model"),
                                },
                                {
                                    QStringLiteral("hidden"),
                                    true,
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral(" first-id "),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral(" first-model "),
                                },
                                {
                                    QStringLiteral("displayName"),
                                    QStringLiteral("   "),
                                },
                                {
                                    QStringLiteral("description"),
                                    QStringLiteral("   "),
                                },
                                {
                                    QStringLiteral("isDefault"),
                                    true,
                                },
                                {
                                    QStringLiteral(
                                        "defaultReasoningEffort"),
                                    QStringLiteral("   "),
                                },
                                {
                                    QStringLiteral(
                                        "supportedReasoningEfforts"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral(
                                                    "reasoningEffort"),
                                                QStringLiteral(
                                                    " vERY hIGH "),
                                            },
                                            {
                                                QStringLiteral(
                                                    "description"),
                                                QStringLiteral(
                                                    "   "),
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral(
                                                    "reasoningEffort"),
                                                QStringLiteral(
                                                    " low "),
                                            },
                                            {
                                                QStringLiteral(
                                                    "description"),
                                                QStringLiteral(
                                                    " economical "),
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral(
                                                    "reasoningEffort"),
                                                QStringLiteral(
                                                    "   "),
                                            },
                                        },
                                    },
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral(" second-id "),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral(" second-model "),
                                },
                                {
                                    QStringLiteral("displayName"),
                                    QStringLiteral(" Second "),
                                },
                                {
                                    QStringLiteral("description"),
                                    QStringLiteral(" Detailed "),
                                },
                                {
                                    QStringLiteral("isDefault"),
                                    QStringLiteral("true"),
                                },
                                {
                                    QStringLiteral(
                                        "defaultReasoningEffort"),
                                    QStringLiteral(" xhigh "),
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("   "),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral("invalid"),
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("invalid"),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral("   "),
                                },
                            },
                        },
                    },
                },
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });

        QVERIFY(result.hasValue());
        const QVector<BridgeModel>& models =
            result.value().models;
        QCOMPARE(models.size(), 2);

        QCOMPARE(
            models.at(0).id,
            QStringLiteral("first-id"));
        QCOMPARE(
            models.at(0).model,
            QStringLiteral("first-model"));
        QCOMPARE(
            models.at(0).displayName,
            QStringLiteral("first-model"));
        QCOMPARE(
            models.at(0).description,
            QStringLiteral("Codex model"));
        QVERIFY(models.at(0).isDefault);
        QCOMPARE(
            models.at(0).defaultReasoningEffort,
            QStringLiteral("vERY hIGH"));
        QCOMPARE(
            models.at(0).supportedReasoningEfforts,
            QVector<BridgeReasoningEffort>({
                {
                    QStringLiteral("vERY hIGH"),
                    QStringLiteral("Very High"),
                },
                {
                    QStringLiteral("low"),
                    QStringLiteral("economical"),
                },
            }));

        QCOMPARE(
            models.at(1).id,
            QStringLiteral("second-id"));
        QCOMPARE(
            models.at(1).model,
            QStringLiteral("second-model"));
        QCOMPARE(
            models.at(1).displayName,
            QStringLiteral("Second"));
        QCOMPARE(
            models.at(1).description,
            QStringLiteral("Detailed"));
        QVERIFY(!models.at(1).isDefault);
        QCOMPARE(
            models.at(1).defaultReasoningEffort,
            QStringLiteral("xhigh"));
        QVERIFY(
            models.at(1)
                .supportedReasoningEfforts
                .isEmpty());
    }

    void modelEffortFallbackCapitalizesMixedCaseWords()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("model-id"),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral("model"),
                                },
                                {
                                    QStringLiteral(
                                        "supportedReasoningEfforts"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral(
                                                    "reasoningEffort"),
                                                QStringLiteral(
                                                    "  mIxEd cASE  "),
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral(
                                                    "reasoningEffort"),
                                                QStringLiteral(
                                                    "  xHIGH  "),
                                            },
                                        },
                                    },
                                },
                            },
                        },
                    },
                },
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });

        QVERIFY(result.hasValue());
        const QVector<BridgeReasoningEffort>& efforts =
            result.value()
                .models
                .front()
                .supportedReasoningEfforts;
        QCOMPARE(
            efforts,
            QVector<BridgeReasoningEffort>({
                {
                    QStringLiteral("mIxEd cASE"),
                    QStringLiteral("Mixed Case"),
                },
                {
                    QStringLiteral("xHIGH"),
                    QStringLiteral("Xhigh"),
                },
            }));
    }

    void parsesSkillsWithFirstPathDedupeAndSourceSort()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("skills"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" beta "),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral(
                                                    " C:\\skills\\b "),
                                            },
                                            {
                                                QStringLiteral("scope"),
                                                QStringLiteral(" repo "),
                                            },
                                            {
                                                QStringLiteral("interface"),
                                                QJsonObject{
                                                    {
                                                        QStringLiteral(
                                                            "displayName"),
                                                        QStringLiteral(
                                                            " Bravo "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "shortDescription"),
                                                        QStringLiteral(
                                                            " Interface brief "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "defaultPrompt"),
                                                        QStringLiteral(
                                                            " Use beta "),
                                                    },
                                                },
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(
                                                    "duplicate"),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral(
                                                    "C:\\skills\\b"),
                                            },
                                            {
                                                QStringLiteral("interface"),
                                                QJsonObject{
                                                    {
                                                        QStringLiteral(
                                                            "displayName"),
                                                        QStringLiteral(
                                                            "Aardvark duplicate"),
                                                    },
                                                },
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("disabled"),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral(
                                                    "C:\\skills\\disabled"),
                                            },
                                            {
                                                QStringLiteral("enabled"),
                                                false,
                                            },
                                        },
                                    },
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("skills"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" Alpha "),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral(
                                                    " C:\\skills\\a "),
                                            },
                                            {
                                                QStringLiteral("scope"),
                                                QStringLiteral("   "),
                                            },
                                            {
                                                QStringLiteral(
                                                    "shortDescription"),
                                                QStringLiteral(
                                                    " Row short "),
                                            },
                                            {
                                                QStringLiteral("interface"),
                                                QJsonObject{
                                                    {
                                                        QStringLiteral(
                                                            "defaultPrompt"),
                                                        QStringLiteral(
                                                            "   "),
                                                    },
                                                },
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" charlie "),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral(
                                                    " C:\\skills\\c "),
                                            },
                                            {
                                                QStringLiteral("enabled"),
                                                QStringLiteral("false"),
                                            },
                                            {
                                                QStringLiteral("scope"),
                                                QStringLiteral(" project "),
                                            },
                                            {
                                                QStringLiteral("description"),
                                                QStringLiteral(
                                                    " Row description "),
                                            },
                                            {
                                                QStringLiteral("interface"),
                                                QJsonObject{
                                                    {
                                                        QStringLiteral(
                                                            "displayName"),
                                                        QStringLiteral(
                                                            " Charlie "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "shortDescription"),
                                                        QStringLiteral(
                                                            "   "),
                                                    },
                                                },
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" Delta "),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral(
                                                    " C:\\skills\\d "),
                                            },
                                        },
                                    },
                                },
                            },
                        },
                    },
                },
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });

        QVERIFY(result.hasValue());
        const QVector<BridgeSkill>& skills =
            result.value().skills;
        QCOMPARE(skills.size(), 4);
        QCOMPARE(
            skills.at(0),
            BridgeSkill({
                QStringLiteral("Alpha"),
                QStringLiteral("Alpha"),
                QStringLiteral("Row short"),
                QStringLiteral("C:\\skills\\a"),
                QStringLiteral("user"),
                std::nullopt,
            }));
        QCOMPARE(
            skills.at(1),
            BridgeSkill({
                QStringLiteral("beta"),
                QStringLiteral("Bravo"),
                QStringLiteral("Interface brief"),
                QStringLiteral("C:\\skills\\b"),
                QStringLiteral("repo"),
                QStringLiteral("Use beta"),
            }));
        QCOMPARE(
            skills.at(2),
            BridgeSkill({
                QStringLiteral("charlie"),
                QStringLiteral("Charlie"),
                QStringLiteral("Row description"),
                QStringLiteral("C:\\skills\\c"),
                QStringLiteral("project"),
                std::nullopt,
            }));
        QCOMPARE(
            skills.at(3),
            BridgeSkill({
                QStringLiteral("Delta"),
                QStringLiteral("Delta"),
                QStringLiteral("Codex skill"),
                QStringLiteral("C:\\skills\\d"),
                QStringLiteral("user"),
                std::nullopt,
            }));
    }

    void parsesPluginsWithFirstIdDedupeBeforeInstalledFilter()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("plugins"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral(" duplicate "),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("first"),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                false,
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral("duplicate"),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("later"),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral(" z "),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" zulu-name "),
                                            },
                                            {
                                                QStringLiteral("enabled"),
                                                true,
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                            {
                                                QStringLiteral("interface"),
                                                QJsonObject{
                                                    {
                                                        QStringLiteral(
                                                            "displayName"),
                                                        QStringLiteral(
                                                            " Zulu "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "shortDescription"),
                                                        QStringLiteral(
                                                            "   "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "longDescription"),
                                                        QStringLiteral(
                                                            " Long detail "),
                                                    },
                                                },
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral("non-bool"),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("non-bool"),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                QStringLiteral("true"),
                                            },
                                        },
                                    },
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("plugins"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral(" a "),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" Alpha "),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral(" b "),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral(" beta-name "),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                            {
                                                QStringLiteral("interface"),
                                                QJsonObject{
                                                    {
                                                        QStringLiteral(
                                                            "displayName"),
                                                        QStringLiteral(
                                                            " Beta "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "shortDescription"),
                                                        QStringLiteral(
                                                            " Short "),
                                                    },
                                                    {
                                                        QStringLiteral(
                                                            "longDescription"),
                                                        QStringLiteral(
                                                            " Long "),
                                                    },
                                                },
                                            },
                                        },
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral("   "),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("invalid"),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                        },
                                    },
                                },
                            },
                        },
                    },
                });

        QVERIFY(result.hasValue());
        const QVector<BridgePlugin>& plugins =
            result.value().plugins;
        QCOMPARE(plugins.size(), 3);
        QCOMPARE(
            plugins.at(0),
            BridgePlugin({
                QStringLiteral("a"),
                QStringLiteral("Alpha"),
                QStringLiteral("Alpha"),
                QStringLiteral("Codex plugin"),
                false,
                true,
            }));
        QCOMPARE(
            plugins.at(1),
            BridgePlugin({
                QStringLiteral("b"),
                QStringLiteral("beta-name"),
                QStringLiteral("Beta"),
                QStringLiteral("Short"),
                false,
                true,
            }));
        QCOMPARE(
            plugins.at(2),
            BridgePlugin({
                QStringLiteral("z"),
                QStringLiteral("zulu-name"),
                QStringLiteral("Zulu"),
                QStringLiteral("Long detail"),
                true,
                true,
            }));
    }

    void malformedOrMissingListsBecomeEmpty()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {
                    {
                        QStringLiteral("data"),
                        QJsonObject{},
                    },
                },
                {},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QStringLiteral("invalid"),
                    },
                });

        QVERIFY(result.hasValue());
        QVERIFY(result.value().models.isEmpty());
        QVERIFY(result.value().skills.isEmpty());
        QVERIFY(result.value().plugins.isEmpty());
    }

    void mixedModelDataInvalidatesTheWholeModelList()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("valid"),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral("valid"),
                                },
                            },
                            7,
                        },
                    },
                },
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });

        QVERIFY(result.hasValue());
        QVERIFY(result.value().models.isEmpty());
    }

    void mixedEffortDataInvalidatesOnlyThatEffortList()
    {
        const Result<BridgeCapabilities> result =
            loadWith(
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("valid"),
                                },
                                {
                                    QStringLiteral("model"),
                                    QStringLiteral("valid"),
                                },
                                {
                                    QStringLiteral(
                                        "supportedReasoningEfforts"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral(
                                                    "reasoningEffort"),
                                                QStringLiteral("high"),
                                            },
                                        },
                                        QStringLiteral("invalid"),
                                    },
                                },
                            },
                        },
                    },
                },
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().models.size(), 1);
        QVERIFY(
            result.value()
                .models
                .front()
                .supportedReasoningEfforts
                .isEmpty());
        QCOMPARE(
            result.value()
                .models
                .front()
                .defaultReasoningEffort,
            QStringLiteral("medium"));
    }

    void mixedSkillArraysMatchSwiftWholeArrayCasts()
    {
        const Result<BridgeCapabilities> mixedEntries =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("skills"),
                                    QJsonArray{},
                                },
                            },
                            true,
                        },
                    },
                },
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });
        QVERIFY(mixedEntries.hasValue());
        QVERIFY(mixedEntries.value().skills.isEmpty());

        const Result<BridgeCapabilities> mixedRows =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("data"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("skills"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("lost"),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral("lost"),
                                            },
                                        },
                                        9,
                                    },
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("skills"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("kept"),
                                            },
                                            {
                                                QStringLiteral("path"),
                                                QStringLiteral("kept"),
                                            },
                                        },
                                    },
                                },
                            },
                        },
                    },
                },
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                });
        QVERIFY(mixedRows.hasValue());
        QCOMPARE(mixedRows.value().skills.size(), 1);
        QCOMPARE(
            mixedRows.value().skills.front().name,
            QStringLiteral("kept"));
    }

    void mixedPluginArraysMatchSwiftWholeArrayCasts()
    {
        const Result<BridgeCapabilities> mixedMarketplaces =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("plugins"),
                                    QJsonArray{},
                                },
                            },
                            QStringLiteral("invalid"),
                        },
                    },
                });
        QVERIFY(mixedMarketplaces.hasValue());
        QVERIFY(
            mixedMarketplaces
                .value()
                .plugins
                .isEmpty());

        const Result<BridgeCapabilities> mixedRows =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral("plugins"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral("lost"),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("lost"),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                        },
                                        false,
                                    },
                                },
                            },
                            QJsonObject{
                                {
                                    QStringLiteral("plugins"),
                                    QJsonArray{
                                        QJsonObject{
                                            {
                                                QStringLiteral("id"),
                                                QStringLiteral("kept"),
                                            },
                                            {
                                                QStringLiteral("name"),
                                                QStringLiteral("kept"),
                                            },
                                            {
                                                QStringLiteral("installed"),
                                                true,
                                            },
                                        },
                                    },
                                },
                            },
                        },
                    },
                });
        QVERIFY(mixedRows.hasValue());
        QCOMPARE(mixedRows.value().plugins.size(), 1);
        QCOMPARE(
            mixedRows.value().plugins.front().id,
            QStringLiteral("kept"));
    }

    void chatModelsFollowEveryCredentialCombination_data()
    {
        QTest::addColumn<bool>("hasOpenAIKey");
        QTest::addColumn<bool>("hasLumoKey");

        QTest::newRow("neither") << false << false;
        QTest::newRow("openai") << true << false;
        QTest::newRow("lumo") << false << true;
        QTest::newRow("both") << true << true;
    }

    void chatModelsFollowEveryCredentialCombination()
    {
        QFETCH(bool, hasOpenAIKey);
        QFETCH(bool, hasLumoKey);
        QVector<ChatProvider> probes;

        const Result<BridgeCapabilities> result =
            loadWith(
                {{QStringLiteral("data"), QJsonArray{}}},
                {{QStringLiteral("data"), QJsonArray{}}},
                {
                    {
                        QStringLiteral("marketplaces"),
                        QJsonArray{},
                    },
                },
                [&](ChatProvider provider) {
                    probes.append(provider);
                    if (provider
                        == ChatProvider::OpenAIAPI) {
                        return hasOpenAIKey;
                    }
                    if (provider
                        == ChatProvider::LumoAPI) {
                        return hasLumoKey;
                    }
                    return false;
                });

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().chatAgents,
            ChatCatalog::agents());
        QVERIFY(
            result.value().chatModels.has_value());
        QCOMPARE(
            *result.value().chatModels,
            ChatCatalog::capabilities({
                false,
                false,
                hasOpenAIKey,
                hasLumoKey,
            }));
        verifyCloudAvailability(
            *result.value().chatModels,
            hasOpenAIKey,
            hasLumoKey);
        QCOMPARE(
            probes,
            QVector<ChatProvider>({
                ChatProvider::OpenAIAPI,
                ChatProvider::LumoAPI,
            }));
    }

    void typedAvailabilityProviderControlsAllFields()
    {
        int providerCalls = 0;
        CapabilityService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    emptyResponses());
            },
            CapabilityChatAvailabilityProvider(
                [&providerCalls] {
                    ++providerCalls;
                    return ChatCatalogAvailability{
                        true,
                        true,
                        true,
                        false,
                    };
                }));

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\repo"));

        QVERIFY(result.hasValue());
        QCOMPARE(providerCalls, 1);
        QVERIFY(
            result.value().chatModels.has_value());
        const QVector<BridgeChatModel>& models =
            *result.value().chatModels;
        QVERIFY(models.at(0).isAvailable);
        QVERIFY(
            models.at(0).supportsAttachments);
        QVERIFY(models.at(1).isAvailable);
        QVERIFY(!models.at(4).isAvailable);
    }

    void emptyCredentialProbeDoesNotFailLoad()
    {
        CapabilityService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    emptyResponses());
            },
            CapabilityCredentialProbe{});

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\repo"));

        QVERIFY(result.hasValue());
        QVERIFY(
            result.value().chatModels.has_value());
        verifyCloudAvailability(
            *result.value().chatModels,
            false,
            false);
    }

    void throwingCredentialProbeDoesNotFailLoad()
    {
        CapabilityService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    emptyResponses());
            },
            [](ChatProvider) -> bool {
                throw std::runtime_error(
                    "companion.openai-api-key "
                    "SECRET_CREDENTIAL");
            });

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\repo"));

        QVERIFY(result.hasValue());
        QVERIFY(
            result.value().chatModels.has_value());
        verifyCloudAvailability(
            *result.value().chatModels,
            false,
            false);
    }

    void throwingTypedProviderDoesNotFailLoad()
    {
        CapabilityService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    emptyResponses());
            },
            CapabilityChatAvailabilityProvider(
                []() -> ChatCatalogAvailability {
                    throw std::runtime_error(
                        "SECRET_PROVIDER");
                }));

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\repo"));

        QVERIFY(result.hasValue());
        QVERIFY(
            result.value().chatModels.has_value());
        QCOMPARE(
            *result.value().chatModels,
            ChatCatalog::capabilities({}));
    }

    void operationalFailuresAreSanitized_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("message");
        QTest::addColumn<bool>("retryable");

        QTest::newRow("missing executable")
            << QStringLiteral(
                   "codex.executable_not_found")
            << QStringLiteral(
                   "Could not find an installed Codex executable.")
            << false;
        QTest::newRow("timed out")
            << QStringLiteral(
                   "codex.app_server_timed_out")
            << QStringLiteral(
                   "Codex app-server did not respond in time.")
            << true;
        QTest::newRow("launch failed")
            << QStringLiteral(
                   "codex.app_server_launch_failed")
            << QStringLiteral(
                   "Codex app-server could not start.")
            << false;
        QTest::newRow("process exited")
            << QStringLiteral(
                   "codex.app_server_process_exited")
            << QStringLiteral(
                   "Codex app-server exited before completing the request.")
            << true;
        QTest::newRow("initialize failed")
            << QStringLiteral(
                   "codex.app_server_initialize_failed")
            << QStringLiteral(
                   "Codex app-server initialization failed.")
            << false;
        QTest::newRow("invalid response")
            << QStringLiteral(
                   "codex.app_server_invalid_response")
            << QStringLiteral(
                   "Codex app-server returned an invalid response.")
            << false;
        QTest::newRow("invalid response id")
            << QStringLiteral(
                   "codex.app_server_invalid_response_id")
            << QStringLiteral(
                   "Codex app-server response did not contain a numeric ID.")
            << false;
        QTest::newRow("canceled")
            << QStringLiteral(
                   "codex.operation_canceled")
            << QStringLiteral(
                   "The Codex operation was canceled.")
            << false;
    }

    void operationalFailuresAreSanitized()
    {
        QFETCH(QString, code);
        QFETCH(QString, message);
        QFETCH(bool, retryable);
        CapabilityService service(
            [=](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::failure({
                    code,
                    QStringLiteral(
                        "SECRET_STDERR server failure"),
                    retryable,
                    {
                        {
                            QStringLiteral("stderr"),
                            QStringLiteral(
                                "SECRET_STDERR"),
                        },
                        {
                            QStringLiteral("cwd"),
                            QStringLiteral(
                                "C:\\private\\cwd"),
                        },
                        {
                            QStringLiteral("service"),
                            QStringLiteral(
                                "companion.openai-api-key"),
                        },
                    },
                });
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(
                QStringLiteral(
                    "C:\\private\\cwd"));

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, code);
        QCOMPARE(result.error().message, message);
        QCOMPARE(
            result.error().retryable,
            retryable);
        QVERIFY(result.error().context.isEmpty());
        const QString publicText =
            publicErrorText(result.error());
        QVERIFY(
            !publicText.contains(
                QStringLiteral("SECRET_STDERR")));
        QVERIFY(
            !publicText.contains(
                QStringLiteral(
                    "C:\\private\\cwd")));
        QVERIFY(
            !publicText.contains(
                QStringLiteral(
                    "companion.openai-api-key")));
    }

    void unknownPerformerFailureBecomesUnavailable()
    {
        CapabilityService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::failure({
                    QStringLiteral("private.failure"),
                    QStringLiteral(
                        "SECRET_SERVER_ERROR"),
                    true,
                    {
                        {
                            QStringLiteral("body"),
                            QStringLiteral(
                                "SECRET_RESPONSE_BODY"),
                        },
                    },
                });
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.capabilities_unavailable"));
        QCOMPARE(
            result.error().message,
            QStringLiteral(
                "Codex capabilities are unavailable."));
        QVERIFY(!result.error().retryable);
        QVERIFY(result.error().context.isEmpty());
        const QString publicText =
            publicErrorText(result.error());
        QVERIFY(
            !publicText.contains(
                QStringLiteral("SECRET")));
        QVERIFY(
            !publicText.contains(
                QStringLiteral("C:\\secret")));
    }

    void thrownPerformerBecomesUnavailable()
    {
        CapabilityService service(
            [](
                const QVector<RpcRequest>&,
                std::stop_token)
                -> Result<QHash<int, RpcResponse>> {
                throw std::runtime_error(
                    "SECRET_THROWN_SERVER_ERROR");
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.capabilities_unavailable"));
        QCOMPARE(
            result.error().message,
            QStringLiteral(
                "Codex capabilities are unavailable."));
        QVERIFY(!result.error().retryable);
        QVERIFY(result.error().context.isEmpty());
        QVERIFY(
            !publicErrorText(result.error())
                 .contains(QStringLiteral("SECRET")));
    }

    void missingResponseIdBecomesUnavailable()
    {
        QHash<int, RpcResponse> responses =
            emptyResponses();
        responses.remove(3);
        CapabilityService service(
            [responses = std::move(responses)](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    responses);
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.capabilities_unavailable"));
        QCOMPARE(result.error().context.size(), 2);
        QCOMPARE(
            result.error()
                .context
                .value(QStringLiteral("requestId"))
                .toInt(),
            3);
        QCOMPARE(
            result.error()
                .context
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("skills/list"));
        QVERIFY(
            !publicErrorText(result.error())
                 .contains(
                     QStringLiteral("C:\\secret")));
    }

    void appServerErrorBecomesUnavailable()
    {
        QHash<int, RpcResponse> responses =
            emptyResponses();
        responses.insert(
            4,
            {
                {},
                QStringLiteral(
                    "SECRET_SERVER_ERROR "
                    "companion.lumo-api-key"),
                true,
            });
        CapabilityService service(
            [responses = std::move(responses)](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    responses);
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.capabilities_unavailable"));
        QCOMPARE(result.error().context.size(), 2);
        QCOMPARE(
            result.error()
                .context
                .value(QStringLiteral("requestId"))
                .toInt(),
            4);
        QCOMPARE(
            result.error()
                .context
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("plugin/list"));
        const QString publicText =
            publicErrorText(result.error());
        QVERIFY(
            !publicText.contains(
                QStringLiteral("SECRET")));
        QVERIFY(
            !publicText.contains(
                QStringLiteral(
                    "companion.lumo-api-key")));
        QVERIFY(
            !publicText.contains(
                QStringLiteral("C:\\secret")));
    }

    void nonObjectResultBecomesUnavailable()
    {
        QHash<int, RpcResponse> responses =
            emptyResponses();
        responses.insert(
            2,
            {
                QJsonArray{
                    QStringLiteral(
                        "SECRET_RESPONSE_BODY"),
                },
                {},
                false,
            });
        CapabilityService service(
            [responses = std::move(responses)](
                const QVector<RpcRequest>&,
                std::stop_token) {
                return Result<
                    QHash<int, RpcResponse>>::success(
                    responses);
            },
            [](ChatProvider) { return false; });

        const Result<BridgeCapabilities> result =
            service.load(QStringLiteral("C:\\secret"));

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "codex.capabilities_unavailable"));
        QCOMPARE(result.error().context.size(), 2);
        QCOMPARE(
            result.error()
                .context
                .value(QStringLiteral("requestId"))
                .toInt(),
            2);
        QCOMPARE(
            result.error()
                .context
                .value(QStringLiteral("method"))
                .toString(),
            QStringLiteral("model/list"));
        const QString publicText =
            publicErrorText(result.error());
        QVERIFY(
            !publicText.contains(
                QStringLiteral("SECRET")));
        QVERIFY(
            !publicText.contains(
                QStringLiteral("C:\\secret")));
    }
};

QTEST_GUILESS_MAIN(CapabilityServiceTests)
#include "CapabilityServiceTests.moc"
