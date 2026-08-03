#pragma once

#include "core/Result.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringView>
#include <QUuid>

#include <optional>

namespace companion {

class CodexThreadAccountBindingStore final {
public:
    explicit CodexThreadAccountBindingStore(
        QString filePath);

    static QString defaultFilePath();

    std::optional<CompanionError>
    loadError() const;
    std::optional<QUuid> profileIdFor(
        QStringView threadId) const;
    bool hasBindingsTo(
        const QUuid& profileId) const;

    Result<void> bind(
        QString threadId,
        const QUuid& profileId);
    Result<void> remove(
        QStringView threadId);

private:
    void load();
    Result<void> persist(
        const QHash<QString, QUuid>&
            bindings) const;

    QString filePath_;
    mutable QMutex mutex_;
    QHash<QString, QUuid> bindings_;
    std::optional<CompanionError>
        loadError_;
};

} // namespace companion
