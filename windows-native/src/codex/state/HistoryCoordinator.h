#pragma once

#include "codex/state/HistoryModels.h"
#include "core/Result.h"

#include <QFuture>
#include <QString>
#include <QtGlobal>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace companion {

using HistoryTimePoint =
    std::chrono::steady_clock::time_point;
using HistoryClock =
    std::function<HistoryTimePoint()>;
using HistoryLoader =
    std::function<Result<HistorySnapshot>()>;
using CancellableHistoryLoader =
    std::function<Result<HistorySnapshot>(
        std::stop_token)>;

struct HistoryKey final {
    QString threadId;
    std::optional<QString> cursor;
    int limit = static_cast<int>(kDefaultMessagePageSize);

    friend bool operator==(
        const HistoryKey&,
        const HistoryKey&) = default;
};

size_t qHash(
    const HistoryKey& key,
    size_t seed = 0) noexcept;

class HistoryCancellationLease final {
public:
    ~HistoryCancellationLease();

    HistoryCancellationLease(
        const HistoryCancellationLease&) = delete;
    HistoryCancellationLease& operator=(
        const HistoryCancellationLease&) = delete;
    HistoryCancellationLease(
        HistoryCancellationLease&&) = delete;
    HistoryCancellationLease& operator=(
        HistoryCancellationLease&&) = delete;

    void requestStop() noexcept;

private:
    explicit HistoryCancellationLease(
        std::function<void()> release);

    friend class HistoryCoordinator;

    std::mutex mutex_;
    std::function<void()> release_;
};

struct HistoryLoadHandle final {
    QFuture<Result<HistorySnapshot>> future;
    std::shared_ptr<
        HistoryCancellationLease>
        cancellationLease;
};

class HistoryCoordinator final {
public:
    HistoryCoordinator();
    HistoryCoordinator(
        std::chrono::milliseconds cacheLifetime,
        qsizetype maximumCacheEntryCount,
        HistoryClock clock);
    ~HistoryCoordinator();

    HistoryCoordinator(const HistoryCoordinator&) = delete;
    HistoryCoordinator& operator=(
        const HistoryCoordinator&) = delete;
    HistoryCoordinator(HistoryCoordinator&&) noexcept = default;
    HistoryCoordinator& operator=(
        HistoryCoordinator&&) noexcept = default;

    QFuture<Result<HistorySnapshot>> load(
        const HistoryKey& key,
        HistoryLoader loader);
    HistoryLoadHandle loadCancellable(
        const HistoryKey& key,
        CancellableHistoryLoader loader);

private:
    struct State;
    HistoryLoadHandle loadInternal(
        const HistoryKey& key,
        CancellableHistoryLoader loader,
        bool legacySubscriber);

    std::shared_ptr<State> state_;
};

} // namespace companion
