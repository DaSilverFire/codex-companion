#include "mobile/security/RelayStateStore.h"
#include "mobile/security/SecretProtector.h"
#include "platform/windows/security/WindowsDpapiProtector.h"

#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {

using namespace companion;

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
        unsigned char mask = 0x5A;
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
        if (isValid()) {
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

QByteArray readAll(
    const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool writeAll(
    const QString& path,
    const QByteArray& bytes)
{
    const QFileInfo information(path);
    if (!QDir().mkpath(
            information.absolutePath())) {
        return false;
    }
    QFile file(path);
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes)
        == bytes.size();
}

QByteArray oneEntryState(
    QByteArray nextOutbound,
    QByteArray highestInbound =
        QByteArray("0"))
{
    return QByteArray(
        "{\"entries\":[{"
        "\"channelID\":\"channel\","
        "\"highestInbound\":\"")
        + std::move(highestInbound)
        + QByteArray(
            "\",\"nextOutbound\":\"")
        + std::move(nextOutbound)
        + QByteArray(
            "\",\"senderID\":\"sender\"}],"
            "\"version\":1}");
}

bool writeProtectedState(
    const QString& path,
    const SecretProtector& protector,
    QByteArray plaintext,
    QByteArray entropy =
        RelayStateStore::
            protectionEntropy())
{
    const auto protectedState =
        protector.protect(
            plaintext,
            entropy);
    return protectedState.hasValue()
        && writeAll(
            path,
            protectedState.value());
}

} // namespace

class RelayStateStoreTests final
    : public QObject {
    Q_OBJECT

private slots:
    void defaultPathAndEntropyAreVersioned()
    {
        const QString normalized =
            QDir::fromNativeSeparators(
                RelayStateStore::
                    defaultFilePath());
        QVERIFY(
            normalized.endsWith(
                QStringLiteral(
                    "Codex Companion/Security/"
                    "relay-state.v1.dpapi"),
                Qt::CaseInsensitive));
        QCOMPARE(
            RelayStateStore::
                protectionEntropy(),
            QByteArray(
                "Codex Companion relay state v1"));
    }

    void outboundSequencesPersistAndUseTupleKeys()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "Security/relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);

        const auto first =
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"));
        QVERIFY(first.hasValue());
        QCOMPARE(first.value(), quint64(1));
        const auto second =
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"));
        QVERIFY(second.hasValue());
        QCOMPARE(second.value(), quint64(2));

        const auto otherSender =
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-b"));
        QVERIFY(otherSender.hasValue());
        QCOMPARE(
            otherSender.value(),
            quint64(1));
        const auto otherChannel =
            store.nextOutbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender-a"));
        QVERIFY(otherChannel.hasValue());
        QCOMPARE(
            otherChannel.value(),
            quint64(1));

        RelayStateStore restarted(
            path,
            protector);
        const auto afterRestart =
            restarted.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"));
        QVERIFY(afterRestart.hasValue());
        QCOMPARE(
            afterRestart.value(),
            quint64(3));
    }

    void firstSuccessfulReservationIsDurableBeforeReturn()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;

        {
            RelayStateStore store(
                path,
                protector);
            const auto first =
                store.nextOutbound(
                    QStringLiteral("channel"),
                    QStringLiteral("sender"));
            QVERIFY(first.hasValue());
            QCOMPARE(first.value(), quint64(1));
        }

        RelayStateStore restarted(
            path,
            protector);
        const auto second =
            restarted.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(second.hasValue());
        QCOMPARE(second.value(), quint64(2));
    }

    void inboundReplayPersistsAndRejectsNonIncreasing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);

        const auto zero =
            store.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                0);
        QVERIFY(zero.hasValue());
        QVERIFY(!zero.value());

        const auto first =
            store.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                41);
        QVERIFY(first.hasValue());
        QVERIFY(first.value());
        const auto duplicate =
            store.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                41);
        QVERIFY(duplicate.hasValue());
        QVERIFY(!duplicate.value());
        const auto lower =
            store.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                40);
        QVERIFY(lower.hasValue());
        QVERIFY(!lower.value());

        RelayStateStore restarted(
            path,
            protector);
        const auto replay =
            restarted.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                41);
        QVERIFY(replay.hasValue());
        QVERIFY(!replay.value());
        const auto next =
            restarted.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                42);
        QVERIFY(next.hasValue());
        QVERIFY(next.value());
    }

    void stateFileIsProtectedAndDeterministic()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);
        QVERIFY(
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"))
                .hasValue());
        QVERIFY(
            store.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"),
                41)
                .value());
        QVERIFY(
            store.nextOutbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender-a"))
                .hasValue());
        QVERIFY(
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-b"))
                .hasValue());

        const QByteArray protectedBytes =
            readAll(path);
        QVERIFY(!protectedBytes.isEmpty());
        QVERIFY(
            !protectedBytes.contains(
                "channel-a"));
        QVERIFY(
            !protectedBytes.contains(
                "\"entries\""));

        const auto plaintext =
            protector.unprotect(
                protectedBytes,
                RelayStateStore::
                    protectionEntropy());
        QVERIFY(plaintext.hasValue());
        QCOMPARE(
            plaintext.value(),
            QByteArray(
                "{\"entries\":[{"
                "\"channelID\":\"channel-a\","
                "\"highestInbound\":\"41\","
                "\"nextOutbound\":\"2\","
                "\"senderID\":\"sender-a\"},{"
                "\"channelID\":\"channel-a\","
                "\"highestInbound\":\"0\","
                "\"nextOutbound\":\"2\","
                "\"senderID\":\"sender-b\"},{"
                "\"channelID\":\"channel-b\","
                "\"highestInbound\":\"0\","
                "\"nextOutbound\":\"2\","
                "\"senderID\":\"sender-a\"}],"
                "\"version\":1}"));
    }

    void realDpapiStateSurvivesRestart()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        WindowsDpapiProtector protector;
        {
            RelayStateStore store(
                path,
                protector);
            const auto first =
                store.nextOutbound(
                    QStringLiteral("channel"),
                    QStringLiteral("sender"));
            QVERIFY(first.hasValue());
            QCOMPARE(
                first.value(),
                quint64(1));
        }
        QVERIFY(
            !readAll(path).contains(
                "channel"));

        RelayStateStore restarted(
            path,
            protector);
        const auto second =
            restarted.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(second.hasValue());
        QCOMPARE(second.value(), quint64(2));

        QByteArray corrupted =
            readAll(path);
        QVERIFY(corrupted.size() > 8);
        corrupted[corrupted.size() / 2] ^=
            0x01;
        QVERIFY(
            writeAll(
                path,
                corrupted));
        RelayStateStore rejected(
            path,
            protector);
        const auto corruptResult =
            rejected.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(!corruptResult.hasValue());
        QCOMPARE(
            corruptResult.error().code,
            QStringLiteral(
                "relay.state_corrupt"));
    }

    void exhaustionFailsClosedWithoutReuse()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        QVERIFY(
            writeProtectedState(
                path,
                protector,
                oneEntryState(
                    QByteArray(
                        "18446744073709551614"))));

        RelayStateStore store(
            path,
            protector);
        const auto finalIssued =
            store.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(finalIssued.hasValue());
        QCOMPARE(
            finalIssued.value(),
            std::numeric_limits<
                quint64>::max() - 1);

        RelayStateStore restarted(
            path,
            protector);
        const auto stillExhausted =
            restarted.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(!stillExhausted.hasValue());
        QCOMPARE(
            stillExhausted.error().code,
            QStringLiteral(
                "relay.sequence_exhausted"));
    }

    void corruptionAndWrongEntropyFailClosed()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        QVERIFY(
            writeProtectedState(
                path,
                protector,
                QByteArray("not json")));
        const QByteArray corruptBytes =
            readAll(path);

        RelayStateStore malformed(
            path,
            protector);
        const auto malformedResult =
            malformed.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(
            !malformedResult.hasValue());
        QCOMPARE(
            malformedResult.error().code,
            QStringLiteral(
                "relay.state_corrupt"));
        QCOMPARE(readAll(path), corruptBytes);

        QVERIFY(
            writeProtectedState(
                path,
                protector,
                oneEntryState(
                    QByteArray("1")),
                QByteArray("wrong entropy")));
        const QByteArray wrongEntropy =
            readAll(path);
        RelayStateStore locked(
            path,
            protector);
        const auto unlockResult =
            locked.acceptInbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"),
                1);
        QVERIFY(!unlockResult.hasValue());
        QCOMPARE(
            unlockResult.error().code,
            QStringLiteral(
                "relay.state_corrupt"));
        QCOMPARE(readAll(path), wrongEntropy);
    }

    void strictStateParsingRejectsMalformedEntries_data()
    {
        QTest::addColumn<QByteArray>(
            "plaintext");

        QTest::newRow("unsupported-version")
            << QByteArray(
                   "{\"entries\":[],\"version\":2}");
        QTest::newRow("entries-not-array")
            << QByteArray(
                   "{\"entries\":{},\"version\":1}");
        QTest::newRow("unknown-root-key")
            << QByteArray(
                   "{\"entries\":[],"
                   "\"future\":true,"
                   "\"version\":1}");
        QTest::newRow("duplicate-root-key")
            << QByteArray(
                   "{\"entries\":[],"
                   "\"version\":1,"
                   "\"version\":1}");
        QTest::newRow("missing-field")
            << QByteArray(
                   "{\"entries\":[{"
                   "\"channelID\":\"channel\","
                   "\"nextOutbound\":\"1\","
                   "\"senderID\":\"sender\"}],"
                   "\"version\":1}");
        QTest::newRow("unknown-entry-key")
            << QByteArray(
                   "{\"entries\":[{"
                   "\"channelID\":\"channel\","
                   "\"future\":\"value\","
                   "\"highestInbound\":\"0\","
                   "\"nextOutbound\":\"1\","
                   "\"senderID\":\"sender\"}],"
                   "\"version\":1}");
        QTest::newRow("duplicate-entry-key")
            << QByteArray(
                   "{\"entries\":[{"
                   "\"channelID\":\"channel\","
                   "\"highestInbound\":\"0\","
                   "\"nextOutbound\":\"1\","
                   "\"senderID\":\"sender\","
                   "\"senderID\":\"sender\"}],"
                   "\"version\":1}");
        QTest::newRow("noncanonical-decimal")
            << oneEntryState(
                   QByteArray("01"));
        QTest::newRow("zero-next-outbound")
            << oneEntryState(
                   QByteArray("0"));
        QTest::newRow("overflow")
            << oneEntryState(
                   QByteArray(
                       "18446744073709551616"));
        QTest::newRow("negative")
            << oneEntryState(
                   QByteArray("-1"));
        QTest::newRow("duplicate-tuple")
            << QByteArray(
                   "{\"entries\":[{"
                   "\"channelID\":\"channel\","
                   "\"highestInbound\":\"0\","
                   "\"nextOutbound\":\"1\","
                   "\"senderID\":\"sender\"},{"
                   "\"channelID\":\"channel\","
                   "\"highestInbound\":\"1\","
                   "\"nextOutbound\":\"2\","
                   "\"senderID\":\"sender\"}],"
                   "\"version\":1}");
    }

    void strictStateParsingRejectsMalformedEntries()
    {
        QFETCH(QByteArray, plaintext);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        QVERIFY(
            writeProtectedState(
                path,
                protector,
                plaintext));
        const QByteArray original =
            readAll(path);

        RelayStateStore store(
            path,
            protector);
        const auto result =
            store.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "relay.state_corrupt"));
        QCOMPARE(readAll(path), original);
    }

    void writeFailureRollsBackMutation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);
        const auto first =
            store.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(first.hasValue());
        QCOMPARE(first.value(), quint64(1));
        const QByteArray beforeFailure =
            readAll(path);
        QVERIFY(!beforeFailure.isEmpty());

        {
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
            const auto failed =
                store.nextOutbound(
                    QStringLiteral("channel"),
                    QStringLiteral("sender"));
            QVERIFY(!failed.hasValue());
            QCOMPARE(
                failed.error().code,
                QStringLiteral(
                    "relay.state_write_failed"));
        }
        QCOMPARE(readAll(path), beforeFailure);

        RelayStateStore restarted(
            path,
            protector);
        const auto afterRestart =
            restarted.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(afterRestart.hasValue());
        QCOMPARE(
            afterRestart.value(),
            quint64(2));
    }

    void protectionFailureRollsBackAllMutationKinds()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);
        QCOMPARE(
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"))
                .value(),
            quint64(1));
        QVERIFY(
            store.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"),
                7)
                .value());
        QCOMPARE(
            store.nextOutbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender"))
                .value(),
            quint64(1));
        QVERIFY(
            store.acceptInbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender"),
                9)
                .value());
        const QByteArray beforeFailure =
            readAll(path);
        QVERIFY(!beforeFailure.isEmpty());

        protector.failProtect = true;
        const auto outbound =
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"));
        QVERIFY(!outbound.hasValue());
        QCOMPARE(
            outbound.error().code,
            QStringLiteral(
                "relay.state_write_failed"));
        QCOMPARE(readAll(path), beforeFailure);

        const auto inbound =
            store.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"),
                8);
        QVERIFY(!inbound.hasValue());
        QCOMPARE(
            inbound.error().code,
            QStringLiteral(
                "relay.state_write_failed"));
        QCOMPARE(readAll(path), beforeFailure);

        const auto erased =
            store.eraseChannel(
                QStringLiteral("channel-a"));
        QVERIFY(!erased.hasValue());
        QCOMPARE(
            erased.error().code,
            QStringLiteral(
                "relay.state_write_failed"));
        QCOMPARE(readAll(path), beforeFailure);

        protector.failProtect = false;
        RelayStateStore restarted(
            path,
            protector);
        const auto nextA =
            restarted.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"));
        QVERIFY(nextA.hasValue());
        QCOMPARE(nextA.value(), quint64(2));
        const auto replayA =
            restarted.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"),
                7);
        QVERIFY(replayA.hasValue());
        QVERIFY(!replayA.value());
        const auto nextInboundA =
            restarted.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender"),
                8);
        QVERIFY(nextInboundA.hasValue());
        QVERIFY(nextInboundA.value());
        const auto replayB =
            restarted.acceptInbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender"),
                9);
        QVERIFY(replayB.hasValue());
        QVERIFY(!replayB.value());
    }

    void eraseChannelIsPreciseAndIdempotent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);
        QVERIFY(
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"))
                .hasValue());
        QVERIFY(
            store.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-b"))
                .hasValue());
        QVERIFY(
            store.nextOutbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender-a"))
                .hasValue());
        QVERIFY(
            store.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"),
                7)
                .value());
        QVERIFY(
            store.acceptInbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender-a"),
                9)
                .value());
        QVERIFY(
            store.eraseChannel(
                QStringLiteral("channel-a"))
                .hasValue());
        QVERIFY(
            store.eraseChannel(
                QStringLiteral("channel-a"))
                .hasValue());

        RelayStateStore restarted(
            path,
            protector);
        const auto resetA =
            restarted.nextOutbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"));
        QVERIFY(resetA.hasValue());
        QCOMPARE(resetA.value(), quint64(1));
        const auto preservedB =
            restarted.nextOutbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender-a"));
        QVERIFY(preservedB.hasValue());
        QCOMPARE(
            preservedB.value(),
            quint64(2));
        const auto resetInboundA =
            restarted.acceptInbound(
                QStringLiteral("channel-a"),
                QStringLiteral("sender-a"),
                7);
        QVERIFY(resetInboundA.hasValue());
        QVERIFY(resetInboundA.value());
        const auto preservedInboundB =
            restarted.acceptInbound(
                QStringLiteral("channel-b"),
                QStringLiteral("sender-a"),
                9);
        QVERIFY(preservedInboundB.hasValue());
        QVERIFY(!preservedInboundB.value());
    }

    void rejectsEmptyStateKeys()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        XorProtector protector;
        RelayStateStore store(
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi")),
            protector);

        const auto noChannel =
            store.nextOutbound(
                {},
                QStringLiteral("sender"));
        QVERIFY(!noChannel.hasValue());
        QCOMPARE(
            noChannel.error().code,
            QStringLiteral(
                "relay.invalid_state_key"));
        const auto noSender =
            store.acceptInbound(
                QStringLiteral("channel"),
                {},
                1);
        QVERIFY(!noSender.hasValue());
        QCOMPARE(
            noSender.error().code,
            QStringLiteral(
                "relay.invalid_state_key"));
        const auto eraseEmpty =
            store.eraseChannel({});
        QVERIFY(!eraseEmpty.hasValue());
        QCOMPARE(
            eraseEmpty.error().code,
            QStringLiteral(
                "relay.invalid_state_key"));
    }

    void concurrentCallsRemainMonotonic()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.dpapi"));
        XorProtector protector;
        RelayStateStore store(
            path,
            protector);

        constexpr int callCount = 32;
        QSemaphore ready;
        QSemaphore start;
        std::mutex resultsMutex;
        QVector<quint64> sequences;
        std::vector<std::thread> workers;
        workers.reserve(callCount);
        for (int index = 0;
             index < callCount;
             ++index) {
            workers.emplace_back([&] {
                ready.release();
                start.acquire();
                const auto result =
                    store.nextOutbound(
                        QStringLiteral(
                            "channel"),
                        QStringLiteral(
                            "sender"));
                if (result.hasValue()) {
                    const std::scoped_lock lock(
                        resultsMutex);
                    sequences.append(
                        result.value());
                }
            });
        }
        QVERIFY(
            ready.tryAcquire(
                callCount,
                5000));
        start.release(callCount);
        for (std::thread& worker :
             workers) {
            worker.join();
        }

        QCOMPARE(
            sequences.size(),
            callCount);
        std::sort(
            sequences.begin(),
            sequences.end());
        for (int index = 0;
             index < callCount;
             ++index) {
            QCOMPARE(
                sequences.at(index),
                quint64(index + 1));
        }

        std::vector<std::thread> inbound;
        inbound.reserve(callCount);
        for (int sequence = 1;
             sequence <= callCount;
             ++sequence) {
            inbound.emplace_back(
                [&, sequence] {
                    store.acceptInbound(
                        QStringLiteral(
                            "channel-in"),
                        QStringLiteral(
                            "sender"),
                        static_cast<quint64>(
                            sequence));
                });
        }
        for (std::thread& worker :
             inbound) {
            worker.join();
        }
        const auto highestReplay =
            store.acceptInbound(
                QStringLiteral("channel-in"),
                QStringLiteral("sender"),
                callCount);
        QVERIFY(highestReplay.hasValue());
        QVERIFY(!highestReplay.value());
        const auto nextInbound =
            store.acceptInbound(
                QStringLiteral("channel-in"),
                QStringLiteral("sender"),
                callCount + 1);
        QVERIFY(nextInbound.hasValue());
        QVERIFY(nextInbound.value());

        RelayStateStore restarted(
            path,
            protector);
        const auto nextOutbound =
            restarted.nextOutbound(
                QStringLiteral("channel"),
                QStringLiteral("sender"));
        QVERIFY(nextOutbound.hasValue());
        QCOMPARE(
            nextOutbound.value(),
            quint64(callCount + 1));
    }
};

QTEST_MAIN(RelayStateStoreTests)

#include "RelayStateStoreTests.moc"
