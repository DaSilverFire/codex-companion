#include "codex/state/DesktopApprovalState.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <algorithm>
#include <utility>

namespace companion {

namespace {

inline constexpr qsizetype kReadChunkBytes = 64 * 1024;
inline constexpr qsizetype kMaximumEventLines = 4'096;

const QString kCommandApprovalMethod =
    QStringLiteral("item/commandExecution/requestApproval");
const QString kFileApprovalMethod =
    QStringLiteral("item/fileChange/requestApproval");

struct LogCandidate final {
    QString root;
    QString path;
    QString session;
    QDateTime modifiedAt;
};

struct EventCacheEntry final {
    qint64 parsedByteCount = 0;
    QByteArray unfinishedLine;
    QVector<QString> eventLines;
};

QString stableLogsRoot(const QString& localAppData)
{
    return QDir(localAppData).filePath(QStringLiteral(
        "Packages/OpenAI.Codex_2p2nqsd0c76g0/"
        "LocalCache/Local/Codex/Logs"));
}

QString betaLogsRoot(const QString& localAppData)
{
    return QDir(localAppData).filePath(QStringLiteral(
        "Packages/OpenAI.CodexBeta_2p2nqsd0c76g0/"
        "LocalCache/Local/Codex/Logs"));
}

bool isHiddenRelativePath(
    const QString& root,
    const QString& path)
{
    const QString relative = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(path));
    const QStringList parts = relative.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    return std::any_of(
        parts.begin(),
        parts.end(),
        [](const QString& part) {
            return part.startsWith(QLatin1Char('.'));
        });
}

QVector<LogCandidate> eligibleCandidates(
    const QStringList& roots,
    const QDateTime& now,
    std::chrono::milliseconds maximumFileAge)
{
    const QDateTime cutoff = now.addMSecs(
        -maximumFileAge.count());
    QVector<LogCandidate> candidates;

    for (const QString& configuredRoot : roots) {
        const QString root =
            QFileInfo(configuredRoot).absoluteFilePath();
        if (!QFileInfo(root).isDir()) {
            continue;
        }

        QDirIterator iterator(
            root,
            QDir::Files | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QFileInfo info = iterator.fileInfo();
            if (!info.isFile()
                || isHiddenRelativePath(root, path)
                || info.suffix() != QStringLiteral("log")) {
                continue;
            }

            const QString filename = info.fileName();
            const qsizetype marker =
                filename.indexOf(QStringLiteral("-t0-"));
            if (marker < 0) {
                continue;
            }

            const QDateTime modifiedAt =
                info.lastModified().toUTC();
            if (!modifiedAt.isValid()
                || modifiedAt < cutoff) {
                continue;
            }

            candidates.append({
                root,
                info.absoluteFilePath(),
                filename.left(marker),
                modifiedAt,
            });
        }
    }
    return candidates;
}

QVector<QString> currentPrimaryLogPaths(
    const QStringList& roots,
    const QDateTime& now,
    std::chrono::milliseconds maximumFileAge)
{
    const QVector<LogCandidate> candidates =
        eligibleCandidates(roots, now, maximumFileAge);
    if (candidates.isEmpty()) {
        return {};
    }

    const auto newest = std::max_element(
        candidates.begin(),
        candidates.end(),
        [](const LogCandidate& left,
           const LogCandidate& right) {
            if (left.modifiedAt != right.modifiedAt) {
                return left.modifiedAt < right.modifiedAt;
            }
            if (left.root != right.root) {
                return left.root < right.root;
            }
            return left.path < right.path;
        });

    QVector<LogCandidate> selected;
    for (const LogCandidate& candidate : candidates) {
        if (candidate.root == newest->root
            && candidate.session == newest->session) {
            selected.append(candidate);
        }
    }
    std::sort(
        selected.begin(),
        selected.end(),
        [](const LogCandidate& left,
           const LogCandidate& right) {
            if (left.modifiedAt != right.modifiedAt) {
                return left.modifiedAt < right.modifiedAt;
            }
            return left.path < right.path;
        });

    QVector<QString> paths;
    paths.reserve(selected.size());
    for (const LogCandidate& candidate : selected) {
        paths.append(candidate.path);
    }
    return paths;
}

bool isApprovalEvent(const QString& line)
{
    return line.contains(
               QStringLiteral(
                   "[desktop-notifications] show approval"))
        || line.contains(
            QStringLiteral("Sending server response"));
}

void appendEventLine(
    EventCacheEntry& entry,
    QByteArrayView bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString line = decoder.decode(bytes);
    if (decoder.hasError() || !isApprovalEvent(line)) {
        return;
    }

    entry.eventLines.append(line);
    if (entry.eventLines.size() > kMaximumEventLines) {
        entry.eventLines.remove(
            0,
            entry.eventLines.size() - kMaximumEventLines);
    }
}

QVector<QString> eventLines(
    QHash<QString, EventCacheEntry>& cache,
    const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const qint64 fileSize = file.size();
    EventCacheEntry entry = cache.value(path);
    if (fileSize < entry.parsedByteCount) {
        entry = {};
        cache.insert(path, entry);
    }
    if (fileSize <= entry.parsedByteCount) {
        return entry.eventLines;
    }
    if (!file.seek(entry.parsedByteCount)) {
        return entry.eventLines;
    }

    QByteArray buffer = std::move(entry.unfinishedLine);
    entry.unfinishedLine.clear();
    while (entry.parsedByteCount < fileSize) {
        const qint64 remaining =
            fileSize - entry.parsedByteCount;
        const qint64 requested = std::min(
            remaining,
            static_cast<qint64>(kReadChunkBytes));
        const QByteArray chunk = file.read(requested);
        if (chunk.isEmpty()) {
            break;
        }

        entry.parsedByteCount += chunk.size();
        buffer.append(chunk);

        qsizetype completeBytes = 0;
        while (true) {
            const qsizetype newline =
                buffer.indexOf('\n', completeBytes);
            if (newline < 0) {
                break;
            }
            appendEventLine(
                entry,
                QByteArrayView(buffer)
                    .sliced(
                        completeBytes,
                        newline - completeBytes));
            completeBytes = newline + 1;
        }
        if (completeBytes > 0) {
            buffer.remove(0, completeBytes);
        }
    }

    entry.unfinishedLine = std::move(buffer);
    cache.insert(path, entry);
    return entry.eventLines;
}

std::optional<QString> token(
    QStringView name,
    const QString& line)
{
    const QString marker =
        name.toString() + QLatin1Char('=');
    const qsizetype markerIndex = line.indexOf(marker);
    if (markerIndex < 0) {
        return std::nullopt;
    }

    const qsizetype start =
        markerIndex + marker.size();
    qsizetype end = start;
    while (end < line.size()
           && !line.at(end).isSpace()) {
        ++end;
    }
    if (end == start) {
        return std::nullopt;
    }
    return line.mid(start, end - start);
}

std::optional<qint64> integerToken(
    QStringView name,
    const QString& line)
{
    const auto value = token(name, line);
    if (!value.has_value()) {
        return std::nullopt;
    }
    bool valid = false;
    const qint64 parsed = value->toLongLong(&valid);
    return valid
        ? std::optional<qint64>(parsed)
        : std::nullopt;
}

std::optional<PendingApprovalMethod> methodForKind(
    const QString& kind)
{
    if (kind == QStringLiteral("commandExecution")) {
        return PendingApprovalMethod::CommandExecution;
    }
    if (kind == QStringLiteral("fileChange")) {
        return PendingApprovalMethod::FileChange;
    }
    return std::nullopt;
}

bool isApprovalResponse(const QString& line)
{
    return line.contains(
               QStringLiteral("method=")
               + kCommandApprovalMethod)
        || line.contains(
            QStringLiteral("method=")
            + kFileApprovalMethod);
}

QHash<QString, PendingApproval> parsePendingApprovals(
    QVector<QString> lines)
{
    std::sort(lines.begin(), lines.end());

    QHash<QString, PendingApproval> pendingByThread;
    QHash<qint64, QString> threadByRequestId;
    for (const QString& line : lines) {
        if (line.contains(QStringLiteral(
                "[desktop-notifications] show approval"))) {
            const auto threadId = token(
                u"conversationId", line);
            const auto requestId = integerToken(
                u"requestId", line);
            const auto kind = token(u"kind", line);
            const auto method = kind.has_value()
                ? methodForKind(*kind)
                : std::nullopt;
            if (!threadId.has_value()
                || !requestId.has_value()
                || !method.has_value()) {
                continue;
            }

            const auto previous =
                pendingByThread.constFind(*threadId);
            if (previous != pendingByThread.constEnd()) {
                threadByRequestId.remove(
                    previous->requestId);
            }
            pendingByThread.insert(
                *threadId,
                PendingApproval{
                    *threadId,
                    *requestId,
                    *method,
                    std::nullopt,
                });
            threadByRequestId.insert(
                *requestId, *threadId);
            continue;
        }

        if (!line.contains(
                QStringLiteral("Sending server response"))
            || !isApprovalResponse(line)) {
            continue;
        }
        const auto requestId = integerToken(u"id", line);
        if (!requestId.has_value()) {
            continue;
        }
        const auto thread = threadByRequestId.find(
            *requestId);
        if (thread == threadByRequestId.end()) {
            continue;
        }

        const QString threadId = thread.value();
        threadByRequestId.erase(thread);
        const auto pending =
            pendingByThread.find(threadId);
        if (pending != pendingByThread.end()
            && pending->requestId == *requestId) {
            pendingByThread.erase(pending);
        }
    }
    return pendingByThread;
}

} // namespace

ApprovalPromotionTracker::ApprovalPromotionTracker(
    std::chrono::milliseconds holdDuration)
    : holdDuration_(holdDuration)
{
}

QSet<QString> ApprovalPromotionTracker::promotedThreadIds(
    const QSet<QString>& pendingThreadIds,
    const QDateTime& now)
{
    std::lock_guard lock(mutex_);

    if (previousPendingThreadIds_.has_value()) {
        const QSet<QString> resolved =
            *previousPendingThreadIds_ - pendingThreadIds;
        for (const QString& threadId : resolved) {
            holdUntilByThreadId_.insert(
                threadId,
                now.addMSecs(holdDuration_.count()));
        }
    }
    previousPendingThreadIds_ = pendingThreadIds;

    for (auto iterator = holdUntilByThreadId_.begin();
         iterator != holdUntilByThreadId_.end();) {
        if (iterator.value() <= now) {
            iterator = holdUntilByThreadId_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    QSet<QString> promoted = pendingThreadIds;
    for (auto iterator = holdUntilByThreadId_.cbegin();
         iterator != holdUntilByThreadId_.cend();
         ++iterator) {
        promoted.insert(iterator.key());
    }
    return promoted;
}

struct DesktopApprovalStateStore::State final {
    State(
        QStringList roots,
        std::chrono::milliseconds maximumAge)
        : logRoots(std::move(roots)),
          maximumFileAge(maximumAge)
    {
    }

    std::mutex mutex;
    QStringList logRoots;
    std::chrono::milliseconds maximumFileAge;
    QHash<QString, EventCacheEntry> eventCache;
    ApprovalPromotionTracker promotionTracker;
};

DesktopApprovalStateStore::DesktopApprovalStateStore(
    const CodexEnvironment& environment)
    : DesktopApprovalStateStore(
          environment.localAppData.isEmpty()
              ? QStringList{}
              : QStringList{
                    stableLogsRoot(
                        environment.localAppData),
                    betaLogsRoot(
                        environment.localAppData),
                },
          std::chrono::hours(48))
{
}

DesktopApprovalStateStore::DesktopApprovalStateStore(
    QStringList logRoots,
    std::chrono::milliseconds maximumFileAge)
    : state_(std::make_unique<State>(
          std::move(logRoots),
          maximumFileAge))
{
}

DesktopApprovalStateStore::~DesktopApprovalStateStore() =
    default;

TaskProjectionState DesktopApprovalStateStore::snapshot(
    const QDateTime& now)
{
    std::lock_guard lock(state_->mutex);
    const QDateTime capturedNow = now.isValid()
        ? now.toUTC()
        : QDateTime::currentDateTimeUtc();

    QVector<QString> lines;
    const QVector<QString> paths = currentPrimaryLogPaths(
        state_->logRoots,
        capturedNow,
        state_->maximumFileAge);
    for (const QString& path : paths) {
        lines.append(eventLines(
            state_->eventCache, path));
    }

    QHash<QString, PendingApproval> pending =
        parsePendingApprovals(std::move(lines));
    QSet<QString> pendingThreadIds;
    pendingThreadIds.reserve(pending.size());
    for (auto iterator = pending.cbegin();
         iterator != pending.cend();
         ++iterator) {
        pendingThreadIds.insert(iterator.key());
    }
    QSet<QString> promoted =
        state_->promotionTracker.promotedThreadIds(
            pendingThreadIds, capturedNow);

    return {
        std::move(pending),
        std::move(pendingThreadIds),
        std::move(promoted),
    };
}

} // namespace companion
