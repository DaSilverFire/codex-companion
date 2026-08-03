#pragma once

#include "core/CompanionError.h"
#include "core/Result.h"

#include <QObject>
#include <QString>

namespace companion {

class UpdateService;

class UpdateViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QString phase
        READ phase
        NOTIFY changed)
    Q_PROPERTY(
        QString title
        READ title
        NOTIFY changed)
    Q_PROPERTY(
        QString detail
        READ detail
        NOTIFY changed)
    Q_PROPERTY(
        QString installedVersion
        READ installedVersion
        NOTIFY changed)
    Q_PROPERTY(
        qint64 installedBuild
        READ installedBuild
        NOTIFY changed)
    Q_PROPERTY(
        QString availableVersion
        READ availableVersion
        NOTIFY changed)
    Q_PROPERTY(
        qint64 availableBuild
        READ availableBuild
        NOTIFY changed)
    Q_PROPERTY(
        double downloadProgress
        READ downloadProgress
        NOTIFY changed)
    Q_PROPERTY(
        QString primaryActionText
        READ primaryActionText
        NOTIFY changed)
    Q_PROPERTY(
        bool primaryActionEnabled
        READ primaryActionEnabled
        NOTIFY changed)
    Q_PROPERTY(
        QString secondaryActionText
        READ secondaryActionText
        NOTIFY changed)
    Q_PROPERTY(
        QString errorCode
        READ errorCode
        NOTIFY changed)

public:
    explicit UpdateViewModel(
        UpdateService& service,
        QObject* parent = nullptr);

    QString phase() const;
    QString title() const;
    QString detail() const;
    QString installedVersion() const;
    qint64 installedBuild() const noexcept;
    QString availableVersion() const;
    qint64 availableBuild() const noexcept;
    double downloadProgress() const
        noexcept;
    QString primaryActionText() const;
    bool primaryActionEnabled() const
        noexcept;
    QString secondaryActionText() const;
    QString errorCode() const;

    Q_INVOKABLE bool checkForUpdates();
    Q_INVOKABLE bool
    downloadAvailableUpdate();
    Q_INVOKABLE bool installReadyUpdate();

signals:
    void changed();
    void runtimeErrorOccurred(
        companion::CompanionError error);

private:
    bool publishCommandResult(
        const Result<void>& result);

    UpdateService* service_ = nullptr;
};

} // namespace companion
