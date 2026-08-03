#pragma once

#include "core/Result.h"

#include <QFuture>
#include <QPromise>
#include <QThread>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

template <typename T>
struct CommitAwareMutationHandle final {
    QFuture<Result<T>> terminalFuture;
    std::function<void()> requestStopBeforeCommit;
};

namespace detail {

enum class CommitAwareMutationClaim {
    Committed,
    Aborted,
    Unavailable,
};

class CommitAwareMutationControl final {
public:
    void requestStopBeforeCommit() noexcept;
    CommitAwareMutationClaim tryCommit() noexcept;
    bool tryMarkTerminal() noexcept;

private:
    enum class Phase {
        Open,
        Committed,
        Aborted,
        Terminal,
    };

    std::mutex mutex_;
    Phase phase_ = Phase::Open;
    bool stopRequested_ = false;
};

QThreadPool& mutationObservationPool() noexcept;

inline CompanionError mutationCanceledError()
{
    return {
        QStringLiteral(
            "codex.operation_canceled"),
        QStringLiteral(
            "The Codex operation was canceled."),
        false,
        {},
    };
}

} // namespace detail

template <typename T>
class CommitAwareMutation final
    : public std::enable_shared_from_this<
          CommitAwareMutation<T>> {
public:
    static std::shared_ptr<
        CommitAwareMutation<T>> create()
    {
        auto promise =
            std::make_shared<
                QPromise<Result<T>>>();
        promise->start();
        return std::shared_ptr<
            CommitAwareMutation<T>>(
            new CommitAwareMutation<T>(
                std::move(promise)));
    }

    CommitAwareMutationHandle<T> handle()
    {
        const auto self =
            this->shared_from_this();
        return {
            future_,
            [self] {
                self->control_
                    .requestStopBeforeCommit();
            },
        };
    }

    bool tryCommit() noexcept
    {
        const detail::CommitAwareMutationClaim claim =
            control_.tryCommit();
        if (claim
            == detail::CommitAwareMutationClaim::
                Aborted) {
            completePromise(
                Result<T>::failure(
                    detail::
                        mutationCanceledError()));
            return false;
        }
        return claim
            == detail::CommitAwareMutationClaim::
                Committed;
    }

    bool finish(Result<T> result) noexcept
    {
        return finishWithCallbacks(
            std::move(result),
            {},
            {});
    }

    bool finishWithCallbacks(
        Result<T> result,
        std::function<void()> beforeAddResult,
        std::function<void()> afterAddResultBeforeFinish) noexcept
    {
        if (!control_.tryMarkTerminal()) {
            return false;
        }
        completePromise(
            std::move(result),
            std::move(beforeAddResult),
            std::move(afterAddResultBeforeFinish));
        return true;
    }

    CommitAwareMutation(
        const CommitAwareMutation&) = delete;
    CommitAwareMutation& operator=(
        const CommitAwareMutation&) = delete;

private:
    explicit CommitAwareMutation(
        std::shared_ptr<
            QPromise<Result<T>>> promise)
        : promise_(std::move(promise)),
          future_(promise_->future())
    {
    }

    void completePromise(
        Result<T> result,
        std::function<void()> beforeAddResult = {},
        std::function<void()> afterAddResultBeforeFinish = {}) noexcept
    {
        try {
            invoke(beforeAddResult);
            promise_->addResult(
                std::move(result));
            invoke(afterAddResultBeforeFinish);
            promise_->finish();
        } catch (...) {
            try {
                promise_->finish();
            } catch (...) {
            }
        }
    }

    static void invoke(
        const std::function<void()>& callback) noexcept
    {
        if (!callback) {
            return;
        }
        try {
            callback();
        } catch (...) {
        }
    }

    detail::CommitAwareMutationControl control_;
    std::shared_ptr<
        QPromise<Result<T>>> promise_;
    QFuture<Result<T>> future_;
};

template <typename T>
QFuture<Result<T>> cancellationDetachedMutationFuture(
    QFuture<Result<T>> terminalFuture)
{
    auto promise =
        std::make_shared<QPromise<Result<T>>>();
    promise->start();
    QFuture<Result<T>> future = promise->future();
    try {
        detail::mutationObservationPool().start(
            [terminalFuture = std::move(terminalFuture),
             promise]() mutable {
                try {
                    while (!terminalFuture.isFinished()
                           && !promise->isCanceled()) {
                        QThread::msleep(1);
                    }
                    if (!promise->isCanceled()
                        && !terminalFuture.isCanceled()
                        && terminalFuture.resultCount() == 1) {
                        promise->addResult(
                            terminalFuture.result());
                    }
                } catch (...) {
                }
                try {
                    promise->finish();
                } catch (...) {
                }
            });
    } catch (...) {
        try {
            promise->finish();
        } catch (...) {
        }
    }
    return future;
}

} // namespace companion
