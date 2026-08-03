#include "update/UpdateFeedClient.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPromise>
#include <QSslError>
#include <QVariantMap>

namespace companion {
namespace {

using ManifestResult =
    Result<UpdateManifest>;

CompanionError feedError(
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

ManifestResult feedFailure(
    QString code,
    QString message,
    const QUrl& url,
    bool retryable = false,
    QVariantMap context = {})
{
    return ManifestResult::failure(
        feedError(
            std::move(code),
            std::move(message),
            url,
            retryable,
            std::move(context)));
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

QFuture<ManifestResult> readyFuture(
    ManifestResult result)
{
    QPromise<ManifestResult> promise;
    promise.start();
    QFuture<ManifestResult> future =
        promise.future();
    promise.addResult(std::move(result));
    promise.finish();
    return future;
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

} // namespace

struct UpdateFeedClient::ActiveRequest final {
    std::shared_ptr<
        QPromise<ManifestResult>>
        promise;
    QFuture<ManifestResult> future;
    QPointer<QNetworkReply> reply;
    QUrl currentUrl;
    QByteArray body;
    int redirectCount = 0;
    bool completed = false;
};

UpdateFeedClient::UpdateFeedClient(
    UpdateFeedClientOptions options,
    QObject* parent)
    : QObject(parent),
      ownedNetwork_(
          std::make_unique<
              QNetworkAccessManager>()),
      network_(ownedNetwork_.get()),
      options_(std::move(options)),
      validator_(verifier_)
{
    initialize();
}

UpdateFeedClient::UpdateFeedClient(
    QNetworkAccessManager& network,
    UpdateFeedClientOptions options,
    QObject* parent)
    : QObject(parent),
      network_(&network),
      options_(std::move(options)),
      validator_(verifier_)
{
    initialize();
}

UpdateFeedClient::~UpdateFeedClient()
{
    cancel();
}

void UpdateFeedClient::initialize()
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
                feedFailure(
                    QStringLiteral(
                        "update.manifest_timeout"),
                    QStringLiteral(
                        "The update manifest request timed out."),
                    request->currentUrl,
                    true),
                true);
        });
}

QFuture<ManifestResult>
UpdateFeedClient::loadManifest(
    const QUrl& url)
{
    cancel();
    if (!isSecureHttpsUrl(url)) {
        return readyFuture(
            feedFailure(
                QStringLiteral(
                    "update.insecure_manifest_url"),
                QStringLiteral(
                    "The update manifest must use HTTPS."),
                url));
    }

    auto request =
        std::make_shared<ActiveRequest>();
    request->promise =
        std::make_shared<
            QPromise<ManifestResult>>();
    request->promise->start();
    request->future =
        request->promise->future();
    request->currentUrl = url;
    active_ = request;

    deadlineTimer_.start(
        timerInterval(
            options_.manifestDeadline));
    issueRequest(request, url);
    return request->future;
}

void UpdateFeedClient::cancel()
{
    const auto request = active_;
    if (request == nullptr) {
        return;
    }
    finish(
        request,
        feedFailure(
            QStringLiteral(
                "update.cancelled"),
            QStringLiteral(
                "The update request was cancelled."),
            request->currentUrl),
        true);
}

bool UpdateFeedClient::isLoading() const noexcept
{
    return active_ != nullptr;
}

void UpdateFeedClient::issueRequest(
    const std::shared_ptr<ActiveRequest>& request,
    const QUrl& url)
{
    if (request->completed
        || active_ != request) {
        return;
    }

    request->currentUrl = url;
    request->body.clear();

    QNetworkRequest networkRequest(url);
    networkRequest.setRawHeader(
        QByteArrayLiteral("Accept"),
        QByteArrayLiteral(
            "application/json"));
    networkRequest.setRawHeader(
        QByteArrayLiteral("User-Agent"),
        options_.userAgent);
    networkRequest.setTransferTimeout(
        timerInterval(
            options_.manifestDeadline));
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
        maximumManifestBytes + 1);
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
            const QVariant contentLength =
                request->reply->header(
                    QNetworkRequest::
                        ContentLengthHeader);
            if (contentLength.isValid()
                && contentLength.toLongLong()
                    > maximumManifestBytes) {
                finish(
                    request,
                    feedFailure(
                        QStringLiteral(
                            "update.manifest_too_large"),
                        QStringLiteral(
                            "The update manifest exceeds the 64 KiB limit."),
                        request->currentUrl),
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
                feedFailure(
                    QStringLiteral(
                        "update.tls_error"),
                    QStringLiteral(
                        "The update feed TLS connection could not be verified."),
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

void UpdateFeedClient::readAvailable(
    const std::shared_ptr<ActiveRequest>& request)
{
    if (request->completed
        || request->reply == nullptr) {
        return;
    }

    const QByteArray chunk =
        request->reply->readAll();
    if (chunk.size()
        > maximumManifestBytes
              - request->body.size()) {
        finish(
            request,
            feedFailure(
                QStringLiteral(
                    "update.manifest_too_large"),
                QStringLiteral(
                    "The update manifest exceeds the 64 KiB limit."),
                request->currentUrl),
            true);
        return;
    }
    request->body.append(chunk);
}

void UpdateFeedClient::handleFinished(
    const std::shared_ptr<ActiveRequest>& request)
{
    if (request->completed
        || request->reply == nullptr) {
        return;
    }

    readAvailable(request);
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
        if (request->redirectCount
            >= maximumRedirects) {
            finish(
                request,
                feedFailure(
                    QStringLiteral(
                        "update.too_many_redirects"),
                    QStringLiteral(
                        "The update manifest redirected too many times."),
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
                feedFailure(
                    QStringLiteral(
                        "update.insecure_redirect"),
                    QStringLiteral(
                        "The update manifest redirected to an insecure URL."),
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
        && (*status < 200
            || *status >= 300)) {
        finish(
            request,
            feedFailure(
                QStringLiteral(
                    "update.http_status"),
                QStringLiteral(
                    "The update feed returned an unsuccessful HTTP status."),
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
            feedFailure(
                QStringLiteral(
                    "update.network_error"),
                reply->errorString()
                        .trimmed()
                        .isEmpty()
                    ? QStringLiteral(
                          "The update feed request failed.")
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

    ManifestResult validated =
        validator_.validate(
            request->body,
            options_.publicKeyBase64);
    finish(
        request,
        std::move(validated),
        false);
}

void UpdateFeedClient::finish(
    const std::shared_ptr<ActiveRequest>& request,
    ManifestResult result,
    bool abortReply)
{
    if (request->completed) {
        return;
    }
    request->completed = true;

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
