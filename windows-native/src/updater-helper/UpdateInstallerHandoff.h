#pragma once

#include "core/Result.h"
#include "update/UpdateArtifactStore.h"
#include "update/UpdateManifest.h"
#include "updater-helper/UpdateInstallRequest.h"
#include "updater-helper/UpdateInstallTransaction.h"

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QUuid>

#include <chrono>
#include <functional>

namespace companion {

struct UpdateInstallerHandoffReceipt final {
    QString transactionRoot;
    QString requestPath;
    QString helperPath;
    quint32 helperProcessId = 0;

    friend bool operator==(
        const UpdateInstallerHandoffReceipt&,
        const UpdateInstallerHandoffReceipt&) =
        default;
};

struct UpdateInstallerHandoffOptions final {
    QString applicationDirectory;
    QString temporaryRoot;
    QStringList runtimeFileNames;
    std::function<QUuid()> idFactory;
    std::function<quint32()>
        currentProcessId;
    std::chrono::milliseconds
        helperReadyTimeout{
            std::chrono::seconds(10),
        };
};

struct UpdateInstallerHandoffDependencies final {
    std::function<Result<void>(QStringView)>
        createTransactionDirectory;
    std::function<
        Result<void>(
            QStringView,
            QStringView)>
        copyFile;
    std::function<
        Result<void>(
            const UpdateInstallRequest&,
            QStringView)>
        writeRequest;
    std::function<
        Result<UpdateAcknowledgementHandle>(
            QStringView)>
        prepareEvent;
    std::function<
        Result<quint32>(
            QStringView,
            const QStringList&,
            QStringView)>
        launchHelper;
    std::function<
        Result<void>(
            const UpdateAcknowledgementHandle&,
            std::chrono::milliseconds)>
        waitForEvent;
    std::function<Result<void>(quint32)>
        terminateProcess;
    std::function<void(QStringView)>
        cleanupTransactionDirectory;
};

class UpdateInstallerHandoff final {
public:
    UpdateInstallerHandoff(
        UpdateInstallerHandoffOptions options,
        UpdateInstallerHandoffDependencies
            dependencies);

    Result<UpdateInstallerHandoffReceipt>
    launch(
        const UpdateManifest& manifest,
        const VerifiedArtifact& artifact);

    static UpdateInstallerHandoff
    createProduction(
        QString applicationDirectory);

    static QStringList
    defaultRuntimeFileNames();

private:
    void cleanup(
        QStringView transactionRoot)
        const;

    UpdateInstallerHandoffOptions options_;
    UpdateInstallerHandoffDependencies
        dependencies_;
};

UpdateInstallerHandoffDependencies
productionUpdateInstallerHandoffDependencies();

} // namespace companion
