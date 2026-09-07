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
        {QStringLiteral("ToolbarWidget\u001fVolume"), QStringLiteral("音量")},
        {QStringLiteral("ToolbarWidget\u001fPin"), QStringLiteral("置顶")},
        {QStringLiteral("ToolbarWidget\u001fAspect"), QStringLiteral("比例")},
        {QStringLiteral("ToolbarWidget\u001fFit"), QStringLiteral("适应")},
        {QStringLiteral("ToolbarWidget\u001fSettings"), QStringLiteral("设置")},
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
    void exposesRequiredControls() {
        ToolbarWidget toolbar;
        QVERIFY(toolbar.findChild<QToolButton *>("volumeButton"));
        QVERIFY(toolbar.findChild<QSlider *>("volumeSlider"));
        QVERIFY(toolbar.findChild<QToolButton *>("alwaysOnTopButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("aspectRatioButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("videoFitButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("recordingButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("settingsButton"));
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
        QVERIFY(settingsIdx == recordingIdx + 1);
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
        QCOMPARE(recording->text(), translatedText);
        QCOMPARE(recording->isChecked(), checked);
        QCOMPARE(recording->isEnabled(), enabled);
        QVERIFY(pin->isChecked());
        QVERIFY(aspect->isChecked());
        QVERIFY(fit->isChecked());
    }

    void storesShortcutTooltips() {
        ToolbarWidget toolbar;
        toolbar.setVolumeShortcutTooltip("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down");
        toolbar.setAlwaysOnTopShortcutTooltip("Pin: Ctrl+Alt+T");
        toolbar.setAspectRatioShortcutTooltip("Aspect: Ctrl+Alt+A");
        toolbar.setVideoFitShortcutTooltip("Fit: Ctrl+Alt+F");
        toolbar.setRecordingShortcutTooltip("Record: Ctrl+Alt+R");

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
