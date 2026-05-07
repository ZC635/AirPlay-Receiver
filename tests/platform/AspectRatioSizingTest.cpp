#include <QtTest/QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "platform/AspectRatioSizing.h"

class AspectRatioSizingTest : public QObject {
    Q_OBJECT

private slots:
    void rightEdgeUsesWidthAndKeepsHorizontalAnchor() {
        RECT rect{100, 100, 436, 400};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_RIGHT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 140, 436, 360});
    }

    void leftEdgeUsesWidthAndKeepsHorizontalAnchor() {
        RECT rect{64, 100, 400, 400};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_LEFT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{64, 140, 400, 360});
    }

    void bottomEdgeUsesHeightAndKeepsVerticalAnchor() {
        RECT rect{100, 100, 500, 320};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_BOTTOM, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{132, 100, 468, 320});
    }

    void topEdgeUsesHeightAndKeepsVerticalAnchor() {
        RECT rect{100, 80, 500, 300};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_TOP, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{132, 80, 468, 300});
    }

    void bottomRightCornerUsesWidthWhenRectIsTooWide() {
        RECT rect{100, 100, 452, 320};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_BOTTOMRIGHT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 100, 452, 329});
    }

    void bottomRightCornerUsesHeightWhenRectIsTooTall() {
        RECT rect{100, 100, 436, 340};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_BOTTOMRIGHT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 100, 472, 340});
    }

    void topLeftCornerKeepsOppositeCorner() {
        RECT rect{84, 100, 436, 320};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_TOPLEFT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{84, 91, 436, 320});
    }

    void topRightCornerKeepsOppositeCorner() {
        RECT rect{100, 100, 452, 320};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_TOPRIGHT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 91, 452, 320});
    }

    void bottomLeftCornerKeepsOppositeCorner() {
        RECT rect{84, 100, 436, 320};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_BOTTOMLEFT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{84, 100, 436, 329});
    }

    void rightEdgeRespectsMinimumHeightByExpandingWidth() {
        RECT rect{100, 100, 436, 400};
        AspectRatioSizeConstraints constraints;
        constraints.minOuterHeight = 260;

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_RIGHT, 16.0 / 9.0, margins(), constraints));

        compareRect(rect, RECT{100, 120, 507, 380});
    }

    void resizeCopyBitsDiscardedWhenTopLeftIsUnchanged() {
        WINDOWPOS windowPos{};
        windowPos.x = 100;
        windowPos.y = 100;
        windowPos.cx = 360;
        windowPos.cy = 240;
        windowPos.flags = 0;

        updateWindowPosCopyBitsForResize(windowPos, RECT{100, 100, 420, 300});

        QVERIFY((windowPos.flags & SWP_NOCOPYBITS) != 0);
    }

    void resizeCopyBitsDiscardedWhenMoveFlagSaysTopLeftIsUnchanged() {
        WINDOWPOS windowPos{};
        windowPos.x = 80;
        windowPos.y = 80;
        windowPos.cx = 360;
        windowPos.cy = 240;
        windowPos.flags = SWP_NOMOVE;

        updateWindowPosCopyBitsForResize(windowPos, RECT{100, 100, 420, 300});

        QVERIFY((windowPos.flags & SWP_NOCOPYBITS) != 0);
    }

    void resizeCopyBitsUnchangedWhenWindowIsNotResizing() {
        WINDOWPOS windowPos{};
        windowPos.x = 80;
        windowPos.y = 80;
        windowPos.cx = 320;
        windowPos.cy = 200;
        windowPos.flags = SWP_NOSIZE | SWP_NOCOPYBITS;

        updateWindowPosCopyBitsForResize(windowPos, RECT{100, 100, 420, 300});

        QVERIFY((windowPos.flags & SWP_NOCOPYBITS) != 0);
    }

    void resizeCopyBitsPreservedWhenLeftEdgeMoves() {
        WINDOWPOS windowPos{};
        windowPos.x = 80;
        windowPos.y = 100;
        windowPos.cx = 340;
        windowPos.cy = 200;
        windowPos.flags = SWP_NOCOPYBITS;

        updateWindowPosCopyBitsForResize(windowPos, RECT{100, 100, 420, 300});

        QVERIFY((windowPos.flags & SWP_NOCOPYBITS) == 0);
    }

    void resizeCopyBitsPreservedWhenTopEdgeMoves() {
        WINDOWPOS windowPos{};
        windowPos.x = 100;
        windowPos.y = 80;
        windowPos.cx = 320;
        windowPos.cy = 220;
        windowPos.flags = SWP_NOCOPYBITS;

        updateWindowPosCopyBitsForResize(windowPos, RECT{100, 100, 420, 300});

        QVERIFY((windowPos.flags & SWP_NOCOPYBITS) == 0);
    }

    void invalidInputReturnsFalse() {
        RECT rect{0, 0, 100, 100};
        QVERIFY(!adjustWindowRectForAspectRatio(rect, WMSZ_RIGHT, 0.0, margins()));

        RECT zeroWidth{0, 0, 0, 100};
        QVERIFY(!adjustWindowRectForAspectRatio(zeroWidth, WMSZ_RIGHT, 16.0 / 9.0, margins()));

        RECT unknownEdge{0, 0, 100, 100};
        QVERIFY(!adjustWindowRectForAspectRatio(unknownEdge, 999u, 16.0 / 9.0, margins()));
    }

private:
    static AspectRatioFrameMargins margins() {
        return AspectRatioFrameMargins{8, 32, 8, 8};
    }

    static void compareRect(const RECT &actual, const RECT &expected) {
        QCOMPARE(actual.left, expected.left);
        QCOMPARE(actual.top, expected.top);
        QCOMPARE(actual.right, expected.right);
        QCOMPARE(actual.bottom, expected.bottom);
    }
};

QTEST_MAIN(AspectRatioSizingTest)
#include "AspectRatioSizingTest.moc"
