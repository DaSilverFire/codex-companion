#include "update/UpdateFeedClient.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QUrl>
#include <QtTest>

#include <chrono>
#include <optional>
#include <utility>

namespace {

using ManifestResult =
    companion::Result<
        companion::UpdateManifest>;

constexpr auto kPublicKeyBase64 =
    "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
constexpr auto kUserAgent =
    "CodexCompanionWindows/0.3.4 (34)";

QByteArray fixture(const QString& name)
{
    QFile file(
        QStringLiteral(
            COMPANION_UPDATE_FIXTURE_ROOT)
        + QLatin1Char('/')
        + name);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("update fixture missing");
    }
    return file.readAll();
}

bool waitForFinished(
    QFuture<ManifestResult>& future,
    int timeoutMilliseconds = 15'000)
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

QString errorCode(
    const ManifestResult& result)
{
    return result.hasValue()
        ? QStringLiteral("<success>")
        : result.error().code;
}

struct TestTlsIdentity final {
    QSslCertificate certificate;
    QSslKey key;
};

TestTlsIdentity loadTestIdentity()
{
    QByteArray pfx =
        QByteArray::fromBase64(
            fixture(
                QStringLiteral(
                    "localhost-test.pfx.b64")));
    QBuffer buffer(&pfx);
    if (!buffer.open(
            QIODevice::ReadOnly)) {
        qFatal(
            "could not open test PFX");
    }

    TestTlsIdentity identity;
    QList<QSslCertificate> chain;
    if (!QSslCertificate::importPkcs12(
            &buffer,
            &identity.key,
            &identity.certificate,
            &chain,
            QByteArrayLiteral(
                "companion-test"))
        || identity.key.isNull()
        || identity.certificate.isNull()) {
        qFatal(
            "could not import test PFX");
    }
    return identity;
}

class HttpsTestServer final {
public:
    struct Response final {
        int status = 200;
        QByteArray body;
        QByteArray location;
        bool hold = false;
    };

    HttpsTestServer()
        : identity_(loadTestIdentity())
    {
        QSslConfiguration configuration =
            QSslConfiguration::
                defaultConfiguration();
        configuration.setLocalCertificate(
            identity_.certificate);
        configuration.setPrivateKey(
            identity_.key);
        configuration.setPeerVerifyMode(
            QSslSocket::VerifyNone);
        configuration.setProtocol(
            QSsl::TlsV1_2OrLater);
        configuration.setAllowedNextProtocols({
            QSslConfiguration::
                NextProtocolHttp1_1,
        });
        server_.setSslConfiguration(
            configuration);
        server_.setHandshakeTimeout(10'000);
        QObject::connect(
            &server_,
            &QTcpServer::
                pendingConnectionAvailable,
            &server_,
            [this] {
                acceptConnections();
            });
        QObject::connect(
            &server_,
            &QSslServer::
                startedEncryptionHandshake,
            &server_,
            [this](QSslSocket*) {
                diagnostics_.append(
                    QStringLiteral(
                        "handshake-started"));
            });
        QObject::connect(
            &server_,
            &QSslServer::sslErrors,
            &server_,
            [this](
                QSslSocket*,
                const QList<QSslError>&
                    errors) {
                for (const QSslError& error :
                     errors) {
                    diagnostics_.append(
                        QStringLiteral(
                            "ssl:%1")
                            .arg(
                                error
                                    .errorString()));
                }
            });
        QObject::connect(
            &server_,
            &QSslServer::errorOccurred,
            &server_,
            [this](
                QSslSocket*,
                QAbstractSocket::
                    SocketError error) {
                diagnostics_.append(
                    QStringLiteral(
                        "socket:%1")
                        .arg(
                            static_cast<int>(
                                error)));
            });
        if (!server_.listen(
                QHostAddress::LocalHost,
                0)) {
            qFatal(
                "could not start TLS test server");
        }
    }

    ~HttpsTestServer()
    {
        server_.close();
        for (QSslSocket* socket :
             buffers_.keys()) {
            socket->abort();
        }
    }

    void route(
        QString path,
        Response response)
    {
        routes_.insert(
            std::move(path),
            std::move(response));
    }

    QUrl url(QString path) const
    {
        QUrl value;
        value.setScheme(
            QStringLiteral("https"));
        value.setHost(
            QStringLiteral("127.0.0.1"));
        value.setPort(
            server_.serverPort());
        value.setPath(std::move(path));
        return value;
    }

    QSslConfiguration
    trustedClientConfiguration() const
    {
        QSslConfiguration configuration =
            QSslConfiguration::
                defaultConfiguration();
        configuration.setCaCertificates({
            identity_.certificate,
        });
        configuration.setPeerVerifyMode(
            QSslSocket::VerifyPeer);
        configuration.setProtocol(
            QSsl::TlsV1_2OrLater);
        configuration.setAllowedNextProtocols({
            QSslConfiguration::
                NextProtocolHttp1_1,
        });
        return configuration;
    }

    const QStringList& paths() const
    {
        return paths_;
    }

    QByteArray requestFor(
        const QString& path) const
    {
        return requests_.value(path);
    }

    QString diagnostics() const
    {
        return diagnostics_.join(
            QStringLiteral(" | "));
    }

private:
    void acceptConnections()
    {
        while (server_
                   .hasPendingConnections()) {
            auto* const socket =
                qobject_cast<QSslSocket*>(
                    server_
                        .nextPendingConnection());
            if (socket == nullptr) {
                qFatal(
                    "TLS server returned a non-TLS socket");
            }
            buffers_.insert(
                socket,
                QByteArray());
            QObject::connect(
                socket,
                &QIODevice::readyRead,
                socket,
                [this, socket] {
                    readRequest(socket);
                });
            QObject::connect(
                socket,
                &QAbstractSocket::disconnected,
                socket,
                [this, socket] {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
        }
    }

    void readRequest(QSslSocket* socket)
    {
        auto iterator =
            buffers_.find(socket);
        if (iterator == buffers_.end()) {
            return;
        }
        iterator.value().append(
            socket->readAll());
        const qsizetype headerEnd =
            iterator.value().indexOf(
                QByteArrayLiteral(
                    "\r\n\r\n"));
        if (headerEnd < 0) {
            return;
        }

        const QByteArray request =
            iterator.value().first(
                headerEnd + 4);
        const QList<QByteArray> lineParts =
            request.first(
                       request.indexOf(
                           QByteArrayLiteral(
                               "\r\n")))
                .split(' ');
        if (lineParts.size() < 2) {
            socket->abort();
            return;
        }
        const QString path =
            QUrl::fromEncoded(
                lineParts.at(1))
                .path();
        paths_.append(path);
        requests_.insert(path, request);

        const Response response =
            routes_.value(
                path,
                Response{
                    404,
                    QByteArrayLiteral(
                        "not found"),
                    {},
                    false,
                });
        if (response.hold) {
            return;
        }

        QByteArray bytes =
            QByteArrayLiteral("HTTP/1.1 ")
            + QByteArray::number(
                  response.status)
            + QByteArrayLiteral(" ")
            + reasonPhrase(
                  response.status)
            + QByteArrayLiteral("\r\n");
        if (!response.location.isEmpty()) {
            bytes +=
                QByteArrayLiteral(
                    "Location: ")
                + response.location
                + QByteArrayLiteral(
                    "\r\n");
        }
        bytes +=
            QByteArrayLiteral(
                "Content-Type: application/json\r\n"
                "Content-Length: ")
            + QByteArray::number(
                  response.body.size())
            + QByteArrayLiteral(
                "\r\nConnection: close\r\n\r\n")
            + response.body;
        socket->write(bytes);
        socket->flush();
        socket->disconnectFromHost();
    }

    static QByteArray reasonPhrase(
        int status)
    {
        switch (status) {
        case 200:
            return QByteArrayLiteral("OK");
        case 302:
            return QByteArrayLiteral("Found");
        case 503:
            return QByteArrayLiteral(
                "Service Unavailable");
        default:
            return QByteArrayLiteral(
                "Test Status");
        }
    }

    TestTlsIdentity identity_;
    QSslServer server_;
    QHash<QString, Response> routes_;
    QHash<QSslSocket*, QByteArray>
        buffers_;
    QStringList paths_;
    QHash<QString, QByteArray>
        requests_;
    QStringList diagnostics_;
};

companion::UpdateFeedClientOptions
trustedOptions(
    const HttpsTestServer& server,
    std::chrono::milliseconds deadline =
        std::chrono::seconds(10))
{
    companion::UpdateFeedClientOptions
        options;
    options.publicKeyBase64 =
        QString::fromLatin1(
            kPublicKeyBase64);
    options.userAgent =
        QByteArrayLiteral(kUserAgent);
    options.sslConfiguration =
        server.trustedClientConfiguration();
    options.manifestDeadline =
        deadline;
    return options;
}

ManifestResult takeResult(
    QFuture<ManifestResult>& future)
{
    if (!waitForFinished(future)) {
        qFatal(
            "update transport future timed out");
    }
    return future.result();
}

} // namespace

class UpdateFeedClientTests final
    : public QObject {
    Q_OBJECT

private slots:
    void loadsSignedManifestOverTrustedTls()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/manifest"),
            {
                200,
                fixture(
                    QStringLiteral(
                        "manifest-valid.json")),
            });
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral(
                        "/manifest")));
        const ManifestResult result =
            takeResult(future);

        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error().message
                          + QStringLiteral(
                                " Server: ")
                          + server
                                .diagnostics()));
        QCOMPARE(
            result.value().version,
            QStringLiteral("0.3.4"));
        const QStringList expectedPaths{
            QStringLiteral("/manifest"),
        };
        QCOMPARE(
            server.paths(),
            expectedPaths);
        const QByteArray request =
            server.requestFor(
                QStringLiteral(
                    "/manifest"));
        QVERIFY(request.contains(
            QByteArrayLiteral(
                "Accept: application/json")));
        QVERIFY(request.contains(
            QByteArrayLiteral(
                "User-Agent: ")
            + QByteArrayLiteral(
                  kUserAgent)));
    }

    void followsFiveHttpsRedirects()
    {
        HttpsTestServer server;
        for (int index = 0;
             index < 5;
             ++index) {
            server.route(
                QStringLiteral("/r%1")
                    .arg(index),
                {
                    302,
                    {},
                    index == 4
                        ? QByteArrayLiteral(
                              "/manifest")
                        : QByteArray(
                              "/r"
                              + QByteArray::number(
                                  index + 1)),
                });
        }
        server.route(
            QStringLiteral("/manifest"),
            {
                200,
                fixture(
                    QStringLiteral(
                        "manifest-valid.json")),
            });
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral("/r0")));
        const ManifestResult result =
            takeResult(future);

        QVERIFY(result.hasValue());
        QCOMPARE(server.paths().size(), 6);
        QCOMPARE(
            server.paths().last(),
            QStringLiteral("/manifest"));
    }

    void rejectsSixthRedirect()
    {
        HttpsTestServer server;
        for (int index = 0;
             index < 6;
             ++index) {
            server.route(
                QStringLiteral("/r%1")
                    .arg(index),
                {
                    302,
                    {},
                    index == 5
                        ? QByteArrayLiteral(
                              "/manifest")
                        : QByteArray(
                              "/r"
                              + QByteArray::number(
                                  index + 1)),
                });
        }
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral("/r0")));
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.too_many_redirects"));
        QCOMPARE(server.paths().size(), 6);
        QVERIFY(
            !server.paths().contains(
                QStringLiteral(
                    "/manifest")));
    }

    void rejectsHttpDowngradeBeforeRequest()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/downgrade"),
            {
                302,
                {},
                QByteArray(
                    "http://localhost:")
                    + QByteArray::number(
                          server.url(
                                    QStringLiteral(
                                        "/"))
                              .port())
                    + QByteArrayLiteral(
                          "/manifest"),
            });
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral(
                        "/downgrade")));
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.insecure_redirect"));
        const QStringList expectedPaths{
            QStringLiteral("/downgrade"),
        };
        QCOMPARE(
            server.paths(),
            expectedPaths);
    }

    void rejectsInsecureInitialUrlBeforeNetwork()
    {
        HttpsTestServer server;
        companion::UpdateFeedClient client(
            trustedOptions(server));
        QUrl insecure =
            server.url(
                QStringLiteral(
                    "/manifest"));
        insecure.setScheme(
            QStringLiteral("http"));

        auto future =
            client.loadManifest(insecure);
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.insecure_manifest_url"));
        QVERIFY(server.paths().isEmpty());
    }

    void failsClosedOnTlsError()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/manifest"),
            {
                200,
                fixture(
                    QStringLiteral(
                        "manifest-valid.json")),
            });
        companion::UpdateFeedClientOptions
            options;
        options.publicKeyBase64 =
            QString::fromLatin1(
                kPublicKeyBase64);
        options.userAgent =
            QByteArrayLiteral(kUserAgent);
        options.manifestDeadline =
            std::chrono::seconds(2);
        companion::UpdateFeedClient client(
            std::move(options));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral(
                        "/manifest")));
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.tls_error"));
    }

    void rejectsNonSuccessfulFinalStatus()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/unavailable"),
            {
                503,
                QByteArrayLiteral(
                    "try later"),
            });
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral(
                        "/unavailable")));
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.http_status"));
        QCOMPARE(
            result.error()
                .context
                .value(
                    QStringLiteral(
                        "status"))
                .toInt(),
            503);
        QVERIFY(result.error().retryable);
    }

    void abortsResponseOver64KiB()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/oversized"),
            {
                200,
                QByteArray(
                    companion::
                        UpdateFeedClient::
                            maximumManifestBytes
                        + 1,
                    'x'),
            });
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral(
                        "/oversized")));
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.manifest_too_large"));
    }

    void requestDeadlineCoversHeldResponse()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/hold"),
            {
                200,
                {},
                {},
                true,
            });
        companion::UpdateFeedClient client(
            trustedOptions(
                server,
                std::chrono::
                    milliseconds(60)));

        auto future =
            client.loadManifest(
                server.url(
                    QStringLiteral("/hold")));
        const ManifestResult result =
            takeResult(future);

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.manifest_timeout"));
        QVERIFY(result.error().retryable);
    }

    void secondRequestCancelsAndSupersedesFirst()
    {
        HttpsTestServer server;
        server.route(
            QStringLiteral("/hold"),
            {
                200,
                {},
                {},
                true,
            });
        server.route(
            QStringLiteral("/manifest"),
            {
                200,
                fixture(
                    QStringLiteral(
                        "manifest-valid.json")),
            });
        companion::UpdateFeedClient client(
            trustedOptions(server));

        auto first =
            client.loadManifest(
                server.url(
                    QStringLiteral("/hold")));
        QTRY_VERIFY_WITH_TIMEOUT(
            server.paths().contains(
                QStringLiteral("/hold")),
            2'000);
        auto second =
            client.loadManifest(
                server.url(
                    QStringLiteral(
                        "/manifest")));

        const ManifestResult firstResult =
            takeResult(first);
        const ManifestResult secondResult =
            takeResult(second);

        QCOMPARE(
            errorCode(firstResult),
            QStringLiteral(
                "update.cancelled"));
        QVERIFY(secondResult.hasValue());
        QCOMPARE(
            server.paths().last(),
            QStringLiteral("/manifest"));
        QVERIFY(!client.isLoading());
    }
};

QTEST_GUILESS_MAIN(UpdateFeedClientTests)
#include "UpdateFeedClientTests.moc"
