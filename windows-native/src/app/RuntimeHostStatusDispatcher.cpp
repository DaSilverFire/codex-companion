#include "app/RuntimeHostStatusDispatcher.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <mutex>
#include <utility>

namespace companion::detail {

struct RuntimeHostStatusDispatcher::State final {
    State(Delivery requestedDelivery, Queue requestedQueue)
        : delivery(std::move(requestedDelivery)),
          queue(std::move(requestedQueue))
    {
    }

    std::mutex mutex;
    Delivery delivery;
    Queue queue;
    bool active = true;
};

namespace {

bool queueOnApplication(
    std::function<void()> delivery)
{
    QCoreApplication* const application =
        QCoreApplication::instance();
    return application != nullptr
        && QMetaObject::invokeMethod(
            application,
            std::move(delivery),
            Qt::QueuedConnection);
}

} // namespace

RuntimeHostStatusDispatcher::
RuntimeHostStatusDispatcher(
    Delivery delivery,
    Queue queue)
    : state_(
          std::make_shared<State>(
              std::move(delivery),
              queue
                  ? std::move(queue)
                  : Queue(queueOnApplication)))
{
}

void RuntimeHostStatusDispatcher::publish(
    WindowsOnDeviceChatStatus status) const
{
    const auto state = state_;
    if (state == nullptr) {
        return;
    }

    Queue queue;
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active
            || !state->delivery
            || !state->queue) {
            return;
        }
        queue = state->queue;
    }

    try {
        queue(
            [weakState =
                 std::weak_ptr<State>(state),
             status] {
                const auto queuedState =
                    weakState.lock();
                if (queuedState == nullptr) {
                    return;
                }

                Delivery delivery;
                {
                    const std::scoped_lock lock(
                        queuedState->mutex);
                    if (!queuedState->active
                        || !queuedState->delivery) {
                        return;
                    }
                    delivery =
                        queuedState->delivery;
                }

                try {
                    delivery(status);
                } catch (...) {
                }
            });
    } catch (...) {
    }
}

void RuntimeHostStatusDispatcher::invalidate()
    noexcept
{
    try {
        const std::scoped_lock lock(
            state_->mutex);
        state_->active = false;
        state_->delivery = {};
    } catch (...) {
    }
}

} // namespace companion::detail
