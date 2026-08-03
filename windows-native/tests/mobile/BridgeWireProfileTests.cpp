#include "codex/models/BridgeJsonCodec.h"
#include "mobile/MobileTypes.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtTest>

#include <array>

namespace {

using namespace companion;

QByteArray fixture(const QString& name)
{
    QFile file(
        QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral("/mobile-v034/")
        + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll().trimmed();
}

QUuid fixedId()
{
    return QUuid(
        QStringLiteral(
            "{33333333-3333-3333-3333-333333333333}"));
}

QString shadowContentHash()
{
    return QStringLiteral(
        "69b1fbb5730390bf24190d53a312b504834b94db51ec2397d5a1984f4ad40e9f");
}

BridgePresencePetFile shadowThumbnail()
{
    return {
        QStringLiteral("thumbnail.png"),
        QStringLiteral(
            "2069b23af654b2e22d683538f667b0155431801d74806cb3081918563104a970"),
        16296,
    };
}

BridgePresencePetManifest shadowManifest()
{
    return {
        1,
        QStringLiteral("shadow-16-mobile-presence-v10"),
        QStringLiteral("shadow-16"),
        QStringLiteral("Shadow"),
        QStringLiteral("10"),
        {
            {
                QStringLiteral("atlas.png"),
                QStringLiteral(
                    "86d033656517c8ed807a33a5a41ed8bdf4e1e59fd0ef3c3fe3e8c9f7d2f75946"),
                500955,
            },
            144,
            144,
            12,
            3,
        },
        shadowThumbnail(),
        {
            {
                BridgePresencePetState::Idle,
                0,
                12,
                {620, 220, 120, 100, 90, 100, 90, 100, 120, 220, 260, 760},
                0,
            },
            {
                BridgePresencePetState::Thinking,
                1,
                12,
                {220, 180, 160, 160, 180, 300, 320, 160, 180, 160, 180, 450},
                0,
            },
            {
                BridgePresencePetState::Talking,
                2,
                12,
                {130, 90, 105, 90, 110, 100, 90, 105, 130, 90, 105, 150},
                0,
            },
        },
        shadowContentHash(),
    };
}

BridgePresencePetCatalogEntry shadowCatalogEntry()
{
    return {
        QStringLiteral("shadow-16-mobile-presence-v10"),
        QStringLiteral("shadow-16"),
        QStringLiteral("Shadow"),
        QStringLiteral("10"),
        shadowContentHash(),
        518820,
        shadowThumbnail(),
    };
}

BridgeResponse datedResponse(double secondsSinceReferenceDate)
{
    BridgeTask task;
    task.id = QStringLiteral("thread");
    task.title = QStringLiteral("Task");
    task.preview = QStringLiteral("Preview");
    task.updatedAt.secondsSinceReferenceDate =
        secondsSinceReferenceDate;
    task.status = TaskStatus::Waiting;

    BridgeGoal goal;
    goal.threadId = task.id;
    goal.objective = QStringLiteral("Finish");
    goal.createdAt = 123;
    goal.updatedAt = 456;
    task.goal = goal;

    BridgeResponse response;
    response.id = fixedId();
    response.operation = BridgeOperation::ListTasks;
    response.succeeded = true;
    response.tasks = QVector<BridgeTask>{task};
    return response;
}

} // namespace

class BridgeWireProfileTests final : public QObject {
    Q_OBJECT

private slots:
    void mobileTypesExposeTransportNeutralStates()
    {
        QCOMPARE(
            MobileTransportRoute::Nearby,
            MobileTransportRoute::Nearby);
        QCOMPARE(
            MobileTransportRoute::Relay,
            MobileTransportRoute::Relay);
        QCOMPARE(
            MobileConnectionState::Pairing,
            MobileConnectionState::Pairing);
        QCOMPARE(
            MobileConnectionState::RelayConnected,
            MobileConnectionState::RelayConnected);
    }

    void allBridgeOperationsRoundTripAcrossBothProfiles()
    {
        constexpr std::array operations{
            BridgeOperation::Handshake,
            BridgeOperation::ListTasks,
            BridgeOperation::LoadMessages,
            BridgeOperation::SendMessage,
            BridgeOperation::RespondToApproval,
            BridgeOperation::CreateTask,
            BridgeOperation::LoadCapabilities,
            BridgeOperation::SendCasualChat,
            BridgeOperation::LoadUsage,
            BridgeOperation::ConsumeUsageReset,
            BridgeOperation::CreateGoal,
            BridgeOperation::ResumeGoal,
            BridgeOperation::UpdateGoal,
            BridgeOperation::LoadPresencePetManifest,
            BridgeOperation::LoadPresencePetChunk,
        };
        constexpr std::array profiles{
            BridgeWireProfile::NearbyV1Milliseconds,
            BridgeWireProfile::RelayV1Canonical,
        };

        for (const BridgeWireProfile profile : profiles) {
            for (const BridgeOperation operation : operations) {
                BridgeRequest request;
                request.id = fixedId();
                request.operation = operation;

                const auto encoded =
                    BridgeJsonCodec::encodeRequest(
                        request,
                        profile);
                QVERIFY(encoded.hasValue());
                const auto decoded =
                    BridgeJsonCodec::decodeRequest(
                        encoded.value(),
                        profile);
                QVERIFY(decoded.hasValue());
                QCOMPARE(
                    decoded.value().operation,
                    operation);
                QCOMPARE(
                    decoded.value().protocolVersion,
                    kBridgeProtocolVersion);
            }
        }
    }

    void presencePetRequestFieldsRoundTripAcrossBothProfiles()
    {
        constexpr std::array profiles{
            BridgeWireProfile::NearbyV1Milliseconds,
            BridgeWireProfile::RelayV1Canonical,
        };

        BridgeRequest request;
        request.id = fixedId();
        request.operation =
            BridgeOperation::LoadPresencePetChunk;
        request.presencePetPackageId =
            QStringLiteral(
                "shadow-16-mobile-presence-v10");
        request.presencePetContentHash =
            shadowContentHash();
        request.presencePetFileName =
            QStringLiteral("atlas.png");
        request.presencePetOffset = 196608;
        request.presencePetLength = 196608;

        for (const BridgeWireProfile profile : profiles) {
            const auto encoded =
                BridgeJsonCodec::encodeRequest(
                    request,
                    profile);
            QVERIFY(encoded.hasValue());

            const QJsonObject object =
                QJsonDocument::fromJson(encoded.value())
                    .object();
            QCOMPARE(
                object.value(QStringLiteral("operation"))
                    .toString(),
                QStringLiteral("loadPresencePetChunk"));
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "presencePetPackageID"))
                    .toString(),
                QStringLiteral(
                    "shadow-16-mobile-presence-v10"));
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "presencePetContentHash"))
                    .toString(),
                shadowContentHash());
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "presencePetFileName"))
                    .toString(),
                QStringLiteral("atlas.png"));
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "presencePetOffset"))
                    .toInteger(),
                196608);
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "presencePetLength"))
                    .toInteger(),
                196608);

            const auto decoded =
                BridgeJsonCodec::decodeRequest(
                    encoded.value(),
                    profile);
            QVERIFY(decoded.hasValue());
            QVERIFY(decoded.value() == request);
        }
    }

    void presencePetResponseFieldsRoundTripAcrossBothProfiles()
    {
        constexpr std::array profiles{
            BridgeWireProfile::NearbyV1Milliseconds,
            BridgeWireProfile::RelayV1Canonical,
        };

        BridgeResponse response;
        response.id = fixedId();
        response.operation =
            BridgeOperation::LoadPresencePetChunk;
        response.succeeded = true;
        response.features = QVector{
            BridgeFeature::TaskStreamV1,
            BridgeFeature::PresencePetPackageV1,
            BridgeFeature::AttachmentUploadV1,
        };
        response.selectedDesktopPetId =
            QStringLiteral("shadow-16");
        response.presencePetCatalog =
            QVector{shadowCatalogEntry()};
        response.presencePetManifest =
            shadowManifest();
        response.presencePetChunk =
            BridgePresencePetChunk{
                QStringLiteral(
                    "shadow-16-mobile-presence-v10"),
                shadowContentHash(),
                QStringLiteral("atlas.png"),
                0,
                QByteArray("png"),
                3,
                false,
            };

        for (const BridgeWireProfile profile : profiles) {
            const auto encoded =
                BridgeJsonCodec::encodeResponse(
                    response,
                    profile);
            QVERIFY(encoded.hasValue());

            const QJsonObject object =
                QJsonDocument::fromJson(encoded.value())
                    .object();
            const QJsonArray features =
                object.value(QStringLiteral("features"))
                    .toArray();
            QCOMPARE(features.size(), 3);
            QCOMPARE(
                features.at(0).toString(),
                QStringLiteral("task_stream_v1"));
            QCOMPARE(
                features.at(1).toString(),
                QStringLiteral(
                    "presence_pet_package_v1"));
            QCOMPARE(
                features.at(2).toString(),
                QStringLiteral(
                    "attachment_upload_v1"));
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "selectedDesktopPetID"))
                    .toString(),
                QStringLiteral("shadow-16"));
            QCOMPARE(
                object.value(
                    QStringLiteral(
                        "presencePetChunk"))
                    .toObject()
                    .value(QStringLiteral("data"))
                    .toString(),
                QStringLiteral("cG5n"));

            const auto decoded =
                BridgeJsonCodec::decodeResponse(
                    encoded.value(),
                    profile);
            QVERIFY(decoded.hasValue());
            QVERIFY(decoded.value() == response);
        }
    }

    void nearbyFixturesPreserveV034AdditiveFields()
    {
        const QByteArray requestBytes =
            fixture(QStringLiteral(
                "bridge-request-nearby.json"));
        QVERIFY(!requestBytes.isEmpty());
        const auto request =
            BridgeJsonCodec::decodeRequest(
                requestBytes,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(request.hasValue());
        QVERIFY(request.value().chatProvider.has_value());
        QCOMPARE(
            *request.value().chatProvider,
            ChatProvider::OpenAIAPI);
        QCOMPARE(
            request.value().chatModelId,
            std::optional<QString>(
                QStringLiteral("gpt56Terra")));
        QVERIFY(request.value().attachments.has_value());
        QCOMPARE(
            request.value().attachments->size(),
            2);

        const QByteArray responseBytes =
            fixture(QStringLiteral(
                "bridge-response-nearby.json"));
        QVERIFY(!responseBytes.isEmpty());
        const auto response =
            BridgeJsonCodec::decodeResponse(
                responseBytes,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(response.hasValue());
        QVERIFY(response.value().messages.has_value());
        QVERIFY(
            response.value().messages->at(0)
                .attachments.has_value());
        QVERIFY(response.value().capabilities.has_value());
        QVERIFY(
            response.value().capabilities->chatModels
                .has_value());
        QCOMPARE(
            response.value()
                .capabilities->chatModels->size(),
            7);
    }

    void absentOptionalFieldsStayAbsent()
    {
        BridgeRequest request;
        request.id = fixedId();
        request.operation = BridgeOperation::Handshake;

        const auto encodedRequest =
            BridgeJsonCodec::encodeRequest(
                request,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(encodedRequest.hasValue());
        const QJsonObject requestObject =
            QJsonDocument::fromJson(
                encodedRequest.value())
                .object();
        QVERIFY(!requestObject.contains(
            QStringLiteral("chatProvider")));
        QVERIFY(!requestObject.contains(
            QStringLiteral("chatModelID")));
        QVERIFY(!requestObject.contains(
            QStringLiteral("attachments")));

        BridgeResponse response;
        response.id = fixedId();
        response.operation =
            BridgeOperation::LoadCapabilities;
        response.succeeded = true;
        response.capabilities = BridgeCapabilities{};

        const auto encodedResponse =
            BridgeJsonCodec::encodeResponse(
                response,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(encodedResponse.hasValue());
        const QJsonObject capabilities =
            QJsonDocument::fromJson(
                encodedResponse.value())
                .object()
                .value(QStringLiteral("capabilities"))
                .toObject();
        QVERIFY(!capabilities.contains(
            QStringLiteral("chatModels")));
    }

    void profilesUseDeliberateDateConversions()
    {
        const BridgeResponse response =
            datedResponse(0.0);
        const auto nearby =
            BridgeJsonCodec::encodeResponse(
                response,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        const auto relay =
            BridgeJsonCodec::encodeResponse(
                response,
                BridgeWireProfile::
                    RelayV1Canonical);
        QVERIFY(nearby.hasValue());
        QVERIFY(relay.hasValue());

        const QJsonObject nearbyTask =
            QJsonDocument::fromJson(nearby.value())
                .object()
                .value(QStringLiteral("tasks"))
                .toArray()
                .at(0)
                .toObject();
        const QJsonObject relayTask =
            QJsonDocument::fromJson(relay.value())
                .object()
                .value(QStringLiteral("tasks"))
                .toArray()
                .at(0)
                .toObject();
        QCOMPARE(
            nearbyTask.value(
                QStringLiteral("updatedAt"))
                .toDouble(),
            978307200000.0);
        QCOMPARE(
            relayTask.value(
                QStringLiteral("updatedAt"))
                .toDouble(),
            0.0);
        QCOMPARE(
            nearbyTask.value(QStringLiteral("goal"))
                .toObject()
                .value(QStringLiteral("createdAt"))
                .toInteger(),
            123);
        QCOMPARE(
            relayTask.value(QStringLiteral("goal"))
                .toObject()
                .value(QStringLiteral("updatedAt"))
                .toInteger(),
            456);

        const BridgeResponse dated =
            datedResponse(42.25);
        const auto nearbyBytes =
            BridgeJsonCodec::encodeResponse(
                dated,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(nearbyBytes.hasValue());
        const auto decodedWithWrongProfile =
            BridgeJsonCodec::decodeResponse(
                nearbyBytes.value(),
                BridgeWireProfile::
                    RelayV1Canonical);
        QVERIFY(decodedWithWrongProfile.hasValue());
        QVERIFY(
            decodedWithWrongProfile.value()
                .tasks->at(0)
                .updatedAt
                .secondsSinceReferenceDate
            != 42.25);
    }

    void attachmentBytesUseStandardPaddedBase64()
    {
        BridgeAttachment attachment;
        attachment.id = fixedId();
        attachment.kind = AttachmentKind::File;
        attachment.filename =
            QStringLiteral("bytes.bin");
        attachment.data =
            QByteArray::fromHex("0001");

        BridgeRequest request;
        request.id = fixedId();
        request.operation =
            BridgeOperation::SendMessage;
        request.attachments =
            QVector<BridgeAttachment>{attachment};

        const auto encoded =
            BridgeJsonCodec::encodeRequest(
                request,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(encoded.hasValue());
        const QString data =
            QJsonDocument::fromJson(encoded.value())
                .object()
                .value(QStringLiteral("attachments"))
                .toArray()
                .at(0)
                .toObject()
                .value(QStringLiteral("data"))
                .toString();
        QCOMPARE(data, QStringLiteral("AAE="));
    }
};

QTEST_GUILESS_MAIN(BridgeWireProfileTests)
#include "BridgeWireProfileTests.moc"
