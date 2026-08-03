#include "codex/state/HistoryCoordinator.h"

#include <QHash>
#include <QPromise>
#include <QSet>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <mutex>
#include <utility>

namespace companion {

namespace {

using HistoryResult = Result<HistorySnapshot>;

HistoryResult loaderFailure(
    QString message,
    const QString& detail = {})
{
    QVariantMap context;
    if (!detail.isEmpty()) {
        context.insert(QStringLiteral("detail"), detail);
    }
    return HistoryResult::failure({
        QStringLiteral("codex.history_load_failed"),
        std::move(message),
        false,
        std::move(context),
    });
}

HistoryResult invokeLoader(
    const CancellableHistoryLoader& loader,
    std::stop_token stopToken)
{
    if (!loader) {
        return loaderFailure(
            QStringLiteral("No Codex history loader was provided."));
    }

    try {
        return loader(stopToken);
    } catch (const std::exception& error) {
        return loaderFailure(
            QStringLiteral("The Codex history loader failed."),
            QString::fromUtf8(error.what()));
    } catch (...) {
        return loaderFailure(
            QStringLiteral("The Codex history loader failed."));
    }
}

QFuture<HistoryResult> readyFuture(HistoryResult result)
{
    QPromise<HistoryResult> promise;
    promise.start();
    promise.addResult(std::move(result));
    promise.finish();
    return promise.future();
}

} // namespace

struct HistoryCoordinator::State final {
    struct CacheEntry final {
        HistorySnapshot snapshot;
        HistoryTimePoint expiresAt;
    };

    struct InFlightEntry final {
        std::shared_ptr<
            QPromise<HistoryResult>>
            promise;
        QFuture<HistoryResult> future;
        std::stop_source stopSource;
        QSet<quint64> cancellableSubscribers;
        bool legacySubscriber = false;
        bool stopRequested = false;
    };

    State(
        std::chrono::milliseconds requestedLifetime,
        qsizetype requestedMaximumEntries,
        HistoryClock requestedClock)
        : cacheLifetime(std::max(
              requestedLifetime,
              std::chrono::milliseconds::zero())),
          maximumCacheEntryCount(std::max<qsizetype>(
              1, requestedMaximumEntries)),
          clock(requestedClock
                    ? std::move(requestedClock)
                    : HistoryClock([] {
                          return std::chrono::
                              steady_clock::now();
                      }))
    {
    }

    void removeExpiredEntries(HistoryTimePoint now)
    {
        for (auto iterator = cache.begin();
             iterator != cache.end();) {
            if (iterator.value().expiresAt <= now) {
                iterator = cache.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void trimCacheIfNeeded()
    {
        while (cache.size() > maximumCacheEntryCount) {
            auto earliest = cache.begin();
            for (auto iterator = std::next(cache.begin());
                 iterator != cache.end();
                 ++iterator) {
                if (iterator.value().expiresAt <
                    earliest.value().expiresAt) {
                    earliest = iterator;
                }
            }
            cache.erase(earliest);
        }
    }

    std::mutex mutex;
    QHash<
        HistoryKey,
        std::shared_ptr<InFlightEntry>>
        inFlight;
    QHash<HistoryKey, CacheEntry> cache;
    std::chrono::milliseconds cacheLifetime;
    qsizetype maximumCacheEntryCount = 8;
    HistoryClock clock;
    quint64 nextSubscriberId = 1;
};

size_t qHash(
    const HistoryKey& key,
    size_t seed) noexcept
{
    if (key.cursor.has_value()) {
        return qHashMulti(
            seed,
            key.threadId,
            true,
            *key.cursor,
            key.limit);
    }
    return qHashMulti(
        seed,
        key.threadId,
        false,
        key.limit);
}

HistoryCoordinator::HistoryCoordinator()
    : HistoryCoordinator(
          std::chrono::milliseconds(750),
          8,
          [] {
              return std::chrono::steady_clock::now();
          })
{
}

HistoryCoordinator::HistoryCoordinator(
    std::chrono::milliseconds cacheLifetime,
    qsizetype maximumCacheEntryCount,
    HistoryClock clock)
    : state_(std::make_shared<State>(
          cacheLifetime,
          maximumCacheEntryCount,
          std::move(clock)))
{
}

HistoryCoordinator::~HistoryCoordinator() = default;

HistoryCancellationLease::
HistoryCancellationLease(
    std::function<void()> release)
    : release_(std::move(release))
{
}

HistoryCancellationLease::
~HistoryCancellationLease()
{
    requestStop();
}

void HistoryCancellationLease::requestStop() noexcept
{
    std::function<void()> release;
    try {
        {
            const std::scoped_lock lock(mutex_);
            release = std::move(release_);
        }
        if (release) {
            release();
        }
    } catch (...) {
    }
}

QFuture<Result<HistorySnapshot>> HistoryCoordinator::load(
    const HistoryKey& key,
    HistoryLoader loader)
{
    if (!loader) {
        return readyFuture(
            invokeLoader({}, {}));
    }
    HistoryLoadHandle handle =
        loadInternal(
            key,
            [loader = std::move(loader)](
                std::stop_token) {
                return loader();
            },
            true);
    return std::move(handle.future);
}

HistoryLoadHandle
HistoryCoordinator::loadCancellable(
    const HistoryKey& key,
    CancellableHistoryLoader loader)
{
    if (!loader) {
        return {
            readyFuture(
                invokeLoader({}, {})),
            {},
        };
    }
    return loadInternal(
        key,
        std::move(loader),
        false);
}

HistoryLoadHandle HistoryCoordinator::loadInternal(
    const HistoryKey& key,
    CancellableHistoryLoader loader,
    bool legacySubscriber)
{
    const std::shared_ptr<State> state = state_;
    std::shared_ptr<State::InFlightEntry>
        entry;
    std::shared_ptr<
        HistoryCancellationLease>
        cancellationLease;
    const auto subscribeCancellable =
        [&state, &key](
            const std::shared_ptr<
                State::InFlightEntry>&
                subscribedEntry) {
            const quint64 subscriberId =
                state->nextSubscriberId;
            ++state->nextSubscriberId;
            if (state->nextSubscriberId == 0) {
                state->nextSubscriberId = 1;
            }
            subscribedEntry
                ->cancellableSubscribers
                .insert(subscriberId);
            return std::shared_ptr<
                HistoryCancellationLease>(
                new HistoryCancellationLease(
                    [weakState =
                         std::weak_ptr<State>(
                             state),
                     key,
                     weakEntry =
                         std::weak_ptr<
                             State::InFlightEntry>(
                             subscribedEntry),
                     subscriberId] {
                        const auto state =
                            weakState.lock();
                        const auto entry =
                            weakEntry.lock();
                        if (state == nullptr
                            || entry == nullptr) {
                            return;
                        }

                        bool requestStop = false;
                        {
                            const std::scoped_lock lock(
                                state->mutex);
                            const auto current =
                                state->inFlight
                                    .constFind(key);
                            if (current
                                    == state->inFlight
                                        .constEnd()
                                || current.value()
                                    != entry
                                || !entry
                                        ->cancellableSubscribers
                                        .remove(
                                            subscriberId)) {
                                return;
                            }
                            if (entry
                                    ->cancellableSubscribers
                                    .isEmpty()
                                && !entry
                                        ->legacySubscriber
                                && !entry
                                        ->stopRequested) {
                                entry->stopRequested =
                                    true;
                                requestStop = true;
                            }
                        }
                        if (requestStop) {
                            entry->stopSource
                                .request_stop();
                        }
                    }));
        };

    {
        const std::scoped_lock lock(state->mutex);
        const HistoryTimePoint now = state->clock();
        state->removeExpiredEntries(now);

        const auto cached = state->cache.constFind(key);
        if (cached != state->cache.constEnd() &&
            cached.value().expiresAt > now) {
            return {
                readyFuture(
                    HistoryResult::success(
                        cached.value().snapshot)),
                {},
            };
        }

        const auto inFlight =
            state->inFlight.find(key);
        if (inFlight != state->inFlight.end()
            && !inFlight.value()->stopRequested) {
            entry = inFlight.value();
            if (legacySubscriber) {
                entry->legacySubscriber = true;
            } else {
                cancellationLease =
                    subscribeCancellable(entry);
            }
            return {
                entry->future,
                std::move(cancellationLease),
            };
        }

        entry =
            std::make_shared<
                State::InFlightEntry>();
        entry->promise =
            std::make_shared<
                QPromise<HistoryResult>>();
        entry->promise->start();
        entry->future =
            entry->promise->future();
        entry->legacySubscriber =
            legacySubscriber;
        if (!legacySubscriber) {
            cancellationLease =
                subscribeCancellable(entry);
        }
        state->inFlight.insert(key, entry);
    }

    QThreadPool::globalInstance()->start(
        [state,
         key,
         loader = std::move(loader),
         entry]() mutable {
            HistoryResult result =
                invokeLoader(
                    loader,
                    entry->stopSource.get_token());
            {
                const std::scoped_lock lock(state->mutex);
                const auto current =
                    state->inFlight.find(key);
                const bool ownsEntry =
                    current
                        != state->inFlight.end()
                    && current.value() == entry;
                if (ownsEntry) {
                    state->inFlight.erase(current);
                }
                if (ownsEntry
                    && result.hasValue()
                    && !entry->stopRequested) {
                    state->cache.insert(
                        key,
                        State::CacheEntry{
                            result.value(),
                            state->clock() +
                                state->cacheLifetime,
                        });
                    state->trimCacheIfNeeded();
                }
            }
            entry->promise->addResult(
                std::move(result));
            entry->promise->finish();
        });

    return {
        entry->future,
        std::move(cancellationLease),
    };
}

} // namespace companion
