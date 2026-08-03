#include "codex/accounts/CodexAccountProfileStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace companion {
namespace {

constexpr qint64 kMaximumStoreBytes =
    4 * 1024 * 1024;

CompanionError profileStoreError(
    QString code,
    QString message,
    const QString& path)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {QStringLiteral("path"),
             path},
        },
    };
}

std::optional<QString> normalizedLabel(
    QString label)
{
    label = label.trimmed();
    if (label.isEmpty()) {
        return std::nullopt;
    }
    return label;
}

} // namespace

CodexAccountProfileStore::
    CodexAccountProfileStore(
        QString filePath,
        IdGenerator idGenerator)
    : filePath_(
          QFileInfo(std::move(filePath))
              .absoluteFilePath()),
      idGenerator_(
          std::move(idGenerator))
{
    if (!idGenerator_) {
        idGenerator_ =
            QUuid::createUuid;
    }
    load();
}

QString CodexAccountProfileStore::
    defaultFilePath()
{
    const QString root =
        QStandardPaths::writableLocation(
            QStandardPaths::
                GenericDataLocation);
    return QDir(root).filePath(
        QStringLiteral(
            "Codex Companion/"
            "codex-account-profiles.json"));
}

std::optional<CompanionError>
CodexAccountProfileStore::loadError()
    const
{
    QMutexLocker locker(&mutex_);
    return loadError_;
}

QVector<CodexAccountProfile>
CodexAccountProfileStore::profiles()
    const
{
    QMutexLocker locker(&mutex_);
    return profiles_;
}

std::optional<QUuid>
CodexAccountProfileStore::
    selectedProfileId() const
{
    QMutexLocker locker(&mutex_);
    return selectedProfileId_;
}

std::optional<CodexAccountProfile>
CodexAccountProfileStore::
    selectedProfile() const
{
    QMutexLocker locker(&mutex_);
    if (!selectedProfileId_
             .has_value()) {
        return std::nullopt;
    }
    const auto iterator =
        std::find_if(
            profiles_.cbegin(),
            profiles_.cend(),
            [this](
                const CodexAccountProfile&
                    profile) {
                return profile.id
                    == *selectedProfileId_;
            });
    return iterator
            == profiles_.cend()
        ? std::nullopt
        : std::optional<
              CodexAccountProfile>(
              *iterator);
}

std::optional<CodexAccountProfile>
CodexAccountProfileStore::profile(
    const QUuid& id) const
{
    if (id.isNull()) {
        return std::nullopt;
    }
    QMutexLocker locker(&mutex_);
    const auto iterator =
        std::find_if(
            profiles_.cbegin(),
            profiles_.cend(),
            [&id](
                const CodexAccountProfile&
                    candidate) {
                return candidate.id == id;
            });
    return iterator
            == profiles_.cend()
        ? std::nullopt
        : std::optional<
              CodexAccountProfile>(
              *iterator);
}

Result<CodexAccountProfile>
CodexAccountProfileStore::add(
    QString label)
{
    const auto normalized =
        normalizedLabel(
            std::move(label));
    if (!normalized.has_value()) {
        return Result<
            CodexAccountProfile>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_label_invalid"),
                QStringLiteral(
                    "The Codex account profile label is empty."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    const QUuid id =
        idGenerator_();
    if (id.isNull()
        || std::any_of(
            profiles_.cbegin(),
            profiles_.cend(),
            [&id](
                const CodexAccountProfile&
                    profile) {
                return profile.id == id;
            })) {
        return Result<
            CodexAccountProfile>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_id_invalid"),
                QStringLiteral(
                    "A unique Codex account profile ID could not be created."),
                filePath_));
    }

    QVector<CodexAccountProfile>
        candidate = profiles_;
    CodexAccountProfile profile{
        id,
        *normalized,
    };
    candidate.append(profile);
    const bool selectingFirstProfile =
        profiles_.isEmpty()
        && !selectedProfileId_.has_value();
    const std::optional<QUuid>
        nextSelected =
            selectingFirstProfile
        ? std::optional<QUuid>(id)
        : selectedProfileId_;
    const auto persisted =
        persist(
            candidate,
            nextSelected);
    if (!persisted.hasValue()) {
        return Result<
            CodexAccountProfile>::failure(
            persisted.error());
    }

    profiles_ = std::move(candidate);
    selectedProfileId_ =
        nextSelected;
    loadError_.reset();
    return Result<
        CodexAccountProfile>::success(
        std::move(profile));
}

Result<bool>
CodexAccountProfileStore::remove(
    const QUuid& id)
{
    if (id.isNull()) {
        return Result<bool>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_id_invalid"),
                QStringLiteral(
                    "The Codex account profile ID is empty."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    QVector<CodexAccountProfile>
        candidate = profiles_;
    const auto iterator =
        std::find_if(
            candidate.begin(),
            candidate.end(),
            [&id](
                const CodexAccountProfile&
                    profile) {
                return profile.id == id;
            });
    if (iterator == candidate.end()) {
        return Result<bool>::success(
            false);
    }
    candidate.erase(iterator);

    std::optional<QUuid> nextSelected =
        selectedProfileId_;
    if (nextSelected == id) {
        nextSelected =
            candidate.isEmpty()
            ? std::nullopt
            : std::optional<QUuid>(
                  candidate.constFirst().id);
    }
    const auto persisted =
        persist(
            candidate,
            nextSelected);
    if (!persisted.hasValue()) {
        return Result<bool>::failure(
            persisted.error());
    }

    profiles_ = std::move(candidate);
    selectedProfileId_ =
        nextSelected;
    loadError_.reset();
    return Result<bool>::success(true);
}

Result<void>
CodexAccountProfileStore::select(
    const QUuid& id)
{
    if (id.isNull()) {
        return Result<void>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_id_invalid"),
                QStringLiteral(
                    "The selected Codex account profile ID is empty."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    const bool exists =
        std::any_of(
            profiles_.cbegin(),
            profiles_.cend(),
            [&id](
                const CodexAccountProfile&
                    profile) {
                return profile.id == id;
            });
    if (!exists) {
        return Result<void>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_missing"),
                QStringLiteral(
                    "The selected Codex account profile does not exist."),
                filePath_));
    }
    if (selectedProfileId_ == id) {
        return Result<void>::success();
    }

    const auto persisted =
        persist(profiles_, id);
    if (!persisted.hasValue()) {
        return persisted;
    }
    selectedProfileId_ = id;
    loadError_.reset();
    return Result<void>::success();
}

Result<void>
CodexAccountProfileStore::selectCurrentAccount()
{
    QMutexLocker locker(&mutex_);
    if (!selectedProfileId_.has_value()) {
        return Result<void>::success();
    }

    const auto persisted =
        persist(profiles_, std::nullopt);
    if (!persisted.hasValue()) {
        return persisted;
    }
    selectedProfileId_.reset();
    loadError_.reset();
    return Result<void>::success();
}

void CodexAccountProfileStore::load()
{
    const QFileInfo information(
        filePath_);
    if (!information.exists()) {
        return;
    }

    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)
        || file.size() < 0
        || file.size()
            > kMaximumStoreBytes) {
        loadError_ =
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_store_corrupt"),
                QStringLiteral(
                    "The Codex account profile store could not be read."),
                filePath_);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            file.readAll(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        loadError_ =
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_store_corrupt"),
                QStringLiteral(
                    "The Codex account profile store is malformed."),
                filePath_);
        return;
    }

    const QJsonObject root =
        document.object();
    const QJsonValue version =
        root.value(
            QStringLiteral("version"));
    const QJsonValue profilesValue =
        root.value(
            QStringLiteral("profiles"));
    if (!version.isDouble()
        || version.toInteger(-1) != 1
        || !profilesValue.isArray()) {
        loadError_ =
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_store_corrupt"),
                QStringLiteral(
                    "The Codex account profile store version is invalid."),
                filePath_);
        return;
    }

    QVector<CodexAccountProfile> loaded;
    QSet<QUuid> loadedIds;
    for (const QJsonValue value :
         profilesValue.toArray()) {
        if (!value.isObject()) {
            loadError_ =
                profileStoreError(
                    QStringLiteral(
                        "codex.account_profile_store_corrupt"),
                    QStringLiteral(
                        "A Codex account profile is malformed."),
                    filePath_);
            return;
        }
        const QJsonObject object =
            value.toObject();
        const QJsonValue idValue =
            object.value(
                QStringLiteral("id"));
        const QJsonValue labelValue =
            object.value(
                QStringLiteral("label"));
        if (!idValue.isString()
            || !labelValue.isString()) {
            loadError_ =
                profileStoreError(
                    QStringLiteral(
                        "codex.account_profile_store_corrupt"),
                    QStringLiteral(
                        "A Codex account profile is incomplete."),
                    filePath_);
            return;
        }
        const auto id =
            parseCodexAccountProfileId(
                idValue.toString());
        const auto label =
            normalizedLabel(
                labelValue.toString());
        if (!id.has_value()
            || !label.has_value()) {
            continue;
        }
        if (loadedIds.contains(*id)) {
            continue;
        }
        loadedIds.insert(*id);
        loaded.append({
            *id,
            *label,
        });
    }

    std::optional<QUuid> selected;
    const QJsonValue selectedValue =
        root.value(
            QStringLiteral(
                "selectedProfileId"));
    if (selectedValue.isString()) {
        selected =
            parseCodexAccountProfileId(
                selectedValue.toString());
    } else if (!selectedValue
                    .isUndefined()
               && !selectedValue
                       .isNull()) {
        loadError_ =
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_store_corrupt"),
                QStringLiteral(
                    "The selected Codex account profile is malformed."),
                filePath_);
        return;
    }
    if (selected.has_value()
        && !loadedIds.contains(
            *selected)) {
        selected =
            loaded.isEmpty()
            ? std::nullopt
            : std::optional<QUuid>(
                  loaded.constFirst().id);
    }

    profiles_ = std::move(loaded);
    selectedProfileId_ =
        selected;
    loadError_.reset();
}

Result<void>
CodexAccountProfileStore::persist(
    const QVector<CodexAccountProfile>&
        profiles,
    std::optional<QUuid>
        selectedProfileId) const
{
    QJsonArray encodedProfiles;
    for (const CodexAccountProfile&
             profile :
         profiles) {
        encodedProfiles.append(
            QJsonObject{
                {QStringLiteral("id"),
                 codexAccountProfileIdString(
                     profile.id)},
                {QStringLiteral("label"),
                 profile.label},
            });
    }

    QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("profiles"),
         encodedProfiles},
    };
    root.insert(
        QStringLiteral(
            "selectedProfileId"),
        selectedProfileId.has_value()
            ? QJsonValue(
                  codexAccountProfileIdString(
                      *selectedProfileId))
            : QJsonValue(QJsonValue::Null));

    const QFileInfo information(
        filePath_);
    const QDir directory =
        information.dir();
    if (!directory.exists()
        && !QDir().mkpath(
            directory.absolutePath())) {
        return Result<void>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_store_write_failed"),
                QStringLiteral(
                    "The Codex account profile directory could not be created."),
                filePath_));
    }

    const QByteArray output =
        QJsonDocument(root).toJson(
            QJsonDocument::Indented);
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(output)
            != output.size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<void>::failure(
            profileStoreError(
                QStringLiteral(
                    "codex.account_profile_store_write_failed"),
                QStringLiteral(
                    "The Codex account profile store could not be written."),
                filePath_));
    }
    return Result<void>::success();
}

} // namespace companion
