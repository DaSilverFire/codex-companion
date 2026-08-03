#include "update/UpdateArtifactDownloader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <cstring>
#include <deque>
#include <optional>
#include <utility>

namespace {

using ArtifactResult =
    companion::Result<
        companion::VerifiedArtifact>;

constexpr auto kTrustedSigner =
    "0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF";

class ScriptedReply final
    : public QNetworkReply {
public:
    struct Response final {
        int status = 200;
        QByteArray body;
        QUrl redirect;
        std::optional<qint64>
            contentLength;
        QByteArray contentEncoding;
        QNetworkReply::NetworkError
            networkError =
                QNetworkReply::NoError;
        QString networkErrorText;
        bool hold = false;
    };

    ScriptedReply(
        QNetworkRequest request,
        Response response,
        QObject* parent)
        : QNetworkReply(parent),
          body_(std::move(response.body)),
          response_(std::move(response))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(
            QNetworkAccessManager::
                GetOperation);
        setAttribute(
            QNetworkRequest::
                HttpStatusCodeAttribute,
            response_.status);
        if (!response_.redirect.isEmpty()) {
            setAttribute(
                QNetworkRequest::
                    RedirectionTargetAttribute,
                response_.redirect);
        }
        const qint64 contentLength =
            response_.contentLength
                .value_or(body_.size());
        setHeader(
            QNetworkRequest::
                ContentLengthHeader,
            contentLength);
        if (!response_
                 .contentEncoding
                 .isEmpty()) {
            setRawHeader(
                QByteArrayLiteral(
                    "Content-Encoding"),
                response_
                    .contentEncoding);
        }
        open(
            QIODevice::ReadOnly
            | QIODevice::Unbuffered);
        QTimer::singleShot(
            0,
            this,
            [this] {
                emit metaDataChanged();
                if (response_.hold
                    || isFinished()) {
                    return;
                }
                if (!body_.isEmpty()) {
                    emit readyRead();
                }
                if (response_.networkError
                    != QNetworkReply::
                        NoError) {
                    setError(
                        response_
                            .networkError,
                        response_
                                .networkErrorText);
                }
                setFinished(true);
                emit finished();
            });
    }

    void abort() override
    {
        if (isFinished()) {
            return;
        }
        setError(
            QNetworkReply::
                OperationCanceledError,
            QStringLiteral(
                "Cancelled."));
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable()
        const override
    {
        return body_.size() - offset_
            + QNetworkReply::
                  bytesAvailable();
    }

protected:
    qint64 readData(
        char* data,
        qint64 maximumSize) override
    {
        if (offset_ >= body_.size()) {
            return -1;
        }
        const qint64 count =
            std::min(
                maximumSize,
                body_.size() - offset_);
        std::memcpy(
            data,
            body_.constData() + offset_,
            static_cast<std::size_t>(
                count));
        offset_ += count;
        return count;
    }

private:
    QByteArray body_;
    Response response_;
    qint64 offset_ = 0;
};

class ScriptedNetworkAccessManager final
    : public QNetworkAccessManager {
public:
    std::deque<
        ScriptedReply::Response>
        responses;
    QList<QUrl> urls;
    QList<QNetworkRequest> requests;

protected:
    QNetworkReply* createRequest(
        Operation operation,
        const QNetworkRequest& request,
        QIODevice* outgoingData) override
    {
        Q_UNUSED(outgoingData);
        if (operation
            != QNetworkAccessManager::
                GetOperation) {
            qFatal(
                "Unexpected update request operation.");
        }
        urls.append(request.url());
        requests.append(request);
        if (responses.empty()) {
            qFatal(
                "No scripted update response was queued.");
        }
        auto response =
            std::move(responses.front());
        responses.pop_front();
        return new ScriptedReply(
            request,
            std::move(response),
            this);
    }
};

companion::UpdateManifest manifestFor(
    const QByteArray& payload,
    QString downloadUrl =
        QStringLiteral(
            "https://updates.example.test/"
            "Codex-Companion-0.3.5-2-windows-x64.exe"))
{
    companion::UpdateManifest manifest;
    manifest.schemaVersion = 1;
    manifest.version =
        QStringLiteral("0.3.5");
    manifest.build = 2;
    manifest.minimumSystemVersion =
        QStringLiteral("10.0.22000");
    manifest.publishedAt =
        QStringLiteral(
            "2026-07-25T00:00:00Z");
    manifest.downloadUrl =
        std::move(downloadUrl);
    manifest.sha256 =
        QString::fromLatin1(
            QCryptographicHash::hash(
                payload,
                QCryptographicHash::Sha256)
                .toHex());
    manifest.size = payload.size();
    manifest.signature =
        QStringLiteral("test-signature");
    return manifest;
}

companion::UpdateArtifactStoreOptions
storeOptions(const QString& root)
{
    companion::UpdateArtifactStoreOptions
        options;
    options.rootPath = root;
    options.currentWindowsVersion = {
        10,
        0,
        26100,
        0,
    };
    options.allowedSignerSha256 = {
        QString::fromLatin1(
            kTrustedSigner),
    };
    options.peInspector =
        [](QStringView) {
            return companion::Result<
                companion::PeMachine>::
                success(
                    companion::PeMachine::
                        X64);
        };
    options.metadataInspector =
        [](QStringView) {
            return companion::Result<
                companion::
                    InstallerMetadata>::
                success(
                    {
                        QStringLiteral(
                            "Codex Companion"),
                        QStringLiteral(
                            "cc-update/1|0.3.5|2|w|x64|10.0.22000"),
                        QStringLiteral(
                            "Codex-Companion-0.3.5-2-windows-x64.exe"),
                    });
        };
    options.signerInspector =
        [](QStringView) {
            return companion::Result<
                companion::
                    AuthenticodeIdentity>::
                success(
                    {
                        QString::fromLatin1(
                            kTrustedSigner),
                        QStringLiteral(
                            "CN=DaSilverFire"),
                    });
        };
    return options;
}

bool waitForFinished(
    QFuture<ArtifactResult>& future,
    int timeoutMilliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!future.isFinished()
           && timer.elapsed()
               < timeoutMilliseconds) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            10);
        QTest::qWait(1);
    }
    return future.isFinished();
}

QStringList filesBelow(
    const QString& root)
{
    QStringList files;
    QDirIterator iterator(
        root,
        QDir::Files,
        QDirIterator::
            Subdirectories);
    while (iterator.hasNext()) {
        files.append(iterator.next());
    }
    return files;
}

} // namespace

class UpdateArtifactDownloaderTests final
    : public QObject {
    Q_OBJECT

private slots:
    void streamsIntoVerifiedArtifactStore()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload(
            180 * 1024,
            '\x5A');
        const auto manifest =
            manifestFor(payload);
        companion::UpdateArtifactStore store(
            storeOptions(
                directory.path()));
        ScriptedNetworkAccessManager
            network;
        network.responses.push_back({
            200,
            payload,
        });
        companion::UpdateArtifactDownloader
            downloader(
                network,
                {
                    QByteArrayLiteral(
                        "CodexCompanionWindows/"
                        "0.3.4 (1)"),
                });
        QVector<qint64> progress;

        auto future = downloader.download(
            manifest,
            store,
            [&progress](
                qint64 received,
                qint64) {
                progress.append(received);
            });

        QVERIFY(waitForFinished(future));
        const ArtifactResult result =
            future.result();
        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error()
                          .message));
        QFile file(result.value().path);
        QVERIFY(
            file.open(
                QIODevice::ReadOnly));
        QCOMPARE(
            file.readAll(),
            payload);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(
            progress.last(),
            payload.size());
        QCOMPARE(network.urls.size(), 1);
        QCOMPARE(
            network.requests
                .first()
                .rawHeader(
                    QByteArrayLiteral(
                        "Accept-Encoding")),
            QByteArrayLiteral(
                "identity"));
    }

    void rejectsDeclaredLengthMismatch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload(
            4096,
            '\x31');
        const auto manifest =
            manifestFor(payload);
        companion::UpdateArtifactStore store(
            storeOptions(
                directory.path()));
        ScriptedNetworkAccessManager
            network;
        ScriptedReply::Response response;
        response.body = payload;
        response.contentLength =
            payload.size() + 1;
        network.responses.push_back(
            std::move(response));
        companion::UpdateArtifactDownloader
            downloader(
                network,
                {});

        auto future = downloader.download(
            manifest,
            store);

        QVERIFY(waitForFinished(future));
        const ArtifactResult result =
            future.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "update.artifact_content_length_mismatch"));
        QVERIFY(
            filesBelow(
                directory.path())
                .isEmpty());
    }

    void followsSecureRedirectWithoutStagingRedirectBody()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload(
            8192,
            '\x42');
        const auto manifest =
            manifestFor(
                payload,
                QStringLiteral(
                    "https://updates.example.test/start"));
        companion::UpdateArtifactStore store(
            storeOptions(
                directory.path()));
        ScriptedNetworkAccessManager
            network;
        network.responses.push_back({
            302,
            QByteArrayLiteral(
                "redirect-body"),
            QUrl(QStringLiteral(
                "https://cdn.example.test/"
                "installer.exe")),
        });
        network.responses.push_back({
            200,
            payload,
        });
        companion::UpdateArtifactDownloader
            downloader(
                network,
                {});

        auto future = downloader.download(
            manifest,
            store);

        QVERIFY(waitForFinished(future));
        const ArtifactResult result =
            future.result();
        QVERIFY(result.hasValue());
        QCOMPARE(network.urls.size(), 2);
        QCOMPARE(
            network.urls.last(),
            QUrl(QStringLiteral(
                "https://cdn.example.test/"
                "installer.exe")));
        QCOMPARE(
            result.value().size,
            payload.size());
    }

    void rejectsInsecureRedirect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload(
            1024,
            '\x20');
        const auto manifest =
            manifestFor(payload);
        companion::UpdateArtifactStore store(
            storeOptions(
                directory.path()));
        ScriptedNetworkAccessManager
            network;
        network.responses.push_back({
            302,
            {},
            QUrl(QStringLiteral(
                "http://updates.example.test/"
                "installer.exe")),
        });
        companion::UpdateArtifactDownloader
            downloader(
                network,
                {});

        auto future = downloader.download(
            manifest,
            store);

        QVERIFY(waitForFinished(future));
        const ArtifactResult result =
            future.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "update.artifact_insecure_redirect"));
        QCOMPARE(network.urls.size(), 1);
        QVERIFY(
            filesBelow(
                directory.path())
                .isEmpty());
    }

    void cancellationRemovesPartialArtifact()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload(
            2048,
            '\x10');
        const auto manifest =
            manifestFor(payload);
        companion::UpdateArtifactStore store(
            storeOptions(
                directory.path()));
        ScriptedNetworkAccessManager
            network;
        ScriptedReply::Response response;
        response.body = payload;
        response.hold = true;
        network.responses.push_back(
            std::move(response));
        companion::UpdateArtifactDownloader
            downloader(
                network,
                {});

        auto future = downloader.download(
            manifest,
            store);
        QCoreApplication::processEvents();
        downloader.cancel();

        QVERIFY(waitForFinished(future));
        const ArtifactResult result =
            future.result();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "update.cancelled"));
        QVERIFY(
            filesBelow(
                directory.path())
                .isEmpty());
    }
};

QTEST_GUILESS_MAIN(
    UpdateArtifactDownloaderTests)
#include "UpdateArtifactDownloaderTests.moc"
