#pragma once

#include "core/Result.h"

#include <QMutex>
#include <QString>

namespace companion {

class CompanionInstallationIdentityStore final {
public:
    explicit CompanionInstallationIdentityStore(
        QString filePath =
            defaultFilePath());

    CompanionInstallationIdentityStore(
        const CompanionInstallationIdentityStore&) =
        delete;
    CompanionInstallationIdentityStore& operator=(
        const CompanionInstallationIdentityStore&) =
        delete;

    static QString defaultFilePath();

    Result<QString> loadOrCreate();
    const QString& filePath() const noexcept;

private:
    QString filePath_;
    QMutex mutex_;
    QString cached_;
};

} // namespace companion
