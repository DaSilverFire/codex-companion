#include "update/UpdateArtifactDownloader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPromise>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <utility>

namespace companion {
namespace {

using ArtifactResult =
    Result<VerifiedArtifact>;

CompanionError downloadError(
    QString code,
    QString message,
    const QUrl& url,
    bool retryable = false,
    QVariantMap context = {})
{
    context.insert(
        QStringLiteral("url"),
        url.toString());
    return {
        std::move(code),
        std::move(message),
        retryable,
        std::move(context),
    };
}

ArtifactResult downloadFailure(
    QString code,
    QString message,
    const QUrl& url,
    bool retryable = false,
    QVariantMap context = {})
{
    return ArtifactResult::failure(
        downloadError(
            std::move(code),
            std::move(message),
            url,
            retryable,
            std::move(context)));
}

QFuture<ArtifactResult> readyFuture(
    ArtifactResult result)
{
    QPromise<ArtifactResult> promise;
    promise.start();
    QFuture<ArtifactResult> future =
        promise.future();
    promise.addResult(std::move(result));
    promise.finish();
    return future;
}

bool isSecureHttpsUrl(const QUrl& url)
{
    return url.isValid()
        && !url.isRelative()
        && url.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive) == 0
        && !url.host().isEmpty()
        && url.userInfo().isEmpty();
}

int timerInterval(
    std::chrono::milliseconds duration)
{
    return static_cast<int>(
        std::clamp<qint64>(
            duration.count(),
            1,
            std::numeric_limits<int>::max()));
}

bool isSuccessfulStatus(int status)
{
    return status >= 200
        && status < 300;
}

} // namespace

struct UpdateArtifactDownloader::
    ActiveRequest final {
    std::shared_ptr<
        QPromise<ArtifactResult>>
        promise;
    QFuture<ArtifactResult> future;
    QPointer<QNetworkReply> reply;
    QUrl currentUrl;
    UpdateManifest manifest;
    std::unique_ptr<
        UpdateArtifactStagingSession>
        session;
    UpdateArtifactProgress progress;
    qint64 bytesReceived = 0;
    int redirectCount = 0;
    bool completed = false;
};

UpdateArtifactDownloader::
UpdateArtifactDownloader(
    UpdateArtifactDownloaderOptions options,
    QObject* parent)
    : QObject(parent),
      ownedNetwork_(
          std::make_unique<
              QNetworkAccessManager>()),
      network_(ownedNetwork_.get()),
      options_(std::move(options))
{
    initialize();
}

UpdateArtifactDownloader::
UpdateArtifactDownloader(
    QNetworkAccessManager& network,
    UpdateArtifactDownloaderOptions options,
    QObject* parent)
    : QObject(parent),
      network_(&network),
      options_(std::move(options))
{
    initialize();
}

UpdateArtifactDownloader::
~UpdateArtifactDownloader()
{
    cancel();
}

void UpdateArtifactDownloader::initialize()
{
    deadlineTimer_.setParent(this);
    deadlineTimer_.setSingleShot(true);
    QObject::connect(
        &deadlineTimer_,
        &QTimer::timeout,
        this,
        [this] {
            const auto request = active_;
            if (request == nullptr) {
                return;
            }
            finish(
                request,
                downloadFailure(
                    QStringLiteral(
                        "update.artifact_timeout"),
                    QStringLiteral(
                        "The update installer download timed out."),
                    request->currentUrl,
                    true),
                true);
        });
}

QFuture<ArtifactResult>
UpdateArtifactDownloader::download(
    const UpdateManifest& manifest,
    UpdateArtifactStore& store,
    UpdateArtifactProgress progress)
{
    cancel();

    const QUrl url(manifest.downloadUrl);
    if (!isSecureHttpsUrl(url)) {
        return readyFuture(
            downloadFailure(
                QStringLiteral(
                    "update.insecure_artifact_url"),
                QStringLiteral(
                    "The update installer must use HTTPS."),
                url));
    }

    auto staging = store.begin(manifest);
    if (!staging.hasValue()) {
        return readyFuture(
            ArtifactResult::failure(
                staging.error()));
    }

    auto request =
        std::make_shared<ActiveRequest>();
    request->promise =
        std::make_shared<
            QPromise<ArtifactResult>>();
    request->promise->start();
    request->future =
        request->promise->future();
    request->currentUrl = url;
    request->manifest = manifest;
    request->session =
        std::move(staging.value());
    request->progress =
        std::move(progress);
    active_ = request;

    deadlineTimer_.start(
        timerInterval(
            options_.artifactDeadline));
    issueRequest(request, url);
    return request->future;
}

void UpdateArtifactDownloader::cancel()
{
    const auto request = active_;
    if (request == nullptr) {
        return;
    }
    finish(
        request,
        downloadFailure(
            QStringLiteral(
                "update.cancelled"),
            QStringLiteral(
                "The update request was cancelled."),
            request->currentUrl),
        true);
}

bool UpdateArtifactDownloader::
isDownloading() const noexcept
{
    return active_ != nullptr;
}

void UpdateArtifactDownloader::issueRequest(
    const std::shared_ptr<
        ActiveRequest>& request,
    const QUrl& url)
{
    if (request->completed
        || active_ != request) {
        return;
    }

    request->currentUrl = url;
    QNetworkRequest networkRequest(url);
    networkRequest.setRawHeader(
        QByteArrayLiteral("Accept"),
        QByteArrayLiteral(
            "application/octet-stream"));
    networkRequest.setRawHeader(
        QByteArrayLiteral(
            "Accept-Encoding"),
        QByteArrayLiteral("identity"));
    networkRequest.setRawHeader(
        QByteArrayLiteral("User-Agent"),
        options_.userAgent);
    networkRequest.setTransferTimeout(
        timerInterval(
            options_.artifactDeadline));
    networkRequest.setAttribute(
        QNetworkRequest::
            RedirectPolicyAttribute,
        QNetworkRequest::
            ManualRedirectPolicy);
    if (options_.sslConfiguration
            .has_value()) {
        networkRequest.setSslConfiguration(
            *options_.sslConfiguration);
    }

    QNetworkReply* const reply =
        network_->get(networkRequest);
    reply->setReadBufferSize(
        256 * 1024);
    request->reply = reply;
    const std::weak_ptr<ActiveRequest>
        weakRequest(request);

    QObject::connect(
        reply,
        &QNetworkReply::metaDataChanged,
        this,
        [this, weakRequest] {
            const auto request =
                weakRequest.lock();
            if (request == nullptr
                || request->completed
                || request->reply == nullptr) {
                return;
            }

            const QVariant statusValue =
                request->reply->attribute(
                    QNetworkRequest::
                        HttpStatusCodeAttribute);
            if (!statusValue.isValid()
                || !isSuccessfulStatus(
                    statusValue.toInt())) {
                return;
            }

            const QByteArray encoding =
                request->reply
                    ->rawHeader(
                        QByteArrayLiteral(
                            "Content-Encoding"))
                    .trimmed();
            if (!encoding.isEmpty()
                && encoding.compare(
                       QByteArrayLiteral(
                           "identity"),
                       Qt::CaseInsensitive)
                    != 0) {
                finish(
                    request,
                    downloadFailure(
                        QStringLiteral(
                            "update.artifact_content_encoding"),
                        QStringLiteral(
                            "The update installer response used an unsupported content encoding."),
                        request->currentUrl),
                    true);
                return;
            }

            const QVariant contentLength =
                request->reply->header(
                    QNetworkRequest::
                        ContentLengthHeader);
            if (contentLength.isValid()
                && contentLength.toLongLong()
                    != request->manifest.size) {
                finish(
                    request,
                    downloadFailure(
                        QStringLiteral(
                            "update.artifact_content_length_mismatch"),
                        QStringLiteral(
                            "The update installer response size does not match the signed manifest."),
                        request->currentUrl,
                        false,
                        {
                            {
                                QStringLiteral(
                                    "expectedSize"),
                                request
                                    ->manifest
                                    .size,
                            },
                            {
                                QStringLiteral(
                                    "contentLength"),
                                contentLength
                                    .toLongLong(),
                            },
                        }),
                    true);
            }
        });
    QObject::connect(
        reply,
        &QIODevice::readyRead,
        this,
        [this, weakRequest] {
            const auto request =
                weakRequest.lock();
            if (request != nullptr) {
                readAvailable(request);
            }
        });
    QObject::connect(
        reply,
        &QNetworkReply::sslErrors,
        this,
        [this, weakRequest](
            const QList<QSslError>& errors) {
            const auto request =
                weakRequest.lock();
            if (request == nullptr
                || request->completed) {
                return;
            }
            finish(
                request,
                downloadFailure(
                    QStringLiteral(
                        "update.artifact_tls_error"),
                    QStringLiteral(
                        "The update installer TLS connection could not be verified."),
                    request->currentUrl,
                    true,
                    {
                        {
                            QStringLiteral(
                                "errorCount"),
                            errors.size(),
                        },
                    }),
                true);
        });
    QObject::connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, weakRequest] {
            const auto request =
                weakRequest.lock();
            if (request != nullptr) {
                handleFinished(request);
            }
        });
}

void UpdateArtifactDownloader::readAvailable(
    const std::shared_ptr<
        ActiveRequest>& request)
{
    if (request->completed
        || request->reply == nullptr) {
        return;
    }

    const QVariant statusValue =
        request->reply->attribute(
            QNetworkRequest::
                HttpStatusCodeAttribute);
    if (!statusValue.isValid()) {
        return;
    }
    if (!isSuccessfulStatus(
            statusValue.toInt())) {
        request->reply->readAll();
        return;
    }

    while (request->reply->bytesAvailable()
           > 0) {
        const QByteArray chunk =
            request->reply->read(
                64 * 1024);
        if (chunk.isEmpty()) {
            break;
        }

        const auto appended =
            request->session->append(chunk);
        if (!appended.hasValue()) {
            finish(
                request,
                ArtifactResult::failure(
                    appended.error()),
                true);
            return;
        }
        request->bytesReceived +=
            chunk.size();
        if (request->progress) {
            try {
                request->progress(
                    request->bytesReceived,
                    request->manifest.size);
            } catch (...) {
            }
        }
    }
}

void UpdateArtifactDownloader::handleFinished(
    const std::shared_ptr<
        ActiveRequest>& request)
{
    if (request->completed
        || request->reply == nullptr) {
        return;
    }

    QNetworkReply* const reply =
        request->reply;
    const QVariant statusValue =
        reply->attribute(
            QNetworkRequest::
                HttpStatusCodeAttribute);
    const std::optional<int> status =
        statusValue.isValid()
        ? std::optional<int>(
              statusValue.toInt())
        : std::nullopt;
    const QVariant redirectValue =
        reply->attribute(
            QNetworkRequest::
                RedirectionTargetAttribute);

    if (status.has_value()
        && *status >= 300
        && *status < 400
        && redirectValue.isValid()) {
        reply->readAll();
        if (request->redirectCount
            >= maximumRedirects) {
            finish(
                request,
                downloadFailure(
                    QStringLiteral(
                        "update.artifact_too_many_redirects"),
                    QStringLiteral(
                        "The update installer redirected too many times."),
                    request->currentUrl),
                false);
            return;
        }

        const QUrl redirected =
            request->currentUrl.resolved(
                redirectValue.toUrl());
        if (!isSecureHttpsUrl(redirected)) {
            finish(
                request,
                downloadFailure(
                    QStringLiteral(
                        "update.artifact_insecure_redirect"),
                    QStringLiteral(
                        "The update installer redirected to an insecure URL."),
                    redirected),
                false);
            return;
        }

        ++request->redirectCount;
        request->reply = nullptr;
        reply->deleteLater();
        issueRequest(
            request,
            redirected);
        return;
    }

    if (status.has_value()
        && !isSuccessfulStatus(
            *status)) {
        reply->readAll();
        finish(
            request,
            downloadFailure(
                QStringLiteral(
                    "update.artifact_http_status"),
                QStringLiteral(
                    "The update installer server returned an unsuccessful HTTP status."),
                request->currentUrl,
                *status >= 500,
                {
                    {
                        QStringLiteral(
                            "status"),
                        *status,
                    },
                }),
            false);
        return;
    }

    if (!status.has_value()
        || reply->error()
            != QNetworkReply::NoError) {
        finish(
            request,
            downloadFailure(
                QStringLiteral(
                    "update.artifact_network_error"),
                reply->errorString()
                        .trimmed()
                        .isEmpty()
                    ? QStringLiteral(
                          "The update installer download failed.")
                    : reply->errorString(),
                request->currentUrl,
                reply->error()
                    != QNetworkReply::
                        OperationCanceledError,
                {
                    {
                        QStringLiteral(
                            "networkError"),
                        static_cast<int>(
                            reply->error()),
                    },
                }),
            false);
        return;
    }

    readAvailable(request);
    if (request->completed) {
        return;
    }

    ArtifactResult verified =
        request->session->finish();
    finish(
        request,
        std::move(verified),
        false);
}

void UpdateArtifactDownloader::finish(
    const std::shared_ptr<
        ActiveRequest>& request,
    ArtifactResult result,
    bool abortReply)
{
    if (request->completed) {
        return;
    }
    request->completed = true;

    if (!result.hasValue()
        && request->session) {
        request->session->cancel();
    }

    QPointer<QNetworkReply> reply =
        request->reply;
    request->reply = nullptr;
    if (active_ == request) {
        deadlineTimer_.stop();
        active_.reset();
    }

    if (reply != nullptr) {
        if (abortReply
            && reply->isRunning()) {
            reply->abort();
        }
        reply->deleteLater();
    }

    request->promise->addResult(
        std::move(result));
    request->promise->finish();
}

} // namespace companion
