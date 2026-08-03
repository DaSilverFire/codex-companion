#include "codex/chat/WindowsOnDeviceChatBackendRegistryInternal.h"

#include <QtTest>

#include <atomic>
#include <future>
#include <memory>
#include <utility>

using namespace companion;

namespace {

class RegistryDriver final
    : public
          detail::WindowsOnDeviceChatDriver {
public:
    Result<void> prepare(
        detail::
            WindowsOnDevicePreparationObserver,
        std::stop_token) override
    {
        return Result<void>::success();
    }

    Result<ChatResult> send(
        const ChatRequest&) override
    {
        return Result<ChatResult>::failure({
            QStringLiteral("unused"),
            QStringLiteral("unused"),
            false,
            {},
        });
    }

    void shutdown() noexcept override
    {
        ++shutdownCalls;
    }

    std::atomic_int shutdownCalls = 0;
};

} // namespace

class WindowsOnDeviceChatBackendRegistryTests
    final : public QObject {
    Q_OBJECT

private slots:
    void repeatedAndConcurrentAcquireShareOneOwner()
    {
        const auto driver =
            std::make_shared<
                RegistryDriver>();
        std::atomic_int factoryCalls = 0;
        detail::
            WindowsOnDeviceChatBackendRegistry
                registry(
                    [driver,
                     &factoryCalls] {
                        ++factoryCalls;
                        return Result<
                            std::shared_ptr<
                                detail::
                                    WindowsOnDeviceChatDriver>>::
                            success(driver);
                    });

        auto firstFuture = std::async(
            std::launch::async,
            [&registry] {
                return registry.acquire();
            });
        auto secondFuture = std::async(
            std::launch::async,
            [&registry] {
                return registry.acquire();
            });
        auto first = firstFuture.get();
        auto second = secondFuture.get();
        QVERIFY(first.hasValue());
        QVERIFY(second.hasValue());
        QCOMPARE(
            first.value().get(),
            second.value().get());
        QCOMPARE(factoryCalls.load(), 1);

        std::weak_ptr<
            WindowsOnDeviceChatBackend>
            weak = first.value();
        first.value().reset();
        second.value().reset();
        QVERIFY(!weak.expired());
        QCOMPARE(
            driver->shutdownCalls.load(),
            0);

        registry.shutdownForProcessExit();
        QVERIFY(weak.expired());
        QCOMPARE(
            driver->shutdownCalls.load(),
            1);
    }

    void shutdownIsPermanentAndIdempotent()
    {
        const auto driver =
            std::make_shared<
                RegistryDriver>();
        std::atomic_int factoryCalls = 0;
        detail::
            WindowsOnDeviceChatBackendRegistry
                registry(
                    [driver,
                     &factoryCalls] {
                        ++factoryCalls;
                        return Result<
                            std::shared_ptr<
                                detail::
                                    WindowsOnDeviceChatDriver>>::
                            success(driver);
                    });
        auto acquired = registry.acquire();
        QVERIFY(acquired.hasValue());
        acquired.value().reset();

        registry.shutdownForProcessExit();
        registry.shutdownForProcessExit();
        QCOMPARE(
            driver->shutdownCalls.load(),
            1);
        QCOMPARE(factoryCalls.load(), 1);

        auto late = registry.acquire();
        QVERIFY(!late.hasValue());
        QCOMPARE(factoryCalls.load(), 1);
    }

    void failedFactoryCanRetryBeforeShutdown()
    {
        const auto driver =
            std::make_shared<
                RegistryDriver>();
        std::atomic_int factoryCalls = 0;
        detail::
            WindowsOnDeviceChatBackendRegistry
                registry(
                    [driver,
                     &factoryCalls] {
                        const int call =
                            ++factoryCalls;
                        if (call == 1) {
                            return Result<
                                std::shared_ptr<
                                    detail::
                                        WindowsOnDeviceChatDriver>>::
                                failure({
                                    QStringLiteral(
                                        "failed"),
                                    QStringLiteral(
                                        "failed"),
                                    false,
                                    {},
                                });
                        }
                        return Result<
                            std::shared_ptr<
                                detail::
                                    WindowsOnDeviceChatDriver>>::
                            success(driver);
                    });

        QVERIFY(
            !registry.acquire().hasValue());
        auto retried = registry.acquire();
        QVERIFY(retried.hasValue());
        QCOMPARE(factoryCalls.load(), 2);
        retried.value().reset();
        registry.shutdownForProcessExit();
        QCOMPARE(
            driver->shutdownCalls.load(),
            1);
    }
};

QTEST_GUILESS_MAIN(
    WindowsOnDeviceChatBackendRegistryTests)

#include "WindowsOnDeviceChatBackendRegistryTests.moc"
