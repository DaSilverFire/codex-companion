#pragma once

#include "core/Result.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class QThread;

namespace companion {

class CompanionCommandBus;
class CodexRuntime;

namespace detail {
struct CompanionCommandBusDeliveryState;
struct CompanionCommandBusTestAccess;

enum class CommandBusDeliveryPhase {
    BeforePost,
    AfterPost,
    BeforeDispatch,
    DeliveryOwnerAccessAcquired,
    HandlerOwnerAccessAcquired,
    DestructionWaitingForAccess,
    DestructionStarted,
};

using CommandBusDeliveryHook =
    std::function<void(CommandBusDeliveryPhase)>;
}

class CompanionCommandBus final : public QObject {
    Q_OBJECT

public:
    using Completion = std::function<void(Result<void>)>;
    using Handler =
        std::function<void(const QVariantMap&, Completion)>;

    struct HandlerEntry final {
        QString command;
        Handler handler;
    };

    explicit CompanionCommandBus(QObject* parent = nullptr);
    ~CompanionCommandBus() override;

    Result<void> registerHandler(
        const QString& command,
        Handler handler);
    Result<void> replaceHandlerGroup(
        const QString& group,
        QVector<HandlerEntry> handlers);

    Q_INVOKABLE quint64 execute(
        const QString& command,
        const QVariantMap& arguments = {});

signals:
    void commandStarted(const QString& command);
    void commandFinished(
        const QString& command,
        bool succeeded,
        const QString& errorCode,
        const QString& message);
    void commandFinishedDetailed(
        const QString& command,
        quint64 executionId,
        bool succeeded,
        const QString& errorCode,
        const QString& message);

private:
    struct RegisteredHandler final {
        QString group;
        Handler handler;
        quint64 registrationId = 0;
        bool active = false;
    };

    CompanionCommandBus(
        detail::CommandBusDeliveryHook deliveryHook,
        QObject* parent);

    Result<void> insertHandler(
        const QString& command,
        Handler handler,
        bool active,
        quint64& registrationId);
    Result<void> registerHandlerTransaction(
        const QString& command,
        Handler handler,
        quint64& registrationId);
    bool commitHandlerRegistration(
        const QString& command,
        quint64 registrationId) noexcept;
    void rollbackHandlerRegistration(
        const QString& command,
        quint64 registrationId) noexcept;
    void executeWithId(
        const QString& command,
        const QVariantMap& arguments,
        quint64 executionId);
    static bool tryGetOwnerThread(
        const std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>&
            deliveryState,
        QThread*& ownerThread) noexcept;
    static Result<void>
    registerHandlerTransactionGuarded(
        const std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>&
            deliveryState,
        const QString& command,
        Handler handler,
        quint64& registrationId);
    static Result<void> replaceHandlerGroupGuarded(
        const std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>&
            deliveryState,
        const QString& group,
        QVector<HandlerEntry> handlers);
    static bool commitHandlerRegistrationGuarded(
        const std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>&
            deliveryState,
        const QString& command,
        quint64 registrationId) noexcept;
    static void rollbackHandlerRegistrationGuarded(
        const std::shared_ptr<
            detail::CompanionCommandBusDeliveryState>&
            deliveryState,
        const QString& command,
        quint64 registrationId) noexcept;

    friend class CodexRuntime;
    friend struct detail::CompanionCommandBusTestAccess;

    std::mutex handlersMutex_;
    QHash<QString, RegisteredHandler> handlers_;
    quint64 nextRegistrationId_ = 1;
    std::atomic<quint64> nextExecutionId_ = 1;
    std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>
        deliveryState_;
};

} // namespace companion
