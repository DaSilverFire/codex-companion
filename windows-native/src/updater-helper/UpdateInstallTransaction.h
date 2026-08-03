#pragma once

#include "core/Result.h"
#include "updater-helper/UpdateInstallRequest.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>

namespace companion {

struct UpdateRegistryValueSnapshot final {
    QString name;
    quint32 type = 0;
    QByteArray data;

    friend bool operator==(
        const UpdateRegistryValueSnapshot&,
        const UpdateRegistryValueSnapshot&) =
        default;
};

struct UpdateRegistrySnapshot final {
    bool existed = false;
    QVector<UpdateRegistryValueSnapshot>
        values;

    friend bool operator==(
        const UpdateRegistrySnapshot&,
        const UpdateRegistrySnapshot&) =
        default;
};

struct UpdateShortcutSnapshot final {
    bool existed = false;
    QByteArray bytes;
    quint32 attributes = 0;
    quint64 creationTime = 0;
    quint64 accessTime = 0;
    quint64 writeTime = 0;

    friend bool operator==(
        const UpdateShortcutSnapshot&,
        const UpdateShortcutSnapshot&) =
        default;
};

struct UpdateInstallStateSnapshot final {
    UpdateRegistrySnapshot
        uninstallRegistry;
    UpdateShortcutSnapshot
        startMenuShortcut;

    friend bool operator==(
        const UpdateInstallStateSnapshot&,
        const UpdateInstallStateSnapshot&) =
        default;
};

using UpdateAcknowledgementHandle =
    std::shared_ptr<void>;

struct UpdateInstallTransactionOptions final {
    QString transactionRoot;
    std::chrono::milliseconds
        parentExitTimeout{
            std::chrono::seconds(30),
        };
    std::chrono::milliseconds
        acknowledgementTimeout{
            std::chrono::seconds(20),
        };
};

struct UpdateInstallTransactionDependencies final {
    std::function<
        Result<UpdateAcknowledgementHandle>(
            QStringView)>
        prepareAcknowledgement;
    std::function<
        Result<void>(
            quint32,
            std::chrono::milliseconds)>
        waitForParentExit;
    std::function<
        Result<void>(
            const UpdateInstallRequest&)>
        signalReady;
    std::function<
        Result<void>(
            const UpdateInstallRequest&)>
        revalidateInstaller;
    std::function<
        Result<UpdateInstallStateSnapshot>(
            const UpdateInstallRequest&)>
        snapshotState;
    std::function<
        Result<void>(
            const UpdateInstallRequest&)>
        moveInstallToRollback;
    std::function<
        Result<int>(
            const UpdateInstallRequest&,
            QStringView)>
        runInstaller;
    std::function<
        Result<void>(
            const UpdateInstallRequest&)>
        verifyInstalledTree;
    std::function<
        Result<void>(
            const UpdateInstallRequest&)>
        afterReplacement;
    std::function<
        Result<quint32>(
            QStringView,
            const QStringList&,
            QStringView)>
        launchApplication;
    std::function<
        Result<void>(
            const UpdateAcknowledgementHandle&,
            std::chrono::milliseconds)>
        waitForAcknowledgement;
    std::function<
        Result<void>(quint32)>
        terminateProcess;
    std::function<
        Result<void>(
            const UpdateInstallRequest&,
            const UpdateInstallStateSnapshot&)>
        restoreRollback;
    std::function<
        Result<void>(
            const UpdateInstallRequest&)>
        commitRollback;
};

class UpdateInstallTransaction final {
public:
    UpdateInstallTransaction(
        UpdateInstallTransactionOptions
            options,
        UpdateInstallTransactionDependencies
            dependencies);

    Result<void> run(
        const UpdateInstallRequest& request);

    static UpdateInstallTransaction
    createProduction(
        QString transactionRoot);

    static QString installedExecutablePath(
        const UpdateInstallRequest& request);

private:
    Result<void> rollback(
        const UpdateInstallRequest& request,
        const UpdateInstallStateSnapshot&
            snapshot,
        const CompanionError& cause,
        quint32 newProcessId);

    UpdateInstallTransactionOptions
        options_;
    UpdateInstallTransactionDependencies
        dependencies_;
};

UpdateInstallTransactionDependencies
productionUpdateInstallDependencies();

} // namespace companion
