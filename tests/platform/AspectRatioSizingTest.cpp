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
    void copyBitsPolicyPreservesCopyBitsWhenTopLeftUnchanged() {
        WINDOWPOS pos{};
        pos.x = 100;
        pos.y = 200;
        pos.flags = 0;

        updateWindowPosCopyBitsForResize(pos);

        QVERIFY((pos.flags & SWP_NOCOPYBITS) == 0);
    }

    void copyBitsPolicyPreservesCopyBitsWhenNoMove() {
        WINDOWPOS pos{};
        pos.x = 80;
        pos.y = 160;
        pos.flags = SWP_NOMOVE;

        updateWindowPosCopyBitsForResize(pos);

        QVERIFY((pos.flags & SWP_NOCOPYBITS) == 0);
    }

    void copyBitsPolicyClearsNoCopyBitsWhenLeftEdgeMoved() {
        WINDOWPOS pos{};
        pos.x = 90;
        pos.y = 200;
        pos.flags = SWP_NOCOPYBITS;

        updateWindowPosCopyBitsForResize(pos);

        QVERIFY((pos.flags & SWP_NOCOPYBITS) == 0);
    }

    void copyBitsPolicyClearsNoCopyBitsWhenTopEdgeMoved() {
        WINDOWPOS pos{};
        pos.x = 100;
        pos.y = 190;
        pos.flags = SWP_NOCOPYBITS;

        updateWindowPosCopyBitsForResize(pos);

        QVERIFY((pos.flags & SWP_NOCOPYBITS) == 0);
    }

    void copyBitsPolicyLeavesFlagsUnchangedWhenNoSize() {
        WINDOWPOS pos{};
        pos.x = 120;
        pos.y = 220;
        pos.flags = SWP_NOSIZE | SWP_NOCOPYBITS | SWP_NOZORDER;
        const UINT originalFlags = pos.flags;

        updateWindowPosCopyBitsForResize(pos);

        QCOMPARE(pos.flags, originalFlags);
    }

    void rightEdgeUsesWidthAndKeepsTopLeftStable() {
        RECT rect{100, 100, 436, 400};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_RIGHT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 100, 436, 320});
    }

    void leftEdgeUsesWidthAndKeepsTopRightStable() {
        RECT rect{64, 100, 400, 400};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_LEFT, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{64, 100, 400, 320});
    }

    void bottomEdgeUsesHeightAndKeepsTopLeftStable() {
        RECT rect{100, 100, 500, 320};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_BOTTOM, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 100, 436, 320});
    }

    void topEdgeUsesHeightAndKeepsBottomLeftStable() {
        RECT rect{100, 80, 500, 300};

        QVERIFY(adjustWindowRectForAspectRatio(rect, WMSZ_TOP, 16.0 / 9.0, margins()));

        compareRect(rect, RECT{100, 80, 436, 300});
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

        compareRect(rect, RECT{100, 100, 507, 360});
    }

    void invalidInputReturnsFalse() {
        RECT rect{0, 0, 100, 100};
        QVERIFY(!adjustWindowRectForAspectRatio(rect, WMSZ_RIGHT, 0.0, margins()));

        RECT zeroWidth{0, 0, 0, 100};
        QVERIFY(!adjustWindowRectForAspectRatio(zeroWidth, WMSZ_RIGHT, 16.0 / 9.0, margins()));

        RECT unknownEdge{0, 0, 100, 100};
        QVERIFY(!adjustWindowRectForAspectRatio(unknownEdge, 999u, 16.0 / 9.0, margins()));
    }

    void adjustedOuterSizeDrivenByHeightComputesOuterWidthFromHeightAndMargins() {
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(320, 16.0 / 9.0, margins());
        QCOMPARE(size.width, 514);
        QCOMPARE(size.height, 320);
    }

    void adjustedOuterSizeDrivenByHeightRespectsMinimumWidthConstraint() {
        AspectRatioSizeConstraints constraints;
        constraints.minOuterWidth = 600;
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(320, 16.0 / 9.0, margins(), constraints);
        QCOMPARE(size.width, 600);
        QCOMPARE(size.height, 369);
    }

    void adjustedOuterSizeDrivenByHeightRespectsMaximumHeightConstraint() {
        AspectRatioSizeConstraints constraints{};
        constraints.maxOuterHeight = 200;
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(320, 16.0 / 9.0, margins(), constraints);
        QCOMPARE(size.width, 300);
        QCOMPARE(size.height, 200);
    }

    void adjustedOuterSizeDrivenByHeightWithZeroMarginsMatchesBasicMath() {
        const double ratio = 4.0 / 3.0;
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(300, ratio, {});
        QCOMPARE(size.width, 400);
        QCOMPARE(size.height, 300);
    }

    void adjustedOuterSizeDrivenByHeightReturnsDefaultForZeroHeight() {
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(0, 16.0 / 9.0, margins());
        QCOMPARE(size.width, 1);
        QCOMPARE(size.height, 1);
    }

    void adjustedOuterSizeDrivenByHeightReturnsDefaultForNegativeHeight() {
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(-10, 16.0 / 9.0, margins());
        QCOMPARE(size.width, 1);
        QCOMPARE(size.height, 1);
    }

    void adjustedOuterSizeDrivenByHeightReturnsDefaultForZeroRatio() {
        const AspectRatioOuterSize size = adjustedOuterSizeDrivenByHeight(320, 0.0, margins());
        QCOMPARE(size.width, 1);
        QCOMPARE(size.height, 1);
    }

    void clientWidthForOuterWidthSubtractsHorizontalFrame() {
        QCOMPARE(clientWidthForOuterWidth(514, margins()), 498);
    }

    void clientHeightForOuterHeightSubtractsVerticalFrame() {
        QCOMPARE(clientHeightForOuterHeight(320, margins()), 280);
    }

    void clientWidthForOuterWidthClampsToMinimumOfOne() {
        QCOMPARE(clientWidthForOuterWidth(5, AspectRatioFrameMargins{10, 0, 10, 0}), 1);
    }

    void clientHeightForOuterHeightClampsToMinimumOfOne() {
        QCOMPARE(clientHeightForOuterHeight(5, AspectRatioFrameMargins{0, 10, 0, 10}), 1);
    }

    void clientWidthForOuterWidthWithZeroMarginsIsIdentity() {
        QCOMPARE(clientWidthForOuterWidth(400, {}), 400);
    }

    void clientHeightForOuterHeightWithZeroMarginsIsIdentity() {
        QCOMPARE(clientHeightForOuterHeight(300, {}), 300);
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
