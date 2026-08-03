#include "app/RuntimeHostStatusDispatcher.h"

#include <QTest>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace {

class RuntimeHostStatusDispatcherTests final : public QObject {
    Q_OBJECT

private slots:
    void inFlightPublishCannotDeliverAfterHostTeardown()
    {
        std::mutex mutex;
        std::condition_variable entered;
        std::condition_variable released;
        bool queueEntered = false;
        bool releaseQueue = false;
        std::function<void()> queuedDelivery;
        int deliveryCount = 0;

        auto dispatcher = std::make_shared<
            companion::detail::RuntimeHostStatusDispatcher>(
            [&deliveryCount](
                companion::WindowsOnDeviceChatStatus) {
                ++deliveryCount;
            },
            [&](std::function<void()> delivery) {
                std::unique_lock lock(mutex);
                queuedDelivery = std::move(delivery);
                queueEntered = true;
                entered.notify_all();
                released.wait(
                    lock,
                    [&releaseQueue] {
                        return releaseQueue;
                    });
                return true;
            });
        const std::weak_ptr<
            companion::detail::RuntimeHostStatusDispatcher>
            weakDispatcher(dispatcher);

        std::thread publisher([weakDispatcher] {
            if (const auto active = weakDispatcher.lock()) {
                active->publish({});
            }
        });

        {
            std::unique_lock lock(mutex);
            QVERIFY(entered.wait_for(
                lock,
                std::chrono::seconds(5),
                [&queueEntered] {
                    return queueEntered;
                }));
        }

        dispatcher->invalidate();
        dispatcher.reset();

        {
            const std::scoped_lock lock(mutex);
            releaseQueue = true;
        }
        released.notify_all();
        publisher.join();

        QVERIFY(queuedDelivery);
        queuedDelivery();
        QCOMPARE(deliveryCount, 0);
    }

    void activePublishDeliversExactlyOnce()
    {
        std::function<void()> queuedDelivery;
        int deliveryCount = 0;
        companion::detail::RuntimeHostStatusDispatcher dispatcher(
            [&deliveryCount](
                companion::WindowsOnDeviceChatStatus) {
                ++deliveryCount;
            },
            [&queuedDelivery](std::function<void()> delivery) {
                queuedDelivery = std::move(delivery);
                return true;
            });

        dispatcher.publish({});

        QVERIFY(queuedDelivery);
        queuedDelivery();
        QCOMPARE(deliveryCount, 1);
    }
};

} // namespace

QTEST_GUILESS_MAIN(RuntimeHostStatusDispatcherTests)
#include "RuntimeHostStatusDispatcherTests.moc"
