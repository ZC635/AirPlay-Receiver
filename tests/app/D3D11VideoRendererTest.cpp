#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QImage>
#include <QWidget>

#include <windows.h>

#include "app/D3D11VideoRenderer.h"

class D3D11VideoRendererTest : public QObject {
    Q_OBJECT

private slots:
    void rendersUploadedFrameToWindowWithoutCrashing() {
        if (QGuiApplication::platformName() != QStringLiteral("windows")) {
            QSKIP("D3D11 HWND rendering requires the Windows QPA platform");
        }

        QWidget widget;
        widget.resize(160, 90);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        D3D11VideoRenderer renderer;
        if (!renderer.initialize(reinterpret_cast<HWND>(widget.winId()))) {
            QSKIP("D3D11 is unavailable on this system");
        }

        QImage frame(64, 64, QImage::Format_RGBA8888);
        frame.fill(Qt::red);

        QVERIFY(renderer.uploadFrame(frame));
        QVERIFY(renderer.render(true));

        widget.resize(200, 120);
        QVERIFY(renderer.resize(200, 120));
        QVERIFY(renderer.render(true));
        QVERIFY(renderer.isInitialized());
    }
};

QTEST_MAIN(D3D11VideoRendererTest)
#include "D3D11VideoRendererTest.moc"
