#include "core/CompanionError.h"
#include "core/Result.h"

#include <QtTest>

#if !defined(WINVER) || !defined(_WIN32_WINNT) || !defined(NTDDI_VERSION)
#error Windows API floor macros must be defined for native targets.
#endif

static_assert(WINVER == 0x0A00,
              "WINVER must target Windows 10+ APIs.");
static_assert(_WIN32_WINNT == 0x0A00,
              "_WIN32_WINNT must target Windows 10+ APIs.");
static_assert(NTDDI_VERSION == 0x0A00000B,
              "NTDDI_VERSION must target Windows 10 build 22000.");

class BuildSmokeTests final : public QObject {
    Q_OBJECT

private slots:
    void typedErrorRetainsStableFields()
    {
        const companion::CompanionError error{
            QStringLiteral("foundation.probe"),
            QStringLiteral("probe"),
            true,
            {{QStringLiteral("phase"), QStringLiteral("configure")}},
        };

        QCOMPARE(error.code, QStringLiteral("foundation.probe"));
        QVERIFY(error.retryable);
        QCOMPARE(error.context.value(QStringLiteral("phase")).toString(),
                 QStringLiteral("configure"));
    }

    void typedResultSuccessRetainsValue()
    {
        const auto result = companion::Result<int>::success(7);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value(), 7);
    }

    void typedResultFailureRetainsError()
    {
        const auto result = companion::Result<int>::failure({
            QStringLiteral("foundation.failure"),
            QStringLiteral("probe failed"),
            false,
            {{QStringLiteral("phase"), QStringLiteral("build")}},
        });

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, QStringLiteral("foundation.failure"));
        QCOMPARE(result.error().message, QStringLiteral("probe failed"));
        QCOMPARE(result.error().context.value(QStringLiteral("phase")).toString(),
                 QStringLiteral("build"));
    }

    void voidResultSupportsSuccessAndFailure()
    {
        const auto success = companion::Result<void>::success();
        QVERIFY(success.hasValue());

        const auto failure = companion::Result<void>::failure({
            QStringLiteral("foundation.void-failure"),
            QStringLiteral("void result failed"),
            true,
            {{QStringLiteral("phase"), QStringLiteral("test")}},
        });

        QVERIFY(!failure.hasValue());
        QCOMPARE(failure.error().code, QStringLiteral("foundation.void-failure"));
        QVERIFY(failure.error().retryable);
    }
};

QTEST_GUILESS_MAIN(BuildSmokeTests)
#include "BuildSmokeTests.moc"
