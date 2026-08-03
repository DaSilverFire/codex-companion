#include "codex/chat/WindowsOnDeviceChatBackendInternal.h"

#include <QFuture>
#include <QPromise>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace companion {

namespace {

CompanionError backendError(
    QString code,
    QString message,
    bool retryable = false)
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        {},
    };
}

CompanionError consentRequiredError()
{
    return backendError(
        QStringLiteral(
            "chat.on_device_consent_required"),
        QStringLiteral(
            "Allow the on-device model download before using Windows on-device chat."));
}

CompanionError unavailableError()
{
    return backendError(
        QStringLiteral(
            "chat.on_device_unavailable"),
        QStringLiteral(
            "The Windows on-device chat provider is unavailable."));
}

CompanionError stoppingError()
{
    return backendError(
        QStringLiteral(
            "chat.on_device_stopping"),
        QStringLiteral(
            "The Windows on-device chat provider is stopping."));
}

CompanionError canceledError()
{
    return backendError(
        QStringLiteral("foundry.canceled"),
        QStringLiteral(
            "On-device model preparation was canceled."));
}

CompanionError preparationFailure()
{
    return backendError(
        QStringLiteral(
            "foundry.prepare_failed"),
        QStringLiteral(
            "The Windows on-device model could not be prepared."),
        true);
}

CompanionError sendFailure()
{
    return backendError(
        QStringLiteral(
            "chat.on_device_failed"),
        QStringLiteral(
            "The Windows on-device chat provider failed."),
        true);
}

QFuture<Result<void>> readyFuture(
    Result<void> result)
{
    QPromise<Result<void>> promise;
    promise.start();
    promise.addResult(std::move(result));
    promise.finish();
    return promise.future();
}

double normalizedProgress(double value) noexcept
{
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 100.0);
}

struct StatusObserverSlot final {
    explicit StatusObserverSlot(
        std::function<void(
            WindowsOnDeviceChatStatus)>
            requestedObserver)
        : observer(
              std::move(requestedObserver))
    {
    }

    std::atomic_bool active = true;
    std::function<void(
        WindowsOnDeviceChatStatus)>
        observer;
};

struct WindowsOnDeviceBackendState final {
    explicit WindowsOnDeviceBackendState(
        std::shared_ptr<
            detail::
                WindowsOnDeviceChatDriver>
            requestedDriver)
        : driver(std::move(requestedDriver))
    {
        worker.setMaxThreadCount(1);
        worker.setExpiryTimeout(-1);
    }

    std::mutex mutex;
    QThreadPool worker;
    std::shared_ptr<
        detail::WindowsOnDeviceChatDriver>
        driver;
    WindowsOnDeviceChatStatus status;
    std::map<
        quint64,
        std::shared_ptr<StatusObserverSlot>>
        observers;
    quint64 nextObserverId = 1;
    bool preparing = false;
    bool stopping = false;
    bool shutdownQueued = false;
    quint64 preparationGeneration = 0;
    std::stop_source preparationStopSource;
    QFuture<Result<void>> preparationFuture;
};

using StatusObservers =
    std::vector<
        std::shared_ptr<
            StatusObserverSlot>>;

void normalizeStatus(
    WindowsOnDeviceChatStatus& status,
    bool stopping) noexcept
{
    status.progressPercent =
        normalizedProgress(
            status.progressPercent);
    status.supportsAttachments = false;
    if (stopping) {
        status.phase =
            WindowsOnDeviceChatPhase::
                Stopping;
        status.available = false;
        return;
    }
    if (!status.downloadConsentGranted) {
        status.phase =
            WindowsOnDeviceChatPhase::
                ConsentRequired;
        status.available = false;
        status.progressPercent = 0.0;
        return;
    }
    status.available =
        status.phase
        == WindowsOnDeviceChatPhase::Ready;
    if (status.available) {
        status.progressPercent = 100.0;
    }
}

bool sameStatusWithoutRevision(
    WindowsOnDeviceChatStatus first,
    WindowsOnDeviceChatStatus second)
{
    first.revision = 0;
    second.revision = 0;
    return first == second;
}

StatusObservers observersLocked(
    const WindowsOnDeviceBackendState&
        state)
{
    StatusObservers observers;
    observers.reserve(
        state.observers.size());
    for (const auto& [id, observer] :
         state.observers) {
        Q_UNUSED(id);
        observers.push_back(observer);
    }
    return observers;
}

void notifyObservers(
    const StatusObservers& observers,
    const WindowsOnDeviceChatStatus&
        status) noexcept
{
    for (const auto& slot : observers) {
        if (slot == nullptr
            || !slot->active.load()) {
            continue;
        }
        try {
            slot->observer(status);
        } catch (...) {
        }
    }
}

template <typename Mutator>
void mutateStatus(
    const std::shared_ptr<
        WindowsOnDeviceBackendState>&
        state,
    Mutator mutator)
{
    StatusObservers observers;
    std::optional<
        WindowsOnDeviceChatStatus>
        published;
    {
        const std::scoped_lock lock(
            state->mutex);
        WindowsOnDeviceChatStatus next =
            state->status;
        mutator(*state, next);
        normalizeStatus(
            next,
            state->stopping);
        if (sameStatusWithoutRevision(
                state->status,
                next)) {
            return;
        }
        if (state->status.revision
            == std::numeric_limits<
                quint64>::max()) {
            return;
        }
        next.revision =
            state->status.revision + 1;
        state->status = next;
        observers = observersLocked(*state);
        published = next;
    }
    notifyObservers(
        observers,
        *published);
}

void publishPreparationProgress(
    const std::weak_ptr<
        WindowsOnDeviceBackendState>&
        weakState,
    quint64 generation,
    WindowsOnDeviceChatPhase phase,
    double progressPercent)
{
    const auto state = weakState.lock();
    if (state == nullptr) {
        return;
    }
    mutateStatus(
        state,
        [generation,
         phase,
         progressPercent](
            WindowsOnDeviceBackendState&
                current,
            WindowsOnDeviceChatStatus&
                status) {
            if (current.stopping
                || !current.preparing
                || current
                       .preparationGeneration
                    != generation
                || !status
                        .downloadConsentGranted) {
                return;
            }
            status.phase = phase;
            status.available = false;
            status.progressPercent =
                progressPercent;
        });
}

class StatusSubscription final
    : public
          WindowsOnDeviceChatStatusSubscription {
public:
    StatusSubscription(
        std::weak_ptr<
            WindowsOnDeviceBackendState>
            state,
        quint64 identifier,
        std::weak_ptr<StatusObserverSlot>
            slot)
        : state_(std::move(state)),
          identifier_(identifier),
          slot_(std::move(slot))
    {
    }

    ~StatusSubscription() override
    {
        const auto slot = slot_.lock();
        if (slot != nullptr) {
            slot->active.store(false);
        }
        const auto state = state_.lock();
        if (state == nullptr) {
            return;
        }
        const std::scoped_lock lock(
            state->mutex);
        state->observers.erase(
            identifier_);
    }

private:
    std::weak_ptr<
        WindowsOnDeviceBackendState>
        state_;
    quint64 identifier_ = 0;
    std::weak_ptr<StatusObserverSlot> slot_;
};

Result<void> normalizedPreparationResult(
    Result<void> result,
    bool canceled)
{
    if (canceled) {
        return Result<void>::failure(
            canceledError());
    }
    return result;
}

void completePreparation(
    const std::shared_ptr<
        WindowsOnDeviceBackendState>&
        state,
    quint64 generation,
    const std::stop_token stopToken,
    Result<void> result,
    const std::shared_ptr<
        QPromise<Result<void>>>& promise)
{
    StatusObservers observers;
    std::optional<
        WindowsOnDeviceChatStatus>
        published;
    bool canceled = stopToken.stop_requested();
    {
        const std::scoped_lock lock(
            state->mutex);
        if (state->preparationGeneration
            != generation) {
            canceled = true;
        } else {
            state->preparing = false;
            state->preparationFuture = {};

            WindowsOnDeviceChatStatus next =
                state->status;
            if (state->stopping) {
                canceled = true;
                next.phase =
                    WindowsOnDeviceChatPhase::
                        Stopping;
            } else if (!next
                            .downloadConsentGranted) {
                canceled = true;
                next.phase =
                    WindowsOnDeviceChatPhase::
                        ConsentRequired;
            } else if (result.hasValue()
                       && !canceled) {
                next.phase =
                    WindowsOnDeviceChatPhase::
                        Ready;
                next.progressPercent = 100.0;
            } else {
                next.phase =
                    WindowsOnDeviceChatPhase::
                        Failed;
                next.progressPercent = 0.0;
            }
            normalizeStatus(
                next,
                state->stopping);
            if (!sameStatusWithoutRevision(
                    state->status,
                    next)
                && state->status.revision
                    != std::numeric_limits<
                        quint64>::max()) {
                next.revision =
                    state->status.revision
                    + 1;
                state->status = next;
                observers =
                    observersLocked(*state);
                published = next;
            }
        }
    }

    if (published.has_value()) {
        notifyObservers(
            observers,
            *published);
    }
    try {
        promise->addResult(
            normalizedPreparationResult(
                std::move(result),
                canceled));
        promise->finish();
    } catch (...) {
        try {
            promise->finish();
        } catch (...) {
        }
    }
}

void runPreparation(
    const std::shared_ptr<
        WindowsOnDeviceBackendState>&
        state,
    quint64 generation,
    std::stop_token stopToken,
    const std::shared_ptr<
        QPromise<Result<void>>>& promise)
{
    Result<void> result =
        Result<void>::failure(
            preparationFailure());
    try {
        result = state->driver->prepare(
            [weakState =
                 std::weak_ptr<
                     WindowsOnDeviceBackendState>(
                     state),
             generation](
                WindowsOnDeviceChatPhase
                    phase,
                double progressPercent) {
                publishPreparationProgress(
                    weakState,
                    generation,
                    phase,
                    progressPercent);
            },
            stopToken);
    } catch (...) {
        result = Result<void>::failure(
            preparationFailure());
    }
    completePreparation(
        state,
        generation,
        stopToken,
        std::move(result),
        promise);
}

class WindowsOnDeviceChatBackendImpl final
    : public WindowsOnDeviceChatBackend {
public:
    explicit WindowsOnDeviceChatBackendImpl(
        std::shared_ptr<
            detail::
                WindowsOnDeviceChatDriver>
            driver)
        : state_(
              std::make_shared<
                  WindowsOnDeviceBackendState>(
                  std::move(driver)))
    {
    }

    ~WindowsOnDeviceChatBackendImpl()
        override
    {
        shutdown();
    }

    WindowsOnDeviceChatStatus status()
        const override
    {
        const std::scoped_lock lock(
            state_->mutex);
        return state_->status;
    }

    Result<void> setDownloadConsent(
        bool granted) override
    {
        {
            const std::scoped_lock lock(
                state_->mutex);
            if (state_->stopping) {
                return Result<void>::failure(
                    stoppingError());
            }
            if (state_->status
                    .downloadConsentGranted
                == granted) {
                return Result<void>::success();
            }
        }

        mutateStatus(
            state_,
            [granted](
                WindowsOnDeviceBackendState&
                    state,
                WindowsOnDeviceChatStatus&
                    status) {
                if (state.stopping) {
                    return;
                }
                status
                    .downloadConsentGranted =
                    granted;
                status.available = false;
                status.progressPercent = 0.0;
                status.phase =
                    granted
                    ? WindowsOnDeviceChatPhase::
                          Idle
                    : WindowsOnDeviceChatPhase::
                          ConsentRequired;
                if (!granted
                    && state.preparing) {
                    state
                        .preparationStopSource
                        .request_stop();
                }
            });
        return Result<void>::success();
    }

    QFuture<Result<void>> prepare()
        override
    {
        std::shared_ptr<
            QPromise<Result<void>>> promise;
        QFuture<Result<void>> future;
        quint64 generation = 0;
        std::stop_token stopToken;
        {
            const std::scoped_lock lock(
                state_->mutex);
            if (state_->stopping) {
                return readyFuture(
                    Result<void>::failure(
                        stoppingError()));
            }
            if (!state_->status
                     .downloadConsentGranted) {
                return readyFuture(
                    Result<void>::failure(
                        consentRequiredError()));
            }
            if (state_->status.available
                && state_->status.phase
                    == WindowsOnDeviceChatPhase::
                        Ready) {
                return readyFuture(
                    Result<void>::success());
            }
            if (state_->preparing) {
                return state_
                    ->preparationFuture;
            }

            promise =
                std::make_shared<
                    QPromise<Result<void>>>();
            promise->start();
            future = promise->future();
            state_->preparationFuture =
                future;
            state_->preparing = true;
            ++state_
                  ->preparationGeneration;
            generation =
                state_
                    ->preparationGeneration;
            state_->preparationStopSource =
                std::stop_source();
            stopToken =
                state_
                    ->preparationStopSource
                    .get_token();
        }

        try {
            state_->worker.start(
                [state = state_,
                 generation,
                 stopToken,
                 promise] {
                    runPreparation(
                        state,
                        generation,
                        stopToken,
                        promise);
                });
        } catch (...) {
            completePreparation(
                state_,
                generation,
                stopToken,
                Result<void>::failure(
                    preparationFailure()),
                promise);
        }
        return future;
    }

    std::shared_ptr<
        WindowsOnDeviceChatStatusSubscription>
    subscribeStatus(
        std::function<void(
            WindowsOnDeviceChatStatus)>
            observer) override
    {
        if (!observer) {
            return {};
        }
        auto slot =
            std::make_shared<
                StatusObserverSlot>(
                std::move(observer));
        quint64 identifier = 0;
        {
            const std::scoped_lock lock(
                state_->mutex);
            if (state_->stopping) {
                return {};
            }
            identifier =
                state_->nextObserverId++;
            state_->observers.emplace(
                identifier,
                slot);
        }
        return std::make_shared<
            StatusSubscription>(
            state_,
            identifier,
            slot);
    }

    Result<ChatResult> send(
        const ChatRequest& request) override
    {
        if (request.provider
            != ChatProvider::OnDevice
            || !request.attachments.isEmpty()) {
            return Result<
                ChatResult>::failure(
                unavailableError());
        }
        {
            const std::scoped_lock lock(
                state_->mutex);
            if (state_->stopping) {
                return Result<
                    ChatResult>::failure(
                    stoppingError());
            }
            if (!state_->status
                     .downloadConsentGranted
                || !state_->status.available
                || state_->status.phase
                    != WindowsOnDeviceChatPhase::
                        Ready) {
                return Result<
                    ChatResult>::failure(
                    unavailableError());
            }
        }

        auto promise =
            std::make_shared<
                std::promise<
                    Result<ChatResult>>>();
        std::future<Result<ChatResult>>
            future = promise->get_future();
        try {
            state_->worker.start(
                [state = state_,
                 request,
                 promise] {
                    Result<ChatResult> result =
                        Result<
                            ChatResult>::failure(
                            unavailableError());
                    bool available = false;
                    {
                        const std::scoped_lock lock(
                            state->mutex);
                        available =
                            !state->stopping
                            && state->status
                                   .downloadConsentGranted
                            && state->status
                                   .available
                            && state->status.phase
                                == WindowsOnDeviceChatPhase::
                                    Ready;
                    }
                    if (available) {
                        try {
                            result =
                                state->driver
                                    ->send(
                                        request);
                        } catch (...) {
                            result = Result<
                                ChatResult>::
                                failure(
                                    sendFailure());
                        }
                    }
                    try {
                        promise->set_value(
                            std::move(result));
                    } catch (...) {
                    }
                });
        } catch (...) {
            return Result<ChatResult>::failure(
                sendFailure());
        }
        try {
            return future.get();
        } catch (...) {
            return Result<ChatResult>::failure(
                sendFailure());
        }
    }

private:
    void shutdown() noexcept
    {
        StatusObservers observers;
        std::optional<
            WindowsOnDeviceChatStatus>
            published;
        bool queueShutdown = false;
        {
            const std::scoped_lock lock(
                state_->mutex);
            if (state_->shutdownQueued) {
                return;
            }
            state_->shutdownQueued = true;
            state_->stopping = true;
            if (state_->preparing) {
                state_
                    ->preparationStopSource
                    .request_stop();
            }
            WindowsOnDeviceChatStatus next =
                state_->status;
            normalizeStatus(next, true);
            if (!sameStatusWithoutRevision(
                    state_->status,
                    next)
                && state_->status.revision
                    != std::numeric_limits<
                        quint64>::max()) {
                next.revision =
                    state_->status.revision
                    + 1;
                state_->status = next;
                observers =
                    observersLocked(*state_);
                published = next;
            }
            queueShutdown = true;
        }

        if (published.has_value()) {
            notifyObservers(
                observers,
                *published);
        }
        if (queueShutdown) {
            try {
                state_->worker.start(
                    [state = state_] {
                        state->driver
                            ->shutdown();
                    });
                state_->worker.waitForDone();
            } catch (...) {
                try {
                    state_->driver->shutdown();
                } catch (...) {
                }
            }
        }

        const std::scoped_lock lock(
            state_->mutex);
        for (const auto& [id, observer] :
             state_->observers) {
            Q_UNUSED(id);
            observer->active.store(false);
        }
        state_->observers.clear();
    }

    std::shared_ptr<
        WindowsOnDeviceBackendState>
        state_;
};

} // namespace

namespace detail {

Result<std::shared_ptr<
    WindowsOnDeviceChatBackend>>
createWindowsOnDeviceChatBackend(
    std::shared_ptr<
        WindowsOnDeviceChatDriver> driver)
{
    if (driver == nullptr) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            failure(unavailableError());
    }
    try {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            success(
                std::make_shared<
                    WindowsOnDeviceChatBackendImpl>(
                    std::move(driver)));
    } catch (...) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            failure(unavailableError());
    }
}

} // namespace detail

} // namespace companion
