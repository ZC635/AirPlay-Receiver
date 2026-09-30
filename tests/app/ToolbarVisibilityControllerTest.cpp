#include <QtTest/QtTest>
#include "app/ToolbarVisibilityController.h"
#include <QDialog>
#include <QMenu>
#include <QSignalSpy>
#include <QWidget>

class ToolbarVisibilityControllerTest : public QObject {
    Q_OBJECT
private slots:
    void baselineAndManualPolicy() {
        QWidget content;
        QWidget toolbar(&content);
        ToolbarVisibilityController controller(&content, &toolbar);
        QSignalSpy changes(&controller, &ToolbarVisibilityController::visibilityChanged);
        QVERIFY(controller.isVisible());
        controller.receiverStateChanged(ReceiverState::Connected);
        QVERIFY(!controller.isVisible());
        controller.toggleManually();
        controller.evaluatePointer(QPoint(-9999, -9999), true);
        QVERIFY(controller.isVisible());
        controller.receiverStateChanged(ReceiverState::Connected);
        QVERIFY(controller.isVisible());
        controller.receiverStateChanged(ReceiverState::Discoverable);
        controller.toggleManually();
        QVERIFY(!controller.isVisible());
        controller.receiverStateChanged(ReceiverState::Discoverable);
        QVERIFY(!controller.isVisible());
        for (auto state : {ReceiverState::Starting, ReceiverState::Connecting, ReceiverState::Error, ReceiverState::Idle}) {
            controller.receiverStateChanged(state);
            QVERIFY(controller.isVisible());
            controller.toggleManually();
        }
        QVERIFY(changes.count() > 0);
    }
    void fullWidthLogicalBandAndExplicitHide() {
        QWidget content;
        content.setGeometry(-700, -500, 600, 300);
        QWidget toolbar(&content);
        toolbar.setGeometry(450, 0, 150, 30);
        content.show();
        ToolbarVisibilityController controller(&content, &toolbar);
        connect(&controller, &ToolbarVisibilityController::visibilityChanged, &toolbar, &QWidget::setVisible);
        controller.receiverStateChanged(ReceiverState::Connected);
        const QPoint origin = content.mapToGlobal(QPoint());
        for (int x : {0, 300, 599}) {
            controller.evaluatePointer(origin + QPoint(x, 1), true);
            QVERIFY(controller.isVisible());
            controller.evaluatePointer(origin + QPoint(x, 31), true);
            QVERIFY(!controller.isVisible());
        }
        controller.evaluatePointer(origin + QPoint(5, 5), true);
        controller.toggleManually();
        controller.evaluatePointer(origin + QPoint(300, 5), true);
        QVERIFY(!controller.isVisible());
        controller.evaluatePointer(origin + QPoint(300, 100), true);
        controller.evaluatePointer(origin + QPoint(300, 5), true);
        QVERIFY(controller.isVisible());
        controller.setHoverRevealEnabled(false);
        QVERIFY(!controller.isVisible());
        controller.evaluatePointer(origin + QPoint(300, 5), true);
        QVERIFY(!controller.isVisible());
        controller.setHoverRevealEnabled(true);
        controller.evaluatePointer(origin + QPoint(300, 5), true);
        QVERIFY(controller.isVisible());
        controller.evaluatePointer(origin + QPoint(300, 5), false);
        QVERIFY(!controller.isVisible());
        controller.toggleManually();
        controller.evaluatePointer(origin + QPoint(300, 5), false);
        QVERIFY(controller.isVisible());
    }
    void manualToggleUsesCurrentPointerWithoutChangingTheAction() {
        QWidget content;
        content.resize(600, 300);
        QWidget toolbar(&content);
        toolbar.setGeometry(450, 0, 150, 30);
        ToolbarVisibilityController controller(&content, &toolbar);
        const QPoint top = content.mapToGlobal(QPoint(5, 1));
        const QPoint away = content.mapToGlobal(QPoint(5, 100));
        controller.evaluatePointer(away, true);
        // Native video can consume the move to the top immediately before a hotkey.
        controller.toggleManually(top);
        QVERIFY(!controller.isVisible());
        controller.evaluatePointer(top, true);
        QVERIFY(!controller.isVisible());
        controller.toggleManually(top);
        QVERIFY(controller.isVisible());
        controller.setHoverRevealEnabled(false);
        QVERIFY(controller.isVisible());
        controller.setHoverRevealEnabled(true);
        controller.receiverStateChanged(ReceiverState::Connected);
        controller.evaluatePointer(top, true);
        QVERIFY(controller.isVisible());
        controller.toggleManually(away);
        QVERIFY(!controller.isVisible());
    }
    void temporaryRevealSurvivesGeometryUntilPointerMoves() {
        QWidget content;
        content.setGeometry(100, 100, 600, 300);
        QWidget toolbar(&content);
        toolbar.setGeometry(450, 0, 150, 30);
        ToolbarVisibilityController controller(&content, &toolbar);
        controller.receiverStateChanged(ReceiverState::Connected);
        const QPoint originalTop = content.mapToGlobal(QPoint(5, 1));
        controller.evaluatePointer(originalTop, true);
        QVERIFY(controller.isVisible());
        controller.preserveVisibilityUntilPointerMoves(originalTop);
        content.setGeometry(0, 0, 1600, 900);
        controller.evaluatePointer(originalTop, true);
        QVERIFY(controller.isVisible());
        controller.evaluatePointer(originalTop + QPoint(1, 0), true);
        QVERIFY(!controller.isVisible());
        const QPoint newTop = content.mapToGlobal(QPoint(5, 1));
        controller.evaluatePointer(newTop, true);
        controller.preserveVisibilityUntilPointerMoves(newTop);
        controller.evaluatePointer(newTop, false);
        QVERIFY(!controller.isVisible());
        controller.evaluatePointer(newTop, true);
        controller.preserveVisibilityUntilPointerMoves(newTop);
        controller.setHoverRevealEnabled(false);
        QVERIFY(!controller.isVisible());
    }
    void hiddenToolbarSurvivesGeometryUntilPointerMoves_data() {
        QTest::addColumn<QRect>("before");
        QTest::addColumn<QRect>("after");
        QTest::newRow("fullscreen-entry") << QRect(100, 100, 600, 300) << QRect(0, 0, 1600, 900);
        QTest::newRow("fullscreen-exit") << QRect(0, 0, 1600, 900) << QRect(100, 100, 600, 300);
    }
    void hiddenToolbarSurvivesGeometryUntilPointerMoves() {
        QFETCH(QRect, before);
        QFETCH(QRect, after);
        QWidget content;
        content.setGeometry(before);
        QWidget toolbar(&content);
        toolbar.setGeometry(450, 0, 150, 30);
        ToolbarVisibilityController controller(&content, &toolbar);
        controller.receiverStateChanged(ReceiverState::Connected);
        const QPoint stationary = after.topLeft() + QPoint(5, 1);
        controller.evaluatePointer(stationary, true);
        QVERIFY(!controller.isVisible());
        controller.preserveVisibilityUntilPointerMoves(stationary);
        content.setGeometry(after);
        controller.evaluatePointer(stationary, true);
        QVERIFY(!controller.isVisible());
        controller.evaluatePointer(stationary + QPoint(1, 0), true);
        QVERIFY(controller.isVisible());
        controller.evaluatePointer(stationary + QPoint(1, 100), true);
        QVERIFY(!controller.isVisible());
    }
    void onlyVisibleOwnedControlsRetain() {
        QWidget content;
        content.resize(600, 300);
        QWidget toolbar(&content);
        toolbar.setGeometry(400, 0, 200, 30);
        QWidget volume(&toolbar);
        volume.setGeometry(0, 30, 100, 40);
        QMenu owned(&toolbar);
        owned.addAction("Owned");
        QMenu foreign;
        foreign.addAction("Foreign");
        QDialog modal(&toolbar);
        modal.setModal(true);
        content.show();
        ToolbarVisibilityController controller(&content, &toolbar);
        controller.receiverStateChanged(ReceiverState::Connected);
        const QPoint top = content.mapToGlobal(QPoint(10, 1));
        controller.evaluatePointer(top, true);
        controller.evaluatePointer(volume.mapToGlobal(QPoint(2, 2)), true);
        QVERIFY(controller.isVisible());
        owned.popup(content.mapToGlobal(QPoint(200, 100)));
        controller.evaluatePointer(owned.mapToGlobal(QPoint(2, 2)), true);
        QVERIFY(controller.isVisible());
        const QPoint popupPoint = owned.mapToGlobal(QPoint(2, 2));
        owned.hide();
        controller.evaluatePointer(popupPoint, true);
        QVERIFY(!controller.isVisible());
        controller.evaluatePointer(top, true);
        foreign.popup(content.mapToGlobal(QPoint(200, 150)));
        controller.evaluatePointer(foreign.mapToGlobal(QPoint(2, 2)), true);
        QVERIFY(!controller.isVisible());
        foreign.hide();
        controller.evaluatePointer(top, true);
        modal.setGeometry(QRect(content.mapToGlobal(QPoint(200, 150)), QSize(100, 100)));
        modal.show();
        controller.evaluatePointer(modal.mapToGlobal(QPoint(2, 2)), true);
        QVERIFY(!controller.isVisible());
    }
};
QTEST_MAIN(ToolbarVisibilityControllerTest)
#include "ToolbarVisibilityControllerTest.moc"
