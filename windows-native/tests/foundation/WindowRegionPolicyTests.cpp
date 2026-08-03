#include "platform/windows/WindowRegionPolicy.h"

#include <QMarginsF>
#include <QQuickWindow>
#include <QtTest>

#define NOMINMAX
#include <windows.h>

class WindowRegionPolicyTests final : public QObject {
    Q_OBJECT

private slots:
    void scalesRoundedRegionGeometryAtFractionalDpi()
    {
        const auto geometry =
            companion::WindowRegionPolicy::geometryFor(
                QSize(292, 196),
                QMarginsF(4, 14, 4, 14),
                28,
                1.25);

        QCOMPARE(geometry.bounds, QRect(5, 18, 355, 209));
        QCOMPARE(geometry.ellipse, QSize(70, 70));
    }

    void appliesAndClearsRoundedRegionOnNativeQuickWindow()
    {
        QQuickWindow window;
        window.resize(292, 196);
        window.setProperty(
            "nativeBackdropRegionEnabled",
            true);
        window.setProperty(
            "nativeBackdropRegionInsetLeft",
            4);
        window.setProperty(
            "nativeBackdropRegionInsetTop",
            14);
        window.setProperty(
            "nativeBackdropRegionInsetRight",
            4);
        window.setProperty(
            "nativeBackdropRegionInsetBottom",
            14);
        window.setProperty(
            "nativeBackdropRegionRadius",
            28);
        window.create();
        QVERIFY(window.handle() != nullptr);

        const auto applied =
            companion::WindowRegionPolicy::apply(
                window);
        QVERIFY(applied.hasValue());

        HRGN observed = CreateRectRgn(0, 0, 1, 1);
        QVERIFY(observed != nullptr);
        const int regionType = GetWindowRgn(
            reinterpret_cast<HWND>(
                window.winId()),
            observed);
        QVERIFY(regionType != ERROR);

        RECT nativeBounds {};
        QVERIFY(GetRgnBox(
                    observed,
                    &nativeBounds)
                != ERROR);
        const auto expected =
            companion::WindowRegionPolicy::geometryFor(
                window.size(),
                QMarginsF(4, 14, 4, 14),
                28,
                window.devicePixelRatio());
        QCOMPARE(nativeBounds.left, expected.bounds.x());
        QCOMPARE(nativeBounds.top, expected.bounds.y());
        QCOMPARE(
            nativeBounds.right,
            expected.bounds.x()
                + expected.bounds.width());
        QCOMPARE(
            nativeBounds.bottom,
            expected.bounds.y()
                + expected.bounds.height());

        window.setProperty(
            "nativeBackdropRegionEnabled",
            false);
        const auto cleared =
            companion::WindowRegionPolicy::apply(
                window);
        QVERIFY(cleared.hasValue());
        QCOMPARE(
            GetWindowRgn(
                reinterpret_cast<HWND>(
                    window.winId()),
                observed),
            ERROR);

        QVERIFY(DeleteObject(observed) != FALSE);
    }
};

QTEST_MAIN(WindowRegionPolicyTests)
#include "WindowRegionPolicyTests.moc"
