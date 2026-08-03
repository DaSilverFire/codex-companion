#pragma once

#include "core/Result.h"
#include "update/MonocypherEd25519Verifier.h"
#include "update/UpdateManifest.h"
#include "update/UpdateManifestValidator.h"

#include <QByteArray>
#include <QFuture>
#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace companion {

struct UpdateFeedClientOptions final {
    QString publicKeyBase64;
    QByteArray userAgent;
    std::optional<QSslConfiguration>
        sslConfiguration;
    std::chrono::milliseconds
        manifestDeadline{
            std::chrono::seconds(20),
        };
};

class UpdateFeedClient final
    : public QObject {
public:
    static constexpr qsizetype
        maximumManifestBytes =
            64 * 1024;
    static constexpr int maximumRedirects =
        5;

    explicit UpdateFeedClient(
        UpdateFeedClientOptions options,
        QObject* parent = nullptr);
    UpdateFeedClient(
        QNetworkAccessManager& network,
        UpdateFeedClientOptions options,
        QObject* parent = nullptr);
    ~UpdateFeedClient() override;

    UpdateFeedClient(
        const UpdateFeedClient&) = delete;
    UpdateFeedClient& operator=(
        const UpdateFeedClient&) = delete;

    QFuture<Result<UpdateManifest>>
    loadManifest(const QUrl& url);

    void cancel();
    bool isLoading() const noexcept;

private:
    struct ActiveRequest;

    void initialize();
    void issueRequest(
        const std::shared_ptr<
            ActiveRequest>& request,
        const QUrl& url);
    void readAvailable(
        const std::shared_ptr<
            ActiveRequest>& request);
    void handleFinished(
        const std::shared_ptr<
            ActiveRequest>& request);
    void finish(
        const std::shared_ptr<
            ActiveRequest>& request,
        Result<UpdateManifest> result,
        bool abortReply);

    std::unique_ptr<
        QNetworkAccessManager>
        ownedNetwork_;
    QNetworkAccessManager* network_ =
        nullptr;
    UpdateFeedClientOptions options_;
    MonocypherEd25519Verifier verifier_;
    UpdateManifestValidator validator_;
    QTimer deadlineTimer_;
    std::shared_ptr<ActiveRequest>
        active_;
};

} // namespace companion
