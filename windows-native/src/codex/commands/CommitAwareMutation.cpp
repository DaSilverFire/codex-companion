#include "codex/commands/CommitAwareMutation.h"

#include <QThread>

#include <algorithm>

namespace {

class MutationObservationPool final {
public:
    MutationObservationPool()
    {
        pool.setExpiryTimeout(-1);
        pool.setMaxThreadCount(
            std::max(2, QThread::idealThreadCount()));
    }

    QThreadPool pool;
};

} // namespace

namespace companion::detail {

QThreadPool& mutationObservationPool() noexcept
{
    static MutationObservationPool holder;
    return holder.pool;
}

void CommitAwareMutationControl::
requestStopBeforeCommit() noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        if (phase_ == Phase::Open) {
            stopRequested_ = true;
        }
    } catch (...) {
    }
}

CommitAwareMutationClaim
CommitAwareMutationControl::tryCommit() noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        switch (phase_) {
        case Phase::Open:
            if (stopRequested_) {
                phase_ = Phase::Aborted;
                phase_ = Phase::Terminal;
                return CommitAwareMutationClaim::
                    Aborted;
            }
            phase_ = Phase::Committed;
            return CommitAwareMutationClaim::
                Committed;
        case Phase::Committed:
            return CommitAwareMutationClaim::
                Committed;
        case Phase::Aborted:
        case Phase::Terminal:
            return CommitAwareMutationClaim::
                Unavailable;
        }
    } catch (...) {
    }
    return CommitAwareMutationClaim::
        Unavailable;
}

bool CommitAwareMutationControl::
tryMarkTerminal() noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        if (phase_ == Phase::Aborted
            || phase_ == Phase::Terminal) {
            return false;
        }
        phase_ = Phase::Terminal;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace companion::detail
