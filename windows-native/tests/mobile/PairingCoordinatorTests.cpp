#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/SecretProtector.h"
#include "platform/windows/security/WindowsDpapiProtector.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {

using namespace companion;
using namespace std::chrono_literals;

constexpr qint64 kNowMilliseconds =
    1770000000123;

QDateTime fixedNow()
{
    return QDateTime::fromMSecsSinceEpoch(
        kNowMilliseconds,
        QTimeZone::UTC);
}

QByteArray repeatedSecret(char value)
{
    return QByteArray(32, value);
}

class XorProtector final
    : public SecretProtector {
public:
    mutable bool failProtect = false;
    mutable bool failUnprotect = false;

    Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView entropy)
        const override
    {
        if (failProtect) {
            return Result<QByteArray>::failure(
                {
                    QStringLiteral(
                        "test.protect_failed"),
                    QStringLiteral(
                        "Injected protection failure."),
                    false,
                    {},
                });
        }
        return Result<QByteArray>::success(
            transform(plaintext, entropy));
    }

    Result<QByteArray> unprotect(
        QByteArrayView protectedData,
        QByteArrayView entropy)
        const override
    {
        if (failUnprotect) {
            return Result<QByteArray>::failure(
                {
                    QStringLiteral(
                        "test.unprotect_failed"),
                    QStringLiteral(
                        "Injected unlock failure."),
                    false,
                    {},
                });
        }
        return Result<QByteArray>::success(
            transform(
                protectedData,
                entropy));
    }

private:
    static QByteArray transform(
        QByteArrayView input,
        QByteArrayView entropy)
    {
        QByteArray output(
            input.data(),
            input.size());
        unsigned char mask = 0xA5;
        for (const char byte : entropy) {
            mask ^= static_cast<
                unsigned char>(byte);
        }
        for (char& byte : output) {
            byte = static_cast<char>(
                static_cast<unsigned char>(
                    byte)
                ^ mask);
        }
        return output;
    }
};

class WinHandle final {
public:
    explicit WinHandle(HANDLE value)
        : value_(value)
    {
    }

    ~WinHandle()
    {
        if (value_ != nullptr
            && value_
                != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(
        const WinHandle&) = delete;

    bool isValid() const noexcept
    {
        return value_ != nullptr
            && value_
                != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

PairingRecord record(
    QString deviceId,
    QString displayName,
    char secretByte,
    qint64 pairedAtMilliseconds,
    std::optional<QString> relayUrl =
        std::nullopt)
{
    return {
        std::move(deviceId),
        std::move(displayName),
        repeatedSecret(secretByte),
        QDateTime::fromMSecsSinceEpoch(
            pairedAtMilliseconds,
            QTimeZone::UTC),
        std::move(relayUrl),
    };
}

BridgeInvitation pairingInvitation(
    QString code = QStringLiteral(
        "123-456"))
{
    return {
        BridgeSecurity::invitationVersion,
        QStringLiteral("iphone-alpha"),
        QStringLiteral("Harlin iPhone"),
        kNowMilliseconds,
        QByteArray(16, '\0'),
        std::nullopt,
        std::move(code),
    };
}

} // namespace

class PairingCoordinatorTests final
    : public QObject {
    Q_OBJECT

private slots:
    void windowsDpapiRoundTripsAndBindsEntropy()
    {
        WindowsDpapiProtector protector;
        const QByteArray plaintext(
            "paired-device-secret-test");
        const QByteArray entropy(
            "entropy-a");

        const auto protectedData =
            protector.protect(
                plaintext,
                entropy);
        QVERIFY(protectedData.hasValue());
        QVERIFY(
            protectedData.value()
            != plaintext);

        const auto restored =
            protector.unprotect(
                protectedData.value(),
                entropy);
        QVERIFY(restored.hasValue());
        QCOMPARE(
            restored.value(),
            plaintext);

        QVERIFY(
            !protector.unprotect(
                protectedData.value(),
                QByteArray("entropy-b"))
                 .hasValue());

        QByteArray corrupt =
            protectedData.value();
        corrupt[corrupt.size() / 2] ^= 0x01;
        QVERIFY(
            !protector.unprotect(
                corrupt,
                entropy)
                 .hasValue());
    }

    void pairingRecordsPersistProtectedAndSorted()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "Security/paired-devices.v1.json"));
        XorProtector protector;
        PairingRecordStore store(
            path,
            protector);
        QVERIFY(!store.loadError().has_value());

        const QByteArray firstSecret =
            repeatedSecret('\x11');
        QVERIFY(
            store.save(record(
                QStringLiteral("phone-b"),
                QStringLiteral("Phone 10"),
                '\x22',
                kNowMilliseconds))
                .hasValue());
        QVERIFY(
            store.save(record(
                QStringLiteral("phone-a"),
                QStringLiteral("Phone 2"),
                '\x11',
                kNowMilliseconds,
                QStringLiteral(
                     "wss://relay.example.test/socket")))
                .hasValue());
        QVERIFY(
            store.save(record(
                QStringLiteral("phone-c"),
                QStringLiteral("phone 2"),
                '\x33',
                kNowMilliseconds))
                .hasValue());

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray raw = file.readAll();
        QVERIFY(
            !raw.contains(firstSecret));
        QVERIFY(
            !raw.contains(
                firstSecret.toBase64()));
        const QJsonDocument document =
            QJsonDocument::fromJson(raw);
        QVERIFY(document.isObject());
        QCOMPARE(
            document.object()
                .value(QStringLiteral("version"))
                .toInt(),
            1);

        PairingRecordStore reloaded(
            path,
            protector);
        QVERIFY(
            !reloaded.loadError()
                 .has_value());
        const QVector<PairingRecord> records =
            reloaded.records();
        QCOMPARE(records.size(), 3);
        QCOMPARE(
            records.at(0).deviceId,
            QStringLiteral("phone-a"));
        QCOMPARE(
            records.at(1).deviceId,
            QStringLiteral("phone-c"));
        QCOMPARE(
            records.at(2).deviceId,
            QStringLiteral("phone-b"));
        QCOMPARE(
            reloaded.record(
                QStringLiteral("phone-a"))
                ->relayUrlString,
            std::optional<QString>(
                QStringLiteral(
                    "wss://relay.example.test/socket")));

        PairingRecord external =
            *reloaded.record(
                QStringLiteral("phone-a"));
        external.secret[0] = '\x7F';
        QCOMPARE(
            reloaded.record(
                QStringLiteral("phone-a"))
                ->secret.at(0),
            '\x11');
    }

    void pairingRecordCopiesOwnSecretStorage()
    {
        PairingRecord original =
            record(
                QStringLiteral("phone-a"),
                QStringLiteral("Phone A"),
                '\x11',
                kNowMilliseconds);
        PairingRecord copied = original;
        QCOMPARE(
            copied.secret,
            original.secret);
        QVERIFY(
            copied.secret.constData()
            != original.secret.constData());

        PairingRecord assigned =
            record(
                QStringLiteral("phone-b"),
                QStringLiteral("Phone B"),
                '\x22',
                kNowMilliseconds + 1);
        assigned = original;
        QCOMPARE(
            assigned.secret,
            original.secret);
        QVERIFY(
            assigned.secret.constData()
            != original.secret.constData());

        PairingRecord moved =
            std::move(copied);
        QCOMPARE(
            moved.secret,
            original.secret);
        QVERIFY(copied.secret.isEmpty());
    }

    void pairingStoreRejectsInvalidSecretsAndCorruption()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json"));
        XorProtector protector;
        PairingRecordStore store(
            path,
            protector);

        PairingRecord weak = record(
            QStringLiteral("phone"),
            QStringLiteral("Phone"),
            '\x11',
            kNowMilliseconds);
        weak.secret.resize(31);
        QVERIFY(!store.save(weak).hasValue());

        PairingRecord oversized = weak;
        oversized.secret.resize(33);
        QVERIFY(
            !store.save(oversized)
                 .hasValue());
        QVERIFY(store.records().isEmpty());

        QVERIFY(
            store.save(record(
                QStringLiteral("phone"),
                QStringLiteral("Phone"),
                '\x11',
                kNowMilliseconds))
                .hasValue());
        QVERIFY(
            store.save(record(
                QStringLiteral("phone"),
                QStringLiteral("Renamed Phone"),
                '\x22',
                kNowMilliseconds + 1))
                .hasValue());
        QCOMPARE(store.records().size(), 1);
        QCOMPARE(
            store.record(
                QStringLiteral("phone"))
                ->displayName,
            QStringLiteral("Renamed Phone"));

        QFile file(path);
        QVERIFY(
            file.open(
                QIODevice::WriteOnly
                | QIODevice::Truncate));
        QCOMPARE(
            file.write("not json"),
            qint64(8));
        file.close();

        PairingRecordStore corrupted(
            path,
            protector);
        QVERIFY(
            corrupted.records().isEmpty());
        QVERIFY(
            corrupted.loadError()
                .has_value());
        QCOMPARE(
            corrupted.loadError()->code,
            QStringLiteral(
                "pairing.store_corrupt"));

        const auto protectedSecret =
            protector.protect(
                repeatedSecret('\x44'),
                PairingRecordStore::
                    protectionEntropy());
        QVERIFY(protectedSecret.hasValue());
        const QJsonDocument fractional(
            QJsonObject{
                {QStringLiteral("version"), 1},
                {QStringLiteral("records"),
                 QJsonArray{
                     QJsonObject{
                         {QStringLiteral(
                              "deviceID"),
                          QStringLiteral("phone")},
                         {QStringLiteral(
                              "displayName"),
                          QStringLiteral("Phone")},
                         {QStringLiteral(
                              "secretProtected"),
                          QString::fromLatin1(
                              protectedSecret.value()
                                  .toBase64())},
                         {QStringLiteral(
                              "pairedAtMilliseconds"),
                          1.5},
                     },
                 }},
            });
        QVERIFY(
            file.open(
                QIODevice::WriteOnly
                | QIODevice::Truncate));
        const QByteArray fractionalBytes =
            fractional.toJson(
                QJsonDocument::Compact);
        QCOMPARE(
            file.write(fractionalBytes),
            fractionalBytes.size());
        file.close();

        PairingRecordStore fractionalStore(
            path,
            protector);
        QVERIFY(
            fractionalStore.records()
                .isEmpty());
        QVERIFY(
            fractionalStore.loadError()
                .has_value());
        QCOMPARE(
            fractionalStore.loadError()->code,
            QStringLiteral(
                "pairing.store_corrupt"));
    }

    void pairingStoreRejectsUnsafeTimestampNumbers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json"));
        XorProtector protector;
        const auto protectedSecret =
            protector.protect(
                repeatedSecret('\x44'),
                PairingRecordStore::
                    protectionEntropy());
        QVERIFY(protectedSecret.hasValue());

        const auto writeTimestamp =
            [&](QByteArray timestamp) {
                const QByteArray json =
                    QByteArray(
                        "{\"version\":1,\"records\":[{"
                        "\"deviceID\":\"phone\","
                        "\"displayName\":\"Phone\","
                        "\"secretProtected\":\"")
                    + protectedSecret.value()
                          .toBase64()
                    + QByteArray(
                        "\",\"pairedAtMilliseconds\":")
                    + std::move(timestamp)
                    + QByteArray("}]}");
                QFile file(path);
                if (!file.open(
                        QIODevice::WriteOnly
                        | QIODevice::Truncate)) {
                    return false;
                }
                const bool written =
                    file.write(json)
                    == json.size();
                file.close();
                return written;
            };

        QVERIFY(
            writeTimestamp(
                QByteArray(
                    "9223372036854775808")));
        PairingRecordStore aboveInt64(
            path,
            protector);
        QVERIFY(
            aboveInt64.loadError()
                .has_value());
        QCOMPARE(
            aboveInt64.loadError()->code,
            QStringLiteral(
                "pairing.store_corrupt"));

        QVERIFY(
            writeTimestamp(
                QByteArray(
                    "9007199254740992")));
        PairingRecordStore beyondSafeInteger(
            path,
            protector);
        QVERIFY(
            beyondSafeInteger.loadError()
                .has_value());
        QCOMPARE(
            beyondSafeInteger
                .loadError()->code,
            QStringLiteral(
                "pairing.store_corrupt"));
    }

    void pairingStoreRollsBackReplacementWhenCommitFails()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json"));
        XorProtector protector;
        PairingRecordStore store(
            path,
            protector);
        const PairingRecord original =
            record(
                QStringLiteral("phone"),
                QStringLiteral("Original"),
                '\x11',
                kNowMilliseconds);
        QVERIFY(
            store.save(original)
                .hasValue());

        WinHandle locked(
            CreateFileW(
                reinterpret_cast<LPCWSTR>(
                    path.utf16()),
                GENERIC_READ,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
        QVERIFY(locked.isValid());

        const auto replaced =
            store.save(record(
                QStringLiteral("phone"),
                QStringLiteral("Replacement"),
                '\x22',
                kNowMilliseconds + 1));
        QVERIFY(!replaced.hasValue());

        const auto retained =
            store.record(
                QStringLiteral("phone"));
        QVERIFY(retained.has_value());
        QCOMPARE(
            retained->displayName,
            QStringLiteral("Original"));
        QCOMPARE(
            retained->secret,
            repeatedSecret('\x11'));
    }

    void pairingCoordinatorCompletesAndAuthenticatesTrustedDevice()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        QDateTime current = fixedNow();
        PairingCoordinator coordinator(
            store,
            [&current] { return current; },
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [] {
                return Result<QByteArray>::
                    success(
                        repeatedSecret('\x5A'));
            });
        QSignalSpy spy(
            &coordinator,
            &PairingCoordinator::
                pairingStateChanged);

        const auto pairing =
            coordinator.beginPairing();
        QVERIFY(pairing.hasValue());
        QCOMPARE(
            pairing.value().code,
            QStringLiteral("123456"));
        QCOMPARE(
            pairing.value().expiresAt,
            current.addSecs(300));
        QCOMPARE(spy.count(), 1);

        const BridgeInvitation invitation =
            pairingInvitation();
        QCOMPARE(
            coordinator.invitationDecision(
                invitation),
            InvitationDecision::AcceptPairing);
        const auto completed =
            coordinator.completePairing(
                invitation);
        QVERIFY(completed.hasValue());
        QCOMPARE(
            completed.value().secret,
            repeatedSecret('\x5A'));
        QCOMPARE(spy.count(), 2);
        QVERIFY(
            !coordinator.activePairing()
                 .has_value());

        const auto trusted =
            coordinator.trustedRecord(
                QStringLiteral(
                    "iphone-alpha"));
        QVERIFY(trusted.has_value());
        QCOMPARE(
            trusted->displayName,
            QStringLiteral("Harlin iPhone"));

        const auto authenticated =
            BridgeSecurity::
                authenticatedInvitation(
                    QStringLiteral(
                        "iphone-alpha"),
                    QStringLiteral(
                        "Harlin iPhone"),
                    trusted->secret,
                    current,
                    QByteArray(16, '\x01'));
        QVERIFY(authenticated.hasValue());
        QCOMPARE(
            coordinator.invitationDecision(
                authenticated.value()),
            InvitationDecision::AcceptTrusted);
    }

    void pairingCoordinatorPreservesStateWhenPersistenceFails()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        PairingCoordinator coordinator(
            store,
            fixedNow,
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [] {
                return Result<QByteArray>::
                    success(
                        repeatedSecret('\x33'));
            });
        QSignalSpy spy(
            &coordinator,
            &PairingCoordinator::
                pairingStateChanged);
        QVERIFY(
            coordinator.beginPairing()
                .hasValue());
        QCOMPARE(spy.count(), 1);

        protector.failProtect = true;
        const auto result =
            coordinator.completePairing(
                pairingInvitation());
        QVERIFY(!result.hasValue());
        QVERIFY(
            coordinator.activePairing()
                .has_value());
        QVERIFY(store.records().isEmpty());
        QCOMPARE(spy.count(), 1);

        protector.failProtect = false;
        const auto retried =
            coordinator.completePairing(
                pairingInvitation());
        QVERIFY(retried.hasValue());
        QVERIFY(
            !coordinator.activePairing()
                 .has_value());
        QVERIFY(
            store.record(
                QStringLiteral(
                    "iphone-alpha"))
                .has_value());
        QCOMPARE(spy.count(), 2);
    }

    void pairingCoordinatorAllowsOnlyOneConcurrentCompletion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        QSemaphore firstGeneratorEntered;
        QSemaphore releaseFirstGenerator;
        std::atomic_int generatorCalls = 0;
        PairingCoordinator coordinator(
            store,
            fixedNow,
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [&] {
                const int call =
                    generatorCalls.fetch_add(1);
                if (call == 0) {
                    firstGeneratorEntered.release();
                    releaseFirstGenerator.acquire();
                }
                return Result<QByteArray>::
                    success(
                        repeatedSecret(
                            call == 0
                                ? '\x41'
                                : '\x42'));
            });
        std::atomic_int signalCount = 0;
        const QMetaObject::Connection
            signalConnection =
                QObject::connect(
                    &coordinator,
                    &PairingCoordinator::
                        pairingStateChanged,
                    &coordinator,
                    [&] {
                        signalCount.fetch_add(1);
                    },
                    Qt::DirectConnection);
        QVERIFY(
            coordinator.beginPairing()
                .hasValue());

        std::optional<Result<PairingRecord>>
            firstResult;
        std::optional<Result<PairingRecord>>
            secondResult;
        QSemaphore secondFinished;
        std::thread first([&] {
            firstResult.emplace(
                coordinator.completePairing(
                    pairingInvitation()));
        });
        if (!firstGeneratorEntered.tryAcquire(
                1,
                5000)) {
            releaseFirstGenerator.release();
            first.join();
            QFAIL(
                "The first completion did not reach secret generation.");
        }

        std::thread second([&] {
            secondResult.emplace(
                coordinator.completePairing(
                    pairingInvitation()));
            secondFinished.release();
        });
        if (!secondFinished.tryAcquire(
                1,
                5000)) {
            releaseFirstGenerator.release();
            first.join();
            second.join();
            QFAIL(
                "The second completion did not finish while the first was blocked.");
        }
        releaseFirstGenerator.release();
        first.join();
        second.join();

        QVERIFY(firstResult.has_value());
        QVERIFY(secondResult.has_value());
        const int successCount =
            static_cast<int>(
                firstResult->hasValue())
            + static_cast<int>(
                secondResult->hasValue());
        QCOMPARE(successCount, 1);

        const PairingRecord& winner =
            firstResult->hasValue()
            ? firstResult->value()
            : secondResult->value();
        const auto stored =
            store.record(
                QStringLiteral(
                    "iphone-alpha"));
        QVERIFY(stored.has_value());
        QCOMPARE(
            stored->secret,
            winner.secret);
        QCOMPARE(signalCount.load(), 2);
        QObject::disconnect(
            signalConnection);
    }

    void pairingCoordinatorDoesNotClearReplacementPairing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        QSemaphore generatorEntered;
        QSemaphore releaseGenerator;
        std::atomic_int codeCalls = 0;
        PairingCoordinator coordinator(
            store,
            fixedNow,
            [&] {
                const int call =
                    codeCalls.fetch_add(1);
                return Result<QString>::
                    success(
                        call == 0
                            ? QStringLiteral(
                                  "123456")
                            : QStringLiteral(
                                  "654321"));
            },
            [&] {
                generatorEntered.release();
                releaseGenerator.acquire();
                return Result<QByteArray>::
                    success(
                        repeatedSecret(
                            '\x51'));
            });
        std::atomic_int signalCount = 0;
        const QMetaObject::Connection
            signalConnection =
                QObject::connect(
                    &coordinator,
                    &PairingCoordinator::
                        pairingStateChanged,
                    &coordinator,
                    [&] {
                        signalCount.fetch_add(1);
                    },
                    Qt::DirectConnection);
        QVERIFY(
            coordinator.beginPairing()
                .hasValue());

        std::optional<Result<PairingRecord>>
            completion;
        std::thread worker([&] {
            completion.emplace(
                coordinator.completePairing(
                    pairingInvitation()));
        });
        if (!generatorEntered.tryAcquire(
                1,
                5000)) {
            releaseGenerator.release();
            worker.join();
            QFAIL(
                "The completion did not reach secret generation.");
        }

        const auto replacement =
            coordinator.beginPairing();
        QVERIFY(replacement.hasValue());
        QCOMPARE(
            replacement.value().code,
            QStringLiteral("654321"));
        releaseGenerator.release();
        worker.join();

        QVERIFY(completion.has_value());
        QVERIFY(!completion->hasValue());
        const auto active =
            coordinator.activePairing();
        QVERIFY(active.has_value());
        QCOMPARE(
            active->code,
            QStringLiteral("654321"));
        QVERIFY(store.records().isEmpty());
        QCOMPARE(signalCount.load(), 2);
        QObject::disconnect(
            signalConnection);
    }

    void pairingCoordinatorStaleReleasePreservesNewClaim()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        QSemaphore oldGeneratorEntered;
        QSemaphore releaseOldGenerator;
        QSemaphore newGeneratorEntered;
        QSemaphore releaseNewGenerator;
        std::atomic_int codeCalls = 0;
        std::atomic_int secretCalls = 0;
        PairingCoordinator coordinator(
            store,
            fixedNow,
            [&] {
                const int call =
                    codeCalls.fetch_add(1);
                return Result<QString>::
                    success(
                        call == 0
                            ? QStringLiteral(
                                  "123456")
                            : QStringLiteral(
                                  "654321"));
            },
            [&] {
                const int call =
                    secretCalls.fetch_add(1);
                if (call == 0) {
                    oldGeneratorEntered.release();
                    releaseOldGenerator.acquire();
                } else if (call == 1) {
                    newGeneratorEntered.release();
                    releaseNewGenerator.acquire();
                }
                return Result<QByteArray>::
                    success(
                        repeatedSecret(
                            call == 0
                                ? '\x71'
                                : (call == 1
                                       ? '\x72'
                                       : '\x73')));
            });
        QVERIFY(
            coordinator.beginPairing()
                .hasValue());

        std::optional<Result<PairingRecord>>
            oldCompletion;
        std::thread oldWorker([&] {
            oldCompletion.emplace(
                coordinator.completePairing(
                    pairingInvitation()));
        });
        if (!oldGeneratorEntered.tryAcquire(
                1,
                5000)) {
            releaseOldGenerator.release();
            oldWorker.join();
            QFAIL(
                "The old completion did not reach secret generation.");
        }

        QVERIFY(
            coordinator.beginPairing()
                .hasValue());
        std::optional<Result<PairingRecord>>
            newCompletion;
        std::thread newWorker([&] {
            newCompletion.emplace(
                coordinator.completePairing(
                    pairingInvitation(
                        QStringLiteral(
                            "654-321"))));
        });
        if (!newGeneratorEntered.tryAcquire(
                1,
                5000)) {
            releaseOldGenerator.release();
            releaseNewGenerator.release();
            oldWorker.join();
            newWorker.join();
            QFAIL(
                "The replacement completion did not claim the new session.");
        }

        releaseOldGenerator.release();
        oldWorker.join();
        QVERIFY(oldCompletion.has_value());
        QVERIFY(!oldCompletion->hasValue());

        std::optional<Result<PairingRecord>>
            competingCompletion;
        QSemaphore competingFinished;
        std::thread competingWorker([&] {
            competingCompletion.emplace(
                coordinator.completePairing(
                    pairingInvitation(
                        QStringLiteral(
                            "654-321"))));
            competingFinished.release();
        });
        if (!competingFinished.tryAcquire(
                1,
                5000)) {
            releaseNewGenerator.release();
            competingWorker.join();
            newWorker.join();
            QFAIL(
                "The competing completion did not observe the replacement claim.");
        }
        competingWorker.join();
        releaseNewGenerator.release();
        newWorker.join();

        QVERIFY(
            competingCompletion.has_value());
        QVERIFY(
            !competingCompletion
                 ->hasValue());
        QCOMPARE(
            competingCompletion
                ->error().code,
            QStringLiteral(
                "pairing.completion_in_progress"));
        QVERIFY(newCompletion.has_value());
        QVERIFY(newCompletion->hasValue());
        const auto stored =
            store.record(
                QStringLiteral(
                    "iphone-alpha"));
        QVERIFY(stored.has_value());
        QCOMPARE(
            stored->secret,
            repeatedSecret('\x72'));
    }

    void pairingCoordinatorCancelInvalidatesInflightCompletion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        QSemaphore generatorEntered;
        QSemaphore releaseGenerator;
        PairingCoordinator coordinator(
            store,
            fixedNow,
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [&] {
                generatorEntered.release();
                releaseGenerator.acquire();
                return Result<QByteArray>::
                    success(
                        repeatedSecret(
                            '\x61'));
            });
        std::atomic_int signalCount = 0;
        const QMetaObject::Connection
            signalConnection =
                QObject::connect(
                    &coordinator,
                    &PairingCoordinator::
                        pairingStateChanged,
                    &coordinator,
                    [&] {
                        signalCount.fetch_add(1);
                    },
                    Qt::DirectConnection);
        QVERIFY(
            coordinator.beginPairing()
                .hasValue());

        std::optional<Result<PairingRecord>>
            completion;
        std::thread worker([&] {
            completion.emplace(
                coordinator.completePairing(
                    pairingInvitation()));
        });
        if (!generatorEntered.tryAcquire(
                1,
                5000)) {
            releaseGenerator.release();
            worker.join();
            QFAIL(
                "The completion did not reach secret generation.");
        }

        coordinator.cancelPairing();
        releaseGenerator.release();
        worker.join();

        QVERIFY(completion.has_value());
        QVERIFY(!completion->hasValue());
        QVERIFY(
            !coordinator.activePairing()
                 .has_value());
        QVERIFY(store.records().isEmpty());
        QCOMPARE(signalCount.load(), 2);
        QObject::disconnect(
            signalConnection);
    }

    void pairingCoordinatorExpiryInvalidatesInflightCompletion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        std::atomic<qint64> nowMilliseconds =
            kNowMilliseconds;
        QSemaphore generatorEntered;
        QSemaphore releaseGenerator;
        PairingCoordinator coordinator(
            store,
            [&] {
                return QDateTime::
                    fromMSecsSinceEpoch(
                        nowMilliseconds.load(),
                        QTimeZone::UTC);
            },
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [&] {
                generatorEntered.release();
                releaseGenerator.acquire();
                return Result<QByteArray>::
                    success(
                        repeatedSecret(
                            '\x62'));
            });
        std::atomic_int signalCount = 0;
        const QMetaObject::Connection
            signalConnection =
                QObject::connect(
                    &coordinator,
                    &PairingCoordinator::
                        pairingStateChanged,
                    &coordinator,
                    [&] {
                        signalCount.fetch_add(1);
                    },
                    Qt::DirectConnection);
        QVERIFY(
            coordinator.beginPairing()
                .hasValue());

        std::optional<Result<PairingRecord>>
            completion;
        std::thread worker([&] {
            completion.emplace(
                coordinator.completePairing(
                    pairingInvitation()));
        });
        if (!generatorEntered.tryAcquire(
                1,
                5000)) {
            releaseGenerator.release();
            worker.join();
            QFAIL(
                "The completion did not reach secret generation.");
        }

        nowMilliseconds.store(
            kNowMilliseconds
            + 300000
            + 1);
        QVERIFY(
            !coordinator.activePairing()
                 .has_value());
        releaseGenerator.release();
        worker.join();

        QVERIFY(completion.has_value());
        QVERIFY(!completion->hasValue());
        QVERIFY(store.records().isEmpty());
        QCOMPARE(signalCount.load(), 2);
        QObject::disconnect(
            signalConnection);
    }

    void pairingCoordinatorExpiresCancelsAndForgetsPrecisely()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        QDateTime current = fixedNow();
        PairingCoordinator coordinator(
            store,
            [&current] { return current; },
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            BridgeSecurity::randomSecret);
        QSignalSpy spy(
            &coordinator,
            &PairingCoordinator::
                pairingStateChanged);

        QVERIFY(
            coordinator.beginPairing(300s)
                .hasValue());
        current = fixedNow().addSecs(300);
        QVERIFY(
            coordinator.activePairing()
                .has_value());
        current = current.addMSecs(1);
        QVERIFY(
            !coordinator.activePairing()
                 .has_value());
        QCOMPARE(spy.count(), 2);

        coordinator.cancelPairing();
        coordinator.cancelPairing();
        QCOMPARE(spy.count(), 2);

        QVERIFY(
            coordinator.remember(record(
                QStringLiteral("phone-a"),
                QStringLiteral("Phone A"),
                '\x11',
                kNowMilliseconds))
                .hasValue());
        QVERIFY(
            coordinator.remember(record(
                QStringLiteral("phone-b"),
                QStringLiteral("Phone B"),
                '\x22',
                kNowMilliseconds + 1))
                .hasValue());
        QCOMPARE(spy.count(), 4);

        QVERIFY(
            coordinator.forget(
                QStringLiteral("phone-a"))
                .hasValue());
        QCOMPARE(spy.count(), 5);
        QVERIFY(
            !coordinator.trustedRecord(
                QStringLiteral("phone-a"))
                 .has_value());
        QVERIFY(
            coordinator.trustedRecord(
                QStringLiteral("phone-b"))
                .has_value());
    }

    void pairingCoordinatorRejectsInvalidGenerators()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        PairingRecordStore store(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        PairingCoordinator invalidCode(
            store,
            fixedNow,
            [] {
                return Result<QString>::
                    success(
                        QString::fromUtf8(
                            "\xEF\xBC\x91"
                            "\xEF\xBC\x92"
                            "\xEF\xBC\x93"
                            "456"));
            },
            BridgeSecurity::randomSecret);
        QSignalSpy invalidCodeSpy(
            &invalidCode,
            &PairingCoordinator::
                pairingStateChanged);
        const auto rejectedCode =
            invalidCode.beginPairing();
        QVERIFY(!rejectedCode.hasValue());
        QCOMPARE(
            rejectedCode.error().code,
            QStringLiteral(
                "pairing.invalid_code"));
        QCOMPARE(invalidCodeSpy.count(), 0);

        PairingCoordinator invalidSecret(
            store,
            fixedNow,
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [] {
                return Result<QByteArray>::
                    success(
                        QByteArray(31, '\x55'));
            });
        QVERIFY(
            invalidSecret.beginPairing()
                .hasValue());
        const auto rejectedSecret =
            invalidSecret.completePairing(
                pairingInvitation());
        QVERIFY(!rejectedSecret.hasValue());
        QCOMPARE(
            rejectedSecret.error().code,
            QStringLiteral(
                "pairing.invalid_secret"));
        QVERIFY(
            invalidSecret.activePairing()
                .has_value());
        QVERIFY(store.records().isEmpty());

        invalidSecret.cancelPairing();
        const auto inactive =
            invalidSecret.completePairing(
                pairingInvitation());
        QVERIFY(!inactive.hasValue());
        QCOMPARE(
            inactive.error().code,
            QStringLiteral(
                "pairing.not_active"));
    }
};

QTEST_MAIN(PairingCoordinatorTests)

#include "PairingCoordinatorTests.moc"
