#include "codex/accounts/CodexThreadAccountBindingStore.h"

#include "codex/accounts/CodexAccountProfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace companion {
namespace {

constexpr qint64 kMaximumStoreBytes =
    4 * 1024 * 1024;

CompanionError bindingStoreError(
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

QString normalizedThreadId(
    QStringView threadId)
{
    return threadId.toString().trimmed();
}

} // namespace

CodexThreadAccountBindingStore::
    CodexThreadAccountBindingStore(
        QString filePath)
    : filePath_(
          QFileInfo(std::move(filePath))
              .absoluteFilePath())
{
    load();
}

QString CodexThreadAccountBindingStore::
    defaultFilePath()
{
    const QString root =
        QStandardPaths::writableLocation(
            QStandardPaths::
                GenericDataLocation);
    return QDir(root).filePath(
        QStringLiteral(
            "Codex Companion/"
            "codex-thread-account-bindings.json"));
}

std::optional<CompanionError>
CodexThreadAccountBindingStore::
    loadError() const
{
    QMutexLocker locker(&mutex_);
    return loadError_;
}

std::optional<QUuid>
CodexThreadAccountBindingStore::
    profileIdFor(
        QStringView threadId) const
{
    const QString normalized =
        normalizedThreadId(threadId);
    if (normalized.isEmpty()) {
        return std::nullopt;
    }
    QMutexLocker locker(&mutex_);
    const auto iterator =
        bindings_.constFind(
            normalized);
    return iterator
            == bindings_.cend()
        ? std::nullopt
        : std::optional<QUuid>(
              iterator.value());
}

bool CodexThreadAccountBindingStore::
    hasBindingsTo(
        const QUuid& profileId) const
{
    if (profileId.isNull()) {
        return false;
    }
    QMutexLocker locker(&mutex_);
    return std::any_of(
        bindings_.cbegin(),
        bindings_.cend(),
        [&profileId](
            const QUuid& candidate) {
            return candidate
                == profileId;
        });
}

Result<void>
CodexThreadAccountBindingStore::bind(
    QString threadId,
    const QUuid& profileId)
{
    threadId = threadId.trimmed();
    if (threadId.isEmpty()
        || profileId.isNull()) {
        return Result<void>::failure(
            bindingStoreError(
                QStringLiteral(
                    "codex.account_binding_invalid"),
                QStringLiteral(
                    "The Codex thread account binding is invalid."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (bindings_.value(threadId)
        == profileId) {
        return Result<void>::success();
    }
    QHash<QString, QUuid> candidate =
        bindings_;
    candidate.insert(
        threadId,
        profileId);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return persisted;
    }
    bindings_ = std::move(candidate);
    loadError_.reset();
    return Result<void>::success();
}

Result<void>
CodexThreadAccountBindingStore::remove(
    QStringView threadId)
{
    const QString normalized =
        normalizedThreadId(threadId);
    if (normalized.isEmpty()) {
        return Result<void>::failure(
            bindingStoreError(
                QStringLiteral(
                    "codex.account_binding_invalid"),
                QStringLiteral(
                    "The Codex thread ID is empty."),
                filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (!bindings_.contains(
            normalized)) {
        return Result<void>::success();
    }
    QHash<QString, QUuid> candidate =
        bindings_;
    candidate.remove(normalized);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return persisted;
    }
    bindings_ = std::move(candidate);
    loadError_.reset();
    return Result<void>::success();
}

void CodexThreadAccountBindingStore::
    load()
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
            bindingStoreError(
                QStringLiteral(
                    "codex.account_binding_store_corrupt"),
                QStringLiteral(
                    "The Codex thread account binding store could not be read."),
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
            bindingStoreError(
                QStringLiteral(
                    "codex.account_binding_store_corrupt"),
                QStringLiteral(
                    "The Codex thread account binding store is malformed."),
                filePath_);
        return;
    }

    QHash<QString, QUuid> loaded;
    const QJsonObject root =
        document.object();
    for (auto iterator = root.constBegin();
         iterator != root.constEnd();
         ++iterator) {
        const QString threadId =
            iterator.key().trimmed();
        if (threadId.isEmpty()
            || !iterator.value()
                    .isString()) {
            loadError_ =
                bindingStoreError(
                    QStringLiteral(
                        "codex.account_binding_store_corrupt"),
                    QStringLiteral(
                        "A Codex thread account binding is malformed."),
                    filePath_);
            return;
        }
        const auto profileId =
            parseCodexAccountProfileId(
                iterator.value()
                    .toString());
        if (!profileId.has_value()
            || loaded.contains(
                threadId)) {
            loadError_ =
                bindingStoreError(
                    QStringLiteral(
                        "codex.account_binding_store_corrupt"),
                    QStringLiteral(
                        "A Codex thread account binding is invalid."),
                    filePath_);
            return;
        }
        loaded.insert(
            threadId,
            *profileId);
    }

    bindings_ = std::move(loaded);
    loadError_.reset();
}

Result<void>
CodexThreadAccountBindingStore::persist(
    const QHash<QString, QUuid>&
        bindings) const
{
    QStringList threadIds =
        bindings.keys();
    std::sort(
        threadIds.begin(),
        threadIds.end());
    QJsonObject root;
    for (const QString& threadId :
         threadIds) {
        root.insert(
            threadId,
            codexAccountProfileIdString(
                bindings.value(
                    threadId)));
    }

    const QFileInfo information(
        filePath_);
    const QDir directory =
        information.dir();
    if (!directory.exists()
        && !QDir().mkpath(
            directory.absolutePath())) {
        return Result<void>::failure(
            bindingStoreError(
                QStringLiteral(
                    "codex.account_binding_store_write_failed"),
                QStringLiteral(
                    "The Codex thread account binding directory could not be created."),
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
            bindingStoreError(
                QStringLiteral(
                    "codex.account_binding_store_write_failed"),
                QStringLiteral(
                    "The Codex thread account binding store could not be written."),
                filePath_));
    }
    return Result<void>::success();
}

} // namespace companion
