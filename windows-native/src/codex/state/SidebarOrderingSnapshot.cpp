#include "codex/state/SidebarOrderingSnapshot.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <limits>
#include <utility>

namespace companion {

namespace {

QVector<QString> stringArray(const QJsonValue& value)
{
    if (!value.isArray()) {
        return {};
    }

    QVector<QString> result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const QJsonValue& item : array) {
        if (!item.isString()) {
            return {};
        }
        result.append(item.toString());
    }
    return result;
}

QHash<QString, QString> stringMap(const QJsonValue& value)
{
    if (!value.isObject()) {
        return {};
    }

    QHash<QString, QString> result;
    const QJsonObject object = value.toObject();
    for (auto iterator = object.constBegin();
         iterator != object.constEnd();
         ++iterator) {
        if (!iterator.value().isString()) {
            return {};
        }
        result.insert(iterator.key(), iterator.value().toString());
    }
    return result;
}

QSet<QString> stringSet(const QJsonValue& value)
{
    QSet<QString> result;
    const QVector<QString> values = stringArray(value);
    for (const QString& item : values) {
        result.insert(item);
    }
    return result;
}

} // namespace

SidebarOrderingSnapshot::SidebarOrderingSnapshot(
    QVector<QString> pinnedThreadIds,
    QVector<QString> projectOrder,
    QSet<QString> projectlessThreadIds,
    QHash<QString, QString> threadWorkspaceRoots,
    QHash<QString, QString> workspaceRootLabels)
    : projectlessThreadIds_(std::move(projectlessThreadIds))
{
    for (qsizetype index = 0; index < pinnedThreadIds.size(); ++index) {
        const QString& threadId = pinnedThreadIds.at(index);
        if (!pinnedRanks_.contains(threadId)) {
            pinnedRanks_.insert(threadId, static_cast<int>(index));
        }
    }

    for (const QString& project : projectOrder) {
        const QString display = normalizedPath(project);
        const QString key = pathKey(display);
        if (key.isEmpty() || projectRanks_.contains(key)) {
            continue;
        }

        projectRoots_.append(display);
        projectRanks_.insert(key, projectRanks_.size());
        projectRootDisplays_.insert(key, display);
    }

    for (auto iterator = threadWorkspaceRoots.constBegin();
         iterator != threadWorkspaceRoots.constEnd();
         ++iterator) {
        const QString display = normalizedPath(iterator.value());
        if (!display.isEmpty()) {
            threadWorkspaceRoots_.insert(iterator.key(), display);
        }
    }

    for (auto iterator = workspaceRootLabels.constBegin();
         iterator != workspaceRootLabels.constEnd();
         ++iterator) {
        const QString display = normalizedPath(iterator.key());
        const QString key = pathKey(display);
        const QString label = iterator.value().trimmed();
        if (key.isEmpty() || label.isEmpty()) {
            continue;
        }

        workspaceRootLabels_.insert(key, label);
        if (!projectRootDisplays_.contains(key)) {
            projectRootDisplays_.insert(key, display);
        }
    }
}

SidebarOrderingSnapshot SidebarOrderingSnapshot::read(
    const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return {};
    }

    const QJsonObject object = document.object();
    return SidebarOrderingSnapshot(
        stringArray(object.value(QStringLiteral("pinned-thread-ids"))),
        stringArray(object.value(QStringLiteral("project-order"))),
        stringSet(object.value(QStringLiteral("projectless-thread-ids"))),
        stringMap(object.value(
            QStringLiteral("thread-workspace-root-hints"))),
        stringMap(object.value(
            QStringLiteral("electron-workspace-root-labels"))));
}

BridgeTaskGroup SidebarOrderingSnapshot::taskGroup(
    const QString& threadId,
    const std::optional<QString>& cwd) const
{
    if (projectlessThreadIds_.contains(threadId) ||
        isCodexDocumentsPath(cwd)) {
        return {
            TaskGroupKind::Chats,
            QStringLiteral("Chats"),
            std::nullopt,
        };
    }

    std::optional<QString> normalizedCwd;
    if (cwd.has_value()) {
        const QString value = normalizedPath(*cwd);
        if (!value.isEmpty()) {
            normalizedCwd = value;
        }
    }

    std::optional<QString> knownRoot;
    const auto hinted = threadWorkspaceRoots_.constFind(threadId);
    if (hinted != threadWorkspaceRoots_.constEnd()) {
        const QString key = pathKey(hinted.value());
        if (projectRanks_.contains(key) ||
            workspaceRootLabels_.contains(key)) {
            knownRoot = projectRootDisplays_.value(
                key, hinted.value());
        }
    }

    if (!knownRoot.has_value() && normalizedCwd.has_value()) {
        qsizetype longestMatch = -1;
        for (const QString& root : projectRoots_) {
            if (!pathContains(*normalizedCwd, root)) {
                continue;
            }
            const qsizetype length = pathKey(root).size();
            if (length > longestMatch) {
                longestMatch = length;
                knownRoot = root;
            }
        }
    }

    if (knownRoot.has_value()) {
        const QString key = pathKey(*knownRoot);
        return {
            TaskGroupKind::Project,
            workspaceRootLabels_.value(
                key, fallbackProjectTitle(*knownRoot)),
            *knownRoot,
        };
    }

    if (!normalizedCwd.has_value()) {
        return {
            TaskGroupKind::Chats,
            QStringLiteral("Chats"),
            std::nullopt,
        };
    }

    const QString key = pathKey(*normalizedCwd);
    return {
        TaskGroupKind::Project,
        workspaceRootLabels_.value(
            key, fallbackProjectTitle(*normalizedCwd)),
        *normalizedCwd,
    };
}

bool SidebarOrderingSnapshot::orders(
    const Entry& left,
    const Entry& right) const
{
    const int leftAttention = left.attentionPromoted ? 0 : 1;
    const int rightAttention = right.attentionPromoted ? 0 : 1;
    if (leftAttention != rightAttention) {
        return leftAttention < rightAttention;
    }

    const int leftPin = pinRank(left.threadId);
    const int rightPin = pinRank(right.threadId);
    if (leftPin != rightPin) {
        return leftPin < rightPin;
    }

    const int leftProject = projectRank(left.threadId, left.cwd);
    const int rightProject = projectRank(right.threadId, right.cwd);
    if (leftProject != rightProject) {
        return leftProject < rightProject;
    }

    if (left.statusRank != right.statusRank) {
        return left.statusRank < right.statusRank;
    }

    if (left.updatedAt.has_value() != right.updatedAt.has_value()) {
        return left.updatedAt.has_value();
    }
    if (left.updatedAt.has_value() &&
        *left.updatedAt != *right.updatedAt) {
        return *left.updatedAt > *right.updatedAt;
    }

    return left.threadId.value_or(QString()) <
        right.threadId.value_or(QString());
}

bool SidebarOrderingSnapshot::isPinned(
    const QString& threadId) const
{
    return pinnedRanks_.contains(threadId);
}

QString SidebarOrderingSnapshot::normalizedPath(QString path)
{
    path = path.trimmed();
    if (path.isEmpty()) {
        return {};
    }

    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(std::move(path))));
}

QString SidebarOrderingSnapshot::pathKey(const QString& path)
{
    const QString normalized = normalizedPath(path);
    return normalized.isEmpty()
        ? QString()
        : QDir::fromNativeSeparators(normalized).toCaseFolded();
}

bool SidebarOrderingSnapshot::pathContains(
    const QString& path,
    const QString& root)
{
    const QString pathValue = pathKey(path);
    const QString rootValue = pathKey(root);
    if (pathValue.isEmpty() || rootValue.isEmpty()) {
        return false;
    }
    if (pathValue == rootValue) {
        return true;
    }

    QString prefix = rootValue;
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix.append(QLatin1Char('/'));
    }
    return pathValue.startsWith(prefix);
}

QString SidebarOrderingSnapshot::fallbackProjectTitle(
    const QString& path)
{
    const QString normalized =
        QDir::fromNativeSeparators(normalizedPath(path));
    const QString title = QFileInfo(normalized).fileName();
    return title.isEmpty() ? path : title;
}

bool SidebarOrderingSnapshot::isCodexDocumentsPath(
    const std::optional<QString>& path)
{
    if (!path.has_value()) {
        return false;
    }

    const QString normalized =
        QDir::fromNativeSeparators(normalizedPath(*path));
    const QStringList components =
        normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype index = 0; index + 1 < components.size(); ++index) {
        if (components.at(index).compare(
                QStringLiteral("Documents"),
                Qt::CaseInsensitive) == 0 &&
            components.at(index + 1).compare(
                QStringLiteral("Codex"),
                Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

int SidebarOrderingSnapshot::pinRank(
    const std::optional<QString>& threadId) const
{
    if (!threadId.has_value()) {
        return std::numeric_limits<int>::max();
    }
    return pinnedRanks_.value(
        *threadId, std::numeric_limits<int>::max());
}

int SidebarOrderingSnapshot::projectRank(
    const std::optional<QString>& threadId,
    const std::optional<QString>& cwd) const
{
    if (threadId.has_value()) {
        const auto hinted =
            threadWorkspaceRoots_.constFind(*threadId);
        if (hinted != threadWorkspaceRoots_.constEnd()) {
            const auto rank = projectRanks_.constFind(
                pathKey(hinted.value()));
            if (rank != projectRanks_.constEnd()) {
                return rank.value();
            }
        }
    }

    int matchingRank = std::numeric_limits<int>::max();
    if (cwd.has_value()) {
        for (auto iterator = projectRanks_.constBegin();
             iterator != projectRanks_.constEnd();
             ++iterator) {
            if (pathContains(*cwd, iterator.key())) {
                matchingRank =
                    std::min(matchingRank, iterator.value());
            }
        }
    }
    if (matchingRank != std::numeric_limits<int>::max()) {
        return matchingRank;
    }

    const int projectCount = projectRanks_.size();
    if (threadId.has_value() &&
        projectlessThreadIds_.contains(*threadId)) {
        return projectCount + 1;
    }
    return projectCount;
}

} // namespace companion
