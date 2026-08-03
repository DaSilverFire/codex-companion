#include "app/UpdateViewModel.h"

#include "update/UpdateService.h"

namespace companion {

UpdateViewModel::UpdateViewModel(
    UpdateService& service,
    QObject* parent)
    : QObject(parent),
      service_(&service)
{
    connect(
        service_,
        &UpdateService::stateChanged,
        this,
        &UpdateViewModel::changed);
    connect(
        service_,
        &UpdateService::
            runtimeErrorOccurred,
        this,
        &UpdateViewModel::
            runtimeErrorOccurred);
}

QString UpdateViewModel::phase() const
{
    return updatePhaseName(
        service_->snapshot().phase);
}

QString UpdateViewModel::title() const
{
    return service_->snapshot().title;
}

QString UpdateViewModel::detail() const
{
    return service_->snapshot().detail;
}

QString UpdateViewModel::
installedVersion() const
{
    return service_
        ->snapshot()
        .installedVersion;
}

qint64 UpdateViewModel::
installedBuild() const noexcept
{
    return service_
        ->snapshot()
        .installedBuild;
}

QString UpdateViewModel::
availableVersion() const
{
    return service_
        ->snapshot()
        .availableVersion;
}

qint64 UpdateViewModel::
availableBuild() const noexcept
{
    return service_
        ->snapshot()
        .availableBuild;
}

double UpdateViewModel::
downloadProgress() const noexcept
{
    return service_
        ->snapshot()
        .downloadProgress;
}

QString UpdateViewModel::
primaryActionText() const
{
    switch (service_->snapshot().phase) {
    case UpdatePhase::Idle:
    case UpdatePhase::UpToDate:
    case UpdatePhase::Failed:
        return QStringLiteral(
            "Check for Updates");
    case UpdatePhase::Checking:
        return QStringLiteral(
            "Checking...");
    case UpdatePhase::Available:
        return QStringLiteral(
            "Download Verified Update");
    case UpdatePhase::Downloading:
        return QStringLiteral(
            "Downloading...");
    case UpdatePhase::ReadyToInstall:
        return QStringLiteral(
            "Install and Relaunch");
    case UpdatePhase::Installing:
        return QStringLiteral(
            "Installing...");
    case UpdatePhase::Unavailable:
        return {};
    }
    return {};
}

bool UpdateViewModel::
primaryActionEnabled() const noexcept
{
    switch (service_->snapshot().phase) {
    case UpdatePhase::Idle:
    case UpdatePhase::UpToDate:
    case UpdatePhase::Available:
    case UpdatePhase::ReadyToInstall:
    case UpdatePhase::Failed:
        return true;
    case UpdatePhase::Checking:
    case UpdatePhase::Unavailable:
    case UpdatePhase::Downloading:
    case UpdatePhase::Installing:
        return false;
    }
    return false;
}

QString UpdateViewModel::
secondaryActionText() const
{
    return {};
}

QString UpdateViewModel::errorCode() const
{
    return service_
        ->snapshot()
        .errorCode;
}

bool UpdateViewModel::checkForUpdates()
{
    return publishCommandResult(
        service_->checkForUpdates());
}

bool UpdateViewModel::
downloadAvailableUpdate()
{
    return publishCommandResult(
        service_
            ->downloadAvailableUpdate());
}

bool UpdateViewModel::installReadyUpdate()
{
    return publishCommandResult(
        service_->installReadyUpdate());
}

bool UpdateViewModel::publishCommandResult(
    const Result<void>& result)
{
    if (result.hasValue()) {
        return true;
    }
    if (result.error().code
        == QStringLiteral(
            "update.invalid_state")) {
        emit runtimeErrorOccurred(
            result.error());
    }
    return false;
}

} // namespace companion
