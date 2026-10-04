#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QHBoxLayout>
#include <QTranslator>
#include "app/ToolbarWidget.h"
#include "backend/RecordingTypes.h"

namespace {

class ToolbarTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        return translations.value(QString::fromLatin1(context) + QChar('\x1f')
                                      + QString::fromLatin1(sourceText));
    }

    QHash<QString, QString> translations = {
        {QStringLiteral("ToolbarWidget\u001f%1: %2"), QStringLiteral("%1：%2")},
        {QStringLiteral("ToolbarWidget\u001fVolume"), QStringLiteral("音量")},
        {QStringLiteral("ToolbarWidget\u001fPin"), QStringLiteral("置顶")},
        {QStringLiteral("ToolbarWidget\u001fAspect"), QStringLiteral("比例")},
        {QStringLiteral("ToolbarWidget\u001fFit"), QStringLiteral("适应")},
        {QStringLiteral("ToolbarWidget\u001fSettings"), QStringLiteral("设置")},
        {QStringLiteral("ToolbarWidget\u001fFullscreen"), QStringLiteral("全屏")},
        {QStringLiteral("ToolbarWidget\u001fExit Fullscreen"), QStringLiteral("退出全屏")},
        {QStringLiteral("ToolbarWidget\u001fRecord"), QStringLiteral("录制")},
        {QStringLiteral("ToolbarWidget\u001fStop"), QStringLiteral("停止")},
        {QStringLiteral("ToolbarWidget\u001fSaving..."), QStringLiteral("正在保存…")},
    };
};

class InstalledTranslator final {
public:
    explicit InstalledTranslator(QTranslator *translator)
        : translator_(translator) {
        QCoreApplication::installTranslator(translator_);
    }

    ~InstalledTranslator() {
        QCoreApplication::removeTranslator(translator_);
    }

private:
    QTranslator *translator_;
};

} // namespace

class ToolbarWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void checkedButtonsHaveReadableFill_data() {
        QTest::addColumn<bool>("darkPalette");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void checkedButtonsHaveReadableFill() {
        QFETCH(bool, darkPalette);
        ToolbarWidget toolbar;
        QPalette palette = toolbar.palette();
        palette.setColor(QPalette::Button, QColor(darkPalette ? "#333333" : "#f9f9f9"));
        palette.setColor(QPalette::Window, QColor(darkPalette ? "#222222" : "#ffffff"));
        palette.setColor(QPalette::ButtonText, QColor(darkPalette ? "#eeeeee" : "#202020"));
        palette.setColor(QPalette::Midlight, QColor(darkPalette ? "#444444" : "#dedede"));
        palette.setColor(QPalette::Mid, QColor(darkPalette ? "#666666" : "#b0b0b0"));
        toolbar.setPalette(palette);
        toolbar.setRecordingUi(RecordingState::Recording, true);
        toolbar.resize(toolbar.sizeHint());
        toolbar.layout()->activate();
        for (auto *button : toolbar.findChildren<QToolButton *>()) {
            if (!button->isCheckable()) continue;
            button->setChecked(true);
            button->ensurePolished();
            QImage image(button->size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            button->render(&image);
            const QColor fill = image.pixelColor(4, button->height() / 2);
            const QString detail = button->objectName() + ": " + fill.name();
            QVERIFY2(darkPalette ? fill.lightness() <= 110
                                 : fill.lightness() >= 160 && fill.lightness() <= 235,
                     qPrintable(detail));
            bool hasContrastingGlyph = false;
            for (int y = 8; y < image.height() - 8; ++y) {
                for (int x = 8; x < image.width() - 8; ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    hasContrastingGlyph |= pixel.alpha() == 255
                        && qAbs(pixel.lightness() - fill.lightness()) >= 120;
                }
            }
            QVERIFY2(hasContrastingGlyph, qPrintable(detail));
        }
    }

    void iconOnlyControlsKeepAccessibleLabels() {
        ToolbarWidget toolbar;
        const QHash<QString, QString> labels = {
            {"volumeButton", "Volume"}, {"alwaysOnTopButton", "Pin"},
            {"aspectRatioButton", "Aspect"}, {"videoFitButton", "Fit"},
            {"recordingButton", "Record"}, {"fullscreenButton", "Fullscreen"},
            {"settingsButton", "Settings"},
        };
        for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
            auto *button = toolbar.findChild<QToolButton *>(it.key());
            QVERIFY(button != nullptr);
            QVERIFY2(!button->icon().isNull(), qPrintable(it.key()));
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QCOMPARE(button->accessibleName(), it.value());
            QCOMPARE(button->toolTip(), it.value());
            const QImage image = button->icon().pixmap(QSize(20, 20)).toImage();
            QVERIFY(!image.isNull());
            QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
            bool hasVisiblePixel = false;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    hasVisiblePixel |= image.pixelColor(x, y).alpha() > 0;
                }
            }
            QVERIFY2(hasVisiblePixel, qPrintable(it.key()));
        }
    }

    void recordingIconsAndTooltipsFollowState() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("recordingButton");
        toolbar.setRecordingUi(RecordingState::Idle, true);
        const QImage record = button->icon().pixmap(QSize(20, 20)).toImage();
        QVERIFY(!record.isNull());
        QCOMPARE(button->toolTip(), QString("Record"));
        toolbar.setRecordingUi(RecordingState::Recording, true);
        const QImage stop = button->icon().pixmap(QSize(20, 20)).toImage();
        QVERIFY(stop != record);
        QCOMPARE(button->toolTip(), QString("Stop"));
        QCOMPARE(button->accessibleName(), QString("Stop"));
        toolbar.setRecordingUi(RecordingState::Finalizing, true);
        const QImage saving = button->icon().pixmap(QSize(20, 20)).toImage();
        QVERIFY(saving != stop);
        QVERIFY(saving != record);
        QCOMPARE(button->toolTip(), QString("Saving..."));
        QCOMPARE(button->accessibleName(), QString("Saving..."));
        QVERIFY(!button->isEnabled());
    }

    void fullscreenIconChangesWithAction() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("fullscreenButton");
        const QImage enter = button->icon().pixmap(QSize(20, 20)).toImage();
        QVERIFY(!enter.isNull());
        toolbar.setFullscreenChecked(true);
        const QImage exit = button->icon().pixmap(QSize(20, 20)).toImage();
        QVERIFY(exit != enter);
        QCOMPARE(button->accessibleName(), QString("Exit Fullscreen"));
        toolbar.setFullscreenChecked(false);
        QCOMPARE(button->icon().pixmap(QSize(20, 20)).toImage(), enter);
    }

    void iconsFollowPaletteChangesAndDisabledColors() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("settingsButton");
        QPalette palette = toolbar.palette();
        palette.setColor(QPalette::Active, QPalette::ButtonText, QColor(241, 242, 243));
        palette.setColor(QPalette::Inactive, QPalette::ButtonText, QColor(241, 242, 243));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(101, 102, 103));
        toolbar.setPalette(palette);
        for (const auto mode : {QIcon::Normal, QIcon::Disabled}) {
            const QColor expected = mode == QIcon::Disabled
                ? QColor(101, 102, 103) : QColor(241, 242, 243);
            const QImage image = button->icon().pixmap(QSize(40, 40), mode).toImage();
            QVERIFY(!image.isNull());
            bool hasExpectedColor = false;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    hasExpectedColor |= image.pixelColor(x, y) == expected;
                }
            }
            QVERIFY(hasExpectedColor);
        }
    }

    void exposesRequiredControls() {
        ToolbarWidget toolbar;
        QVERIFY(toolbar.findChild<QToolButton *>("volumeButton"));
        QVERIFY(toolbar.findChild<QSlider *>("volumeSlider"));
        QVERIFY(toolbar.findChild<QToolButton *>("alwaysOnTopButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("aspectRatioButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("videoFitButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("recordingButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("settingsButton"));
        auto *fullscreen = toolbar.findChild<QToolButton *>("fullscreenButton");
        QVERIFY(fullscreen != nullptr);
        QVERIFY(fullscreen->isCheckable());
        QVERIFY(!fullscreen->isChecked());
        QCOMPARE(fullscreen->text(), QString("Fullscreen"));
        QCOMPARE(fullscreen->toolTip(), QString("Fullscreen"));
        auto *pin = toolbar.findChild<QToolButton *>("alwaysOnTopButton");
        QCOMPARE(fullscreen->styleSheet(), pin->styleSheet());
        QCOMPARE(toolbar.layout()->indexOf(fullscreen) + 1,
                 toolbar.layout()->indexOf(toolbar.findChild<QToolButton *>("settingsButton")));
    }

    void fullscreenButtonTextTracksCheckedState() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("fullscreenButton");
        QVERIFY(button != nullptr);
        QSignalSpy spy(&toolbar, &ToolbarWidget::fullscreenToggled);
        button->click();
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toBool());
        QCOMPARE(button->text(), QString("Exit Fullscreen"));
        QCOMPARE(button->toolTip(), QString("Exit Fullscreen"));
        toolbar.setFullscreenChecked(false);
        QCOMPARE(button->text(), QString("Fullscreen"));
    }

    void fullscreenButtonRetranslatesCurrentState() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("fullscreenButton");
        QVERIFY(button != nullptr);
        toolbar.setFullscreenChecked(true);
        ToolbarTranslator translator;
        const InstalledTranslator installedTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&toolbar, &languageChange);
        QCOMPARE(button->text(), QStringLiteral("退出全屏"));
        QCOMPARE(button->toolTip(), QStringLiteral("退出全屏"));
        toolbar.setFullscreenChecked(false);
        QCOMPARE(button->text(), QStringLiteral("全屏"));
        QCOMPARE(button->toolTip(), QStringLiteral("全屏"));
    }

    void exposesAspectRatioButton() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        QVERIFY(!button->isChecked());
    }

    void aspectRatioButtonTogglesSignal() {
        ToolbarWidget toolbar;
        QSignalSpy spy(&toolbar, &ToolbarWidget::aspectRatioToggled);
        auto *button = toolbar.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);
        button->setChecked(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toBool());
    }

    void exposesVideoFitButton() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        QVERIFY(!button->isChecked());
    }

    void videoFitButtonTogglesSignal() {
        ToolbarWidget toolbar;
        QSignalSpy spy(&toolbar, &ToolbarWidget::videoFitToggled);
        auto *button = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        button->setChecked(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toBool());
    }

    void videoFitButtonLayoutOrder() {
        ToolbarWidget toolbar;
        auto *layout = toolbar.layout();
        QVERIFY(layout != nullptr);
        auto *aspectBtn = toolbar.findChild<QToolButton *>("aspectRatioButton");
        auto *fitBtn = toolbar.findChild<QToolButton *>("videoFitButton");
        auto *recordingBtn = toolbar.findChild<QToolButton *>("recordingButton");
        auto *settingsBtn = toolbar.findChild<QToolButton *>("settingsButton");
        int aspectIdx = layout->indexOf(aspectBtn);
        int fitIdx = layout->indexOf(fitBtn);
        int recordingIdx = layout->indexOf(recordingBtn);
        int settingsIdx = layout->indexOf(settingsBtn);
        QVERIFY(aspectIdx >= 0);
        QVERIFY(fitIdx >= 0);
        QVERIFY(recordingIdx >= 0);
        QVERIFY(settingsIdx >= 0);
        QVERIFY(fitIdx == aspectIdx + 1);
        QVERIFY(recordingIdx == fitIdx + 1);
        QVERIFY(settingsIdx == recordingIdx + 2);
    }

    void recordingButtonReflectsAllStates() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        auto *fitButton = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(fitButton != nullptr);
        QCOMPARE(button->styleSheet(), fitButton->styleSheet());

        toolbar.setRecordingUi(RecordingState::Idle, false);
        QCOMPARE(button->text(), QString("Record"));
        QVERIFY(!button->isChecked());
        QVERIFY(!button->isEnabled());

        toolbar.setRecordingUi(RecordingState::Idle, true);
        QCOMPARE(button->text(), QString("Record"));
        QVERIFY(!button->isChecked());
        QVERIFY(button->isEnabled());

        toolbar.setRecordingUi(RecordingState::Recording, false);
        QCOMPARE(button->text(), QString("Stop"));
        QVERIFY(button->isChecked());
        QVERIFY(button->isEnabled());

        toolbar.setRecordingUi(RecordingState::Finalizing, true);
        QCOMPARE(button->text(), QString("Saving..."));
        QVERIFY(button->isChecked());
        QVERIFY(!button->isEnabled());
    }

    void recordingSignalOnlyComesFromEnabledClick() {
        ToolbarWidget toolbar;
        QSignalSpy spy(&toolbar, &ToolbarWidget::recordingToggledRequested);
        auto *button = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);

        toolbar.setRecordingUi(RecordingState::Idle, true);
        QCOMPARE(spy.count(), 0);
        button->click();
        QCOMPARE(spy.count(), 1);

        toolbar.setRecordingUi(RecordingState::Recording, false);
        QCOMPARE(spy.count(), 1);

        toolbar.setRecordingUi(RecordingState::Finalizing, true);
        button->click();
        QCOMPARE(spy.count(), 1);
    }

    void languageChangeRetranslatesControlsWithoutChangingRecordingState_data() {
        QTest::addColumn<int>("recordingState");
        QTest::addColumn<bool>("available");
        QTest::addColumn<QString>("englishText");
        QTest::addColumn<QString>("translatedText");
        QTest::addColumn<bool>("checked");
        QTest::addColumn<bool>("enabled");

        QTest::newRow("idle-unavailable") << static_cast<int>(RecordingState::Idle) << false
                                            << QString("Record") << QString("录制") << false << false;
        QTest::newRow("idle-available") << static_cast<int>(RecordingState::Idle) << true
                                          << QString("Record") << QString("录制") << false << true;
        QTest::newRow("recording") << static_cast<int>(RecordingState::Recording) << false
                                    << QString("Stop") << QString("停止") << true << true;
        QTest::newRow("finalizing") << static_cast<int>(RecordingState::Finalizing) << true
                                     << QString("Saving...") << QString("正在保存…") << true << false;
    }

    void languageChangeRetranslatesControlsWithoutChangingRecordingState() {
        QFETCH(int, recordingState);
        QFETCH(bool, available);
        QFETCH(QString, englishText);
        QFETCH(QString, translatedText);
        QFETCH(bool, checked);
        QFETCH(bool, enabled);

        ToolbarWidget toolbar;
        toolbar.setAlwaysOnTopChecked(true);
        toolbar.setAspectRatioChecked(true);
        toolbar.setVideoFitChecked(true);
        toolbar.setRecordingUi(static_cast<RecordingState>(recordingState), available);

        auto *volume = toolbar.findChild<QToolButton *>("volumeButton");
        auto *pin = toolbar.findChild<QToolButton *>("alwaysOnTopButton");
        auto *aspect = toolbar.findChild<QToolButton *>("aspectRatioButton");
        auto *fit = toolbar.findChild<QToolButton *>("videoFitButton");
        auto *settings = toolbar.findChild<QToolButton *>("settingsButton");
        auto *recording = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(volume != nullptr);
        QVERIFY(pin != nullptr);
        QVERIFY(aspect != nullptr);
        QVERIFY(fit != nullptr);
        QVERIFY(settings != nullptr);
        QVERIFY(recording != nullptr);
        QCOMPARE(volume->text(), QString("Volume"));
        QCOMPARE(pin->text(), QString("Pin"));
        QCOMPARE(aspect->text(), QString("Aspect"));
        QCOMPARE(fit->text(), QString("Fit"));
        QCOMPARE(settings->text(), QString("Settings"));
        QCOMPARE(settings->toolTip(), QString("Settings"));
        QCOMPARE(recording->text(), englishText);
        QCOMPARE(recording->isChecked(), checked);
        QCOMPARE(recording->isEnabled(), enabled);

        ToolbarTranslator translator;
        const InstalledTranslator installedTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&toolbar, &languageChange);

        QCOMPARE(volume->text(), QString("音量"));
        QCOMPARE(pin->text(), QString("置顶"));
        QCOMPARE(aspect->text(), QString("比例"));
        QCOMPARE(fit->text(), QString("适应"));
        QCOMPARE(settings->text(), QString("设置"));
        QCOMPARE(settings->toolTip(), QString("设置"));
        QCOMPARE(recording->toolTip(), translatedText);
        QCOMPARE(recording->accessibleName(), translatedText);
        QCOMPARE(recording->text(), translatedText);
        QCOMPARE(recording->isChecked(), checked);
        QCOMPARE(recording->isEnabled(), enabled);
        QVERIFY(pin->isChecked());
        QVERIFY(aspect->isChecked());
        QVERIFY(fit->isChecked());
    }

    void storesShortcutTooltips() {
        ToolbarWidget toolbar;
        toolbar.setVolumeShortcuts("Ctrl+Alt+Up", "Ctrl+Alt+Down");
        toolbar.setAlwaysOnTopShortcut("Ctrl+Alt+T");
        toolbar.setAspectRatioShortcut("Ctrl+Alt+A");
        toolbar.setVideoFitShortcut("Ctrl+Alt+F");
        toolbar.setRecordingShortcut("Ctrl+Alt+R");

        auto *volumeButton = toolbar.findChild<QToolButton *>("volumeButton");
        auto *pinButton = toolbar.findChild<QToolButton *>("alwaysOnTopButton");
        auto *aspectButton = toolbar.findChild<QToolButton *>("aspectRatioButton");
        auto *fitButton = toolbar.findChild<QToolButton *>("videoFitButton");
        auto *recordingButton = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(volumeButton != nullptr);
        QVERIFY(pinButton != nullptr);
        QVERIFY(aspectButton != nullptr);
        QVERIFY(fitButton != nullptr);
        QVERIFY(recordingButton != nullptr);
        QCOMPARE(volumeButton->toolTip(), QString("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down"));
        QCOMPARE(pinButton->toolTip(), QString("Pin: Ctrl+Alt+T"));
        QCOMPARE(aspectButton->toolTip(), QString("Aspect: Ctrl+Alt+A"));
        QCOMPARE(fitButton->toolTip(), QString("Fit: Ctrl+Alt+F"));
        QCOMPARE(recordingButton->toolTip(), QString("Record: Ctrl+Alt+R"));
    }
};

QTEST_MAIN(ToolbarWidgetTest)
#include "ToolbarWidgetTest.moc"
