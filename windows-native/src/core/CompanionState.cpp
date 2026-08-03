#include "core/CompanionState.h"

#include <QPointer>
#include <QThread>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

namespace detail {

struct CompanionStateAccessState final {
    std::mutex mutex;
    std::condition_variable accessReleased;
    QPointer<CompanionState> state;
    std::size_t activeAccesses = 0;
    bool destroying = false;
};

} // namespace detail

namespace {

class CompanionStateAccess final {
public:
    explicit CompanionStateAccess(
        std::shared_ptr<
            detail::CompanionStateAccessState>
            state)
        : state_(std::move(state))
    {
        if (state_ == nullptr) {
            return;
        }
        const std::scoped_lock lock(state_->mutex);
        if (state_->destroying
            || state_->state.isNull()) {
            state_.reset();
            return;
        }
        companionState_ = state_->state.data();
        ++state_->activeAccesses;
    }

    ~CompanionStateAccess()
    {
        release();
    }

    CompanionStateAccess(
        const CompanionStateAccess&) = delete;
    CompanionStateAccess& operator=(
        const CompanionStateAccess&) = delete;

    explicit operator bool() const noexcept
    {
        return companionState_ != nullptr;
    }

    CompanionState* state() const noexcept
    {
        return companionState_;
    }

private:
    void release() noexcept
    {
        if (state_ == nullptr) {
            companionState_ = nullptr;
            return;
        }

        bool notify = false;
        try {
            const std::scoped_lock lock(
                state_->mutex);
            if (state_->activeAccesses > 0) {
                --state_->activeAccesses;
                notify =
                    state_->activeAccesses == 0;
            }
        } catch (...) {
        }
        if (notify) {
            state_->accessReleased.notify_all();
        }
        state_.reset();
        companionState_ = nullptr;
    }

    std::shared_ptr<
        detail::CompanionStateAccessState>
        state_;
    CompanionState* companionState_ = nullptr;
};

} // namespace

CompanionState::CompanionState(QObject* parent)
    : QObject(parent),
      tasks_(this),
      accessState_(
          std::make_shared<
              detail::CompanionStateAccessState>())
{
    accessState_->state = this;
}

CompanionState::~CompanionState()
{
    std::unique_lock lock(accessState_->mutex);
    accessState_->destroying = true;
    accessState_->accessReleased.wait(
        lock,
        [state = accessState_] {
            return state->activeAccesses == 0;
        });
    accessState_->state.clear();
}

TaskListModel* CompanionState::tasks() noexcept
{
    return &tasks_;
}

bool CompanionState::tryGetOwnerThread(
    const std::shared_ptr<
        detail::CompanionStateAccessState>&
        accessState,
    QThread*& ownerThread) noexcept
{
    ownerThread = nullptr;
    try {
        CompanionStateAccess access(accessState);
        if (!access) {
            return false;
        }
        ownerThread = access.state()->thread();
        return true;
    } catch (...) {
        ownerThread = nullptr;
        return false;
    }
}

} // namespace companion
