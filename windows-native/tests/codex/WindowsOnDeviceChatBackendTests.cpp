#include "codex/chat/WindowsOnDeviceChatBackendInternal.h"

#include <QDeadlineTimer>
#include <QFuture>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

using namespace companion;

namespace {

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds = 5000)
{
    const QDeadlineTimer deadline(
        timeoutMilliseconds);
    while (!predicate()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QThread::msleep(1);
    }
    return true;
}

class FakeDriver final
    : public detail::WindowsOnDeviceChatDriver {
public:
    Result<void> prepare(
        detail::
            WindowsOnDevicePreparationObserver
                observer,
        std::stop_token stopToken) override
    {
        ++prepareCalls;
        recordThread();
        observer(
            WindowsOnDeviceChatPhase::
                DiscoveringExecutionProviders,
            10.0);
        entered.release();
        release.acquire();
        if (stopToken.stop_requested()) {
            stopObserved.store(true);
            return Result<void>::failure({
                QStringLiteral(
                    "foundry.canceled"),
                QStringLiteral(
                    "Preparation canceled."),
                false,
                {},
            });
        }
        observer(
            WindowsOnDeviceChatPhase::
                DownloadingModel,
            65.0);
        observer(
            WindowsOnDeviceChatPhase::
                LoadingModel,
            95.0);
        return Result<void>::success();
    }

    Result<ChatResult> send(
        const ChatRequest& request) override
    {
        ++sendCalls;
        recordThread();
        const int concurrent =
            ++activeSends;
        int previous =
            maximumConcurrentSends.load();
        while (concurrent > previous
               && !maximumConcurrentSends
                       .compare_exchange_weak(
                           previous,
                           concurrent)) {
        }
        QThread::msleep(25);
        --activeSends;
        {
            const QMutexLocker lock(
                &requestMutex);
            requests.append(request);
        }
        return Result<ChatResult>::success({
            QStringLiteral("local answer"),
            std::nullopt,
            std::nullopt,
        });
    }

    void shutdown() noexcept override
    {
        ++shutdownCalls;
        recordThread();
    }

    void allowPrepare()
    {
        release.release();
    }

    QVector<ChatRequest> sentRequests() const
    {
        const QMutexLocker lock(
            &requestMutex);
        return requests;
    }

    QVector<Qt::HANDLE> workerThreads() const
    {
        const QMutexLocker lock(
            &threadMutex);
        return threads;
    }

    std::atomic_int prepareCalls = 0;
    std::atomic_int sendCalls = 0;
    std::atomic_int shutdownCalls = 0;
    std::atomic_int activeSends = 0;
    std::atomic_int maximumConcurrentSends = 0;
    std::atomic_bool stopObserved = false;
    QSemaphore entered;

private:
    void recordThread() const
    {
        const QMutexLocker lock(
            &threadMutex);
        threads.append(
            QThread::currentThreadId());
    }

    QSemaphore release;
    mutable QMutex requestMutex;
    QVector<ChatRequest> requests;
    mutable QMutex threadMutex;
    mutable QVector<Qt::HANDLE> threads;
};

} // namespace

class WindowsOnDeviceChatBackendTests final
    : public QObject {
    Q_OBJECT

private slots:
    void preparationRequiresConsentAndCoalesces()
    {
        const auto driver =
            std::make_shared<FakeDriver>();
        auto created =
            detail::
                createWindowsOnDeviceChatBackend(
                    driver);
        QVERIFY(created.hasValue());
        const auto backend =
            created.value();
        const auto initial =
            backend->status();
        QCOMPARE(
            initial.phase,
            WindowsOnDeviceChatPhase::
                ConsentRequired);
        QVERIFY(!initial.available);
        QVERIFY(
            !initial.downloadConsentGranted);

        QFuture<Result<void>> denied =
            backend->prepare();
        denied.waitForFinished();
        QVERIFY(!denied.result().hasValue());
        QCOMPARE(driver->prepareCalls.load(), 0);

        QVector<WindowsOnDeviceChatStatus>
            observed;
        const auto subscription =
            backend->subscribeStatus(
                [&observed](
                    WindowsOnDeviceChatStatus
                        status) {
                    observed.append(status);
                });
        QVERIFY(subscription != nullptr);
        QVERIFY(
            backend
                ->setDownloadConsent(true)
                .hasValue());

        QFuture<Result<void>> first =
            backend->prepare();
        QFuture<Result<void>> second =
            backend->prepare();
        QVERIFY(
            driver->entered.tryAcquire(
                1,
                5000));
        QCOMPARE(driver->prepareCalls.load(), 1);
        QVERIFY(!first.isFinished());
        QVERIFY(!second.isFinished());

        driver->allowPrepare();
        first.waitForFinished();
        second.waitForFinished();
        QVERIFY(first.result().hasValue());
        QVERIFY(second.result().hasValue());
        QCOMPARE(driver->prepareCalls.load(), 1);

        const auto ready = backend->status();
        QCOMPARE(
            ready.phase,
            WindowsOnDeviceChatPhase::Ready);
        QVERIFY(ready.available);
        QVERIFY(
            ready.downloadConsentGranted);
        QVERIFY(!ready.supportsAttachments);
        QCOMPARE(ready.progressPercent, 100.0);
        QVERIFY(ready.revision > initial.revision);
        for (qsizetype index = 1;
             index < observed.size();
             ++index) {
            QVERIFY(
                observed.at(index).revision
                > observed.at(index - 1)
                      .revision);
        }

        QFuture<Result<void>> cached =
            backend->prepare();
        cached.waitForFinished();
        QVERIFY(cached.result().hasValue());
        QCOMPARE(driver->prepareCalls.load(), 1);
    }

    void consentRevocationCancelsPreparation()
    {
        const auto driver =
            std::make_shared<FakeDriver>();
        auto created =
            detail::
                createWindowsOnDeviceChatBackend(
                    driver);
        QVERIFY(created.hasValue());
        const auto backend =
            created.value();
        QVERIFY(
            backend
                ->setDownloadConsent(true)
                .hasValue());
        QFuture<Result<void>> preparing =
            backend->prepare();
        QVERIFY(
            driver->entered.tryAcquire(
                1,
                5000));

        QVERIFY(
            backend
                ->setDownloadConsent(false)
                .hasValue());
        driver->allowPrepare();
        preparing.waitForFinished();

        QVERIFY(!preparing.result().hasValue());
        QVERIFY(driver->stopObserved.load());
        const auto status = backend->status();
        QCOMPARE(
            status.phase,
            WindowsOnDeviceChatPhase::
                ConsentRequired);
        QVERIFY(!status.available);
        QVERIFY(
            !status.downloadConsentGranted);
        const Result<ChatResult> send =
            backend->send({
                ChatProvider::OnDevice,
                QStringLiteral("on-device"),
                QStringLiteral("prompt"),
                {},
            });
        QVERIFY(!send.hasValue());
        QCOMPARE(driver->sendCalls.load(), 0);
    }

    void sendsSerializeOnDedicatedWorker()
    {
        const auto driver =
            std::make_shared<FakeDriver>();
        auto created =
            detail::
                createWindowsOnDeviceChatBackend(
                    driver);
        QVERIFY(created.hasValue());
        auto backend =
            std::move(created.value());
        QVERIFY(
            backend
                ->setDownloadConsent(true)
                .hasValue());
        QFuture<Result<void>> preparing =
            backend->prepare();
        QVERIFY(
            driver->entered.tryAcquire(
                1,
                5000));
        driver->allowPrepare();
        preparing.waitForFinished();
        QVERIFY(preparing.result().hasValue());

        const Qt::HANDLE ownerThread =
            QThread::currentThreadId();
        auto first = std::async(
            std::launch::async,
            [backend] {
                return backend->send({
                    ChatProvider::OnDevice,
                    QStringLiteral(
                        "on-device"),
                    QStringLiteral("one"),
                    {},
                });
            });
        auto second = std::async(
            std::launch::async,
            [backend] {
                return backend->send({
                    ChatProvider::OnDevice,
                    QStringLiteral(
                        "on-device"),
                    QStringLiteral("two"),
                    {},
                });
            });

        QVERIFY(first.get().hasValue());
        QVERIFY(second.get().hasValue());
        QCOMPARE(driver->sendCalls.load(), 2);
        QCOMPARE(
            driver->maximumConcurrentSends
                .load(),
            1);
        const QVector<Qt::HANDLE> threads =
            driver->workerThreads();
        QVERIFY(threads.size() >= 3);
        for (Qt::HANDLE thread : threads) {
            QVERIFY(thread != ownerThread);
            QCOMPARE(thread, threads.front());
        }

        backend.reset();
        QVERIFY(waitUntil([&] {
            return driver->shutdownCalls.load()
                == 1;
        }));
    }
};

QTEST_GUILESS_MAIN(
    WindowsOnDeviceChatBackendTests)

#include "WindowsOnDeviceChatBackendTests.moc"
