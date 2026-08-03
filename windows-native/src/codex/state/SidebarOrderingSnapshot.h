#pragma once

#include "codex/models/BridgeModels.h"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace companion {

class SidebarOrderingSnapshot final {
public:
    struct Entry final {
        std::optional<QString> threadId;
        std::optional<QString> cwd;
        int statusRank = 3;
        std::optional<QDateTime> updatedAt;
        bool attentionPromoted = false;
    };

    SidebarOrderingSnapshot(
        QVector<QString> pinnedThreadIds = {},
        QVector<QString> projectOrder = {},
        QSet<QString> projectlessThreadIds = {},
        QHash<QString, QString> threadWorkspaceRoots = {},
        QHash<QString, QString> workspaceRootLabels = {});

    static SidebarOrderingSnapshot read(const QString& path);

    BridgeTaskGroup taskGroup(
        const QString& threadId,
        const std::optional<QString>& cwd) const;

    bool orders(const Entry& left, const Entry& right) const;
    bool isPinned(const QString& threadId) const;

private:
    static QString normalizedPath(QString path);
    static QString pathKey(const QString& path);
    static bool pathContains(
        const QString& path,
        const QString& root);
    static QString fallbackProjectTitle(const QString& path);
    static bool isCodexDocumentsPath(
        const std::optional<QString>& path);

    int pinRank(const std::optional<QString>& threadId) const;
    int projectRank(
        const std::optional<QString>& threadId,
        const std::optional<QString>& cwd) const;

    QHash<QString, int> pinnedRanks_;
    QVector<QString> projectRoots_;
    QHash<QString, int> projectRanks_;
    QHash<QString, QString> projectRootDisplays_;
    QSet<QString> projectlessThreadIds_;
    QHash<QString, QString> threadWorkspaceRoots_;
    QHash<QString, QString> workspaceRootLabels_;
};

} // namespace companion
