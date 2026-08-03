#pragma once

#include "core/Result.h"
#include "update/UpdateArtifactStore.h"
#include "update/UpdateManifest.h"

#include <QByteArray>
#include <QFuture>
#include <QObject>
#include <QSslConfiguration>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace companion {

using UpdateArtifactProgress =
    std::function<void(
        qint64 received,
        qint64 total)>;

struct UpdateArtifactDownloaderOptions final {
    QByteArray userAgent;
    std::optional<QSslConfiguration>
        sslConfiguration;
    std::chrono::milliseconds
        artifactDeadline{
            std::chrono::minutes(10),
        };
};

class UpdateArtifactDownloader final
    : public QObject {
public:
    static constexpr int maximumRedirects =
        5;

    explicit UpdateArtifactDownloader(
        UpdateArtifactDownloaderOptions
            options,
        QObject* parent = nullptr);
    UpdateArtifactDownloader(
        QNetworkAccessManager& network,
        UpdateArtifactDownloaderOptions
            options,
        QObject* parent = nullptr);
    ~UpdateArtifactDownloader() override;

    UpdateArtifactDownloader(
        const UpdateArtifactDownloader&) =
        delete;
    UpdateArtifactDownloader& operator=(
        const UpdateArtifactDownloader&) =
        delete;

    QFuture<Result<VerifiedArtifact>>
    download(
        const UpdateManifest& manifest,
        UpdateArtifactStore& store,
        UpdateArtifactProgress progress = {});

    void cancel();
    bool isDownloading() const noexcept;

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
        Result<VerifiedArtifact> result,
        bool abortReply);

    std::unique_ptr<
        QNetworkAccessManager>
        ownedNetwork_;
    QNetworkAccessManager* network_ =
        nullptr;
    UpdateArtifactDownloaderOptions
        options_;
    QTimer deadlineTimer_;
    std::shared_ptr<ActiveRequest>
        active_;
};

} // namespace companion
