#pragma once

#include "core/Result.h"

#include <QThreadPool>

#include <functional>
#include <mutex>

namespace companion {

class RuntimeContinuationHost final {
public:
    RuntimeContinuationHost();
    ~RuntimeContinuationHost();

    RuntimeContinuationHost(
        const RuntimeContinuationHost&) = delete;
    RuntimeContinuationHost& operator=(
        const RuntimeContinuationHost&) = delete;

    Result<void> submit(
        std::function<void()> task);
    void stopAcceptingAndDrain() noexcept;
    bool accepting() const noexcept;

private:
    QThreadPool pool_;
    mutable std::mutex acceptanceMutex_;
    std::mutex drainMutex_;
    bool accepting_ = true;
};

} // namespace companion
