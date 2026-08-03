#include "codex/models/BridgeJsonCodec.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace companion;

namespace {

QByteArray fixture(const QString& name)
{
    QFile file(QStringLiteral(COMPANION_FIXTURE_ROOT)
               + QStringLiteral("/codex-v034/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("fixture missing");
    }
    return file.readAll();
}

QByteArray compactJson(QJsonObject object)
{
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact);
}

QStringList jsonPath(std::initializer_list<const char*> segments)
{
    QStringList result;
    result.reserve(qsizetype(segments.size()));
    for (const char* segment : segments) {
        result.append(QString::fromLatin1(segment));
    }
    return result;
}

QString dottedJsonPath(const QStringList& segments)
{
    QString result;
    for (const QString& segment : segments) {
        bool isIndex = false;
        segment.toInt(&isIndex);
        if (isIndex) {
            result += QLatin1Char('[') + segment + QLatin1Char(']');
        } else {
            if (!result.isEmpty()) {
                result += QLatin1Char('.');
            }
            result += segment;
        }
    }
    return result;
}

bool updateJsonField(
    QJsonValue& value,
    const QStringList& segments,
    qsizetype depth,
    const std::optional<QJsonValue>& replacement)
{
    if (depth >= segments.size()) {
        return false;
    }

    if (depth + 1 == segments.size()) {
        if (!value.isObject()) {
            return false;
        }
        QJsonObject object = value.toObject();
        const QString& key = segments.at(depth);
        if (!object.contains(key)) {
            return false;
        }
        if (replacement.has_value()) {
            object.insert(key, *replacement);
        } else {
            object.remove(key);
        }
        value = object;
        return true;
    }

    const QString& segment = segments.at(depth);
    if (value.isObject()) {
        QJsonObject object = value.toObject();
        if (!object.contains(segment)) {
            return false;
        }
        QJsonValue child = object.value(segment);
        if (!updateJsonField(child, segments, depth + 1, replacement)) {
            return false;
        }
        object.insert(segment, child);
        value = object;
        return true;
    }

    if (value.isArray()) {
        bool validIndex = false;
        const int index = segment.toInt(&validIndex);
        QJsonArray array = value.toArray();
        if (!validIndex || index < 0 || index >= array.size()) {
            return false;
        }
        QJsonValue child = array.at(index);
        if (!updateJsonField(child, segments, depth + 1, replacement)) {
            return false;
        }
        array[index] = child;
        value = array;
        return true;
    }

    return false;
}

bool removeJsonField(QJsonValue& value, const QStringList& segments)
{
    return updateJsonField(value, segments, 0, std::nullopt);
}

bool replaceJsonField(
    QJsonValue& value,
    const QStringList& segments,
    QJsonValue replacement)
{
    return updateJsonField(
        value, segments, 0, std::optional<QJsonValue>(std::move(replacement)));
}

QJsonObject fullRequest()
{
    const auto document = QJsonDocument::fromJson(fixture("bridge-request-full.json"));
    Q_ASSERT(document.isObject());
    return document.object();
}

QJsonObject fullResponse()
{
    const auto document = QJsonDocument::fromJson(fixture("bridge-response-full.json"));
    Q_ASSERT(document.isObject());
    return document.object();
}

template <typename T>
QString resultErrorMessage(const Result<T>& result)
{
    return result.hasValue() ? QString() : result.error().message;
}

BridgeResponse minimumResponse()
{
    return {
        QUuid(QStringLiteral("{33333333-3333-3333-3333-333333333333}")),
        kBridgeProtocolVersion,
        BridgeOperation::LoadCapabilities,
        true,
    };
}

BridgeTask dateTask(double secondsSinceReferenceDate)
{
    return {
        QStringLiteral("thread"),
        QStringLiteral("Title"),
        QStringLiteral("Preview"),
        {secondsSinceReferenceDate},
        std::nullopt,
        TaskStatus::Waiting,
        false,
    };
}

BridgeResponse datedResponse(double secondsSinceReferenceDate)
{
    auto response = minimumResponse();
    response.tasks = QVector<BridgeTask>{dateTask(secondsSinceReferenceDate)};
    return response;
}

void verifyInvalidField(
    const Result<BridgeRequest>& decoded,
    const QString& expectedPath)
{
    QVERIFY(!decoded.hasValue());
    QCOMPARE(decoded.error().code, QStringLiteral("bridge.invalid_field"));
    QVERIFY2(decoded.error().message.contains(expectedPath),
             qPrintable(decoded.error().message));
}

void verifyInvalidField(
    const Result<BridgeResponse>& decoded,
    const QString& expectedPath)
{
    QVERIFY(!decoded.hasValue());
    QCOMPARE(decoded.error().code, QStringLiteral("bridge.invalid_field"));
    QVERIFY2(decoded.error().message.contains(expectedPath),
             qPrintable(decoded.error().message));
}

} // namespace

class BridgeJsonCodecTests final : public QObject {
    Q_OBJECT

private slots:
    void decodesAllV034OptionalFields()
    {
        const auto decoded = BridgeJsonCodec::decodeRequest(
            fixture("bridge-request-full.json"),
            BridgeWireProfile::NearbyV1Milliseconds);

        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QCOMPARE(decoded.value().protocolVersion, 1);
        QCOMPARE(decoded.value().operation, BridgeOperation::SendMessage);
        QVERIFY(decoded.value().chatProvider.has_value());
        QCOMPARE(decoded.value().chatProvider.value(), ChatProvider::OpenAIAPI);
        QCOMPARE(decoded.value().chatModelId.value_or(QString()),
                 QStringLiteral("gpt56Terra"));
        QVERIFY(decoded.value().attachments.has_value());
        QCOMPARE(decoded.value().attachments->size(), 2);
        QCOMPARE(decoded.value().attachments->at(0).filename,
                 QStringLiteral("screen.png"));
        QCOMPARE(decoded.value().attachments->at(0).data, QByteArray(1, '\0'));
        QCOMPARE(decoded.value().attachments->at(1).data,
                 QByteArray::fromHex("0102"));
        QCOMPARE(decoded.value().limit.value(), std::numeric_limits<qint64>::max());
        QCOMPARE(decoded.value().goalTokenBudget.value(),
                 std::numeric_limits<qint64>::min());
        QVERIFY(decoded.value().idempotencyKey.has_value());
        QCOMPARE(decoded.value().idempotencyKey.value(),
                 QUuid(QStringLiteral("{44444444-4444-4444-4444-444444444444}")));

        const auto encoded = BridgeJsonCodec::encodeRequest(
            decoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const auto second = BridgeJsonCodec::decodeRequest(
            encoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(second.hasValue(), qPrintable(resultErrorMessage(second)));
        QCOMPARE(second.value(), decoded.value());
    }

    void acceptsV033PayloadWithV034FieldsAbsent()
    {
        const auto decoded = BridgeJsonCodec::decodeRequest(
            fixture("bridge-request-v033-compatible.json"),
            BridgeWireProfile::NearbyV1Milliseconds);

        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QVERIFY(!decoded.value().chatProvider.has_value());
        QVERIFY(!decoded.value().chatModelId.has_value());
        QVERIFY(!decoded.value().attachments.has_value());

        const auto encoded = BridgeJsonCodec::encodeRequest(
            decoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const auto object = QJsonDocument::fromJson(encoded.value()).object();
        QVERIFY(!object.contains(QStringLiteral("chatProvider")));
        QVERIFY(!object.contains(QStringLiteral("chatModelID")));
        QVERIFY(!object.contains(QStringLiteral("attachments")));
    }

    void acceptsV033ResponseWithV034FieldsAbsent()
    {
        const auto decoded = BridgeJsonCodec::decodeResponse(
            fixture("bridge-response-v033-compatible.json"),
            BridgeWireProfile::NearbyV1Milliseconds);

        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QVERIFY(decoded.value().messages.has_value());
        QVERIFY(!decoded.value().messages->at(0).attachments.has_value());
        QVERIFY(decoded.value().capabilities.has_value());
        QVERIFY(!decoded.value().capabilities->chatModels.has_value());

        const auto encoded = BridgeJsonCodec::encodeResponse(
            decoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const auto object = QJsonDocument::fromJson(encoded.value()).object();
        const auto messages = object.value(QStringLiteral("messages")).toArray();
        QVERIFY(!messages.at(0).toObject().contains(QStringLiteral("attachments")));
        QVERIFY(!object.value(QStringLiteral("capabilities"))
                     .toObject()
                     .contains(QStringLiteral("chatModels")));
    }

    void roundTripsMessageAttachmentsAndCapabilities()
    {
        const auto decoded = BridgeJsonCodec::decodeResponse(
            fixture("bridge-response-full.json"),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QVERIFY(decoded.value().messages.has_value());
        QVERIFY(decoded.value().messages->at(0).attachments.has_value());
        QCOMPARE(decoded.value().messages->at(0).attachments->size(), 1);
        QVERIFY(decoded.value().capabilities.has_value());
        QVERIFY(decoded.value().capabilities->chatModels.has_value());
        QCOMPARE(decoded.value().capabilities->chatModels->size(), 7);

        const auto encoded = BridgeJsonCodec::encodeResponse(
            decoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const auto second = BridgeJsonCodec::decodeResponse(
            encoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(second.hasValue(), qPrintable(resultErrorMessage(second)));
        QCOMPARE(second.value(), decoded.value());
    }

    void acceptsEscapedStringValuesAndObjectKeys()
    {
        const auto decoded = BridgeJsonCodec::decodeRequest(
            QByteArrayLiteral(
                "{\"i\\u0064\":\"33333333-3333-3333-3333-333333333333\","
                "\"protocol\\u0056ersion\":1,"
                "\"operation\":\"handshake\","
                "\"te\\u0078t\":\"line\\n\\\"quoted\\\"\","
                "\"future\\u004Bey\":{\"number\":7}}"),
            BridgeWireProfile::NearbyV1Milliseconds);

        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QCOMPARE(decoded.value().protocolVersion, 1);
        QCOMPARE(decoded.value().text.value_or(QString()),
                 QStringLiteral("line\n\"quoted\""));
    }

    void ignoresUnknownKeysThatResembleDecoderPaths()
    {
        const auto decoded = BridgeJsonCodec::decodeResponse(
            QByteArrayLiteral(
                "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                "\"protocolVersion\":1,\"operation\":\"listTasks\","
                "\"succeeded\":true,\"tasks\":[{\"id\":\"thread\","
                "\"title\":\"Title\",\"preview\":\"Preview\","
                "\"updatedAt\":978307200000,\"status\":\"waiting\","
                "\"needsApproval\":false,\"goal\":{\"threadID\":\"thread\","
                "\"objective\":\"Objective\",\"status\":\"active\","
                "\"tokensUsed\":0,\"elapsedSeconds\":0,"
                "\"createdAt\":9223372036854775807,\"updatedAt\":0}}],"
                "\"tasks[0].goal.createdAt\":7}"),
            BridgeWireProfile::NearbyV1Milliseconds);

        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QCOMPARE(decoded.value().tasks->at(0).goal->createdAt,
                 std::numeric_limits<qint64>::max());
    }

    void matchesSwiftFirstDuplicateKeySemantics()
    {
        const auto first = BridgeJsonCodec::decodeRequest(
            QByteArrayLiteral(
                "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                "\"protocolVersion\":1,\"operation\":\"handshake\","
                "\"li\\u006dit\":1,\"limit\":2}"),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(first.hasValue(), qPrintable(resultErrorMessage(first)));
        QCOMPARE(first.value().limit.value(), 1);

        const auto ignoredOutOfRangeDuplicate = BridgeJsonCodec::decodeRequest(
            QByteArrayLiteral(
                "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                "\"protocolVersion\":1,\"operation\":\"handshake\","
                "\"limit\":2,\"limit\":9223372036854775808}"),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(
            ignoredOutOfRangeDuplicate.hasValue(),
            qPrintable(resultErrorMessage(ignoredOutOfRangeDuplicate)));
        QCOMPARE(ignoredOutOfRangeDuplicate.value().limit.value(), 2);

        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                QByteArrayLiteral(
                    "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                    "\"protocolVersion\":1,\"operation\":\"handshake\","
                    "\"limit\":9223372036854775808,\"limit\":2}"),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("limit"));
    }

    void acceptsUnsupportedProtocolVersionForLaterServerHandling()
    {
        auto request = fullRequest();
        request.insert(QStringLiteral("protocolVersion"), 99);

        const auto decoded = BridgeJsonCodec::decodeRequest(
            compactJson(std::move(request)),
            BridgeWireProfile::NearbyV1Milliseconds);

        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QCOMPARE(decoded.value().protocolVersion, 99);
        QCOMPARE(decoded.value().id,
                 QUuid(QStringLiteral("{33333333-3333-3333-3333-333333333333}")));
        QCOMPARE(decoded.value().operation, BridgeOperation::SendMessage);
    }

    void rejectsMalformedAndNonObjectJson()
    {
        const auto malformed = BridgeJsonCodec::decodeRequest(
            QByteArrayLiteral("{"), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY(!malformed.hasValue());
        QCOMPARE(malformed.error().code, QStringLiteral("bridge.invalid_json"));
        QVERIFY(malformed.error().context.contains(QStringLiteral("offset")));

        const auto root = BridgeJsonCodec::decodeResponse(
            QByteArrayLiteral("[]"), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY(!root.hasValue());
        QCOMPARE(root.error().code, QStringLiteral("bridge.invalid_json"));
    }

    void rejectsMissingAndWrongRequiredFieldsWithDottedPaths()
    {
        const QVector<QStringList> requestPaths = {
            jsonPath({"id"}),
            jsonPath({"protocolVersion"}),
            jsonPath({"operation"}),
            jsonPath({"attachments", "0", "id"}),
            jsonPath({"attachments", "0", "kind"}),
            jsonPath({"attachments", "0", "filename"}),
            jsonPath({"attachments", "0", "data"}),
        };
        for (const QStringList& path : requestPaths) {
            QJsonValue request(fullRequest());
            QVERIFY2(removeJsonField(request, path), qPrintable(dottedJsonPath(path)));
            verifyInvalidField(
                BridgeJsonCodec::decodeRequest(
                    compactJson(request.toObject()),
                    BridgeWireProfile::NearbyV1Milliseconds),
                dottedJsonPath(path));
        }

        const QVector<QStringList> responsePaths = {
            jsonPath({"id"}),
            jsonPath({"protocolVersion"}),
            jsonPath({"operation"}),
            jsonPath({"succeeded"}),
            jsonPath({"tasks", "0", "id"}),
            jsonPath({"tasks", "0", "title"}),
            jsonPath({"tasks", "0", "preview"}),
            jsonPath({"tasks", "0", "updatedAt"}),
            jsonPath({"tasks", "0", "status"}),
            jsonPath({"tasks", "0", "needsApproval"}),
            jsonPath({"tasks", "0", "taskGroup", "kind"}),
            jsonPath({"tasks", "0", "taskGroup", "title"}),
            jsonPath({"tasks", "0", "goal", "threadID"}),
            jsonPath({"tasks", "0", "goal", "objective"}),
            jsonPath({"tasks", "0", "goal", "status"}),
            jsonPath({"tasks", "0", "goal", "tokensUsed"}),
            jsonPath({"tasks", "0", "goal", "elapsedSeconds"}),
            jsonPath({"tasks", "0", "goal", "createdAt"}),
            jsonPath({"tasks", "0", "goal", "updatedAt"}),
            jsonPath({"messages", "0", "id"}),
            jsonPath({"messages", "0", "role"}),
            jsonPath({"messages", "0", "text"}),
            jsonPath({"messages", "0", "attachments", "0", "id"}),
            jsonPath({"messages", "0", "attachments", "0", "kind"}),
            jsonPath({"messages", "0", "attachments", "0", "filename"}),
            jsonPath({"messages", "0", "attachments", "0", "data"}),
            jsonPath({"capabilities", "models"}),
            jsonPath({"capabilities", "skills"}),
            jsonPath({"capabilities", "plugins"}),
            jsonPath({"capabilities", "chatAgents"}),
            jsonPath({"capabilities", "models", "0", "id"}),
            jsonPath({"capabilities", "models", "0", "model"}),
            jsonPath({"capabilities", "models", "0", "displayName"}),
            jsonPath({"capabilities", "models", "0", "description"}),
            jsonPath({"capabilities", "models", "0", "isDefault"}),
            jsonPath({"capabilities", "models", "0", "defaultReasoningEffort"}),
            jsonPath({"capabilities", "models", "0", "supportedReasoningEfforts"}),
            jsonPath({"capabilities", "models", "0", "supportedReasoningEfforts", "0", "value"}),
            jsonPath({"capabilities", "models", "0", "supportedReasoningEfforts", "0", "description"}),
            jsonPath({"capabilities", "skills", "0", "name"}),
            jsonPath({"capabilities", "skills", "0", "displayName"}),
            jsonPath({"capabilities", "skills", "0", "description"}),
            jsonPath({"capabilities", "skills", "0", "path"}),
            jsonPath({"capabilities", "skills", "0", "scope"}),
            jsonPath({"capabilities", "plugins", "0", "id"}),
            jsonPath({"capabilities", "plugins", "0", "name"}),
            jsonPath({"capabilities", "plugins", "0", "displayName"}),
            jsonPath({"capabilities", "plugins", "0", "description"}),
            jsonPath({"capabilities", "plugins", "0", "enabled"}),
            jsonPath({"capabilities", "plugins", "0", "installed"}),
            jsonPath({"capabilities", "chatAgents", "0", "id"}),
            jsonPath({"capabilities", "chatAgents", "0", "name"}),
            jsonPath({"capabilities", "chatAgents", "0", "description"}),
            jsonPath({"capabilities", "chatAgents", "0", "symbolName"}),
            jsonPath({"capabilities", "chatModels", "0", "id"}),
            jsonPath({"capabilities", "chatModels", "0", "provider"}),
            jsonPath({"capabilities", "chatModels", "0", "model"}),
            jsonPath({"capabilities", "chatModels", "0", "displayName"}),
            jsonPath({"capabilities", "chatModels", "0", "description"}),
            jsonPath({"capabilities", "chatModels", "0", "isDefault"}),
            jsonPath({"capabilities", "chatModels", "0", "isAvailable"}),
            jsonPath({"capabilities", "chatModels", "0", "supportsAttachments"}),
            jsonPath({"chatMessage", "id"}),
            jsonPath({"chatMessage", "role"}),
            jsonPath({"chatMessage", "text"}),
            jsonPath({"timelineItems", "0", "id"}),
            jsonPath({"timelineItems", "0", "kind"}),
            jsonPath({"timelineItems", "0", "status"}),
            jsonPath({"timelineItems", "0", "media"}),
            jsonPath({"timelineItems", "0", "media", "0", "id"}),
            jsonPath({"timelineItems", "0", "media", "0", "kind"}),
            jsonPath({"timelineItems", "0", "media", "0", "mimeType"}),
            jsonPath({"timelineItems", "0", "media", "0", "data"}),
            jsonPath({"subagents", "0", "id"}),
            jsonPath({"subagents", "0", "name"}),
            jsonPath({"subagents", "0", "title"}),
            jsonPath({"subagents", "0", "updatedAt"}),
            jsonPath({"subagents", "0", "status"}),
            jsonPath({"contextUsage", "usedTokens"}),
            jsonPath({"contextUsage", "contextWindow"}),
            jsonPath({"usageSnapshot", "groups"}),
            jsonPath({"usageSnapshot", "availableResetCount"}),
            jsonPath({"usageSnapshot", "availableResetCredits"}),
            jsonPath({"usageSnapshot", "updatedAt"}),
            jsonPath({"usageSnapshot", "groups", "0", "id"}),
            jsonPath({"usageSnapshot", "groups", "0", "title"}),
            jsonPath({"usageSnapshot", "groups", "0", "shortWindow", "remainingPercent"}),
            jsonPath({"usageSnapshot", "groups", "0", "shortWindow", "durationLabel"}),
            jsonPath({"usageSnapshot", "availableResetCredits", "0", "id"}),
            jsonPath({"usageSnapshot", "availableResetCredits", "0", "displayTitle"}),
            jsonPath({"goal", "threadID"}),
            jsonPath({"goal", "objective"}),
            jsonPath({"goal", "status"}),
            jsonPath({"goal", "tokensUsed"}),
            jsonPath({"goal", "elapsedSeconds"}),
            jsonPath({"goal", "createdAt"}),
            jsonPath({"goal", "updatedAt"}),
        };
        for (const QStringList& path : responsePaths) {
            QJsonValue response(fullResponse());
            QVERIFY2(removeJsonField(response, path), qPrintable(dottedJsonPath(path)));
            verifyInvalidField(
                BridgeJsonCodec::decodeResponse(
                    compactJson(response.toObject()),
                    BridgeWireProfile::NearbyV1Milliseconds),
                dottedJsonPath(path));
        }

        const struct {
            QStringList path;
            QJsonValue replacement;
        } wrongTypes[] = {
            {jsonPath({"protocolVersion"}), QStringLiteral("one")},
            {jsonPath({"succeeded"}), QStringLiteral("yes")},
            {jsonPath({"tasks", "0", "updatedAt"}), QStringLiteral("today")},
            {jsonPath({"capabilities", "models"}), QJsonObject{}},
            {jsonPath({"timelineItems", "0", "media"}), QJsonObject{}},
            {jsonPath({"usageSnapshot", "groups", "0", "shortWindow", "remainingPercent"}),
             QStringLiteral("half")},
        };
        for (const auto& testCase : wrongTypes) {
            QJsonValue response(fullResponse());
            QVERIFY2(
                replaceJsonField(response, testCase.path, testCase.replacement),
                qPrintable(dottedJsonPath(testCase.path)));
            verifyInvalidField(
                BridgeJsonCodec::decodeResponse(
                    compactJson(response.toObject()),
                    BridgeWireProfile::NearbyV1Milliseconds),
                dottedJsonPath(testCase.path));
        }
    }

    void rejectsUnknownValuesForEveryBridgeEnum()
    {
        const QVector<QStringList> requestPaths = {
            jsonPath({"operation"}),
            jsonPath({"sendAction"}),
            jsonPath({"approvalDecision"}),
            jsonPath({"chatProvider"}),
            jsonPath({"attachments", "0", "kind"}),
        };
        for (const QStringList& path : requestPaths) {
            QJsonValue request(fullRequest());
            QVERIFY(replaceJsonField(request, path, QStringLiteral("future")));
            verifyInvalidField(
                BridgeJsonCodec::decodeRequest(
                    compactJson(request.toObject()),
                    BridgeWireProfile::NearbyV1Milliseconds),
                dottedJsonPath(path));
        }

        const QVector<QStringList> responsePaths = {
            jsonPath({"operation"}),
            jsonPath({"tasks", "0", "status"}),
            jsonPath({"tasks", "0", "taskGroup", "kind"}),
            jsonPath({"tasks", "0", "goal", "status"}),
            jsonPath({"messages", "0", "role"}),
            jsonPath({"timelineItems", "0", "kind"}),
            jsonPath({"timelineItems", "0", "status"}),
            jsonPath({"timelineItems", "0", "role"}),
            jsonPath({"timelineItems", "0", "phase"}),
            jsonPath({"timelineItems", "0", "media", "0", "kind"}),
            jsonPath({"subagents", "0", "status"}),
            jsonPath({"capabilities", "chatModels", "0", "provider"}),
        };
        for (const QStringList& path : responsePaths) {
            QJsonValue response(fullResponse());
            QVERIFY(replaceJsonField(response, path, QStringLiteral("future")));
            verifyInvalidField(
                BridgeJsonCodec::decodeResponse(
                    compactJson(response.toObject()),
                    BridgeWireProfile::NearbyV1Milliseconds),
                dottedJsonPath(path));
        }
    }

    void rejectsWrongOptionalTypesEnumsUuidsAndBase64()
    {
        auto request = fullRequest();
        request.insert(QStringLiteral("chatModelID"), 7);
        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                compactJson(std::move(request)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("chatModelID"));

        request = fullRequest();
        request.insert(QStringLiteral("chatProvider"), QStringLiteral("future"));
        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                compactJson(std::move(request)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("chatProvider"));

        request = fullRequest();
        request.insert(QStringLiteral("id"), QStringLiteral("not-a-uuid"));
        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                compactJson(std::move(request)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("id"));

        request = fullRequest();
        auto attachments = request.value(QStringLiteral("attachments")).toArray();
        auto attachment = attachments.at(0).toObject();
        attachment.insert(QStringLiteral("data"), QStringLiteral("AQ"));
        attachments[0] = attachment;
        request.insert(QStringLiteral("attachments"), attachments);
        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                compactJson(std::move(request)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("attachments[0].data"));

        request = fullRequest();
        request.insert(QStringLiteral("attachments"), QJsonObject{});
        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                compactJson(std::move(request)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("attachments"));

        request = fullRequest();
        request.insert(QStringLiteral("idempotencyKey"), 7);
        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                compactJson(std::move(request)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("idempotencyKey"));

        auto response = fullResponse();
        response.insert(QStringLiteral("message"), 7);
        verifyInvalidField(
            BridgeJsonCodec::decodeResponse(
                compactJson(std::move(response)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("message"));

        response = fullResponse();
        response.insert(QStringLiteral("tasks"), QJsonObject{});
        verifyInvalidField(
            BridgeJsonCodec::decodeResponse(
                compactJson(std::move(response)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("tasks"));

        response = fullResponse();
        response.insert(QStringLiteral("chatMessage"), QStringLiteral("message"));
        verifyInvalidField(
            BridgeJsonCodec::decodeResponse(
                compactJson(std::move(response)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("chatMessage"));

        response = fullResponse();
        response.insert(QStringLiteral("pairingSecret"), 7);
        verifyInvalidField(
            BridgeJsonCodec::decodeResponse(
                compactJson(std::move(response)),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("pairingSecret"));
    }

    void matchesSwiftBase64DecodingSemantics()
    {
        const struct {
            QString encoded;
            QByteArray expected;
        } accepted[] = {
            {QString(), QByteArray()},
            {QStringLiteral("AA=="), QByteArray::fromHex("00")},
            {QStringLiteral("AQI="), QByteArray::fromHex("0102")},
            {QStringLiteral("AQID"), QByteArray::fromHex("010203")},
            {QStringLiteral("AA==="), QByteArray::fromHex("00")},
            {QStringLiteral("===="), QByteArray::fromHex("00")},
            {QStringLiteral("AA=A"), QByteArray::fromHex("0000")},
            {QStringLiteral("AB=="), QByteArray::fromHex("00")},
            {QStringLiteral("AAB="), QByteArray::fromHex("0000")},
        };
        for (const auto& testCase : accepted) {
            auto request = fullRequest();
            auto attachments =
                request.value(QStringLiteral("attachments")).toArray();
            auto attachment = attachments.at(0).toObject();
            attachment.insert(QStringLiteral("data"), testCase.encoded);
            attachments[0] = attachment;
            request.insert(QStringLiteral("attachments"), attachments);

            const auto decoded = BridgeJsonCodec::decodeRequest(
                compactJson(request),
                BridgeWireProfile::NearbyV1Milliseconds);
            QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
            QCOMPARE(decoded.value().attachments->at(0).data, testCase.expected);

            const auto reencoded = BridgeJsonCodec::encodeRequest(
                decoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
            QVERIFY2(
                reencoded.hasValue(), qPrintable(resultErrorMessage(reencoded)));
            QCOMPARE(QJsonDocument::fromJson(reencoded.value())
                         .object()
                         .value(QStringLiteral("attachments"))
                         .toArray()
                         .at(0)
                         .toObject()
                         .value(QStringLiteral("data"))
                         .toString(),
                     QString::fromLatin1(testCase.expected.toBase64()));
        }

        for (const QString& invalid : {
                 QStringLiteral("AQ"),
                 QStringLiteral("A!=="),
                 QStringLiteral("A=AA"),
                 QStringLiteral("==="),
                 QStringLiteral("AA==\n"),
                 QStringLiteral(" AA=="),
                 QStringLiteral("AA== "),
                 QStringLiteral("__8="),
             }) {
            auto request = fullRequest();
            auto attachments =
                request.value(QStringLiteral("attachments")).toArray();
            auto attachment = attachments.at(0).toObject();
            attachment.insert(QStringLiteral("data"), invalid);
            attachments[0] = attachment;
            request.insert(QStringLiteral("attachments"), attachments);
            verifyInvalidField(
                BridgeJsonCodec::decodeRequest(
                    compactJson(request),
                    BridgeWireProfile::NearbyV1Milliseconds),
                QStringLiteral("attachments[0].data"));
        }
    }

    void acceptsNullOptionalsAndIgnoresUnknownNestedKeys()
    {
        auto response = fullResponse();
        response.insert(QStringLiteral("message"), QJsonValue::Null);
        response.insert(QStringLiteral("tasks"), QJsonValue::Null);
        response.insert(QStringLiteral("chatMessage"), QJsonValue::Null);
        response.insert(QStringLiteral("pairingSecret"), QJsonValue::Null);
        response.insert(QStringLiteral("futureRoot"), 7);
        auto capabilities = response.value(QStringLiteral("capabilities")).toObject();
        capabilities.insert(QStringLiteral("chatModels"), QJsonValue::Null);
        capabilities.insert(QStringLiteral("future"), QStringLiteral("ignored"));
        response.insert(QStringLiteral("capabilities"), capabilities);

        const auto decoded = BridgeJsonCodec::decodeResponse(
            compactJson(std::move(response)),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QVERIFY(!decoded.value().message.has_value());
        QVERIFY(!decoded.value().tasks.has_value());
        QVERIFY(!decoded.value().chatMessage.has_value());
        QVERIFY(!decoded.value().pairingSecret.has_value());
        QVERIFY(decoded.value().capabilities.has_value());
        QVERIFY(!decoded.value().capabilities->chatModels.has_value());

        const auto encoded = BridgeJsonCodec::encodeResponse(
            decoded.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const auto object = QJsonDocument::fromJson(encoded.value()).object();
        QVERIFY(!object.contains(QStringLiteral("message")));
        QVERIFY(!object.contains(QStringLiteral("tasks")));
        QVERIFY(!object.contains(QStringLiteral("chatMessage")));
        QVERIFY(!object.contains(QStringLiteral("pairingSecret")));
        QVERIFY(!object.contains(QStringLiteral("futureRoot")));
        QVERIFY(!object.value(QStringLiteral("capabilities"))
                     .toObject()
                     .contains(QStringLiteral("chatModels")));
    }

    void preservesAbsentAndPresentEmptyArrays()
    {
        auto request = fullRequest();
        request.insert(QStringLiteral("attachments"), QJsonArray{});
        const auto presentEmpty = BridgeJsonCodec::decodeRequest(
            compactJson(std::move(request)),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(presentEmpty.hasValue(), qPrintable(resultErrorMessage(presentEmpty)));
        QVERIFY(presentEmpty.value().attachments.has_value());
        QVERIFY(presentEmpty.value().attachments->isEmpty());

        const auto encoded = BridgeJsonCodec::encodeRequest(
            presentEmpty.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        QVERIFY(QJsonDocument::fromJson(encoded.value())
                    .object()
                    .value(QStringLiteral("attachments"))
                    .toArray()
                    .isEmpty());
    }

    void preservesInt64AtTopLevelAndNestedFields()
    {
        const auto decoded = BridgeJsonCodec::decodeResponse(
            fixture("bridge-response-full.json"),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
        QCOMPARE(decoded.value().protocolVersion, 1);
        QCOMPARE(decoded.value().tasks->at(0).goal->createdAt,
                 std::numeric_limits<qint64>::max());
        QCOMPARE(decoded.value().tasks->at(0).goal->updatedAt,
                 std::numeric_limits<qint64>::min());

        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                QByteArrayLiteral(
                    R"({"id":"33333333-3333-3333-3333-333333333333","protocolVersion":1,"operation":"handshake","limit":9223372036854775808})"),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("limit"));

        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                QByteArrayLiteral(
                    R"({"id":"33333333-3333-3333-3333-333333333333","protocolVersion":1,"operation":"handshake","limit":-9223372036854775809})"),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("limit"));
    }

    void matchesSwiftIntegerNumberCoercion()
    {
        const struct {
            const char* token;
            qint64 expected;
        } accepted[] = {
            {"1", 1},
            {"1.0", 1},
            {"1e0", 1},
            {"10e-1", 1},
            {"-0", 0},
            {"1.00000000000000000001", 1},
            {"1000000000000000000000000000000000000001e-39", 1},
            {"1234567890123456789000e-3", 1234567890123456789LL},
            {"9223372036854774784.0", 9223372036854774784LL},
            {"-9223372036854774784.0", -9223372036854774784LL},
        };

        for (const auto& testCase : accepted) {
            const QByteArray payload =
                QByteArrayLiteral(
                    "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                    "\"protocolVersion\":1,\"operation\":\"handshake\",\"limit\":")
                + testCase.token + QByteArrayLiteral("}");
            const auto decoded = BridgeJsonCodec::decodeRequest(
                payload, BridgeWireProfile::NearbyV1Milliseconds);
            QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));
            QCOMPARE(decoded.value().limit.value(), testCase.expected);
        }

        for (const char* token : {
                 "1.5",
                 "1e-1",
                 "1.2301e2",
                 "100000000000000000001e-2",
                 "1234567890123456789001e-3",
                 "9.223372036854775807e18",
                 "-9.223372036854775808e18",
                 "9223372036854775807.0",
                 "-9223372036854775808.0",
             }) {
            const QByteArray payload =
                QByteArrayLiteral(
                    "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                    "\"protocolVersion\":1,\"operation\":\"handshake\",\"limit\":")
                + token + QByteArrayLiteral("}");
            verifyInvalidField(
                BridgeJsonCodec::decodeRequest(
                    payload, BridgeWireProfile::NearbyV1Milliseconds),
                QStringLiteral("limit"));
        }
    }

    void matchesSwiftUuidWireFormat()
    {
        const QUuid expected(
            QStringLiteral("{abcdefab-cdef-abcd-efab-cdefabcdefab}"));
        const auto lowercase = BridgeJsonCodec::decodeRequest(
            QByteArrayLiteral(
                "{\"id\":\"abcdefab-cdef-abcd-efab-cdefabcdefab\","
                "\"protocolVersion\":1,\"operation\":\"handshake\"}"),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(lowercase.hasValue(), qPrintable(resultErrorMessage(lowercase)));
        QCOMPARE(lowercase.value().id, expected);

        const auto zero = BridgeJsonCodec::decodeRequest(
            QByteArrayLiteral(
                "{\"id\":\"00000000-0000-0000-0000-000000000000\","
                "\"protocolVersion\":1,\"operation\":\"handshake\"}"),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(zero.hasValue(), qPrintable(resultErrorMessage(zero)));
        QVERIFY(zero.value().id.isNull());

        verifyInvalidField(
            BridgeJsonCodec::decodeRequest(
                QByteArrayLiteral(
                    "{\"id\":\"{abcdefab-cdef-abcd-efab-cdefabcdefab}\","
                    "\"protocolVersion\":1,\"operation\":\"handshake\"}"),
                BridgeWireProfile::NearbyV1Milliseconds),
            QStringLiteral("id"));

        BridgeRequest request;
        request.id = expected;
        request.operation = BridgeOperation::Handshake;
        request.idempotencyKey = expected;
        request.attachments = QVector<BridgeAttachment>{BridgeAttachment{
            expected,
            AttachmentKind::File,
            QStringLiteral("file.txt"),
            std::nullopt,
            QByteArrayLiteral("x"),
        }};

        const auto encoded = BridgeJsonCodec::encodeRequest(
            request, BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const QJsonObject object = QJsonDocument::fromJson(encoded.value()).object();
        const QString uppercase =
            QStringLiteral("ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB");
        QCOMPARE(object.value(QStringLiteral("id")).toString(), uppercase);
        QCOMPARE(object.value(QStringLiteral("idempotencyKey")).toString(), uppercase);
        QCOMPARE(object.value(QStringLiteral("attachments"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QStringLiteral("id"))
                     .toString(),
                 uppercase);
    }

    void usesProfileSpecificBridgeDateConversions()
    {
        const struct {
            double reference;
            double nearby;
            double relay;
        } cases[] = {
            {0.0, 978307200000.0, 0.0},
            {0.0004999637603759766, 978307200000.5, 0.0004999637603759766},
            {-978307200.0005, -0.49996376037597656, -978307200.0005},
            {1e-9, 978307200000.0, 1e-9},
        };

        for (const auto& testCase : cases) {
            const auto input = datedResponse(testCase.reference);
            const auto nearby = BridgeJsonCodec::encodeResponse(
                input, BridgeWireProfile::NearbyV1Milliseconds);
            QVERIFY2(nearby.hasValue(), qPrintable(resultErrorMessage(nearby)));
            const auto relay = BridgeJsonCodec::encodeResponse(
                input, BridgeWireProfile::RelayV1Canonical);
            QVERIFY2(relay.hasValue(), qPrintable(resultErrorMessage(relay)));

            const auto nearbyValue = QJsonDocument::fromJson(nearby.value())
                                         .object()
                                         .value(QStringLiteral("tasks"))
                                         .toArray()
                                         .at(0)
                                         .toObject()
                                         .value(QStringLiteral("updatedAt"))
                                         .toDouble();
            const auto relayValue = QJsonDocument::fromJson(relay.value())
                                        .object()
                                        .value(QStringLiteral("tasks"))
                                        .toArray()
                                        .at(0)
                                        .toObject()
                                        .value(QStringLiteral("updatedAt"))
                                        .toDouble();
            QCOMPARE(nearbyValue, testCase.nearby);
            QCOMPARE(relayValue, testCase.relay);
        }

        const auto nearby = BridgeJsonCodec::decodeResponse(
            BridgeJsonCodec::encodeResponse(
                datedResponse(1e-9), BridgeWireProfile::NearbyV1Milliseconds).value(),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY(nearby.hasValue());
        QCOMPARE(nearby.value().tasks->at(0).updatedAt.secondsSinceReferenceDate, 0.0);

        const auto relay = BridgeJsonCodec::decodeResponse(
            BridgeJsonCodec::encodeResponse(
                datedResponse(1e-9), BridgeWireProfile::RelayV1Canonical).value(),
            BridgeWireProfile::RelayV1Canonical);
        QVERIFY(relay.hasValue());
        QCOMPARE(relay.value().tasks->at(0).updatedAt.secondsSinceReferenceDate, 1e-9);

        const auto nearbyBytes = BridgeJsonCodec::encodeResponse(
            datedResponse(42.25), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(nearbyBytes.hasValue(), qPrintable(resultErrorMessage(nearbyBytes)));
        const auto nearbyAsRelay = BridgeJsonCodec::decodeResponse(
            nearbyBytes.value(), BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(nearbyAsRelay.hasValue(), qPrintable(resultErrorMessage(nearbyAsRelay)));
        QVERIFY(nearbyAsRelay.value()
                    .tasks->at(0)
                    .updatedAt.secondsSinceReferenceDate
                != 42.25);

        const auto relayBytes = BridgeJsonCodec::encodeResponse(
            datedResponse(42.25), BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(relayBytes.hasValue(), qPrintable(resultErrorMessage(relayBytes)));
        const auto relayAsNearby = BridgeJsonCodec::decodeResponse(
            relayBytes.value(), BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(relayAsNearby.hasValue(), qPrintable(resultErrorMessage(relayAsNearby)));
        QVERIFY(relayAsNearby.value()
                    .tasks->at(0)
                    .updatedAt.secondsSinceReferenceDate
                != 42.25);
    }

    void preservesSwiftSignedZeroAndStringEscaping()
    {
        const auto dated = BridgeJsonCodec::decodeResponse(
            QByteArrayLiteral(
                "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                "\"protocolVersion\":1,\"operation\":\"listTasks\","
                "\"succeeded\":true,\"tasks\":[{\"id\":\"thread\","
                "\"title\":\"Title\",\"preview\":\"Preview\",\"updatedAt\":-0,"
                "\"status\":\"waiting\",\"needsApproval\":false}]}"),
            BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(dated.hasValue(), qPrintable(resultErrorMessage(dated)));
        QVERIFY(std::signbit(
            dated.value().tasks->at(0).updatedAt.secondsSinceReferenceDate));

        const auto datedBytes = BridgeJsonCodec::encodeResponse(
            dated.value(), BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(
            datedBytes.hasValue(), qPrintable(resultErrorMessage(datedBytes)));
        QVERIFY(datedBytes.value().contains("\"updatedAt\":-0"));

        const auto usage = BridgeJsonCodec::decodeResponse(
            QByteArrayLiteral(
                "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                "\"protocolVersion\":1,\"operation\":\"loadUsage\","
                "\"succeeded\":true,\"usageSnapshot\":{\"groups\":[{\"id\":"
                "\"group\",\"title\":\"Group\",\"shortWindow\":{"
                "\"remainingPercent\":-0,\"durationLabel\":\"5h\"}}],"
                "\"availableResetCount\":0,\"availableResetCredits\":[],"
                "\"updatedAt\":0}}"),
            BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(usage.hasValue(), qPrintable(resultErrorMessage(usage)));
        QVERIFY(std::signbit(usage.value()
                                 .usageSnapshot->groups.at(0)
                                 .shortWindow->remainingPercent));

        const auto usageBytes = BridgeJsonCodec::encodeResponse(
            usage.value(), BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(
            usageBytes.hasValue(), qPrintable(resultErrorMessage(usageBytes)));
        QVERIFY(usageBytes.value().contains("\"remainingPercent\":-0"));

        auto request = BridgeRequest{
            QUuid(QStringLiteral("{33333333-3333-3333-3333-333333333333}")),
            kBridgeProtocolVersion,
            BridgeOperation::Handshake,
        };
        request.text = QStringLiteral("slash / quote \" newline\n");
        const auto nearby = BridgeJsonCodec::encodeRequest(
            request, BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY2(nearby.hasValue(), qPrintable(resultErrorMessage(nearby)));
        QVERIFY(nearby.value().contains(
            QByteArrayLiteral("\"text\":\"slash \\/ quote \\\" newline\\n\"")));
    }

    void matchesSwiftLargeDoubleFormatting()
    {
        const struct {
            double value;
            QByteArray token;
        } cases[] = {
            {9007199254740992.0, QByteArrayLiteral("9007199254740992")},
            {9007199254740994.0, QByteArrayLiteral("9.007199254740994e+15")},
            {-9007199254740994.0, QByteArrayLiteral("-9.007199254740994e+15")},
        };

        for (const auto& testCase : cases) {
            const auto encoded = BridgeJsonCodec::encodeResponse(
                datedResponse(testCase.value),
                BridgeWireProfile::RelayV1Canonical);
            QVERIFY2(
                encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
            QVERIFY(encoded.value().contains(
                QByteArrayLiteral("\"updatedAt\":") + testCase.token));
        }
    }

    void emitsRelayProfileAsCanonicalSortedGoldenBytes()
    {
        const auto decoded = BridgeJsonCodec::decodeResponse(
            fixture("bridge-relay-canonical.json"),
            BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(decoded.hasValue(), qPrintable(resultErrorMessage(decoded)));

        const auto encoded = BridgeJsonCodec::encodeResponse(
            decoded.value(), BridgeWireProfile::RelayV1Canonical);
        QVERIFY2(encoded.hasValue(), qPrintable(resultErrorMessage(encoded)));
        const QByteArray expected = fixture("bridge-relay-canonical.json");
        if (encoded.value() != expected) {
            qsizetype difference = 0;
            while (difference < encoded.value().size()
                   && difference < expected.size()
                   && encoded.value().at(difference) == expected.at(difference)) {
                ++difference;
            }
            const qsizetype contextStart = std::max<qsizetype>(0, difference - 40);
            QFAIL(qPrintable(QStringLiteral(
                "canonical JSON first differs at byte %1; actual[%2], expected[%3]")
                                 .arg(difference)
                                 .arg(QString::fromUtf8(
                                     encoded.value().mid(contextStart, 100)))
                                 .arg(QString::fromUtf8(
                                     expected.mid(contextStart, 100)))));
        }
    }

    void rejectsNonfiniteValuesAndNearbyOverflowOnEncode()
    {
        for (const double value : {
                 std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(),
             }) {
            for (const BridgeWireProfile profile : {
                     BridgeWireProfile::NearbyV1Milliseconds,
                     BridgeWireProfile::RelayV1Canonical,
                 }) {
                const auto nonfiniteDate = BridgeJsonCodec::encodeResponse(
                    datedResponse(value), profile);
                QVERIFY(!nonfiniteDate.hasValue());
                QCOMPARE(
                    nonfiniteDate.error().code,
                    QStringLiteral("bridge.invalid_field"));
            }

            auto nonfiniteUsage = minimumResponse();
            nonfiniteUsage.usageSnapshot = BridgeUsageSnapshot{
                std::nullopt,
                {BridgeUsageGroup{
                    QStringLiteral("group"),
                    QStringLiteral("Group"),
                    BridgeUsageWindow{
                        value,
                        QStringLiteral("5h"),
                        std::nullopt,
                    },
                    std::nullopt,
                }},
                0,
                {},
                {0.0},
            };
            const auto encodedUsage = BridgeJsonCodec::encodeResponse(
                nonfiniteUsage, BridgeWireProfile::NearbyV1Milliseconds);
            QVERIFY(!encodedUsage.hasValue());
            QCOMPARE(
                encodedUsage.error().code,
                QStringLiteral("bridge.invalid_field"));
        }

        const auto overflow = BridgeJsonCodec::encodeResponse(
            datedResponse(std::numeric_limits<double>::max()),
            BridgeWireProfile::NearbyV1Milliseconds);
        QVERIFY(!overflow.hasValue());
        QCOMPARE(overflow.error().code, QStringLiteral("bridge.invalid_field"));
    }

    void rejectsNonfiniteIncomingJsonNumbers()
    {
        for (const QByteArray& token : {
                 QByteArrayLiteral("NaN"),
                 QByteArrayLiteral("Infinity"),
                 QByteArrayLiteral("-Infinity"),
                 QByteArrayLiteral("1e9999"),
             }) {
            const QByteArray payload =
                QByteArrayLiteral(
                    "{\"id\":\"33333333-3333-3333-3333-333333333333\","
                    "\"protocolVersion\":1,\"operation\":\"handshake\","
                    "\"succeeded\":true,\"tasks\":[{\"id\":\"thread\","
                    "\"title\":\"Title\",\"preview\":\"Preview\",\"updatedAt\":")
                + token
                + QByteArrayLiteral(
                    ",\"status\":\"waiting\",\"needsApproval\":false}]}");
            const auto decoded = BridgeJsonCodec::decodeResponse(
                payload, BridgeWireProfile::NearbyV1Milliseconds);
            QVERIFY(!decoded.hasValue());
            QCOMPARE(decoded.error().code, QStringLiteral("bridge.invalid_json"));
        }
    }
};

QTEST_GUILESS_MAIN(BridgeJsonCodecTests)
#include "BridgeJsonCodecTests.moc"
