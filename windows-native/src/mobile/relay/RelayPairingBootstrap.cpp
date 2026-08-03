#include "mobile/relay/RelayPairingBootstrap.h"

#include "mobile/relay/RelayConnection.h"
#include "mobile/relay/RelayModels.h"
#include "mobile/relay/RelaySettings.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/RelayStateStore.h"
#include "platform/windows/security/WindowsCrypto.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <cmath>
#include <limits>
#include <utility>

namespace companion {
namespace {

constexpr qint64 kMaximumExactJsonInteger =
    9'007'199'254'740'991LL;

CompanionError bootstrapError(
    QString code,
    QString message,
    bool retryable = false)
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        {},
    };
}

QByteArray jsonString(
    const QString& value)
{
    const QByteArray array =
        QJsonDocument(
            QJsonArray{value})
            .toJson(QJsonDocument::Compact);
    return array.mid(
        1,
        array.size() - 2);
}

QByteArray base64Url(
    QByteArrayView bytes)
{
    return bytes.toByteArray()
        .toBase64(
            QByteArray::Base64UrlEncoding
            | QByteArray::
                OmitTrailingEquals);
}

Result<QByteArray> decodeBase64Url(
    QStringView value)
{
    const QByteArray encoded =
        value.toLatin1();
    if (encoded.isEmpty()
        || encoded.contains('=')) {
        return Result<QByteArray>::failure(
            bootstrapError(
                QStringLiteral(
                    "relay.pairing_link_invalid"),
                QStringLiteral(
                    "The Companion pairing payload is invalid.")));
    }
    const auto decoded =
        QByteArray::fromBase64Encoding(
            encoded,
            QByteArray::Base64UrlEncoding
                | QByteArray::
                    AbortOnBase64DecodingErrors);
    if (!decoded) {
        return Result<QByteArray>::failure(
            bootstrapError(
                QStringLiteral(
                    "relay.pairing_link_invalid"),
                QStringLiteral(
                    "The Companion pairing payload is invalid.")));
    }
    return Result<QByteArray>::success(
        decoded.decoded);
}

Result<QByteArray> decodeBase64(
    const QJsonValue& value)
{
    if (!value.isString()) {
        return Result<QByteArray>::failure(
            bootstrapError(
                QStringLiteral(
                    "relay.pairing_request_invalid"),
                QStringLiteral(
                    "The secure mobile pairing request is invalid.")));
    }
    const auto decoded =
        QByteArray::fromBase64Encoding(
            value.toString().toLatin1(),
            QByteArray::
                AbortOnBase64DecodingErrors);
    if (!decoded) {
        return Result<QByteArray>::failure(
            bootstrapError(
                QStringLiteral(
                    "relay.pairing_request_invalid"),
                QStringLiteral(
                    "The secure mobile pairing request is invalid.")));
    }
    return Result<QByteArray>::success(
        decoded.decoded);
}

std::optional<qint64> exactInteger(
    const QJsonValue& value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number =
        value.toDouble();
    if (!std::isfinite(number)
        || std::trunc(number) != number
        || number
            < -static_cast<double>(
                kMaximumExactJsonInteger)
        || number
            > static_cast<double>(
                kMaximumExactJsonInteger)) {
        return std::nullopt;
    }
    const qint64 integer =
        static_cast<qint64>(number);
    if (static_cast<double>(integer)
        != number) {
        return std::nullopt;
    }
    return integer;
}

Result<QString> validatedPairingCode(
    const QString& value)
{
    const auto normalized =
        BridgeSecurity::
            normalizedPairingCode(value);
    if (!normalized.has_value()
        || normalized->size() != 6) {
        return Result<QString>::failure(
            bootstrapError(
                QStringLiteral(
                    "relay.pairing_code_invalid"),
                QStringLiteral(
                    "The pairing code must contain six digits.")));
    }
    return Result<QString>::success(
        *normalized);
}

Result<QUrl> validatedRelayUrl(
    const QUrl& value)
{
    return RelaySettings::validatedUrl(
        value.toString(
            QUrl::FullyEncoded));
}

Result<void> validateOffer(
    const RelayPairingBootstrapOffer&
        offer)
{
    if (offer.version
            != RelayPairingBootstrapOffer::
                   currentVersion
        || !RelayModels::isValidOpaqueId(
            offer.hostDeviceId)
        || offer.hostDisplayName
               .trimmed()
               .isEmpty()
        || offer.expiresAtMilliseconds <= 0
        || offer.expiresAtMilliseconds
            > kMaximumExactJsonInteger
        || offer.bootstrapSecret.size()
            != 32) {
        return Result<void>::failure(
            bootstrapError(
                QStringLiteral(
                    "relay.pairing_offer_invalid"),
                QStringLiteral(
                    "The Companion pairing offer is invalid.")));
    }
    const auto relay =
        validatedRelayUrl(
            offer.relayUrl);
    if (!relay.hasValue()) {
        return Result<void>::failure(
            relay.error());
    }
    const auto code =
        validatedPairingCode(
            offer.pairingCode);
    if (!code.hasValue()) {
        return Result<void>::failure(
            code.error());
    }
    return Result<void>::success();
}

struct PairingRequest final {
    QUuid id;
    int version = 1;
    BridgeInvitation invitation;
};

Result<BridgeInvitation>
decodeInvitation(
    const QJsonValue& value)
{
    if (!value.isObject()) {
        return Result<BridgeInvitation>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
    }
    const QJsonObject object =
        value.toObject();
    const QSet<QString> allowed{
        QStringLiteral("version"),
        QStringLiteral("deviceID"),
        QStringLiteral("displayName"),
        QStringLiteral(
            "issuedAtMilliseconds"),
        QStringLiteral("nonce"),
        QStringLiteral("authenticator"),
        QStringLiteral("pairingCode"),
    };
    for (auto iterator =
             object.constBegin();
         iterator != object.constEnd();
         ++iterator) {
        if (!allowed.contains(
                iterator.key())) {
            return Result<
                BridgeInvitation>::failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
        }
    }

    const auto version =
        exactInteger(
            object.value(
                QStringLiteral("version")));
    const auto issuedAt =
        exactInteger(
            object.value(
                QStringLiteral(
                    "issuedAtMilliseconds")));
    const QJsonValue deviceValue =
        object.value(
            QStringLiteral("deviceID"));
    const QJsonValue displayValue =
        object.value(
            QStringLiteral("displayName"));
    const auto nonce =
        decodeBase64(
            object.value(
                QStringLiteral("nonce")));
    if (!version.has_value()
        || *version
            < std::numeric_limits<int>::min()
        || *version
            > std::numeric_limits<int>::max()
        || !issuedAt.has_value()
        || !deviceValue.isString()
        || !displayValue.isString()
        || !nonce.hasValue()
        || nonce.value().size() != 16
        || !RelayModels::isValidOpaqueId(
            deviceValue.toString())
        || displayValue.toString()
               .trimmed()
               .isEmpty()
        || displayValue.toString()
               .toUtf8()
               .size()
            > 256) {
        return Result<BridgeInvitation>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
    }

    std::optional<QByteArray>
        authenticator;
    const auto authenticatorValue =
        object.constFind(
            QStringLiteral(
                "authenticator"));
    if (authenticatorValue
        != object.constEnd()) {
        const auto decoded =
            decodeBase64(
                *authenticatorValue);
        if (!decoded.hasValue()
            || decoded.value().size()
                != 32) {
            return Result<
                BridgeInvitation>::failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
        }
        authenticator =
            decoded.value();
    }

    std::optional<QString> pairingCode;
    const auto pairingValue =
        object.constFind(
            QStringLiteral("pairingCode"));
    if (pairingValue
        != object.constEnd()) {
        if (!pairingValue->isString()
            || pairingValue->toString()
                   .size()
                > 64) {
            return Result<
                BridgeInvitation>::failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
        }
        pairingCode =
            pairingValue->toString();
    }

    return Result<BridgeInvitation>::
        success({
            static_cast<int>(*version),
            deviceValue.toString(),
            displayValue.toString(),
            *issuedAt,
            nonce.value(),
            std::move(authenticator),
            std::move(pairingCode),
        });
}

Result<PairingRequest> decodeRequest(
    QByteArrayView bytes)
{
    if (bytes.isEmpty()
        || bytes.size() > 16 * 1024) {
        return Result<PairingRequest>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            bytes.toByteArray(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<PairingRequest>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
    }

    const QJsonObject object =
        document.object();
    const QSet<QString> allowed{
        QStringLiteral("id"),
        QStringLiteral("invitation"),
        QStringLiteral("version"),
    };
    for (auto iterator =
             object.constBegin();
         iterator != object.constEnd();
         ++iterator) {
        if (!allowed.contains(
                iterator.key())) {
            return Result<
                PairingRequest>::failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
        }
    }

    const QJsonValue idValue =
        object.value(
            QStringLiteral("id"));
    const auto version =
        exactInteger(
            object.value(
                QStringLiteral("version")));
    const auto invitation =
        decodeInvitation(
            object.value(
                QStringLiteral(
                    "invitation")));
    const QUuid id =
        idValue.isString()
            ? QUuid(idValue.toString())
            : QUuid();
    if (id.isNull()
        || !version.has_value()
        || *version
            < std::numeric_limits<int>::min()
        || *version
            > std::numeric_limits<int>::max()
        || !invitation.hasValue()) {
        return Result<PairingRequest>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_request_invalid"),
                    QStringLiteral(
                        "The secure mobile pairing request is invalid.")));
    }
    return Result<PairingRequest>::success({
        id,
        static_cast<int>(*version),
        invitation.value(),
    });
}

QByteArray encodeResponse(
    const PairingRequest& request,
    bool succeeded,
    const QString& hostDisplayName,
    const QString& hostDeviceId,
    const std::optional<PairingRecord>&
        record,
    const std::optional<QUrl>& relayUrl,
    const std::optional<CompanionError>&
        error)
{
    QJsonObject response{
        {
            QStringLiteral("id"),
            request.id.toString(
                QUuid::WithoutBraces),
        },
        {
            QStringLiteral("succeeded"),
            succeeded,
        },
        {
            QStringLiteral("version"),
            RelayPairingBootstrapOffer::
                currentVersion,
        },
        {
            QStringLiteral("macName"),
            hostDisplayName,
        },
        {
            QStringLiteral("macDeviceID"),
            hostDeviceId,
        },
    };
    if (record.has_value()) {
        response.insert(
            QStringLiteral(
                "pairingSecret"),
            QString::fromLatin1(
                record->secret.toBase64()));
    }
    if (relayUrl.has_value()) {
        response.insert(
            QStringLiteral(
                "relayURLString"),
            relayUrl->toString(
                QUrl::FullyEncoded));
    }
    if (error.has_value()) {
        response.insert(
            QStringLiteral("errorCode"),
            QStringLiteral(
                "pairing_rejected"));
        response.insert(
            QStringLiteral("message"),
            error->message);
    }
    return QJsonDocument(response)
        .toJson(QJsonDocument::Compact);
}

class RelayConnectionBootstrapEndpoint final
    : public RelayPairingBootstrapEndpoint {
public:
    RelayConnectionBootstrapEndpoint(
        QUrl url,
        QString channelId,
        QString endpointId)
        : connection_(
              std::make_unique<
                  RelayConnection>(
                  std::move(url),
                  std::move(channelId),
                  std::move(endpointId),
                  RelaySenderMode::
                      BootstrapUnknown))
    {
        QObject::connect(
            connection_.get(),
            &RelayConnection::envelopeReceived,
            this,
            &RelayPairingBootstrapEndpoint::
                envelopeReceived);
        QObject::connect(
            connection_.get(),
            &RelayConnection::failureOccurred,
            this,
            &RelayPairingBootstrapEndpoint::
                failureOccurred);
    }

    void start() override
    {
        connection_->start();
    }

    void stop() override
    {
        connection_->stop();
    }

    QFuture<Result<void>> send(
        const EncryptedEnvelope& envelope)
        override
    {
        return connection_->send(envelope);
    }

private:
    std::unique_ptr<RelayConnection>
        connection_;
};

} // namespace

struct RelayPairingBootstrap::
ActiveState final {
    RelayPairingBootstrapOffer offer;
    QString channelId;
    std::unique_ptr<
        RelayPairingBootstrapEndpoint>
        endpoint;
    QPointer<QTimer> expiryTimer;
    bool terminated = false;
};

Result<QString>
RelayPairingBootstrapOffer::pairingLink()
    const
{
    const auto valid =
        validateOffer(*this);
    if (!valid.hasValue()) {
        return Result<QString>::failure(
            valid.error());
    }
    const auto relay =
        validatedRelayUrl(relayUrl);
    if (!relay.hasValue()) {
        return Result<QString>::failure(
            relay.error());
    }
    const auto code =
        validatedPairingCode(
            pairingCode);
    if (!code.hasValue()) {
        return Result<QString>::failure(
            code.error());
    }

    QByteArray json;
    json.reserve(512);
    json.append("{\"version\":");
    json.append(QByteArray::number(version));
    json.append(",\"relayURLString\":");
    json.append(
        jsonString(
            relay.value().toString(
                QUrl::FullyEncoded)));
    json.append(",\"macDeviceID\":");
    json.append(
        jsonString(
            hostDeviceId.trimmed()));
    json.append(",\"macName\":");
    json.append(
        jsonString(
            hostDisplayName.trimmed()));
    json.append(",\"pairingCode\":");
    json.append(
        jsonString(code.value()));
    json.append(
        ",\"expiresAtMilliseconds\":");
    json.append(
        QByteArray::number(
            expiresAtMilliseconds));
    json.append(",\"bootstrapSecret\":");
    json.append(
        jsonString(
            QString::fromLatin1(
                base64Url(
                    bootstrapSecret))));
    json.append('}');

    return Result<QString>::success(
        QStringLiteral(
            "codex-companion://pair?payload=")
        + QString::fromLatin1(
            base64Url(json)));
}

Result<RelayPairingBootstrapOffer>
RelayPairingBootstrapOffer::parse(
    QStringView link)
{
    const QString trimmed =
        link.trimmed().toString();
    const QUrl url(
        trimmed,
        QUrl::StrictMode);
    if (trimmed.isEmpty()
        || !url.isValid()
        || url.scheme().compare(
               QStringLiteral(
                   "codex-companion"),
               Qt::CaseInsensitive)
            != 0
        || url.host().compare(
               QStringLiteral("pair"),
               Qt::CaseInsensitive)
            != 0
        || (!url.path().isEmpty()
            && url.path()
                != QStringLiteral("/"))
        || url.hasFragment()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_link_invalid"),
                    QStringLiteral(
                        "The Companion pairing link is invalid.")));
    }

    const QUrlQuery query(url);
    const auto items =
        query.queryItems(
            QUrl::FullyDecoded);
    if (items.size() != 1
        || items.constFirst().first
            != QStringLiteral("payload")) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_link_invalid"),
                    QStringLiteral(
                        "The Companion pairing link has no valid payload.")));
    }
    const auto payload =
        decodeBase64Url(
            items.constFirst().second);
    if (!payload.hasValue()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(payload.error());
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            payload.value(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_link_invalid"),
                    QStringLiteral(
                        "The Companion pairing payload is invalid.")));
    }

    const QJsonObject object =
        document.object();
    const QSet<QString> fields{
        QStringLiteral("version"),
        QStringLiteral(
            "relayURLString"),
        QStringLiteral("macDeviceID"),
        QStringLiteral("macName"),
        QStringLiteral("pairingCode"),
        QStringLiteral(
            "expiresAtMilliseconds"),
        QStringLiteral(
            "bootstrapSecret"),
    };
    if (object.size() != fields.size()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_link_invalid"),
                    QStringLiteral(
                        "The Companion pairing payload is unsupported.")));
    }
    for (auto iterator =
             object.constBegin();
         iterator != object.constEnd();
         ++iterator) {
        if (!fields.contains(
                iterator.key())) {
            return Result<
                RelayPairingBootstrapOffer>::
                failure(
                    bootstrapError(
                        QStringLiteral(
                            "relay.pairing_link_invalid"),
                        QStringLiteral(
                            "The Companion pairing payload is unsupported.")));
        }
    }

    const auto version =
        exactInteger(
            object.value(
                QStringLiteral("version")));
    const auto expiresAt =
        exactInteger(
            object.value(
                QStringLiteral(
                    "expiresAtMilliseconds")));
    const QJsonValue relayValue =
        object.value(
            QStringLiteral(
                "relayURLString"));
    const QJsonValue hostIdValue =
        object.value(
            QStringLiteral("macDeviceID"));
    const QJsonValue hostNameValue =
        object.value(
            QStringLiteral("macName"));
    const QJsonValue codeValue =
        object.value(
            QStringLiteral("pairingCode"));
    const QJsonValue secretValue =
        object.value(
            QStringLiteral(
                "bootstrapSecret"));
    if (!version.has_value()
        || *version
            < std::numeric_limits<int>::min()
        || *version
            > std::numeric_limits<int>::max()
        || !expiresAt.has_value()
        || !relayValue.isString()
        || !hostIdValue.isString()
        || !hostNameValue.isString()
        || !codeValue.isString()
        || !secretValue.isString()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_link_invalid"),
                    QStringLiteral(
                        "The Companion pairing payload is invalid.")));
    }

    const auto secret =
        decodeBase64Url(
            secretValue.toString());
    if (!secret.hasValue()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(secret.error());
    }
    RelayPairingBootstrapOffer offer{
        static_cast<int>(*version),
        QUrl(
            relayValue.toString(),
            QUrl::StrictMode),
        hostIdValue.toString(),
        hostNameValue.toString(),
        codeValue.toString(),
        *expiresAt,
        secret.value(),
    };
    const auto valid =
        validateOffer(offer);
    if (!valid.hasValue()) {
        WindowsCrypto::secureZero(
            offer.bootstrapSecret);
        return Result<
            RelayPairingBootstrapOffer>::
            failure(valid.error());
    }
    const auto normalized =
        validatedPairingCode(
            offer.pairingCode);
    offer.pairingCode =
        normalized.value();
    const auto relay =
        validatedRelayUrl(
            offer.relayUrl);
    offer.relayUrl =
        relay.value();
    offer.hostDeviceId =
        offer.hostDeviceId.trimmed();
    offer.hostDisplayName =
        offer.hostDisplayName.trimmed();
    return Result<
        RelayPairingBootstrapOffer>::
        success(std::move(offer));
}

RelayPairingBootstrap::
RelayPairingBootstrap(
    PairingCoordinator& pairingCoordinator,
    RelayStateStore& relayStateStore,
    QString hostDeviceId,
    QString hostDisplayName,
    RelayPairingBootstrapDependencies
        dependencies,
    QObject* parent)
    : QObject(parent),
      pairingCoordinator_(
          &pairingCoordinator),
      relayStateStore_(
          &relayStateStore),
      hostDeviceId_(
          std::move(hostDeviceId)),
      hostDisplayName_(
          std::move(hostDisplayName)),
      dependencies_(
          std::move(dependencies))
{
    if (!dependencies_.clock) {
        dependencies_.clock = [] {
            return QDateTime::
                currentDateTimeUtc();
        };
    }
    if (!dependencies_.secretGenerator) {
        dependencies_.secretGenerator =
            [] {
                return BridgeSecurity::
                    randomSecret();
            };
    }
    if (!dependencies_.secretEraser) {
        dependencies_.secretEraser =
            [](QByteArray& secret) {
                WindowsCrypto::secureZero(
                    secret);
            };
    }
    if (!dependencies_.endpointFactory) {
        dependencies_.endpointFactory =
            [](
                QUrl url,
                QString channelId,
                QString endpointId) {
                return std::make_unique<
                    RelayConnectionBootstrapEndpoint>(
                    std::move(url),
                    std::move(channelId),
                    std::move(endpointId));
            };
    }
}

RelayPairingBootstrap::
~RelayPairingBootstrap()
{
    if (active_) {
        const auto state = active_;
        active_.reset();
        terminate(state, true);
    }
}

void RelayPairingBootstrap::setRelayUrl(
    std::optional<QUrl> relayUrl)
{
    std::optional<QUrl> normalized;
    if (relayUrl.has_value()) {
        const auto validated =
            validatedRelayUrl(*relayUrl);
        if (validated.hasValue()) {
            normalized =
                validated.value();
        }
    }
    if (relayUrl_ == normalized) {
        return;
    }
    if (active_) {
        cancelPairing();
    }
    relayUrl_ = std::move(normalized);
}

std::optional<QUrl>
RelayPairingBootstrap::relayUrl() const
{
    return relayUrl_;
}

Result<RelayPairingBootstrapOffer>
RelayPairingBootstrap::beginPairing(
    std::chrono::seconds validFor)
{
    if (!relayUrl_.has_value()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_relay_unavailable"),
                    QStringLiteral(
                        "Save a secure relay URL before pairing this Windows PC.")));
    }
    if (validFor
            <= std::chrono::seconds::zero()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_duration_invalid"),
                    QStringLiteral(
                        "The secure pairing duration must be positive.")));
    }
    if (!RelayModels::isValidOpaqueId(
            hostDeviceId_)
        || hostDisplayName_
               .trimmed()
               .isEmpty()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_host_invalid"),
                    QStringLiteral(
                        "The Companion installation identity is invalid.")));
    }

    if (active_) {
        const auto previous = active_;
        active_.reset();
        terminate(previous, true);
    } else {
        pairingCoordinator_
            ->cancelPairing();
    }
    lastError_.reset();

    const auto pairing =
        pairingCoordinator_
            ->beginPairing(validFor);
    if (!pairing.hasValue()) {
        return Result<
            RelayPairingBootstrapOffer>::
            failure(pairing.error());
    }
    auto secret =
        dependencies_.secretGenerator();
    if (!secret.hasValue()) {
        pairingCoordinator_
            ->cancelPairing();
        return Result<
            RelayPairingBootstrapOffer>::
            failure(secret.error());
    }
    if (secret.value().size() != 32) {
        dependencies_.secretEraser(
            secret.value());
        pairingCoordinator_
            ->cancelPairing();
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_secret_invalid"),
                    QStringLiteral(
                        "The relay bootstrap secret generator did not return exactly 32 bytes.")));
    }

    const QDateTime now =
        dependencies_.clock();
    if (!now.isValid()) {
        dependencies_.secretEraser(
            secret.value());
        pairingCoordinator_
            ->cancelPairing();
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_time_invalid"),
                    QStringLiteral(
                        "The secure pairing clock returned an invalid time.")));
    }

    auto state =
        std::make_shared<ActiveState>();
    state->offer = {
        RelayPairingBootstrapOffer::
            currentVersion,
        *relayUrl_,
        hostDeviceId_.trimmed(),
        hostDisplayName_.trimmed(),
        pairing.value().code,
        now.addSecs(validFor.count())
            .toMSecsSinceEpoch(),
        std::move(secret.value()),
    };
    const auto channel =
        BridgeSecurity::channelId(
            state->offer
                .bootstrapSecret);
    if (!channel.hasValue()) {
        dependencies_.secretEraser(
            state->offer
                .bootstrapSecret);
        pairingCoordinator_
            ->cancelPairing();
        return Result<
            RelayPairingBootstrapOffer>::
            failure(channel.error());
    }
    state->channelId =
        channel.value();
    state->endpoint =
        dependencies_.endpointFactory(
            *relayUrl_,
            state->channelId,
            hostDeviceId_);
    if (!state->endpoint) {
        dependencies_.secretEraser(
            state->offer
                .bootstrapSecret);
        pairingCoordinator_
            ->cancelPairing();
        return Result<
            RelayPairingBootstrapOffer>::
            failure(
                bootstrapError(
                    QStringLiteral(
                        "relay.pairing_endpoint_unavailable"),
                    QStringLiteral(
                        "The secure mobile pairing endpoint is unavailable.")));
    }

    std::weak_ptr<ActiveState> weakState =
        state;
    QObject::connect(
        state->endpoint.get(),
        &RelayPairingBootstrapEndpoint::
            envelopeReceived,
        this,
        [this, weakState](
            EncryptedEnvelope envelope) {
            if (const auto locked =
                    weakState.lock()) {
                processEnvelope(
                    locked,
                    envelope);
            }
        });
    QObject::connect(
        state->endpoint.get(),
        &RelayPairingBootstrapEndpoint::
            failureOccurred,
        this,
        [this, weakState](
            CompanionError error) {
            if (const auto locked =
                    weakState.lock()) {
                recordFailure(
                    locked,
                    std::move(error));
            }
        });

    QTimer* expiryTimer =
        new QTimer(this);
    expiryTimer->setSingleShot(true);
    state->expiryTimer = expiryTimer;
    QObject::connect(
        expiryTimer,
        &QTimer::timeout,
        this,
        [this, weakState] {
            if (const auto locked =
                    weakState.lock()) {
                expire(locked);
            }
        });

    active_ = state;
    state->endpoint->start();
    const auto milliseconds =
        std::chrono::duration_cast<
            std::chrono::milliseconds>(
            validFor);
    expiryTimer->start(
        static_cast<int>(
            std::min<qint64>(
                milliseconds.count(),
                std::numeric_limits<int>::
                    max())));
    emit stateChanged();
    return Result<
        RelayPairingBootstrapOffer>::
        success(state->offer);
}

void RelayPairingBootstrap::cancelPairing()
{
    const auto state = active_;
    active_.reset();
    lastError_.reset();
    if (state) {
        terminate(state, true);
    } else {
        pairingCoordinator_
            ->cancelPairing();
    }
    emit stateChanged();
}

std::optional<
    RelayPairingBootstrapOffer>
RelayPairingBootstrap::activeOffer() const
{
    if (!active_
        || active_->terminated) {
        return std::nullopt;
    }
    return active_->offer;
}

std::optional<CompanionError>
RelayPairingBootstrap::lastError() const
{
    return lastError_;
}

void RelayPairingBootstrap::
processEnvelope(
    const std::shared_ptr<
        ActiveState>& state,
    const EncryptedEnvelope& envelope)
{
    if (!state
        || state != active_
        || state->terminated) {
        return;
    }
    const QDateTime now =
        dependencies_.clock();
    if (!now.isValid()
        || now.toMSecsSinceEpoch()
            > state->offer
                  .expiresAtMilliseconds) {
        expire(state);
        return;
    }

    auto plaintext =
        BridgeSecurity::open(
            envelope,
            state->offer
                .bootstrapSecret);
    if (!plaintext.hasValue()) {
        return;
    }
    const auto request =
        decodeRequest(
            plaintext.value());
    WindowsCrypto::secureZero(
        plaintext.value());
    if (!request.hasValue()) {
        return;
    }

    const auto replayAccepted =
        relayStateStore_->acceptInbound(
            state->channelId,
            envelope.senderId,
            envelope.sequence);
    if (!replayAccepted.hasValue()) {
        recordFailure(
            state,
            replayAccepted.error());
        return;
    }
    if (!replayAccepted.value()
        || request.value().version
            != RelayPairingBootstrapOffer::
                   currentVersion
        || envelope.senderId
            != request.value()
                   .invitation.deviceId) {
        return;
    }

    std::optional<PairingRecord> record;
    std::optional<CompanionError>
        pairingFailure;
    auto completed =
        pairingCoordinator_
            ->completePairing(
                request.value()
                    .invitation);
    if (!completed.hasValue()) {
        pairingFailure =
            completed.error();
    } else {
        completed.value().relayUrlString =
            state->offer.relayUrl
                .toString(
                    QUrl::FullyEncoded);
        const auto remembered =
            pairingCoordinator_->remember(
                completed.value());
        if (!remembered.hasValue()) {
            pairingFailure =
                remembered.error();
        } else {
            record =
                completed.value();
        }
    }

    QByteArray response =
        encodeResponse(
            request.value(),
            record.has_value(),
            hostDisplayName_,
            hostDeviceId_,
            record,
            record.has_value()
                ? std::optional<QUrl>(
                      state->offer.relayUrl)
                : std::nullopt,
            pairingFailure);
    if (record.has_value()) {
        WindowsCrypto::secureZero(
            record->secret);
    }
    const auto sequence =
        relayStateStore_->nextOutbound(
            state->channelId,
            hostDeviceId_);
    if (!sequence.hasValue()) {
        WindowsCrypto::secureZero(
            response);
        recordFailure(
            state,
            sequence.error());
        return;
    }
    const auto encrypted =
        BridgeSecurity::seal(
            response,
            state->offer
                .bootstrapSecret,
            hostDeviceId_,
            sequence.value(),
            now.toMSecsSinceEpoch());
    WindowsCrypto::secureZero(response);
    if (!encrypted.hasValue()) {
        recordFailure(
            state,
            encrypted.error());
        return;
    }

    QFuture<Result<void>> future =
        state->endpoint->send(
            encrypted.value());
    auto* watcher =
        new QFutureWatcher<Result<void>>(
            this);
    const bool pairingSucceeded =
        record.has_value();
    QObject::connect(
        watcher,
        &QFutureWatcher<Result<void>>::
            finished,
        this,
        [this,
         state,
         watcher,
         pairingSucceeded] {
            const Result<void> result =
                watcher->result();
            watcher->deleteLater();
            finishSend(
                state,
                pairingSucceeded,
                result);
        });
    watcher->setFuture(future);
}

void RelayPairingBootstrap::finishSend(
    const std::shared_ptr<
        ActiveState>& state,
    bool pairingSucceeded,
    Result<void> result)
{
    if (!state
        || state != active_
        || state->terminated) {
        return;
    }
    if (!result.hasValue()) {
        recordFailure(
            state,
            result.error());
        return;
    }
    if (pairingSucceeded) {
        complete(state);
    }
}

void RelayPairingBootstrap::complete(
    const std::shared_ptr<
        ActiveState>& state)
{
    if (!state
        || state != active_
        || state->terminated) {
        return;
    }
    active_.reset();
    lastError_.reset();
    terminate(state, false);
    emit stateChanged();
}

void RelayPairingBootstrap::expire(
    const std::shared_ptr<
        ActiveState>& state)
{
    if (!state
        || state != active_
        || state->terminated) {
        return;
    }
    active_.reset();
    lastError_.reset();
    terminate(state, true);
    emit stateChanged();
}

void RelayPairingBootstrap::
recordFailure(
    const std::shared_ptr<
        ActiveState>& state,
    CompanionError error)
{
    if (!state
        || state != active_
        || state->terminated) {
        return;
    }
    lastError_ =
        bootstrapError(
            QStringLiteral(
                "relay.secure_pairing_failed"),
            QStringLiteral(
                "Secure mobile pairing failed: %1")
                .arg(error.message),
            error.retryable);
    emit stateChanged();
}

void RelayPairingBootstrap::terminate(
    const std::shared_ptr<
        ActiveState>& state,
    bool cancelPairing)
{
    if (!state
        || state->terminated) {
        return;
    }
    state->terminated = true;
    if (state->expiryTimer) {
        state->expiryTimer->stop();
        state->expiryTimer->deleteLater();
        state->expiryTimer = nullptr;
    }
    if (state->endpoint) {
        state->endpoint->stop();
        state->endpoint.reset();
    }
    relayStateStore_->eraseChannel(
        state->channelId);
    dependencies_.secretEraser(
        state->offer.bootstrapSecret);
    if (cancelPairing) {
        pairingCoordinator_
            ->cancelPairing();
    }
}

} // namespace companion
