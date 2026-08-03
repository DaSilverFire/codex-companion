#include "codex/runtime/CapabilityService.h"

#include "codex/chat/ChatCatalog.h"

#include <QCollator>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <optional>
#include <utility>

namespace companion {

namespace {

std::optional<QString> nonempty(
    const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString trimmed =
        value.toString().trimmed();
    return trimmed.isEmpty()
        ? std::nullopt
        : std::optional<QString>(trimmed);
}

std::optional<QJsonArray> objectArray(
    const QJsonValue& value)
{
    if (!value.isArray()) {
        return std::nullopt;
    }
    const QJsonArray array = value.toArray();
    for (const QJsonValue& item : array) {
        if (!item.isObject()) {
            return std::nullopt;
        }
    }
    return array;
}

bool isWordGrapheme(
    const QString& grapheme)
{
    return std::any_of(
        grapheme.cbegin(),
        grapheme.cend(),
        [](QChar character) {
            return character.isLetterOrNumber()
                || character.isMark();
        });
}

QString foundationCapitalized(
    const QString& value)
{
    const QString lowered = value.toLower();
    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme,
        lowered);
    QString result;
    result.reserve(lowered.size());
    bool inWord = false;
    bool needsCasedGrapheme = false;
    finder.toStart();
    qsizetype start = 0;
    while (true) {
        const qsizetype end =
            finder.toNextBoundary();
        if (end < 0) {
            break;
        }
        const QString grapheme =
            lowered.sliced(start, end - start);
        const bool word =
            isWordGrapheme(grapheme);
        if (!word) {
            inWord = false;
            needsCasedGrapheme = false;
            result.append(grapheme);
        } else {
            if (!inWord) {
                inWord = true;
                needsCasedGrapheme = true;
            }
            const QString uppercase =
                grapheme.toUpper();
            const QString lowercase =
                grapheme.toLower();
            if (needsCasedGrapheme
                && uppercase != lowercase) {
                result.append(uppercase);
                needsCasedGrapheme = false;
            } else {
                result.append(grapheme);
            }
        }
        start = end;
    }
    return result;
}

template <typename Value>
void sortByDisplayName(QVector<Value>* values)
{
    QCollator collator;
    collator.setCaseSensitivity(
        Qt::CaseInsensitive);
    std::sort(
        values->begin(),
        values->end(),
        [&collator](
            const Value& left,
            const Value& right) {
            return collator.compare(
                       left.displayName,
                       right.displayName)
                < 0;
        });
}

QVector<BridgeModel> parseModels(
    const QJsonObject& result)
{
    const std::optional<QJsonArray> rows =
        objectArray(
            result.value(
                QStringLiteral("data")));
    if (!rows.has_value()) {
        return {};
    }

    QVector<BridgeModel> models;
    for (const QJsonValue& value : *rows) {
        const QJsonObject row =
            value.toObject();
        const QJsonValue hidden =
            row.value(QStringLiteral("hidden"));
        if (hidden.isBool()
            && hidden.toBool()) {
            continue;
        }
        const std::optional<QString> id =
            nonempty(
                row.value(
                    QStringLiteral("id")));
        const std::optional<QString> model =
            nonempty(
                row.value(
                    QStringLiteral("model")));
        if (!id.has_value()
            || !model.has_value()) {
            continue;
        }

        QVector<BridgeReasoningEffort> efforts;
        const std::optional<QJsonArray>
            effortRows = objectArray(
                row.value(QStringLiteral(
                    "supportedReasoningEfforts")));
        if (effortRows.has_value()) {
            for (const QJsonValue& effortValue :
                 *effortRows) {
                const QJsonObject effortRow =
                    effortValue.toObject();
                const std::optional<QString>
                    effort = nonempty(
                        effortRow.value(
                            QStringLiteral(
                                "reasoningEffort")));
                if (!effort.has_value()) {
                    continue;
                }
                efforts.append({
                    *effort,
                    nonempty(
                        effortRow.value(
                            QStringLiteral(
                                "description")))
                        .value_or(
                            foundationCapitalized(
                                *effort)),
                });
            }
        }

        const QJsonValue defaultValue =
            row.value(QStringLiteral(
                "defaultReasoningEffort"));
        QString defaultEffort =
            nonempty(defaultValue).value_or(
                efforts.isEmpty()
                    ? QStringLiteral("medium")
                    : efforts.front().value);
        const QJsonValue defaultFlag =
            row.value(
                QStringLiteral("isDefault"));
        models.append({
            *id,
            *model,
            nonempty(
                row.value(QStringLiteral(
                    "displayName")))
                .value_or(*model),
            nonempty(
                row.value(QStringLiteral(
                    "description")))
                .value_or(
                    QStringLiteral(
                        "Codex model")),
            defaultFlag.isBool()
                ? defaultFlag.toBool()
                : false,
            std::move(defaultEffort),
            std::move(efforts),
        });
    }
    return models;
}

QVector<BridgeSkill> parseSkills(
    const QJsonObject& result)
{
    const std::optional<QJsonArray> entries =
        objectArray(
            result.value(
                QStringLiteral("data")));
    if (!entries.has_value()) {
        return {};
    }

    QVector<BridgeSkill> skills;
    QSet<QString> seenPaths;
    for (const QJsonValue& entryValue :
         *entries) {
        const QJsonObject entry =
            entryValue.toObject();
        const std::optional<QJsonArray> rows =
            objectArray(
                entry.value(
                    QStringLiteral("skills")));
        if (!rows.has_value()) {
            continue;
        }
        for (const QJsonValue& rowValue :
             *rows) {
            const QJsonObject row =
                rowValue.toObject();
            const QJsonValue enabled =
                row.value(
                    QStringLiteral("enabled"));
            if (enabled.isBool()
                && !enabled.toBool()) {
                continue;
            }
            const std::optional<QString> name =
                nonempty(
                    row.value(
                        QStringLiteral("name")));
            const std::optional<QString> path =
                nonempty(
                    row.value(
                        QStringLiteral("path")));
            if (!name.has_value()
                || !path.has_value()
                || seenPaths.contains(*path)) {
                continue;
            }
            const QJsonValue interfaceValue =
                row.value(
                    QStringLiteral("interface"));
            const QJsonObject interface =
                interfaceValue.isObject()
                ? interfaceValue.toObject()
                : QJsonObject{};
            std::optional<QString> description =
                nonempty(interface.value(
                    QStringLiteral(
                        "shortDescription")));
            if (!description.has_value()) {
                description = nonempty(
                    row.value(QStringLiteral(
                        "shortDescription")));
            }
            if (!description.has_value()) {
                description = nonempty(
                    row.value(QStringLiteral(
                        "description")));
            }
            seenPaths.insert(*path);
            skills.append({
                *name,
                nonempty(interface.value(
                    QStringLiteral("displayName")))
                    .value_or(*name),
                description.value_or(
                    QStringLiteral("Codex skill")),
                *path,
                nonempty(
                    row.value(QStringLiteral(
                        "scope")))
                    .value_or(
                        QStringLiteral("user")),
                nonempty(interface.value(
                    QStringLiteral(
                        "defaultPrompt"))),
            });
        }
    }
    sortByDisplayName(&skills);
    return skills;
}

QVector<BridgePlugin> parsePlugins(
    const QJsonObject& result)
{
    const std::optional<QJsonArray>
        marketplaces = objectArray(
            result.value(QStringLiteral(
                "marketplaces")));
    if (!marketplaces.has_value()) {
        return {};
    }

    QVector<BridgePlugin> plugins;
    QSet<QString> seenIds;
    for (const QJsonValue& marketplaceValue :
         *marketplaces) {
        const QJsonObject marketplace =
            marketplaceValue.toObject();
        const std::optional<QJsonArray> rows =
            objectArray(
                marketplace.value(
                    QStringLiteral("plugins")));
        if (!rows.has_value()) {
            continue;
        }
        for (const QJsonValue& rowValue :
             *rows) {
            const QJsonObject row =
                rowValue.toObject();
            const std::optional<QString> id =
                nonempty(
                    row.value(
                        QStringLiteral("id")));
            const std::optional<QString> name =
                nonempty(
                    row.value(
                        QStringLiteral("name")));
            if (!id.has_value()
                || !name.has_value()
                || seenIds.contains(*id)) {
                continue;
            }
            const QJsonValue interfaceValue =
                row.value(
                    QStringLiteral("interface"));
            const QJsonObject interface =
                interfaceValue.isObject()
                ? interfaceValue.toObject()
                : QJsonObject{};
            std::optional<QString> description =
                nonempty(interface.value(
                    QStringLiteral(
                        "shortDescription")));
            if (!description.has_value()) {
                description = nonempty(
                    interface.value(
                        QStringLiteral(
                            "longDescription")));
            }
            const QJsonValue enabled =
                row.value(
                    QStringLiteral("enabled"));
            const QJsonValue installed =
                row.value(
                    QStringLiteral("installed"));
            seenIds.insert(*id);
            plugins.append({
                *id,
                *name,
                nonempty(interface.value(
                    QStringLiteral("displayName")))
                    .value_or(*name),
                description.value_or(
                    QStringLiteral(
                        "Codex plugin")),
                enabled.isBool()
                    ? enabled.toBool()
                    : false,
                installed.isBool()
                    ? installed.toBool()
                    : false,
            });
        }
    }

    plugins.erase(
        std::remove_if(
            plugins.begin(),
            plugins.end(),
            [](const BridgePlugin& plugin) {
                return !plugin.installed;
            }),
        plugins.end());
    sortByDisplayName(&plugins);
    return plugins;
}

CompanionError unavailableError(
    const RpcRequest* request = nullptr)
{
    QVariantMap context;
    if (request != nullptr) {
        context.insert(
            QStringLiteral("requestId"),
            request->id);
        context.insert(
            QStringLiteral("method"),
            request->method);
    }
    return {
        QStringLiteral(
            "codex.capabilities_unavailable"),
        QStringLiteral(
            "Codex capabilities are unavailable."),
        false,
        std::move(context),
    };
}

std::optional<CompanionError>
sanitizedOperationalError(
    const CompanionError& error)
{
    QString message;
    if (error.code
        == QStringLiteral(
            "codex.executable_not_found")) {
        message = QStringLiteral(
            "Could not find an installed Codex executable.");
    } else if (error.code
               == QStringLiteral(
                   "codex.app_server_timed_out")) {
        message = QStringLiteral(
            "Codex app-server did not respond in time.");
    } else if (error.code
               == QStringLiteral(
                   "codex.app_server_launch_failed")) {
        message = QStringLiteral(
            "Codex app-server could not start.");
    } else if (error.code
               == QStringLiteral(
                   "codex.app_server_process_exited")) {
        message = QStringLiteral(
            "Codex app-server exited before completing the request.");
    } else if (error.code
               == QStringLiteral(
                   "codex.app_server_initialize_failed")) {
        message = QStringLiteral(
            "Codex app-server initialization failed.");
    } else if (error.code
               == QStringLiteral(
                   "codex.app_server_invalid_response")) {
        message = QStringLiteral(
            "Codex app-server returned an invalid response.");
    } else if (error.code
               == QStringLiteral(
                   "codex.app_server_invalid_response_id")) {
        message = QStringLiteral(
            "Codex app-server response did not contain a numeric ID.");
    } else if (error.code
               == QStringLiteral(
                   "codex.operation_canceled")) {
        message = QStringLiteral(
            "The Codex operation was canceled.");
    } else {
        return std::nullopt;
    }

    const bool retryable =
        error.code
                == QStringLiteral(
                    "codex.app_server_invalid_response_id")
            || error.code
                == QStringLiteral(
                    "codex.operation_canceled")
        ? false
        : error.retryable;
    return CompanionError{
        error.code,
        std::move(message),
        retryable,
        {},
    };
}

QVector<RpcRequest> capabilityRequests(
    const QString& cwd)
{
    return {
        {
            2,
            QStringLiteral("model/list"),
            {
                {QStringLiteral("limit"), 100},
                {
                    QStringLiteral("includeHidden"),
                    false,
                },
            },
        },
        {
            3,
            QStringLiteral("skills/list"),
            {
                {
                    QStringLiteral("cwds"),
                    QJsonArray{cwd},
                },
                {
                    QStringLiteral("forceReload"),
                    false,
                },
            },
        },
        {
            4,
            QStringLiteral("plugin/list"),
            {
                {
                    QStringLiteral("cwds"),
                    QJsonArray{cwd},
                },
                {
                    QStringLiteral(
                        "marketplaceKinds"),
                    QJsonArray{
                        QStringLiteral("local"),
                    },
                },
            },
        },
    };
}

} // namespace

CapabilityService::CapabilityService(
    const CodexEnvironment& environment,
    CapabilityChatAvailabilityProvider
        availabilityProvider)
    : CapabilityService(
          [client = AppServerRpcClient(environment)](
              const QVector<RpcRequest>& requests,
              std::stop_token stopToken) {
              return client.perform(
                  requests, stopToken);
          },
          std::move(availabilityProvider))
{
}

CapabilityService::CapabilityService(
    CapabilityRpcPerformer performer,
    CapabilityChatAvailabilityProvider
        availabilityProvider)
    : performer_(std::move(performer)),
      availabilityProvider_(
          std::move(availabilityProvider))
{
}

CapabilityService::CapabilityService(
    const CodexEnvironment& environment,
    CapabilityCredentialProbe credentialProbe)
    : CapabilityService(
          environment,
          [credentialProbe =
               std::move(credentialProbe)] {
              ChatCatalogAvailability
                  availability;
              if (!credentialProbe) {
                  return availability;
              }
              availability.hasOpenAIKey =
                  credentialProbe(
                      ChatProvider::OpenAIAPI);
              availability.hasLumoKey =
                  credentialProbe(
                      ChatProvider::LumoAPI);
              return availability;
          })
{
}

CapabilityService::CapabilityService(
    CapabilityRpcPerformer performer,
    CapabilityCredentialProbe credentialProbe)
    : CapabilityService(
          std::move(performer),
          [credentialProbe =
               std::move(credentialProbe)] {
              ChatCatalogAvailability
                  availability;
              if (!credentialProbe) {
                  return availability;
              }
              availability.hasOpenAIKey =
                  credentialProbe(
                      ChatProvider::OpenAIAPI);
              availability.hasLumoKey =
                  credentialProbe(
                      ChatProvider::LumoAPI);
              return availability;
          })
{
}

Result<BridgeCapabilities>
CapabilityService::load(
    const QString& cwd,
    std::stop_token stopToken) const
{
    const QString trimmedCwd = cwd.trimmed();
    const QString resolvedCwd =
        trimmedCwd.isEmpty()
        ? QDir::homePath()
        : trimmedCwd;
    const QVector<RpcRequest> requests =
        capabilityRequests(resolvedCwd);

    Result<QHash<int, RpcResponse>> performed =
        [&]() {
            try {
                if (!performer_) {
                    return Result<
                        QHash<int, RpcResponse>>::
                        failure(unavailableError());
                }
                return performer_(
                    requests, stopToken);
            } catch (...) {
                return Result<
                    QHash<int, RpcResponse>>::
                    failure(unavailableError());
            }
        }();
    if (!performed.hasValue()) {
        const std::optional<CompanionError>
            operational =
                sanitizedOperationalError(
                    performed.error());
        return Result<BridgeCapabilities>::failure(
            operational.has_value()
                ? *operational
                : unavailableError());
    }

    QHash<int, QJsonObject> results;
    for (const RpcRequest& request : requests) {
        const auto response =
            performed.value().constFind(
                request.id);
        if (response
                == performed.value().constEnd()
            || response->isError
            || !response->result.isObject()) {
            return Result<
                BridgeCapabilities>::failure(
                unavailableError(&request));
        }
        results.insert(
            request.id,
            response->result.toObject());
    }

    ChatCatalogAvailability availability;
    if (availabilityProvider_) {
        try {
            availability =
                availabilityProvider_();
        } catch (...) {
            availability = {};
        }
    }

    return Result<BridgeCapabilities>::success({
        parseModels(results.value(2)),
        parseSkills(results.value(3)),
        parsePlugins(results.value(4)),
        ChatCatalog::agents(),
        ChatCatalog::capabilities(
            availability),
    });
}

} // namespace companion
