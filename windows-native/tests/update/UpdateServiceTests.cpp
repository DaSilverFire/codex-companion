#include "update/UpdateService.h"
#include "app/UpdateViewModel.h"

#include <QFuture>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QSignalSpy>
#include <QtTest>

#include <deque>
#include <memory>
#include <utility>

namespace {

template <typename T>
QFuture<companion::Result<T>> readyFuture(
    companion::Result<T> result)
{
    QPromise<companion::Result<T>> promise;
    promise.start();
    QFuture<companion::Result<T>> future =
        promise.future();
    promise.addResult(std::move(result));
    promise.finish();
    return future;
}

companion::UpdateManifest manifest(
    QString version = QStringLiteral("0.3.5"),
    qint64 build = 2)
{
    companion::UpdateManifest value;
    value.schemaVersion = 1;
    value.version = std::move(version);
    value.build = build;
    value.minimumSystemVersion =
        QStringLiteral("10.0.22000");
    value.publishedAt =
        QStringLiteral("2026-07-24T23:00:00Z");
    value.downloadUrl =
        QStringLiteral(
            "https://updates.example.test/"
            "Codex-Companion.exe");
    value.sha256 =
        QString(64, QLatin1Char('a'));
    value.size = 1024;
    value.signature =
        QStringLiteral("signature");
    return value;
}

companion::VerifiedArtifact artifact(
    QString path =
        QStringLiteral(
            "C:/Updates/ready/0.3.5-2/"
            "installer.exe"))
{
    companion::VerifiedArtifact value;
    value.path = std::move(path);
    value.size = 1024;
    value.sha256 =
        QByteArray(32, '\xAA');
    value.metadata.productName =
        QStringLiteral("Codex Companion");
    value.metadata.productVersionMarker =
        QStringLiteral("0.3.5+2");
    value.metadata.originalFilename =
        QStringLiteral(
            "CodexCompanionSetup.exe");
    value.signer.sha256Thumbprint =
        QString(64, QLatin1Char('B'));
    value.signer.subject =
        QStringLiteral("CN=DaSilverFire");
    return value;
}

companion::CompanionError error(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        true,
        {},
    };
}

companion::UpdateServiceOptions
configuredOptions()
{
    companion::UpdateServiceOptions options;
    options.installedVersion =
        QStringLiteral("0.3.4");
    options.installedBuild = 1;
    options.channel.configured = true;
    options.channel.manifestUrl =
        QUrl(QStringLiteral(
            "https://updates.example.test/"
            "update-windows-x64.json"));
    options.channel.publicKeyBase64 =
        QStringLiteral("trusted-key");
    options.channel.allowedSignerSha256 = {
        QString(64, QLatin1Char('B')),
    };
    options.channel.platform =
        QStringLiteral("windows");
    options.channel.architecture =
        QStringLiteral("x64");
    options.channel.minimumSystemVersion =
        QStringLiteral("10.0.22000");
    options.userAgent =
        QByteArrayLiteral(
            "CodexCompanionWindows/0.3.4");
    return options;
}

struct FakeBackend final {
    std::deque<
        companion::Result<
            companion::UpdateManifest>>
        manifestResults;
    std::deque<
        companion::Result<
            companion::VerifiedArtifact>>
        artifactResults;
    int manifestCalls = 0;
    int artifactCalls = 0;
    int installCalls = 0;
    int pruneCalls = 0;
    int cancelCalls = 0;
    QString installedPath;
    QString activePrunePath;
    companion::UpdateManifest
        downloadedManifest;
    bool emitHalfProgress = true;
    companion::Result<void>
        installResult =
            companion::Result<void>::success();

    companion::UpdateServiceDependencies
    dependencies()
    {
        companion::UpdateServiceDependencies
            dependencies;
        dependencies.requestManifest =
            [this] {
                ++manifestCalls;
                if (manifestResults.empty()) {
                    return readyFuture(
                        companion::Result<
                            companion::UpdateManifest>::
                            failure(
                                error(
                                    QStringLiteral(
                                        "update.test_manifest_missing"),
                                    QStringLiteral(
                                        "No manifest result was queued."))));
                }
                auto result =
                    std::move(
                        manifestResults.front());
                manifestResults.pop_front();
                return readyFuture(
                    std::move(result));
            };
        dependencies.requestArtifact =
            [this](
                const companion::UpdateManifest&
                    requested,
                companion::UpdateDownloadProgress
                    progress) {
                ++artifactCalls;
                downloadedManifest =
                    requested;
                if (emitHalfProgress) {
                    progress(
                        requested.size / 2,
                        requested.size);
                }
                if (artifactResults.empty()) {
                    return readyFuture(
                        companion::Result<
                            companion::VerifiedArtifact>::
                            failure(
                                error(
                                    QStringLiteral(
                                        "update.test_artifact_missing"),
                                    QStringLiteral(
                                        "No artifact result was queued."))));
                }
                auto result =
                    std::move(
                        artifactResults.front());
                artifactResults.pop_front();
                return readyFuture(
                    std::move(result));
            };
        dependencies.launchInstaller =
            [this](
                const companion::UpdateManifest&,
                const companion::VerifiedArtifact&
                    verified) {
                ++installCalls;
                installedPath = verified.path;
                return installResult;
            };
        dependencies.prune =
            [this](QStringView activePath) {
                ++pruneCalls;
                activePrunePath =
                    activePath.toString();
                return companion::Result<void>::
                    success();
            };
        dependencies.cancel =
            [this] {
                ++cancelCalls;
            };
        return dependencies;
    }
};

} // namespace

class UpdateServiceTests final
    : public QObject {
    Q_OBJECT

private slots:
    void unavailableChannelHasNoAction()
    {
        auto options =
            configuredOptions();
        options.channel.configured = false;
        FakeBackend backend;
        companion::UpdateService service(
            std::move(options),
            backend.dependencies());

        QCOMPARE(
            service.snapshot().phase,
            companion::UpdatePhase::Unavailable);
        QCOMPARE(
            companion::updatePhaseName(
                service.snapshot().phase),
            QStringLiteral("unavailable"));
        QVERIFY(
            service.snapshot()
                .detail.contains(
                    QStringLiteral(
                        "not configured"),
                    Qt::CaseInsensitive));

        const auto checked =
            service.checkForUpdates();
        QVERIFY(!checked.hasValue());
        QCOMPARE(
            checked.error().code,
            QStringLiteral(
                "update.invalid_state"));
        QCOMPARE(backend.manifestCalls, 0);
    }

    void checkPublishesUpToDate()
    {
        FakeBackend backend;
        backend.manifestResults.push_back(
            companion::Result<
                companion::UpdateManifest>::
                success(
                    manifest(
                        QStringLiteral("0.3.4"),
                        1)));
        companion::UpdateService service(
            configuredOptions(),
            backend.dependencies());

        QCOMPARE(
            service.snapshot().phase,
            companion::UpdatePhase::Idle);
        QVERIFY(
            service.checkForUpdates()
                .hasValue());
        QCOMPARE(
            service.snapshot().phase,
            companion::UpdatePhase::Checking);
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::UpToDate,
            1000);
        QCOMPARE(backend.manifestCalls, 1);
        QCOMPARE(backend.pruneCalls, 1);
        QVERIFY(
            service.snapshot()
                .availableVersion
                .isEmpty());
    }

    void availableDownloadsVerifiesAndLaunchesOnlyStoredArtifact()
    {
        FakeBackend backend;
        const auto offered =
            manifest();
        const auto verified =
            artifact();
        backend.manifestResults.push_back(
            companion::Result<
                companion::UpdateManifest>::
                success(offered));
        backend.artifactResults.push_back(
            companion::Result<
                companion::VerifiedArtifact>::
                success(verified));
        companion::UpdateService service(
            configuredOptions(),
            backend.dependencies());
        QSignalSpy launched(
            &service,
            &companion::UpdateService::
                installLaunched);

        QVERIFY(
            service.checkForUpdates()
                .hasValue());
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::Available,
            1000);
        QCOMPARE(
            service.snapshot()
                .availableVersion,
            offered.version);
        QCOMPARE(
            service.snapshot()
                .availableBuild,
            offered.build);

        QVERIFY(
            service.downloadAvailableUpdate()
                .hasValue());
        QCOMPARE(
            service.snapshot().phase,
            companion::UpdatePhase::Downloading);
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::
                ReadyToInstall,
            1000);
        QCOMPARE(backend.artifactCalls, 1);
        QCOMPARE(
            backend.downloadedManifest,
            offered);
        QCOMPARE(
            service.snapshot()
                .downloadProgress,
            1.0);

        QVERIFY(
            service.installReadyUpdate()
                .hasValue());
        QCOMPARE(
            service.snapshot().phase,
            companion::UpdatePhase::Installing);
        QCOMPARE(backend.installCalls, 1);
        QCOMPARE(
            backend.installedPath,
            verified.path);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(
            launched.first().first()
                .toString(),
            verified.path);
    }

    void downloadFailurePublishesStableError()
    {
        FakeBackend backend;
        backend.manifestResults.push_back(
            companion::Result<
                companion::UpdateManifest>::
                success(manifest()));
        backend.artifactResults.push_back(
            companion::Result<
                companion::VerifiedArtifact>::
                failure(
                    error(
                        QStringLiteral(
                            "update.artifact_digest_mismatch"),
                        QStringLiteral(
                            "Digest mismatch."))));
        companion::UpdateService service(
            configuredOptions(),
            backend.dependencies());

        QVERIFY(
            service.checkForUpdates()
                .hasValue());
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::Available,
            1000);
        QVERIFY(
            service.downloadAvailableUpdate()
                .hasValue());
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::Failed,
            1000);
        QCOMPARE(
            service.snapshot().errorCode,
            QStringLiteral(
                "update.artifact_digest_mismatch"));
        QCOMPARE(
            service.snapshot().detail,
            QStringLiteral(
                "Digest mismatch."));
    }

    void wrongStateNeverCallsBackend()
    {
        FakeBackend backend;
        companion::UpdateService service(
            configuredOptions(),
            backend.dependencies());

        const auto download =
            service.downloadAvailableUpdate();
        QVERIFY(!download.hasValue());
        QCOMPARE(
            download.error().code,
            QStringLiteral(
                "update.invalid_state"));
        const auto install =
            service.installReadyUpdate();
        QVERIFY(!install.hasValue());
        QCOMPARE(
            install.error().code,
            QStringLiteral(
                "update.invalid_state"));
        QCOMPARE(backend.artifactCalls, 0);
        QCOMPARE(backend.installCalls, 0);
    }

    void installerFailureDoesNotRemainInstalling()
    {
        FakeBackend backend;
        backend.manifestResults.push_back(
            companion::Result<
                companion::UpdateManifest>::
                success(manifest()));
        backend.artifactResults.push_back(
            companion::Result<
                companion::VerifiedArtifact>::
                success(artifact()));
        backend.installResult =
            companion::Result<void>::failure(
                error(
                    QStringLiteral(
                        "update.installer_launch_failed"),
                    QStringLiteral(
                        "Could not launch installer.")));
        companion::UpdateService service(
            configuredOptions(),
            backend.dependencies());

        QVERIFY(
            service.checkForUpdates()
                .hasValue());
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::Available,
            1000);
        QVERIFY(
            service.downloadAvailableUpdate()
                .hasValue());
        QTRY_COMPARE_WITH_TIMEOUT(
            service.snapshot().phase,
            companion::UpdatePhase::
                ReadyToInstall,
            1000);

        const auto installed =
            service.installReadyUpdate();
        QVERIFY(!installed.hasValue());
        QCOMPARE(
            service.snapshot().phase,
            companion::UpdatePhase::Failed);
        QCOMPARE(
            service.snapshot().errorCode,
            QStringLiteral(
                "update.installer_launch_failed"));
    }

    void channelConfigurationRejectsPartialTrustMaterial()
    {
        const QByteArray partial =
            QJsonDocument(
                QJsonObject{
                    {
                        QStringLiteral(
                            "manifestUrl"),
                        QStringLiteral(
                            "https://updates.example.test/update.json"),
                    },
                    {
                        QStringLiteral(
                            "publicKeyBase64"),
                        QString(),
                    },
                    {
                        QStringLiteral(
                            "signerSha256"),
                        QString(),
                    },
                    {
                        QStringLiteral(
                            "platform"),
                        QStringLiteral(
                            "windows"),
                    },
                    {
                        QStringLiteral(
                            "architecture"),
                        QStringLiteral("x64"),
                    },
                    {
                        QStringLiteral(
                            "minimumSystemVersion"),
                        QStringLiteral(
                            "10.0.22000"),
                    },
                })
                .toJson(
                    QJsonDocument::Compact);

        const auto decoded =
            companion::UpdateChannelConfiguration::
                decode(partial);

        QVERIFY(!decoded.hasValue());
        QCOMPARE(
            decoded.error().code,
            QStringLiteral(
                "update.channel_incomplete"));
    }

    void bundledChannelResourceLoads()
    {
        const auto decoded =
            companion::
                UpdateChannelConfiguration::
                    fromBundledResource();

        QVERIFY(decoded.hasValue());
        QCOMPARE(
            decoded.value().platform,
            QStringLiteral("windows"));
        QCOMPARE(
            decoded.value().architecture,
            QStringLiteral("x64"));
        QCOMPARE(
            decoded.value()
                .minimumSystemVersion,
            QStringLiteral(
                "10.0.22000"));
    }

    void releaseTestManifestOverrideParsingIsStrict()
    {
        const auto parsed =
            companion::
                updateManifestUrlOverrideFromArguments(
                    {
                        QStringLiteral(
                            "CodexCompanion.exe"),
                        QStringLiteral(
                            "--update-manifest-url"),
                        QStringLiteral(
                            "https://fixture.example.test/"
                            "update-windows-x64.json"),
                    });

        QVERIFY(parsed.hasValue());
        QVERIFY(
            parsed.value()
                .has_value());
        QCOMPARE(
            *parsed.value(),
            QUrl(QStringLiteral(
                "https://fixture.example.test/"
                "update-windows-x64.json")));

        for (const QStringList& invalid : {
                 QStringList{
                     QStringLiteral(
                         "CodexCompanion.exe"),
                     QStringLiteral(
                         "--update-manifest-url"),
                 },
                 QStringList{
                     QStringLiteral(
                         "CodexCompanion.exe"),
                     QStringLiteral(
                         "--update-manifest-url"),
                     QStringLiteral(
                         "http://fixture.example.test/"
                         "update.json"),
                 },
                 QStringList{
                     QStringLiteral(
                         "CodexCompanion.exe"),
                     QStringLiteral(
                         "--update-manifest-url"),
                     QStringLiteral(
                         "https://fixture.example.test/"
                         "update.json"),
                     QStringLiteral(
                         "--update-manifest-url"),
                     QStringLiteral(
                         "https://other.example.test/"
                         "update.json"),
                 },
             }) {
            const auto rejected =
                companion::
                    updateManifestUrlOverrideFromArguments(
                        invalid);
            QVERIFY(!rejected.hasValue());
            QCOMPARE(
                rejected.error().code,
                QStringLiteral(
                    "update.feed_override_argument_invalid"));
        }
    }

    void manifestOverridePreservesBundledTrust()
    {
        const QString publicKey =
            QString::fromLatin1(
                QByteArray(32, '\x2A')
                    .toBase64());
        const QString signer =
            QString(64, QLatin1Char('b'));
        const QByteArray encoded =
            QJsonDocument(
                QJsonObject{
                    {
                        QStringLiteral(
                            "manifestUrl"),
                        QStringLiteral(
                            "https://updates.example.test/"
                            "update.json"),
                    },
                    {
                        QStringLiteral(
                            "publicKeyBase64"),
                        publicKey,
                    },
                    {
                        QStringLiteral(
                            "signerSha256"),
                        signer,
                    },
                    {
                        QStringLiteral(
                            "platform"),
                        QStringLiteral(
                            "windows"),
                    },
                    {
                        QStringLiteral(
                            "architecture"),
                        QStringLiteral("x64"),
                    },
                    {
                        QStringLiteral(
                            "minimumSystemVersion"),
                        QStringLiteral(
                            "10.0.22000"),
                    },
                })
                .toJson(
                    QJsonDocument::Compact);
        const auto decoded =
            companion::
                UpdateChannelConfiguration::
                    decode(encoded);
        QVERIFY(decoded.hasValue());

        const auto overridden =
            decoded.value()
                .withManifestUrlOverride(
                    QUrl(QStringLiteral(
                        "https://fixture.example.test/"
                        "update-windows-x64.json")));

        QVERIFY(overridden.hasValue());
        QCOMPARE(
            overridden.value()
                .manifestUrl,
            QUrl(QStringLiteral(
                "https://fixture.example.test/"
                "update-windows-x64.json")));
        QCOMPARE(
            overridden.value()
                .publicKeyBase64,
            publicKey);
        const QStringList expectedSigners{
            signer.toUpper(),
        };
        QCOMPARE(
            overridden.value()
                .allowedSignerSha256,
            expectedSigners);

        companion::
            UpdateChannelConfiguration
                unconfigured;
        const auto untrusted =
            unconfigured
                .withManifestUrlOverride(
                    QUrl(QStringLiteral(
                        "https://fixture.example.test/"
                        "update.json")));
        QVERIFY(!untrusted.hasValue());
        QCOMPARE(
            untrusted.error().code,
            QStringLiteral(
                "update.feed_override_untrusted"));
    }

    void viewModelPublishesAndDispatchesStateActions()
    {
        FakeBackend backend;
        const auto offered = manifest();
        const auto verified = artifact();
        backend.manifestResults.push_back(
            companion::Result<
                companion::UpdateManifest>::
                success(offered));
        backend.artifactResults.push_back(
            companion::Result<
                companion::VerifiedArtifact>::
                success(verified));
        companion::UpdateService service(
            configuredOptions(),
            backend.dependencies());
        companion::UpdateViewModel viewModel(
            service);

        QCOMPARE(
            viewModel.phase(),
            QStringLiteral("idle"));
        QCOMPARE(
            viewModel.primaryActionText(),
            QStringLiteral(
                "Check for Updates"));
        QVERIFY(
            viewModel.primaryActionEnabled());

        QVERIFY(viewModel.checkForUpdates());
        QCOMPARE(
            viewModel.phase(),
            QStringLiteral("checking"));
        QVERIFY(
            !viewModel
                 .primaryActionEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(
            viewModel.phase(),
            QStringLiteral("available"),
            1000);
        QCOMPARE(
            viewModel.primaryActionText(),
            QStringLiteral(
                "Download Verified Update"));
        QCOMPARE(
            viewModel.availableVersion(),
            offered.version);

        QVERIFY(
            viewModel
                .downloadAvailableUpdate());
        QTRY_COMPARE_WITH_TIMEOUT(
            viewModel.phase(),
            QStringLiteral(
                "ready-to-install"),
            1000);
        QCOMPARE(
            viewModel.downloadProgress(),
            1.0);
        QCOMPARE(
            viewModel.primaryActionText(),
            QStringLiteral(
                "Install and Relaunch"));

        QVERIFY(viewModel.installReadyUpdate());
        QCOMPARE(
            viewModel.phase(),
            QStringLiteral("installing"));
        QCOMPARE(backend.installCalls, 1);
        QCOMPARE(
            backend.installedPath,
            verified.path);
    }
};

QTEST_GUILESS_MAIN(UpdateServiceTests)
#include "UpdateServiceTests.moc"
