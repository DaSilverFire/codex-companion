#include "core/CompanionCommandBus.h"

#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <optional>
#include <thread>
#include <utility>

using namespace companion;

namespace companion::detail {

struct CompanionCommandBusTestAccess final {
    static Result<void> registerHandlerTransaction(
        CompanionCommandBus& bus,
        const QString& command,
        CompanionCommandBus::Handler handler,
        quint64& registrationId)
    {
        return bus.registerHandlerTransaction(
            command,
            std::move(handler),
            registrationId);
    }

    static void rollbackHandlerRegistration(
        CompanionCommandBus& bus,
        const QString& command,
        quint64 registrationId)
    {
        bus.rollbackHandlerRegistration(
            command,
            registrationId);
    }
};

} // namespace companion::detail

namespace {

CompanionCommandBus::Handler countingHandler(int& calls)
{
    return [&calls](
               const QVariantMap&,
               CompanionCommandBus::Completion completion) {
        ++calls;
        completion(Result<void>::success());
    };
}

} // namespace

class CompanionCommandBusTests final : public QObject {
    Q_OBJECT

private slots:
    void detailedCompletionUsesStableExecutionIdentity()
    {
        CompanionCommandBus bus;
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("runtime.first"),
                   [](const QVariantMap&,
                      CompanionCommandBus::Completion completion) {
                       completion(Result<void>::success());
                   })
                .hasValue());
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("runtime.second"),
                   [](const QVariantMap&,
                      CompanionCommandBus::Completion completion) {
                       completion(Result<void>::failure({
                           QStringLiteral("runtime.synthetic"),
                           QStringLiteral("Synthetic failure."),
                           false,
                           {},
                       }));
                   })
                .hasValue());
        QSignalSpy detailedSpy(
            &bus,
            &CompanionCommandBus::commandFinishedDetailed);
        QVERIFY(detailedSpy.isValid());

        const quint64 firstExecution =
            bus.execute(QStringLiteral("runtime.first"));
        const quint64 secondExecution =
            bus.execute(QStringLiteral("runtime.second"));

        QVERIFY(firstExecution != 0);
        QVERIFY(secondExecution != 0);
        QVERIFY(firstExecution != secondExecution);
        QTRY_COMPARE_WITH_TIMEOUT(
            detailedSpy.count(),
            2,
            1000);
        QCOMPARE(
            detailedSpy.at(0).at(0).toString(),
            QStringLiteral("runtime.first"));
        QCOMPARE(
            detailedSpy.at(0).at(1).toULongLong(),
            firstExecution);
        QVERIFY(detailedSpy.at(0).at(2).toBool());
        QCOMPARE(
            detailedSpy.at(1).at(0).toString(),
            QStringLiteral("runtime.second"));
        QCOMPARE(
            detailedSpy.at(1).at(1).toULongLong(),
            secondExecution);
        QVERIFY(!detailedSpy.at(1).at(2).toBool());
        QCOMPARE(
            detailedSpy.at(1).at(3).toString(),
            QStringLiteral("runtime.synthetic"));
    }

    void partialReplacementFailureLeavesOldGroupActive()
    {
        CompanionCommandBus bus;
        int oldFirstCalls = 0;
        int oldSecondCalls = 0;
        int replacementCalls = 0;
        int blockerCalls = 0;

        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral("runtime.first"),
                           countingHandler(oldFirstCalls),
                       },
                       {
                           QStringLiteral("runtime.second"),
                           countingHandler(oldSecondCalls),
                       },
                   })
                .hasValue());
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("runtime.blocked"),
                   countingHandler(blockerCalls))
                .hasValue());

        const Result<void> replacement =
            bus.replaceHandlerGroup(
                QStringLiteral("codex.runtime"),
                {
                    {
                        QStringLiteral("runtime.first"),
                        countingHandler(replacementCalls),
                    },
                    {
                        QStringLiteral("runtime.blocked"),
                        countingHandler(replacementCalls),
                    },
                });

        QVERIFY(!replacement.hasValue());
        QCOMPARE(
            replacement.error().code,
            QStringLiteral(
                "ui.command_already_registered"));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("runtime.first"));
        bus.execute(QStringLiteral("runtime.second"));
        bus.execute(QStringLiteral("runtime.blocked"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            1000);

        QCOMPARE(oldFirstCalls, 1);
        QCOMPARE(oldSecondCalls, 1);
        QCOMPARE(replacementCalls, 0);
        QCOMPARE(blockerCalls, 1);
    }

    void invalidReplacementLeavesOldGroupActive()
    {
        CompanionCommandBus bus;
        int oldCalls = 0;
        int replacementCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral("runtime.old"),
                           countingHandler(oldCalls),
                       },
                   })
                .hasValue());

        const Result<void> blankGroup =
            bus.replaceHandlerGroup(
                QStringLiteral(" \t "),
                {
                    {
                        QStringLiteral("runtime.new"),
                        countingHandler(replacementCalls),
                    },
                });
        QVERIFY(!blankGroup.hasValue());
        QCOMPARE(
            blankGroup.error().code,
            QStringLiteral("ui.invalid_command"));

        const Result<void> blankCommand =
            bus.replaceHandlerGroup(
                QStringLiteral("codex.runtime"),
                {
                    {
                        QStringLiteral(" \r\n "),
                        countingHandler(replacementCalls),
                    },
                });
        QVERIFY(!blankCommand.hasValue());
        QCOMPARE(
            blankCommand.error().code,
            QStringLiteral("ui.invalid_command"));

        const Result<void> missingHandler =
            bus.replaceHandlerGroup(
                QStringLiteral("codex.runtime"),
                {
                    {
                        QStringLiteral("runtime.new"),
                        {},
                    },
                });
        QVERIFY(!missingHandler.hasValue());
        QCOMPARE(
            missingHandler.error().code,
            QStringLiteral("ui.invalid_command"));

        const Result<void> duplicate =
            bus.replaceHandlerGroup(
                QStringLiteral("codex.runtime"),
                {
                    {
                        QStringLiteral("runtime.new"),
                        countingHandler(replacementCalls),
                    },
                    {
                        QStringLiteral("runtime.new"),
                        countingHandler(replacementCalls),
                    },
                });
        QVERIFY(!duplicate.hasValue());
        QCOMPARE(
            duplicate.error().code,
            QStringLiteral("ui.invalid_command"));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("runtime.old"));
        bus.execute(QStringLiteral("runtime.new"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QCOMPARE(oldCalls, 1);
        QCOMPARE(replacementCalls, 0);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
    }

    void inactiveReservationBlocksReplacementAndStaleRollback()
    {
        CompanionCommandBus bus;
        int oldCalls = 0;
        int replacementCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral("runtime.old"),
                           countingHandler(oldCalls),
                       },
                   })
                .hasValue());

        quint64 reservationId = 0;
        QVERIFY(
            detail::CompanionCommandBusTestAccess::
                registerHandlerTransaction(
                    bus,
                    QStringLiteral("runtime.reserved"),
                    countingHandler(replacementCalls),
                    reservationId)
                .hasValue());
        QVERIFY(reservationId != 0);

        const Result<void> blocked =
            bus.replaceHandlerGroup(
                QStringLiteral("codex.runtime"),
                {
                    {
                        QStringLiteral("runtime.reserved"),
                        countingHandler(replacementCalls),
                    },
                });
        QVERIFY(!blocked.hasValue());
        QCOMPARE(
            blocked.error().code,
            QStringLiteral(
                "ui.command_already_registered"));

        QSignalSpy startedSpy(
            &bus,
            &CompanionCommandBus::commandStarted);
        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("runtime.reserved"));
        bus.execute(QStringLiteral("runtime.old"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QCOMPARE(startedSpy.size(), 1);
        QCOMPARE(oldCalls, 1);
        QCOMPARE(replacementCalls, 0);

        detail::CompanionCommandBusTestAccess::
            rollbackHandlerRegistration(
                bus,
                QStringLiteral("runtime.reserved"),
                reservationId);
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral(
                               "runtime.reserved"),
                           countingHandler(
                               replacementCalls),
                       },
                   })
                .hasValue());

        detail::CompanionCommandBusTestAccess::
            rollbackHandlerRegistration(
                bus,
                QStringLiteral("runtime.reserved"),
                reservationId);
        bus.execute(QStringLiteral("runtime.reserved"));
        bus.execute(QStringLiteral("runtime.old"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            4,
            1000);
        QCOMPARE(replacementCalls, 1);
        QCOMPARE(oldCalls, 1);
        QVERIFY(finishedSpy.at(2).at(1).toBool());
        QVERIFY(!finishedSpy.at(3).at(1).toBool());
    }

    void replacementIsAtomicAcrossCopiedAndFutureHandlers()
    {
        CompanionCommandBus bus;
        int oldCalls = 0;
        int removedCalls = 0;
        int newCalls = 0;
        int addedCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral("runtime.first"),
                           countingHandler(oldCalls),
                       },
                       {
                           QStringLiteral(
                               "runtime.removed"),
                           countingHandler(removedCalls),
                       },
                   })
                .hasValue());

        std::optional<Result<void>> replacement;
        QObject::connect(
            &bus,
            &CompanionCommandBus::commandStarted,
            &bus,
            [&bus, &replacement, &newCalls, &addedCalls](
                const QString& command) {
                if (command
                    != QStringLiteral("runtime.first")
                    || replacement.has_value()) {
                    return;
                }
                replacement.emplace(
                    bus.replaceHandlerGroup(
                        QStringLiteral(
                            "codex.runtime"),
                        {
                            {
                                QStringLiteral(
                                    "runtime.first"),
                                countingHandler(
                                    newCalls),
                            },
                            {
                                QStringLiteral(
                                    "runtime.added"),
                                countingHandler(
                                    addedCalls),
                            },
                        }));
            },
            Qt::DirectConnection);

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("runtime.first"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            1,
            1000);
        QVERIFY(replacement.has_value());
        QVERIFY(replacement->hasValue());
        QCOMPARE(oldCalls, 1);
        QCOMPARE(newCalls, 0);

        bus.execute(QStringLiteral("runtime.first"));
        bus.execute(QStringLiteral("runtime.removed"));
        bus.execute(QStringLiteral("runtime.added"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            4,
            1000);
        QCOMPARE(oldCalls, 1);
        QCOMPARE(newCalls, 1);
        QCOMPARE(removedCalls, 0);
        QCOMPARE(addedCalls, 1);
        QVERIFY(finishedSpy.at(1).at(1).toBool());
        QVERIFY(!finishedSpy.at(2).at(1).toBool());
        QVERIFY(finishedSpy.at(3).at(1).toBool());
    }

    void foreignAndUngroupedHandlersCannotBeOverwritten()
    {
        CompanionCommandBus bus;
        int firstGroupCalls = 0;
        int secondGroupCalls = 0;
        int ungroupedCalls = 0;
        int replacementCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("group.first"),
                   {
                       {
                           QStringLiteral("owned.first"),
                           countingHandler(
                               firstGroupCalls),
                       },
                   })
                .hasValue());
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("group.second"),
                   {
                       {
                           QStringLiteral("owned.second"),
                           countingHandler(
                               secondGroupCalls),
                       },
                   })
                .hasValue());
        QVERIFY(
            bus.registerHandler(
                   QStringLiteral("owned.ungrouped"),
                   countingHandler(ungroupedCalls))
                .hasValue());

        const Result<void> foreignCollision =
            bus.replaceHandlerGroup(
                QStringLiteral("group.first"),
                {
                    {
                        QStringLiteral("owned.second"),
                        countingHandler(replacementCalls),
                    },
                });
        QVERIFY(!foreignCollision.hasValue());
        QCOMPARE(
            foreignCollision.error().code,
            QStringLiteral(
                "ui.command_already_registered"));

        const Result<void> ungroupedCollision =
            bus.replaceHandlerGroup(
                QStringLiteral("group.first"),
                {
                    {
                        QStringLiteral(
                            "owned.ungrouped"),
                        countingHandler(replacementCalls),
                    },
                });
        QVERIFY(!ungroupedCollision.hasValue());
        QCOMPARE(
            ungroupedCollision.error().code,
            QStringLiteral(
                "ui.command_already_registered"));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("owned.first"));
        bus.execute(QStringLiteral("owned.second"));
        bus.execute(QStringLiteral("owned.ungrouped"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            3,
            1000);
        QCOMPARE(firstGroupCalls, 1);
        QCOMPARE(secondGroupCalls, 1);
        QCOMPARE(ungroupedCalls, 1);
        QCOMPARE(replacementCalls, 0);
    }

    void replacementRejectsForeignThreadWithoutMutation()
    {
        CompanionCommandBus bus;
        int oldCalls = 0;
        int replacementCalls = 0;
        QVERIFY(
            bus.replaceHandlerGroup(
                   QStringLiteral("codex.runtime"),
                   {
                       {
                           QStringLiteral("runtime.old"),
                           countingHandler(oldCalls),
                       },
                   })
                .hasValue());

        std::optional<Result<void>> result;
        std::thread worker(
            [&bus, &result, &replacementCalls] {
                result.emplace(
                    bus.replaceHandlerGroup(
                        QStringLiteral(
                            "codex.runtime"),
                        {
                            {
                                QStringLiteral(
                                    "runtime.new"),
                                countingHandler(
                                    replacementCalls),
                            },
                        }));
            });
        worker.join();

        QVERIFY(result.has_value());
        QVERIFY(!result->hasValue());
        QCOMPARE(
            result->error().code,
            QStringLiteral(
                "ui.command_bus_thread_mismatch"));

        QSignalSpy finishedSpy(
            &bus,
            &CompanionCommandBus::commandFinished);
        bus.execute(QStringLiteral("runtime.old"));
        bus.execute(QStringLiteral("runtime.new"));
        QTRY_COMPARE_WITH_TIMEOUT(
            finishedSpy.size(),
            2,
            1000);
        QCOMPARE(oldCalls, 1);
        QCOMPARE(replacementCalls, 0);
        QVERIFY(finishedSpy.at(0).at(1).toBool());
        QVERIFY(!finishedSpy.at(1).at(1).toBool());
    }
};

QTEST_GUILESS_MAIN(CompanionCommandBusTests)

#include "CompanionCommandBusTests.moc"
