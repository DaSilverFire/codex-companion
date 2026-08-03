#include "core/SettingsRepository.h"

#include <cmath>
#include <limits>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QByteArray readAllBytes(const QString& path)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly);
    Q_ASSERT(opened);
    return file.readAll();
}

} // namespace

class SettingsRepositoryTests final : public QObject {
    Q_OBJECT

private slots:
    void missingFileReturnsApprovedDefaults()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));

        const auto result = repository.load();

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().backdrop, companion::BackdropMode::Mica);
        QVERIFY(result.value().selectedPetId.isEmpty());
        QCOMPARE(result.value().selectedChatModelId,
                 QStringLiteral("on-device"));
        QCOMPARE(result.value().animationSpeedScale, 1.15);
        QVERIFY(result.value().petVisible);
        QVERIFY(!result.value().petWindowPosition.has_value());
        QVERIFY(result.value().allowAutonomousMovement);
        QVERIFY(result.value().mobileEnabled);
        QVERIFY(result.value().keepAvailableWhileDisplayOff);
        QVERIFY(
            !result.value()
                 .allowNearbyOnPublicNetworks);
        QCOMPARE(result.value().relayMode, companion::RelayMode::Automatic);

        QSettings persisted(repository.filePath(), QSettings::IniFormat);
        QCOMPARE(
            persisted.value(
                QStringLiteral("pet/animationSpeedTimingVersion"))
                .toInt(),
            2);
        QCOMPARE(
            persisted.value(
                QStringLiteral("pet/animationSpeedScale"))
                .toDouble(),
            1.15);
    }

    void saveAndLoadRoundTripsAllFoundationFields()
    {
        QTemporaryDir directory;
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings expected;
        expected.backdrop = companion::BackdropMode::WindowsGlass;
        expected.selectedPetId = QStringLiteral("custom-pet");
        expected.selectedChatModelId =
            QStringLiteral("lumo:thinking");
        expected.animationSpeedScale = 1.75;
        expected.petVisible = false;
        expected.petWindowPosition = QPoint(482, 316);
        expected.hideControlsUntilHover = true;
        expected.allowAutonomousMovement = false;
        expected.mobileEnabled = false;
        expected.keepAvailableWhileDisplayOff = false;
        expected.allowNearbyOnPublicNetworks =
            true;
        expected.automaticallyContinuesAcrossCodexAccounts =
            true;
        expected.relayMode = companion::RelayMode::Custom;
        expected.customRelayUrl = QStringLiteral("wss://relay.example.test");

        QVERIFY(repository.save(expected).hasValue());
        const auto loaded = repository.load();

        QVERIFY(loaded.hasValue());
        QCOMPARE(loaded.value(), expected);
    }

    void accountContinuationDefaultsOffAndRoundTrips()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));

        const auto defaults = repository.load();

        QVERIFY(defaults.hasValue());
        QVERIFY(
            !defaults.value()
                 .automaticallyContinuesAcrossCodexAccounts);

        companion::AppSettings enabled =
            defaults.value();
        enabled
            .automaticallyContinuesAcrossCodexAccounts =
            true;
        QVERIFY(repository.save(enabled).hasValue());

        const auto restored = repository.load();

        QVERIFY(restored.hasValue());
        QVERIFY(
            restored.value()
                .automaticallyContinuesAcrossCodexAccounts);
        QSettings raw(
            repository.filePath(),
            QSettings::IniFormat);
        QVERIFY(
            raw.value(
                   QStringLiteral(
                       "accounts/automaticallyContinuesAcrossCodexAccounts"))
                .toBool());
    }

    void animationSpeedClampsFiniteAndInfiniteValuesToBounds()
    {
        const struct {
            double input;
            double expected;
        } cases[] = {
            {0.25, 0.75},
            {3.5, 2.5},
            {-std::numeric_limits<double>::infinity(), 0.75},
            {std::numeric_limits<double>::infinity(), 2.5},
        };

        for (const auto& testCase : cases) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString path = directory.filePath(QStringLiteral("settings.ini"));
            companion::SettingsRepository repository(path);

            companion::AppSettings settings;
            settings.animationSpeedScale = testCase.input;

            QVERIFY(repository.save(settings).hasValue());
            const auto loaded = repository.load();

            QVERIFY(loaded.hasValue());
            QVERIFY(std::isfinite(loaded.value().animationSpeedScale));
            QCOMPARE(loaded.value().animationSpeedScale, testCase.expected);
        }
    }

    void animationSpeedUsesFiniteFallbackForNaNOnSaveAndLoad()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        companion::SettingsRepository repository(path);

        companion::AppSettings settings;
        settings.animationSpeedScale = std::numeric_limits<double>::quiet_NaN();

        QVERIFY(repository.save(settings).hasValue());

        QSettings raw(path, QSettings::IniFormat);
        bool ok = false;
        const double persisted =
            raw.value(QStringLiteral("pet/animationSpeedScale")).toDouble(&ok);
        QVERIFY(ok);
        QVERIFY(std::isfinite(persisted));
        QCOMPARE(persisted, 1.15);

        raw.setValue(QStringLiteral("pet/animationSpeedScale"), QStringLiteral("nan"));
        raw.sync();

        const auto loaded = repository.load();

        QVERIFY(loaded.hasValue());
        QVERIFY(std::isfinite(loaded.value().animationSpeedScale));
        QCOMPARE(loaded.value().animationSpeedScale, 1.15);

        raw.setValue(QStringLiteral("pet/animationSpeedScale"), QStringLiteral("invalid"));
        raw.sync();

        const auto invalidLoaded = repository.load();

        QVERIFY(invalidLoaded.hasValue());
        QVERIFY(std::isfinite(invalidLoaded.value().animationSpeedScale));
        QCOMPARE(invalidLoaded.value().animationSpeedScale, 1.15);
    }

    void legacyAnimationTimingMigratesOnceThenPreservesSelection()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(QStringLiteral("pet/animationSpeedScale"), 2.0);
            raw.sync();
        }

        companion::SettingsRepository repository(path);
        const auto migrated = repository.load();

        QVERIFY(migrated.hasValue());
        QCOMPARE(migrated.value().animationSpeedScale, 1.15);

        {
            QSettings raw(path, QSettings::IniFormat);
            QCOMPARE(
                raw.value(
                    QStringLiteral("pet/animationSpeedTimingVersion"))
                    .toInt(),
                2);
            QCOMPARE(
                raw.value(
                    QStringLiteral("pet/animationSpeedScale"))
                    .toDouble(),
                1.15);
            raw.setValue(
                QStringLiteral("pet/animationSpeedScale"),
                1.75);
            raw.sync();
        }

        const auto reloaded = repository.load();

        QVERIFY(reloaded.hasValue());
        QCOMPARE(reloaded.value().animationSpeedScale, 1.75);
    }

    void unknownRelayModeFallsBackToAutomaticAndRetainsCustomRelayUrl()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(QStringLiteral("mobile/relayMode"), QStringLiteral("future"));
            raw.setValue(QStringLiteral("mobile/customRelayUrl"),
                         QStringLiteral("wss://relay.example.test/retained"));
            raw.sync();
        }

        companion::SettingsRepository repository(path);
        const auto loaded = repository.load();

        QVERIFY(loaded.hasValue());
        QCOMPARE(loaded.value().relayMode, companion::RelayMode::Automatic);
        QCOMPARE(loaded.value().customRelayUrl,
                 QStringLiteral("wss://relay.example.test/retained"));
    }

    void customRelayUrlRoundTripsWhenRelayModeIsAutomatic()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));

        companion::AppSettings expected;
        expected.relayMode = companion::RelayMode::Automatic;
        expected.customRelayUrl = QStringLiteral("wss://relay.example.test/automatic");

        QVERIFY(repository.save(expected).hasValue());
        const auto loaded = repository.load();

        QVERIFY(loaded.hasValue());
        QCOMPARE(loaded.value().relayMode, companion::RelayMode::Automatic);
        QCOMPARE(loaded.value().customRelayUrl,
                 QStringLiteral("wss://relay.example.test/automatic"));
    }

    void customRelayUrlRoundTripsWhenRelayModeIsDisabled()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));

        companion::AppSettings expected;
        expected.relayMode = companion::RelayMode::Disabled;
        expected.customRelayUrl = QStringLiteral("wss://relay.example.test/disabled");

        QVERIFY(repository.save(expected).hasValue());
        const auto loaded = repository.load();

        QVERIFY(loaded.hasValue());
        QCOMPARE(loaded.value().relayMode, companion::RelayMode::Disabled);
        QCOMPARE(loaded.value().customRelayUrl,
                 QStringLiteral("wss://relay.example.test/disabled"));
    }

    void loadReturnsReadFailedWhenIniPathIsADirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("settings-as-directory"));
        QVERIFY(QDir().mkpath(path));
        companion::SettingsRepository repository(path);

        const auto loaded = repository.load();

        QVERIFY(!loaded.hasValue());
        QCOMPARE(loaded.error().code, QStringLiteral("settings.read-failed"));
        QCOMPARE(loaded.error().context.value(QStringLiteral("path")).toString(), path);
    }

    void saveReturnsWriteFailedWhenIniPathIsADirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("settings-as-directory"));
        QVERIFY(QDir().mkpath(path));
        companion::SettingsRepository repository(path);

        const auto saved = repository.save(companion::AppSettings{});

        QVERIFY(!saved.hasValue());
        QCOMPARE(saved.error().code, QStringLiteral("settings.write-failed"));
        QCOMPARE(saved.error().context.value(QStringLiteral("path")).toString(), path);
    }

    void unknownBackdropFallsBackWithoutRewritingPreferenceFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("settings.ini"));
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(QStringLiteral("appearance/backdrop"), QStringLiteral("future"));
            raw.setValue(
                QStringLiteral("pet/animationSpeedTimingVersion"),
                2);
            raw.setValue(
                QStringLiteral("pet/animationSpeedScale"),
                1.15);
            raw.sync();
        }
        const QByteArray before = readAllBytes(path);

        companion::SettingsRepository repository(path);
        const auto loaded = repository.load();
        const QByteArray after = readAllBytes(path);

        QVERIFY(loaded.hasValue());
        QCOMPARE(loaded.value().backdrop, companion::BackdropMode::SolidBlack);
        QCOMPARE(after, before);
    }
};

QTEST_GUILESS_MAIN(SettingsRepositoryTests)
#include "SettingsRepositoryTests.moc"
