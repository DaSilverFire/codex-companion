#include "codex/chat/PortableToolHttpTransport.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace companion {
namespace {

CompanionError transportError(
    QString code,
    QString message,
    bool retryable = false,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        std::move(context),
    };
}

class QtPortableToolHttpTransport final
    : public PortableToolHttpTransport {
public:
    Result<PortableToolHttpResponse> get(
        const PortableToolHttpRequest& request,
        std::stop_token stopToken)
        override
    {
        if (stopToken.stop_requested()) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.cancelled"),
                    QStringLiteral(
                        "The portable chat tool request was cancelled.")));
        }
        if (!request.endpoint.isValid()
            || request.endpoint.scheme().compare(
                   QStringLiteral("https"),
                   Qt::CaseInsensitive)
                != 0) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.https_required"),
                    QStringLiteral(
                        "Portable chat tools require an HTTPS endpoint."),
                    false,
                    {
                        {
                            QStringLiteral("endpoint"),
                            request.endpoint.toString(),
                        },
                    }));
        }

        const int timeoutMilliseconds =
            std::clamp(
                request.timeoutMilliseconds,
                1,
                60000);
        const int maximumResponseBytes =
            std::clamp(
                request.maximumResponseBytes,
                1,
                4 * 1024 * 1024);
        QNetworkRequest networkRequest(
            request.endpoint);
        for (auto header =
                 request.headers.constBegin();
             header
             != request.headers.constEnd();
             ++header) {
            networkRequest.setRawHeader(
                header.key(),
                header.value());
        }
        networkRequest.setTransferTimeout(
            timeoutMilliseconds);
        networkRequest.setAttribute(
            QNetworkRequest::
                RedirectPolicyAttribute,
            QNetworkRequest::
                ManualRedirectPolicy);

        QNetworkAccessManager manager;
        QEventLoop eventLoop;
        QNetworkReply* reply =
            manager.get(networkRequest);
        reply->setReadBufferSize(
            static_cast<qint64>(
                maximumResponseBytes)
            + 1);
        QByteArray body;
        body.reserve(
            std::min(
                maximumResponseBytes,
                64 * 1024));
        bool cancelled = false;
        bool timedOut = false;
        bool responseTooLarge = false;
        const auto abortRequest =
            [&eventLoop, reply] {
                if (reply->isRunning()) {
                    reply->abort();
                }
                eventLoop.quit();
            };
        const auto readAvailable =
            [&body,
             &responseTooLarge,
             maximumResponseBytes,
             reply,
             &abortRequest] {
                if (responseTooLarge) {
                    return;
                }
                const qint64 remaining =
                    static_cast<qint64>(
                        maximumResponseBytes)
                    - body.size();
                const QByteArray chunk =
                    reply->read(
                        std::max<qint64>(
                            0,
                            remaining)
                        + 1);
                if (chunk.size() > remaining) {
                    if (remaining > 0) {
                        body.append(
                            chunk.constData(),
                            static_cast<qsizetype>(
                                remaining));
                    }
                    responseTooLarge = true;
                    abortRequest();
                    return;
                }
                body.append(chunk);
            };
        QObject::connect(
            reply,
            &QNetworkReply::metaDataChanged,
            &eventLoop,
            [reply,
             maximumResponseBytes,
             &responseTooLarge,
             &abortRequest] {
                const QVariant contentLength =
                    reply->header(
                        QNetworkRequest::
                            ContentLengthHeader);
                if (contentLength.isValid()
                    && contentLength.toLongLong()
                        > maximumResponseBytes) {
                    responseTooLarge = true;
                    abortRequest();
                }
            });
        QObject::connect(
            reply,
            &QIODevice::readyRead,
            &eventLoop,
            readAvailable);
        QObject::connect(
            reply,
            &QNetworkReply::finished,
            &eventLoop,
            [&eventLoop, &readAvailable] {
                readAvailable();
                eventLoop.quit();
            });
        QTimer deadlineTimer;
        deadlineTimer.setSingleShot(true);
        QObject::connect(
            &deadlineTimer,
            &QTimer::timeout,
            &eventLoop,
            [&timedOut, &abortRequest] {
                timedOut = true;
                abortRequest();
            });
        deadlineTimer.start(
            timeoutMilliseconds);

        QTimer cancellationTimer;
        if (stopToken.stop_possible()) {
            cancellationTimer.setInterval(20);
            cancellationTimer.setTimerType(
                Qt::PreciseTimer);
            QObject::connect(
                &cancellationTimer,
                &QTimer::timeout,
                &eventLoop,
                [&cancelled,
                 &abortRequest,
                 stopToken] {
                    if (stopToken
                            .stop_requested()) {
                        cancelled = true;
                        abortRequest();
                    }
                });
            cancellationTimer.start();
        }
        eventLoop.exec();
        deadlineTimer.stop();
        cancellationTimer.stop();

        const QVariant statusValue =
            reply->attribute(
                QNetworkRequest::
                    HttpStatusCodeAttribute);
        const auto networkError =
            reply->error();
        const QString networkErrorText =
            reply->errorString();
        reply->deleteLater();

        if (cancelled
            || stopToken.stop_requested()) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.cancelled"),
                    QStringLiteral(
                        "The portable chat tool request was cancelled.")));
        }
        if (timedOut) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.timeout"),
                    QStringLiteral(
                        "The portable chat tool request timed out."),
                    true,
                    {
                        {
                            QStringLiteral("endpoint"),
                            request.endpoint.toString(),
                        },
                        {
                            QStringLiteral(
                                "timeoutMilliseconds"),
                            timeoutMilliseconds,
                        },
                    }));
        }
        if (responseTooLarge) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.response_too_large"),
                    QStringLiteral(
                        "The portable chat tool response exceeded its size limit."),
                    false,
                    {
                        {
                            QStringLiteral("endpoint"),
                            request.endpoint.toString(),
                        },
                        {
                            QStringLiteral(
                                "maximumResponseBytes"),
                            maximumResponseBytes,
                        },
                    }));
        }
        if (!statusValue.isValid()) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.transport_failed"),
                    networkErrorText.trimmed()
                            .isEmpty()
                        ? QStringLiteral(
                              "The portable chat tool did not receive an HTTP response.")
                        : networkErrorText,
                    networkError
                        != QNetworkReply::
                            OperationCanceledError,
                    {
                        {
                            QStringLiteral("endpoint"),
                            request.endpoint.toString(),
                        },
                        {
                            QStringLiteral(
                                "networkError"),
                            static_cast<int>(
                                networkError),
                        },
                    }));
        }

        const int statusCode =
            statusValue.toInt();
        if (networkError
                != QNetworkReply::NoError
            && statusCode >= 200
            && statusCode < 300) {
            return Result<
                PortableToolHttpResponse>::failure(
                transportError(
                    QStringLiteral(
                        "portable_tool.transport_failed"),
                    networkErrorText,
                    networkError
                        != QNetworkReply::
                            OperationCanceledError,
                    {
                        {
                            QStringLiteral("endpoint"),
                            request.endpoint.toString(),
                        },
                        {
                            QStringLiteral(
                                "networkError"),
                            static_cast<int>(
                                networkError),
                        },
                    }));
        }

        return Result<
            PortableToolHttpResponse>::success({
            statusCode,
            body,
        });
    }
};

} // namespace

std::shared_ptr<PortableToolHttpTransport>
createDefaultPortableToolHttpTransport()
{
    return std::make_shared<
        QtPortableToolHttpTransport>();
}

} // namespace companion
