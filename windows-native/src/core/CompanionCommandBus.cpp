#include "core/CompanionCommandBus.h"

#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

namespace detail {

struct CompanionCommandBusDeliveryState final {
    explicit CompanionCommandBusDeliveryState(
        CommandBusDeliveryHook requestedHook = {})
        : hook(std::move(requestedHook))
    {
    }

    void notify(CommandBusDeliveryPhase phase) const noexcept
    {
        if (!hook) {
            return;
        }
        try {
            hook(phase);
        } catch (...) {
        }
    }

    std::mutex mutex;
    std::condition_variable accessReleased;
    QPointer<CompanionCommandBus> bus;
    CommandBusDeliveryHook hook;
    std::size_t activeAccesses = 0;
    bool destroying = false;
};

} // namespace detail

namespace {

Result<void> commandFailure(
    QString code,
    QString message)
{
    return Result<void>::failure({
        std::move(code),
        std::move(message),
        false,
        {},
    });
}

Result<void> invalidCommand()
{
    return commandFailure(
        QStringLiteral("ui.invalid_command"),
        QStringLiteral("Command registration is invalid."));
}

Result<void> commandAlreadyRegistered()
{
    return commandFailure(
        QStringLiteral("ui.command_already_registered"),
        QStringLiteral("Command is already registered."));
}

Result<void> unknownCommand()
{
    return commandFailure(
        QStringLiteral("ui.unknown_command"),
        QStringLiteral("Command is not registered."));
}

Result<void> handlerException()
{
    return commandFailure(
        QStringLiteral("ui.command_failed"),
        QStringLiteral("Command failed."));
}

Result<void> commandBusUnavailable()
{
    return commandFailure(
        QStringLiteral("ui.command_bus_unavailable"),
        QStringLiteral("Command bus is unavailable."));
}

Result<void> commandBusThreadMismatch()
{
    return commandFailure(
        QStringLiteral(
            "ui.command_bus_thread_mismatch"),
        QStringLiteral(
            "Command bus must be modified on its owner thread."));
}

class CommandBusAccess final {
public:
    explicit CommandBusAccess(
        std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>
            state)
        : state_(std::move(state))
    {
        if (state_ == nullptr) {
            return;
        }
        const std::scoped_lock lock(state_->mutex);
        if (state_->destroying
            || state_->bus.isNull()) {
            state_.reset();
            return;
        }
        bus_ = state_->bus.data();
        ++state_->activeAccesses;
    }

    ~CommandBusAccess()
    {
        release();
    }

    CommandBusAccess(
        const CommandBusAccess&) = delete;
    CommandBusAccess& operator=(
        const CommandBusAccess&) = delete;

    explicit operator bool() const noexcept
    {
        return bus_ != nullptr;
    }

    CompanionCommandBus* bus() const noexcept
    {
        return bus_;
    }

    void release() noexcept
    {
        if (state_ == nullptr) {
            bus_ = nullptr;
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
        bus_ = nullptr;
    }

private:
    std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>
        state_;
    CompanionCommandBus* bus_ = nullptr;
};

struct DeliveryPayload final {
    QString command;
    quint64 executionId = 0;
    Result<void> result;
};

void queueDelivery(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>& state,
    const std::shared_ptr<const DeliveryPayload>& payload);

void deliverOnCurrentOwnerThread(
    const std::weak_ptr<
        detail::CompanionCommandBusDeliveryState>& weakState,
    const std::shared_ptr<const DeliveryPayload>& payload)
{
    const auto state = weakState.lock();
    if (state == nullptr) {
        return;
    }

    state->notify(
        detail::CommandBusDeliveryPhase::
            BeforeDispatch);

    CommandBusAccess access(state);
    if (!access) {
        return;
    }
    state->notify(
        detail::CommandBusDeliveryPhase::
            DeliveryOwnerAccessAcquired);
    CompanionCommandBus* const bus =
        access.bus();
    if (QThread::currentThread() != bus->thread()) {
        access.release();
        queueDelivery(state, payload);
        return;
    }
    access.release();

    if (payload->result.hasValue()) {
        emit bus->commandFinished(
            payload->command,
            true,
            {},
            {});
        emit bus->commandFinishedDetailed(
            payload->command,
            payload->executionId,
            true,
            {},
            {});
        return;
    }
    emit bus->commandFinished(
        payload->command,
        false,
        payload->result.error().code,
        payload->result.error().message);
    emit bus->commandFinishedDetailed(
        payload->command,
        payload->executionId,
        false,
        payload->result.error().code,
        payload->result.error().message);
}

void queueDelivery(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>& state,
    const std::shared_ptr<const DeliveryPayload>& payload)
{
    state->notify(
        detail::CommandBusDeliveryPhase::BeforePost);

    CommandBusAccess access(state);
    if (!access) {
        return;
    }
    const bool queued = QMetaObject::invokeMethod(
        access.bus(),
        [weakState =
             std::weak_ptr<
                 detail::
                     CompanionCommandBusDeliveryState>(
                 state),
         payload] {
            deliverOnCurrentOwnerThread(
                weakState,
                payload);
        },
        Qt::QueuedConnection);
    access.release();
    if (queued) {
        state->notify(
            detail::CommandBusDeliveryPhase::
                AfterPost);
    }
}

class OneShotCompletion final {
public:
    OneShotCompletion(
        std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>
            deliveryState,
        QString command,
        quint64 executionId)
        : deliveryState_(std::move(deliveryState)),
          command_(std::move(command)),
          executionId_(executionId)
    {
    }

    void finish(Result<void> result)
    {
        {
            const std::scoped_lock lock(mutex_);
            if (finished_) {
                return;
            }
            finished_ = true;
        }
        queueDelivery(
            deliveryState_,
            std::make_shared<const DeliveryPayload>(
                DeliveryPayload{
                    command_,
                    executionId_,
                    std::move(result),
                }));
    }

private:
    std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>
        deliveryState_;
    QString command_;
    quint64 executionId_ = 0;
    std::mutex mutex_;
    bool finished_ = false;
};

struct HandlerInvocationPayload final {
    CompanionCommandBus::Handler handler;
    QVariantMap arguments;
    CompanionCommandBus::Completion completion;
    std::shared_ptr<std::atomic_bool> claimed =
        std::make_shared<std::atomic_bool>(false);
};

void invokeHandlerOnCurrentOwnerThread(
    const std::weak_ptr<
        detail::CompanionCommandBusDeliveryState>& weakState,
    const std::shared_ptr<
        HandlerInvocationPayload>& payload);

void queueHandlerInvocation(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>& state,
    const std::shared_ptr<
        HandlerInvocationPayload>& payload)
{
    CommandBusAccess access(state);
    if (!access) {
        return;
    }
    const bool queued = QMetaObject::invokeMethod(
        access.bus(),
        [weakState =
             std::weak_ptr<
                 detail::
                     CompanionCommandBusDeliveryState>(
                 state),
         payload] {
            invokeHandlerOnCurrentOwnerThread(
                weakState,
                payload);
        },
        Qt::QueuedConnection);
    access.release();
    if (!queued) {
        payload->completion(handlerException());
    }
}

void invokeHandlerOnCurrentOwnerThread(
    const std::weak_ptr<
        detail::CompanionCommandBusDeliveryState>& weakState,
    const std::shared_ptr<
        HandlerInvocationPayload>& payload)
{
    const auto state = weakState.lock();
    if (state == nullptr) {
        return;
    }

    CommandBusAccess access(state);
    if (!access) {
        return;
    }
    state->notify(
        detail::CommandBusDeliveryPhase::
            HandlerOwnerAccessAcquired);
    if (QThread::currentThread()
        != access.bus()->thread()) {
        access.release();
        queueHandlerInvocation(
            state,
            payload);
        return;
    }

    bool expected = false;
    if (!payload->claimed->compare_exchange_strong(
            expected,
            true)) {
        return;
    }
    access.release();
    try {
        payload->handler(
            payload->arguments,
            payload->completion);
    } catch (...) {
        payload->completion(handlerException());
    }
}

} // namespace

CompanionCommandBus::CompanionCommandBus(QObject* parent)
    : CompanionCommandBus({}, parent)
{
}

CompanionCommandBus::CompanionCommandBus(
    detail::CommandBusDeliveryHook deliveryHook,
    QObject* parent)
    : QObject(parent),
      deliveryState_(
          std::make_shared<
              detail::CompanionCommandBusDeliveryState>(
              std::move(deliveryHook)))
{
    deliveryState_->bus = this;
}

CompanionCommandBus::~CompanionCommandBus()
{
    bool waitingForAccess = false;
    {
        std::unique_lock lock(
            deliveryState_->mutex);
        deliveryState_->destroying = true;
        waitingForAccess =
            deliveryState_->activeAccesses > 0;
        if (waitingForAccess) {
            lock.unlock();
            deliveryState_->notify(
                detail::CommandBusDeliveryPhase::
                    DestructionWaitingForAccess);
            lock.lock();
            deliveryState_->accessReleased.wait(
                lock,
                [state = deliveryState_] {
                    return state->activeAccesses == 0;
                });
        }
        deliveryState_->bus.clear();
    }
    deliveryState_->notify(
        detail::CommandBusDeliveryPhase::
            DestructionStarted);
}

bool CompanionCommandBus::tryGetOwnerThread(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>&
        deliveryState,
    QThread*& ownerThread) noexcept
{
    ownerThread = nullptr;
    try {
        CommandBusAccess access(deliveryState);
        if (!access) {
            return false;
        }
        ownerThread = access.bus()->thread();
        return true;
    } catch (...) {
        ownerThread = nullptr;
        return false;
    }
}

Result<void>
CompanionCommandBus::
registerHandlerTransactionGuarded(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>&
        deliveryState,
    const QString& command,
    Handler handler,
    quint64& registrationId)
{
    try {
        CommandBusAccess access(deliveryState);
        if (!access) {
            registrationId = 0;
            return commandBusUnavailable();
        }
        return access.bus()
            ->registerHandlerTransaction(
                command,
                std::move(handler),
                registrationId);
    } catch (...) {
        registrationId = 0;
        return commandBusUnavailable();
    }
}

Result<void>
CompanionCommandBus::replaceHandlerGroupGuarded(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>&
        deliveryState,
    const QString& group,
    QVector<HandlerEntry> handlers)
{
    try {
        CommandBusAccess access(deliveryState);
        if (!access) {
            return commandBusUnavailable();
        }
        return access.bus()->replaceHandlerGroup(
            group,
            std::move(handlers));
    } catch (...) {
        return commandBusUnavailable();
    }
}

bool CompanionCommandBus::
commitHandlerRegistrationGuarded(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>&
        deliveryState,
    const QString& command,
    quint64 registrationId) noexcept
{
    try {
        CommandBusAccess access(deliveryState);
        return access
            && access.bus()
                   ->commitHandlerRegistration(
                       command,
                       registrationId);
    } catch (...) {
        return false;
    }
}

void CompanionCommandBus::
rollbackHandlerRegistrationGuarded(
    const std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>&
        deliveryState,
    const QString& command,
    quint64 registrationId) noexcept
{
    try {
        CommandBusAccess access(deliveryState);
        if (access) {
            access.bus()
                ->rollbackHandlerRegistration(
                    command,
                    registrationId);
        }
    } catch (...) {
    }
}

Result<void> CompanionCommandBus::registerHandler(
    const QString& command,
    Handler handler)
{
    quint64 registrationId = 0;
    return insertHandler(
        command,
        std::move(handler),
        true,
        registrationId);
}

Result<void> CompanionCommandBus::replaceHandlerGroup(
    const QString& group,
    QVector<HandlerEntry> handlers)
{
    if (QThread::currentThread() != thread()) {
        return commandBusThreadMismatch();
    }
    if (group.trimmed().isEmpty()) {
        return invalidCommand();
    }

    QSet<QString> commands;
    commands.reserve(handlers.size());
    for (const HandlerEntry& entry : handlers) {
        if (entry.command.trimmed().isEmpty()
            || !entry.handler
            || commands.contains(entry.command)) {
            return invalidCommand();
        }
        commands.insert(entry.command);
    }

    const std::scoped_lock lock(handlersMutex_);
    for (const HandlerEntry& entry : handlers) {
        const auto existing =
            handlers_.constFind(entry.command);
        if (existing == handlers_.constEnd()) {
            continue;
        }
        if (!existing->active
            || existing->group.isEmpty()
            || existing->group != group) {
            return commandAlreadyRegistered();
        }
    }

    QHash<QString, RegisteredHandler> replacement;
    replacement.reserve(handlers.size());
    quint64 nextRegistrationId =
        nextRegistrationId_;
    for (HandlerEntry& entry : handlers) {
        const quint64 assignedId =
            nextRegistrationId;
        ++nextRegistrationId;
        if (nextRegistrationId == 0) {
            nextRegistrationId = 1;
        }
        replacement.insert(
            entry.command,
            RegisteredHandler{
                group,
                std::move(entry.handler),
                assignedId,
                true,
            });
    }

    QHash<QString, RegisteredHandler> nextHandlers =
        handlers_;
    for (auto handler = nextHandlers.begin();
         handler != nextHandlers.end();) {
        if (handler->group == group) {
            handler = nextHandlers.erase(handler);
        } else {
            ++handler;
        }
    }
    for (auto handler = replacement.begin();
         handler != replacement.end();
         ++handler) {
        nextHandlers.insert(
            handler.key(),
            std::move(handler.value()));
    }
    handlers_.swap(nextHandlers);
    nextRegistrationId_ =
        nextRegistrationId;
    return Result<void>::success();
}

Result<void> CompanionCommandBus::insertHandler(
    const QString& command,
    Handler handler,
    bool active,
    quint64& registrationId)
{
    registrationId = 0;
    if (command.trimmed().isEmpty() || !handler) {
        return invalidCommand();
    }
    const std::scoped_lock lock(handlersMutex_);
    if (handlers_.contains(command)) {
        return commandAlreadyRegistered();
    }
    const quint64 assignedId = nextRegistrationId_;
    ++nextRegistrationId_;
    if (nextRegistrationId_ == 0) {
        nextRegistrationId_ = 1;
    }
    handlers_.insert(
        command,
        RegisteredHandler{
            {},
            std::move(handler),
            assignedId,
            active,
        });
    registrationId = assignedId;
    return Result<void>::success();
}

Result<void>
CompanionCommandBus::registerHandlerTransaction(
    const QString& command,
    Handler handler,
    quint64& registrationId)
{
    return insertHandler(
        command,
        std::move(handler),
        false,
        registrationId);
}

bool CompanionCommandBus::commitHandlerRegistration(
    const QString& command,
    quint64 registrationId) noexcept
{
    if (registrationId == 0) {
        return false;
    }
    try {
        const std::scoped_lock lock(handlersMutex_);
        const auto handler = handlers_.find(command);
        if (handler == handlers_.end()
            || handler->registrationId
                != registrationId
            || handler->active) {
            return false;
        }
        handler->active = true;
        return true;
    } catch (...) {
        return false;
    }
}

void CompanionCommandBus::rollbackHandlerRegistration(
    const QString& command,
    quint64 registrationId) noexcept
{
    if (registrationId == 0) {
        return;
    }
    try {
        const std::scoped_lock lock(handlersMutex_);
        const auto handler = handlers_.find(command);
        if (handler != handlers_.end()
            && handler->registrationId
                == registrationId
            && !handler->active) {
            handlers_.erase(handler);
        }
    } catch (...) {
    }
}

quint64 CompanionCommandBus::execute(
    const QString& command,
    const QVariantMap& arguments)
{
    const auto deliveryState = deliveryState_;
    CommandBusAccess ownerAccess(deliveryState);
    if (!ownerAccess) {
        return 0;
    }
    quint64 executionId =
        nextExecutionId_.fetch_add(
            1,
            std::memory_order_relaxed);
    while (executionId == 0) {
        executionId =
            nextExecutionId_.fetch_add(
                1,
                std::memory_order_relaxed);
    }
    if (QThread::currentThread()
        != ownerAccess.bus()->thread()) {
        const bool queued = QMetaObject::invokeMethod(
            ownerAccess.bus(),
            [this, command, arguments, executionId] {
                executeWithId(
                    command,
                    arguments,
                    executionId);
            },
            Qt::QueuedConnection);
        return queued ? executionId : 0;
    }
    ownerAccess.release();
    executeWithId(
        command,
        arguments,
        executionId);
    return executionId;
}

void CompanionCommandBus::executeWithId(
    const QString& command,
    const QVariantMap& arguments,
    quint64 executionId)
{
    const auto deliveryState = deliveryState_;
    CommandBusAccess ownerAccess(deliveryState);
    if (!ownerAccess) {
        return;
    }
    if (QThread::currentThread()
        != ownerAccess.bus()->thread()) {
        QMetaObject::invokeMethod(
            ownerAccess.bus(),
            [this, command, arguments, executionId] {
                executeWithId(
                    command,
                    arguments,
                    executionId);
            },
            Qt::QueuedConnection);
        return;
    }
    ownerAccess.release();

    const auto completion =
        std::make_shared<OneShotCompletion>(
            deliveryState,
            command,
            executionId);
    const Completion finish =
        [completion](Result<void> result) {
            completion->finish(std::move(result));
        };

    Handler registeredHandler;
    {
        const std::scoped_lock lock(handlersMutex_);
        const auto handler =
            handlers_.constFind(command);
        if (handler != handlers_.constEnd()
            && handler->active) {
            registeredHandler =
                handler->handler;
        }
    }
    if (!registeredHandler) {
        finish(unknownCommand());
        return;
    }

    const auto invocation =
        std::make_shared<HandlerInvocationPayload>(
            HandlerInvocationPayload{
                registeredHandler,
                arguments,
                finish,
            });
    emit commandStarted(command);
    invokeHandlerOnCurrentOwnerThread(
        std::weak_ptr<
            detail::CompanionCommandBusDeliveryState>(
            deliveryState),
        invocation);
}

} // namespace companion
