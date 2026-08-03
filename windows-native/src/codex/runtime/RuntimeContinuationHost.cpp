#include "codex/runtime/RuntimeContinuationHost.h"

#include <QRunnable>
#include <QThread>

#include <algorithm>
#include <memory>
#include <utility>

namespace companion {

namespace {

Result<void> invalidContinuation()
{
    return Result<void>::failure({
        QStringLiteral(
            "codex.continuation_invalid"),
        QStringLiteral(
            "Codex continuation is invalid."),
        false,
        {},
    });
}

Result<void> continuationHostClosed()
{
    return Result<void>::failure({
        QStringLiteral(
            "codex.continuation_host_closed"),
        QStringLiteral(
            "Codex continuation host is closed."),
        false,
        {},
    });
}

Result<void> continuationUnavailable()
{
    return Result<void>::failure({
        QStringLiteral(
            "codex.continuation_unavailable"),
        QStringLiteral(
            "Codex continuation is unavailable."),
        false,
        {},
    });
}

} // namespace

RuntimeContinuationHost::RuntimeContinuationHost()
{
    pool_.setExpiryTimeout(-1);
    pool_.setMaxThreadCount(
        std::max(
            2,
            QThread::idealThreadCount()));
}

RuntimeContinuationHost::~RuntimeContinuationHost()
{
    stopAcceptingAndDrain();
}

Result<void> RuntimeContinuationHost::submit(
    std::function<void()> task)
{
    if (!task) {
        return invalidContinuation();
    }

    std::unique_ptr<QRunnable> runnable;
    try {
        runnable.reset(
            QRunnable::create(
                [task = std::move(task)]() mutable {
                    try {
                        task();
                    } catch (...) {
                    }
                }));
    } catch (...) {
        return continuationUnavailable();
    }
    if (runnable == nullptr) {
        return continuationUnavailable();
    }

    try {
        const std::scoped_lock lock(
            acceptanceMutex_);
        if (!accepting_) {
            return continuationHostClosed();
        }
        pool_.start(runnable.get());
        runnable.release();
        return Result<void>::success();
    } catch (...) {
        return continuationUnavailable();
    }
}

void RuntimeContinuationHost::
stopAcceptingAndDrain() noexcept
{
    try {
        const std::scoped_lock drainLock(
            drainMutex_);
        {
            const std::scoped_lock acceptanceLock(
                acceptanceMutex_);
            accepting_ = false;
        }
        pool_.waitForDone();
    } catch (...) {
    }
}

bool RuntimeContinuationHost::accepting() const noexcept
{
    try {
        const std::scoped_lock lock(
            acceptanceMutex_);
        return accepting_;
    } catch (...) {
        return false;
    }
}

} // namespace companion
