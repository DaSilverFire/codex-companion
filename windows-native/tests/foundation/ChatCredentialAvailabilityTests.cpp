#include "app/ChatCredentialAvailability.h"
#include "platform/windows/DpapiCredentialStore.h"
#include "ui/CompanionShellViewModel.h"
#include "ui/SettingsViewModel.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

class ChatCredentialAvailabilityTests final
    : public QObject {
    Q_OBJECT

private slots:
    void settingsMutationsRefreshSelectedProviderImmediately()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        const auto credentials =
            std::make_shared<
                companion::DpapiCredentialStore>(
                    directory.filePath(
                        QStringLiteral("Credentials")),
                    [](const QString&, bool) {
                        return companion::Result<
                            void>::success();
                    });
        companion::SettingsViewModel settings(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            },
            credentials);
        companion::CompanionShellViewModel shell;
        shell.setSelectedChatModelId(
            QStringLiteral(
                "openai:gpt56Luna"));
        const auto refresh = [&] {
            QVERIFY(
                companion::
                    ChatCredentialAvailability::
                    refresh(
                        shell,
                        credentials,
                        shell.selectedChatModelId()));
        };
        connect(
            &settings,
            &companion::SettingsViewModel::
                chatCredentialsChanged,
            &shell,
            refresh);
        QSignalSpy statusSpy(
            &shell,
            &companion::CompanionShellViewModel::
                chatStatusChanged);
        QVERIFY(statusSpy.isValid());

        refresh();
        QVERIFY(shell.chatSendEnabled());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral(
                "Add an OpenAI API key in Settings"));

        QVERIFY(settings.saveOpenAIAPIKey(
            QStringLiteral("openai-secret")));
        QVERIFY(shell.chatSendEnabled());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral("OpenAI API ready"));

        QVERIFY(settings.removeOpenAIAPIKey());
        QVERIFY(shell.chatSendEnabled());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral(
                "Add an OpenAI API key in Settings"));

        shell.setSelectedChatModelId(
            QStringLiteral("lumo:automatic"));
        refresh();
        QVERIFY(shell.chatSendEnabled());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral(
                "Add a Lumo API key in Settings"));

        QVERIFY(settings.saveLumoAPIKey(
            QStringLiteral("lumo-secret")));
        QVERIFY(shell.chatSendEnabled());
        QCOMPARE(
            shell.chatStatusMessage(),
            QStringLiteral("Lumo API ready"));
        QVERIFY(statusSpy.count() >= 5);
    }
};

QTEST_GUILESS_MAIN(
    ChatCredentialAvailabilityTests)
#include "ChatCredentialAvailabilityTests.moc"
