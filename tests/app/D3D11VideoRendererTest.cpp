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

    void sameSizeUploadReusesResourcesWithoutCrashing() {
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

        QImage frame1(64, 64, QImage::Format_RGBA8888);
        frame1.fill(Qt::red);
        QVERIFY(renderer.uploadFrame(frame1));
        QVERIFY(renderer.render(true));

        QImage frame2(64, 64, QImage::Format_RGBA8888);
        frame2.fill(Qt::blue);
        QVERIFY(renderer.uploadFrame(frame2));
        QVERIFY(renderer.render(true));
        QVERIFY(renderer.isInitialized());

        QImage frame3(128, 72, QImage::Format_RGBA8888);
        frame3.fill(Qt::green);
        QVERIFY(renderer.uploadFrame(frame3));
        QVERIFY(renderer.render(true));
        QVERIFY(renderer.isInitialized());
    }
};

QTEST_MAIN(D3D11VideoRendererTest)
#include "D3D11VideoRendererTest.moc"
