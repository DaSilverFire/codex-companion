#pragma once

#include "codex/accounts/CodexAccountProfile.h"
#include "core/Result.h"

#include <QMutex>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace companion {

class CodexAccountProfileStore final {
public:
    using IdGenerator =
        std::function<QUuid()>;

    explicit CodexAccountProfileStore(
        QString filePath,
        IdGenerator idGenerator =
            QUuid::createUuid);

    static QString defaultFilePath();

    std::optional<CompanionError>
    loadError() const;
    QVector<CodexAccountProfile>
    profiles() const;
    std::optional<QUuid>
    selectedProfileId() const;
    std::optional<CodexAccountProfile>
    selectedProfile() const;
    std::optional<CodexAccountProfile>
    profile(const QUuid& id) const;

    Result<CodexAccountProfile> add(
        QString label);
    Result<bool> remove(const QUuid& id);
    Result<void> select(const QUuid& id);
    Result<void> selectCurrentAccount();

private:
    void load();
    Result<void> persist(
        const QVector<CodexAccountProfile>&
            profiles,
        std::optional<QUuid>
            selectedProfileId) const;

    QString filePath_;
    IdGenerator idGenerator_;
    mutable QMutex mutex_;
    QVector<CodexAccountProfile>
        profiles_;
    std::optional<QUuid>
        selectedProfileId_;
    std::optional<CompanionError>
        loadError_;
};

} // namespace companion
