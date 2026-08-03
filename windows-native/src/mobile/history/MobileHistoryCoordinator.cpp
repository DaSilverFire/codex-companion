#include "mobile/history/MobileHistoryCoordinator.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

namespace companion {
namespace {

Result<HistorySnapshot> deliveryFailure()
{
    return Result<HistorySnapshot>::failure({
        QStringLiteral("mobile.history_delivery_failed"),
        QStringLiteral(
            "Codex Companion could not deliver task history."),
        true,
        {},
    });
}

Result<QString> revisionFailure()
{
    return Result<QString>::failure({
        QStringLiteral("mobile.history_revision_failed"),
        QStringLiteral(
            "Codex Companion could not read the task history revision."),
        true,
        {},
    });
}

} // namespace

struct MobileHistoryCoordinator::State final {
    explicit State(
        std::shared_ptr<HistoryCoordinator>
            requestedCoordinator)
        : coordinator(
              requestedCoordinator != nullptr
                  ? std::move(requestedCoordinator)
                  : std::make_shared<
                        HistoryCoordinator>())
    {
    }

    std::shared_ptr<HistoryCoordinator> coordinator;
    std::mutex mutex;
    std::uint64_t selectionGeneration = 0;
    QString selectedThreadId;
};

MobileHistoryCoordinator::MobileHistoryCoordinator()
    : MobileHistoryCoordinator(
          std::shared_ptr<HistoryCoordinator>{})
{
}

MobileHistoryCoordinator::MobileHistoryCoordinator(
    std::shared_ptr<HistoryCoordinator> coordinator)
    : state_(
          std::make_shared<State>(
              std::move(coordinator)))
{
}

HistoryKey MobileHistoryCoordinator::normalizedKey(
    MobileHistoryKey key)
{
    return {
        std::move(key.threadId),
        std::move(key.cursor),
        std::clamp(
            key.limit,
            1,
            static_cast<int>(kMaximumPageSize)),
    };
}

QFuture<Result<HistorySnapshot>>
MobileHistoryCoordinator::load(
    MobileHistoryKey key,
    HistoryLoader operation)
{
    return state_->coordinator->load(
        normalizedKey(std::move(key)),
        std::move(operation));
}

QFuture<Result<HistorySnapshot>>
MobileHistoryCoordinator::loadSelected(
    MobileHistoryKey key,
    HistoryLoader operation,
    QObject* receiver,
    MobileHistoryCompletion completion)
{
    const HistoryKey normalized =
        normalizedKey(std::move(key));
    std::uint64_t generation = 0;
    {
        const std::scoped_lock lock(
            state_->mutex);
        ++state_->selectionGeneration;
        if (state_->selectionGeneration == 0) {
            ++state_->selectionGeneration;
        }
        generation =
            state_->selectionGeneration;
        state_->selectedThreadId =
            normalized.threadId;
    }

    QFuture<Result<HistorySnapshot>> future =
        state_->coordinator->load(
            normalized,
            std::move(operation));
    if (receiver == nullptr || !completion) {
        return future;
    }

    const std::weak_ptr<State> weakState =
        state_;
    const QString threadId =
        normalized.threadId;
    const QPointer<QObject> guardedReceiver(
        receiver);
    auto attachWatcher =
        [future,
         weakState,
         generation,
         threadId,
         guardedReceiver,
         completion =
             std::move(completion)]() mutable {
            QObject* const context =
                guardedReceiver.data();
            if (context == nullptr) {
                return;
            }
            auto* watcher =
                new QFutureWatcher<
                    Result<HistorySnapshot>>(
                    context);
            QObject::connect(
                watcher,
                &QFutureWatcherBase::finished,
                context,
                [watcher,
                 weakState,
                 generation,
                 threadId,
                 completion =
                     std::move(completion)]() mutable {
                    Result<HistorySnapshot> result =
                        deliveryFailure();
                    try {
                        const auto completed =
                            watcher->future();
                        if (completed.isValid()
                            && !completed.isCanceled()
                            && completed.resultCount()
                                == 1) {
                            result =
                                completed.result();
                        }
                    } catch (...) {
                    }
                    watcher->deleteLater();

                    const auto state =
                        weakState.lock();
                    if (state == nullptr) {
                        return;
                    }
                    bool current = false;
                    {
                        const std::scoped_lock lock(
                            state->mutex);
                        current =
                            state
                                ->selectionGeneration
                                == generation
                            && state
                                   ->selectedThreadId
                                == threadId;
                    }
                    if (!current) {
                        return;
                    }
                    try {
                        completion(
                            std::move(result));
                    } catch (...) {
                    }
                },
                Qt::QueuedConnection);
            watcher->setFuture(future);
        };

    if (QThread::currentThread()
        == receiver->thread()) {
        attachWatcher();
    } else {
        QMetaObject::invokeMethod(
            receiver,
            std::move(attachWatcher),
            Qt::QueuedConnection);
    }
    return future;
}

Result<QString>
MobileHistoryCoordinator::revisionForFile(
    const QString& path)
{
    QFileInfo information(path);
    information.setCaching(false);
    information.refresh();
    const QDateTime modified =
        information.lastModified();
    if (!information.exists()
        || !information.isFile()
        || information.size() < 0
        || !modified.isValid()) {
        return revisionFailure();
    }
    return Result<QString>::success(
        QString::number(information.size())
        + QLatin1Char(':')
        + QString::number(
            modified.toMSecsSinceEpoch()));
}

} // namespace companion
