#include <QtTest/QtTest>

#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHash>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTranslator>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

#include "app/SettingsDialog.h"
#include "app/SettingsApplyTypes.h"
#include "app/LanguageManager.h"
#include "app/UiMessage.h"
#include "backend/VideoQualitySettings.h"
#include "platform/RecordingPathActions.h"

namespace {
bool isAscii(const QString &text) {
    return std::all_of(text.cbegin(), text.cend(), [](QChar character) {
        return character.unicode() <= 0x7f;
    });
}

class FakeRecordingPathActions final : public RecordingPathActions {
public:
    QString chooseExistingDirectory(QWidget *parent,
                                    const QString &initialDirectory) override {
        chooseParent = parent;
        chooseInitialDirectory = initialDirectory;
        return chosenDirectory;
    }

    UiMessage ensureAndOpenDirectory(const QString &directory) override {
        openedDirectories.push_back(directory);
        return UiMessage::raw(openError);
    }

    UiMessage revealFile(const QString &filePath) override {
        revealedFiles.push_back(filePath);
        return UiMessage::raw(revealError);
    }

    QWidget *chooseParent = nullptr;
    QString chooseInitialDirectory;
    QString chosenDirectory;
    QStringList openedDirectories;
    QStringList revealedFiles;
    QString openError;
    QString revealError;
};

class SettingsDialogTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);

        const QString key = QString::fromLatin1(context) + QChar('\x1f')
            + QString::fromLatin1(sourceText);
        return translations.value(key);
    }

    QHash<QString, QString> translations = {
        {QStringLiteral("SettingsDialog\u001fSettings"), QStringLiteral("设置")},
        {QStringLiteral("SettingsDialog\u001fGeneral"), QStringLiteral("常规")},
        {QStringLiteral("SettingsDialog\u001fLanguage"), QStringLiteral("语言")},
        {QStringLiteral("SettingsDialog\u001fApply"), QStringLiteral("应用")},
        {QStringLiteral("SettingsDialog\u001fCancel"), QStringLiteral("取消")},
        {QStringLiteral("SettingsDialog\u001fShow hidden toolbar when the pointer reaches the top"),
         QStringLiteral("工具栏隐藏时，鼠标移到顶部显示")},
        {QStringLiteral("SettingsDialog\u001fApply incomplete; setting count: %n. Correct the highlighted fields."),
         QStringLiteral("应用未完成：1 项设置。请更正突出显示的字段。")},
        {QStringLiteral("SettingsFields\u001fReceiver name"), QStringLiteral("接收器名称")},
        {QStringLiteral("Task8\u001fApp-owned failure: %1"), QStringLiteral("应用错误：%1")},
        {QStringLiteral("Task8\u001fRecovery did not complete: %1"), QStringLiteral("恢复未完成：%1")},
        {QStringLiteral("SettingsDialog\u001f Recovery failed: %1"), QStringLiteral(" 恢复失败：%1")},
        {QStringLiteral("SettingsDialog\u001f%1 (%2): %3"), QStringLiteral("%1 (%2)：%3")},
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
}

class SettingsDialogTest : public QObject {
    Q_OBJECT

private slots:
    void toolbarHoverRevealDraftCancelAndApply() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *checkbox = dialog.findChild<QCheckBox *>("toolbarHoverRevealCheckBox");
        QVERIFY(checkbox != nullptr);
        QVERIFY(checkbox->isChecked());
        QCOMPARE(checkbox->text(), QString("Show hidden toolbar when the pointer reaches the top"));
        checkbox->setChecked(false);
        QVERIFY(!dialog.draftSettings().toolbarHoverReveal());
        QVERIFY(dialog.committedBaseline().toolbarHoverReveal());
        QVERIFY(dialog.hasUnappliedChanges());

        AppSettings committed = AppSettings::defaults();
        committed.setToolbarHoverReveal(false);
        SettingsApplyOutcome outcome;
        outcome.committedSettings = committed;
        outcome.fieldResults = {{SettingsFieldId::toolbarHoverReveal(), false,
                                 SettingsFieldStatus::Applied, {}}};
        outcome.mayClose = true;
        dialog.presentApplyOutcome(outcome);
        QVERIFY(!dialog.committedBaseline().toolbarHoverReveal());
        QVERIFY(!dialog.hasUnappliedChanges());

        SettingsDialog cancelled(AppSettings::defaults());
        auto *cancelCheckbox = cancelled.findChild<QCheckBox *>("toolbarHoverRevealCheckBox");
        QVERIFY(cancelCheckbox != nullptr);
        cancelCheckbox->setChecked(false);
        cancelled.reject();
        QVERIFY(cancelled.committedBaseline().toolbarHoverReveal());
    }

    void toolbarHoverRevealFailedSaveRetainsDraftAndBaseline() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *checkbox = dialog.findChild<QCheckBox *>("toolbarHoverRevealCheckBox");
        QVERIFY(checkbox != nullptr);
        checkbox->setChecked(false);
        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::toolbarHoverReveal(), false,
                                 SettingsFieldStatus::ApplyFailedRolledBack,
                                 "persistence failed"}};
        SettingsApplyGlobalResult global;
        global.persistence.targetPath = "C:/settings.json";
        global.persistence.errorString = "Disk full";
        outcome.globalResult = global;
        dialog.presentApplyOutcome(outcome);

        QVERIFY(!checkbox->isChecked());
        QVERIFY(!dialog.draftSettings().toolbarHoverReveal());
        QVERIFY(dialog.committedBaseline().toolbarHoverReveal());
        QVERIFY(dialog.hasUnappliedChanges());
    }

    void toolbarHoverRevealTextRetranslatesWithoutChangingDraft() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *checkbox = dialog.findChild<QCheckBox *>("toolbarHoverRevealCheckBox");
        QVERIFY(checkbox != nullptr);
        checkbox->setChecked(false);
        SettingsDialogTranslator translator;
        const InstalledTranslator installedTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&dialog, &languageChange);

        QCOMPARE(checkbox->text(), QString::fromUtf8(u8"工具栏隐藏时，鼠标移到顶部显示"));
        QVERIFY(!dialog.draftSettings().toolbarHoverReveal());
    }

    void comboBoxesIgnoreWheelWithoutChangingSelection() {
        SettingsDialog dialog(AppSettings::defaults());
        dialog.show();
        const auto combos = dialog.findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 4);
        for (auto *combo : combos) {
            combo->setCurrentIndex(0);
            combo->setFocus();
            for (const int delta : {-120, 120}) {
                const QPointF position(combo->rect().center());
                QWheelEvent wheel(position, combo->mapToGlobal(position.toPoint()),
                                  QPoint(), QPoint(0, delta), Qt::NoButton,
                                  Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(combo, &wheel);
                QCOMPARE(combo->currentIndex(), 0);
                QVERIFY(!wheel.isAccepted());
            }
            if (combo->count() > 1) {
                QTest::keyClick(combo, Qt::Key_Down);
                QCOMPARE(combo->currentIndex(), 1);
            }
        }
    }

    void languageSelectionIsExposedAndDrafted() {
        AppSettings settings = AppSettings::defaults();
        settings.setLanguage("zh-CN");
        SettingsDialog dialog(settings);
        auto *language = dialog.findChild<QComboBox *>("languageCombo");
        const QVector<LanguageOption> supported = LanguageManager::supportedLanguages();

        QVERIFY(language != nullptr);
        QCOMPARE(language->count(), supported.size() + 1);
        QCOMPARE(language->itemData(0).toString(), QString("system"));
        for (int index = 0; index < supported.size(); ++index) {
            QCOMPARE(language->itemData(index + 1).toString(), supported.at(index).id);
            QCOMPARE(language->itemText(index + 1), supported.at(index).nativeName);
        }
        QCOMPARE(language->currentData().toString(), QString("zh-CN"));

        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(dialog.draftSettings().language(), QString("en"));
    }

    void languageChangeRetranslatesWithoutLosingDraftOrNonLanguageErrors() {
        AppSettings settings = AppSettings::defaults();
        settings.setLanguage("zh-CN");
        SettingsDialog dialog(settings);
        auto *language = dialog.findChild<QComboBox *>("languageCombo");
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        auto *receiverError = dialog.findChild<QLabel *>("receiverNameError");
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *general = dialog.findChild<QGroupBox *>("generalSettingsGroup");
        auto *apply = dialog.findChild<QPushButton *>("applySettingsButton");
        auto *cancel = dialog.findChild<QPushButton *>("cancelSettingsButton");
        auto *languageLabel = dialog.findChild<QLabel *>("languageLabel");
        auto *receiverNameLabel = dialog.findChild<QLabel *>("receiverNameLabel");
        auto *videoResolutionLabel = dialog.findChild<QLabel *>("videoResolutionLabel");
        auto *videoFrameRateLabel = dialog.findChild<QLabel *>("videoFrameRateLabel");
        auto *recordingFormatLabel = dialog.findChild<QLabel *>("recordingFormatLabel");
        auto *recordingOutputFolderLabel = dialog.findChild<QLabel *>("recordingOutputFolderLabel");
        QVERIFY(language != nullptr);
        QVERIFY(receiver != nullptr);
        QVERIFY(receiverError != nullptr);
        QVERIFY(summary != nullptr);
        QVERIFY(general != nullptr);
        QVERIFY(apply != nullptr);
        QVERIFY(cancel != nullptr);
        QVERIFY(languageLabel != nullptr);
        QVERIFY(receiverNameLabel != nullptr);
        QVERIFY(videoResolutionLabel != nullptr);
        QVERIFY(videoFrameRateLabel != nullptr);
        QVERIFY(recordingFormatLabel != nullptr);
        QVERIFY(recordingOutputFolderLabel != nullptr);

        receiver->setText("Draft receiver");
        SettingsApplyOutcome outcome;
        outcome.committedSettings = settings;
        outcome.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Draft receiver"),
             SettingsFieldStatus::ValidationFailed, "not allowed"},
            {SettingsFieldId::language(), QString("zh-CN"),
             SettingsFieldStatus::ValidationFailed, "not available"},
        };
        dialog.presentApplyOutcome(outcome);
        QVERIFY(!receiverError->isHidden());

        language->setCurrentIndex(language->findData("en"));
        QVERIFY(!receiverError->isHidden());
        QVERIFY(summary->text().contains("Apply incomplete; setting count: 1."));
        QVERIFY(dialog.hasUnappliedChanges());

        SettingsDialogTranslator translator;
        const InstalledTranslator installedTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&dialog, &languageChange);

        QCOMPARE(dialog.windowTitle(), QString("设置"));
        QCOMPARE(general->title(), QString("常规"));
        QCOMPARE(languageLabel->text(), QString("语言"));
        QCOMPARE(apply->text(), QString("应用"));
        QCOMPARE(cancel->text(), QString("取消"));
        QCOMPARE(summary->text(), QString("应用未完成：1 项设置。请更正突出显示的字段。"));
        QCOMPARE(receiver->text(), QString("Draft receiver"));
        QCOMPARE(language->currentData().toString(), QString("en"));
        QVERIFY(!receiverError->isHidden());
        QVERIFY(dialog.hasUnappliedChanges());
    }

    void delayedFieldErrorsRetranslateAndKeepRawDetails() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *receiverError = dialog.findChild<QLabel *>("receiverNameError");
        QVERIFY(receiverError != nullptr);

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        SettingsFieldResult result{SettingsFieldId::receiverName(), QString("Receiver"),
                                   SettingsFieldStatus::RecoveryFailed,
                                   "App-owned failure: third-party detail", 123,
                                   "third-party recovery detail"};
        result.userReason = UiMessage::translated("Task8", "App-owned failure: %1",
                                                  {"third-party detail"});
        result.userRecoveryError = UiMessage::translated("Task8", "Recovery did not complete: %1",
                                                         {"third-party recovery detail"});
        outcome.fieldResults = {result};
        dialog.presentApplyOutcome(outcome);
        QCOMPARE(receiverError->text(),
                 QString("Receiver name (Receiver): App-owned failure: third-party detail"
                         " (native error 123) Recovery did not complete: third-party recovery detail"));

        SettingsDialogTranslator translator;
        const InstalledTranslator installedTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&dialog, &languageChange);

        QCOMPARE(receiverError->text(),
                 QString("接收器名称 (Receiver)：应用错误：third-party detail"
                         " (native error 123) 恢复未完成：third-party recovery detail"));
    }

    void summaryAppearsAboveEverySettingsGroup() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *content = dialog.findChild<QScrollArea *>("settingsContentScrollArea");
        QVERIFY(summary != nullptr);
        QVERIFY(content != nullptr);
        auto *layout = qobject_cast<QVBoxLayout *>(content->widget()->layout());
        QVERIFY(layout != nullptr);
        QCOMPARE(layout->indexOf(summary), 0);
        QVERIFY(layout->indexOf(dialog.findChild<QGroupBox *>("generalSettingsGroup")) > 0);
        QVERIFY(summary->isHidden());
    }

    void receiverErrorsAppearBelowTheirControls() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *receiverError = dialog.findChild<QLabel *>("receiverNameError");
        auto *resolutionError = dialog.findChild<QLabel *>("videoResolutionError");
        auto *frameRateError = dialog.findChild<QLabel *>("videoFrameRateError");
        QVERIFY(receiverError != nullptr);
        QVERIFY(resolutionError != nullptr);
        QVERIFY(frameRateError != nullptr);

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Broken receiver"),
             SettingsFieldStatus::ValidationFailed, "must be unique"},
            {SettingsFieldId::videoResolution(), VideoResolution::P720,
             SettingsFieldStatus::ApplyFailedRolledBack, "restart failed"},
            {SettingsFieldId::videoFrameRate(), VideoFrameRate::Fps60,
             SettingsFieldStatus::RecoveryFailed, "restart failed", 12345,
             "previous configuration could not be restored"},
        };
        dialog.presentApplyOutcome(outcome);

        QVERIFY(!receiverError->isHidden());
        QVERIFY(receiverError->text().contains("Receiver name"));
        QVERIFY(receiverError->text().contains("Broken receiver"));
        QVERIFY(!resolutionError->isHidden());
        QVERIFY(resolutionError->text().contains("720p"));
        QVERIFY(!frameRateError->isHidden());
        QVERIFY(frameRateError->text().contains("12345"));
        QVERIFY(frameRateError->text().contains("could not be restored"));
    }

    void shortcutErrorsAppearInStatusColumn() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(table != nullptr);
        QCOMPARE(table->columnCount(), 3);
        QCOMPARE(table->horizontalHeaderItem(2)->text(), QString("Status"));
        QVERIFY(table->isColumnHidden(2));

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                                 QKeySequence("Ctrl+Shift+T"),
                                 SettingsFieldStatus::ApplyFailedRolledBack,
                                 "already registered"}};
        dialog.presentApplyOutcome(outcome);

        QVERIFY(!table->isColumnHidden(2));
        auto *status = qobject_cast<QLabel *>(table->cellWidget(3, 2));
        QVERIFY(status != nullptr);
        QVERIFY(status->wordWrap());
        QVERIFY(status->text().contains("Toggle toolbar"));
        QVERIFY(status->text().contains("Ctrl+Shift+T"));
    }

    void shortcutStatusColumnHidesWhenNoShortcutFailureRemains() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        auto *shortcut = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(table != nullptr);
        QVERIFY(shortcut != nullptr);

        SettingsApplyOutcome nonShortcutAndSuccessfulShortcuts;
        nonShortcutAndSuccessfulShortcuts.committedSettings = AppSettings::defaults();
        nonShortcutAndSuccessfulShortcuts.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Invalid receiver"),
             SettingsFieldStatus::ValidationFailed, "not allowed"},
            {SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+T"),
             SettingsFieldStatus::Applied, {}},
            {SettingsFieldId::shortcut(ShortcutAction::ToggleRecording), QKeySequence("Ctrl+Shift+R"),
             SettingsFieldStatus::Deferred, {}},
        };
        dialog.presentApplyOutcome(nonShortcutAndSuccessfulShortcuts);

        QVERIFY(table->isColumnHidden(2));

        SettingsApplyOutcome shortcutFailure;
        shortcutFailure.committedSettings = AppSettings::defaults();
        shortcutFailure.fieldResults = {{SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                                         QKeySequence("Ctrl+Shift+T"),
                                         SettingsFieldStatus::ApplyFailedRolledBack,
                                         "already registered"}};
        dialog.presentApplyOutcome(shortcutFailure);

        QVERIFY(!table->isColumnHidden(2));
        shortcut->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        QVERIFY(table->isColumnHidden(2));
    }

    void failedDraftRemainsVisibleAfterPartialOutcome() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(receiver != nullptr);
        receiver->setText("Attempted receiver");

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::receiverName(), QString("Attempted receiver"),
                                 SettingsFieldStatus::ApplyFailedRolledBack,
                                 "restart failed"}};
        dialog.presentApplyOutcome(outcome);

        QCOMPARE(receiver->text(), QString("Attempted receiver"));
        QCOMPARE(dialog.draftSettings().receiverName(), QString("Attempted receiver"));
        QCOMPARE(dialog.committedBaseline().receiverName(),
                 AppSettings::defaults().receiverName());
    }

    void editingFailedFieldClearsOnlyItsStaleError() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        auto *resolution = dialog.findChild<QComboBox *>("videoResolutionCombo");
        auto *receiverError = dialog.findChild<QLabel *>("receiverNameError");
        auto *resolutionError = dialog.findChild<QLabel *>("videoResolutionError");
        QVERIFY(receiver != nullptr);
        QVERIFY(resolution != nullptr);
        QVERIFY(receiverError != nullptr);
        QVERIFY(resolutionError != nullptr);

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Attempted receiver"),
             SettingsFieldStatus::ValidationFailed, "not allowed"},
            {SettingsFieldId::videoResolution(), VideoResolution::P720,
             SettingsFieldStatus::ValidationFailed, "not available"},
        };
        dialog.presentApplyOutcome(outcome);

        receiver->setText("Corrected receiver");

        QVERIFY(receiverError->isHidden());
        QVERIFY(!resolutionError->isHidden());
    }

    void partialOutcomeAdoptsSuccessfulCommittedBaseline() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(receiver != nullptr);
        receiver->setText("Rejected receiver");

        AppSettings committed = AppSettings::defaults();
        VideoQualitySettings quality = committed.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        committed.setVideoQuality(quality);
        SettingsApplyOutcome outcome;
        outcome.committedSettings = committed;
        outcome.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Rejected receiver"),
             SettingsFieldStatus::ValidationFailed, "not allowed"},
            {SettingsFieldId::videoFrameRate(), VideoFrameRate::Fps60,
             SettingsFieldStatus::Applied, {}},
        };
        dialog.presentApplyOutcome(outcome);

        QCOMPARE(dialog.committedBaseline().videoQuality().frameRate, VideoFrameRate::Fps60);
        QCOMPARE(dialog.committedBaseline().receiverName(), AppSettings::defaults().receiverName());
        QCOMPARE(dialog.draftSettings().receiverName(), QString("Rejected receiver"));
    }

    void cancelAfterPartialApplyDoesNotReverseCommittedFields() {
        SettingsDialog dialog(AppSettings::defaults());
        AppSettings committed = AppSettings::defaults();
        committed.setReceiverName("Committed receiver");
        SettingsApplyOutcome outcome;
        outcome.committedSettings = committed;
        outcome.fieldResults = {{SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                                 QKeySequence("Ctrl+Shift+T"),
                                 SettingsFieldStatus::ValidationFailed,
                                 "already registered"}};
        dialog.presentApplyOutcome(outcome);

        dialog.reject();

        QCOMPARE(dialog.committedBaseline().receiverName(), QString("Committed receiver"));
    }

    void partialFailureKeepsDialogOpen() {
        SettingsDialog dialog(AppSettings::defaults());
        dialog.show();
        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::receiverName(), QString("Invalid"),
                                 SettingsFieldStatus::ValidationFailed, "not allowed"}};
        outcome.mayClose = false;
        dialog.presentApplyOutcome(outcome);
        QVERIFY(dialog.isVisible());
    }

    void successOrDeferredOnlyOutcomeClosesDialog() {
        SettingsDialog dialog(AppSettings::defaults());
        dialog.show();
        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::videoFrameRate(), VideoFrameRate::Fps60,
                                 SettingsFieldStatus::Deferred, {}}};
        outcome.mayClose = true;
        dialog.presentApplyOutcome(outcome);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
    }

    void applyRequestedEmitsFullDraftWithoutClosing() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        auto *frameRate = dialog.findChild<QComboBox *>("videoFrameRateCombo");
        auto *shortcut = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(receiver != nullptr);
        QVERIFY(frameRate != nullptr);
        QVERIFY(shortcut != nullptr);
        receiver->setText("Desk Receiver");
        frameRate->setCurrentText("60 fps");
        shortcut->setKeySequence(QKeySequence("Ctrl+Shift+T"));

        int requested = 0;
        AppSettings emitted;
        connect(&dialog, &SettingsDialog::applyRequested, this, [&requested, &emitted](AppSettings draft) {
            ++requested;
            emitted = draft;
        });
        dialog.accept();

        QCOMPARE(requested, 1);
        QCOMPARE(emitted.receiverName(), QString("Desk Receiver"));
        QCOMPARE(emitted.videoQuality().frameRate, VideoFrameRate::Fps60);
        QCOMPARE(emitted.shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+T"));
        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
    }

    void summaryShowsUnappliedChangesAndKeepsPathActionErrorsIndependent() {
        FakeRecordingPathActions actions;
        actions.openError = "Could not open recording directory";
        SettingsDialog dialog(AppSettings::defaults(), nullptr, &actions);
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(receiver != nullptr);
        QVERIFY(open != nullptr);
        QVERIFY(summary != nullptr);

        receiver->setText("Unapplied receiver");
        QVERIFY(summary->text().contains("Unapplied change count: 1."));
        open->click();
        QVERIFY(summary->text().contains(actions.openError));
        receiver->setText("Other receiver");
        QVERIFY(summary->text().contains(actions.openError));
    }

    void summaryDescribesPartialAndPersistenceFailuresPrecisely() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(summary != nullptr);

        SettingsApplyOutcome partial;
        partial.committedSettings = AppSettings::defaults();
        partial.fieldResults = {
            {SettingsFieldId::videoFrameRate(), VideoFrameRate::Fps60,
             SettingsFieldStatus::Applied, {}},
            {SettingsFieldId::receiverName(), QString("Invalid"),
             SettingsFieldStatus::ValidationFailed, "not allowed"},
            {SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+T"),
             SettingsFieldStatus::ApplyFailedRolledBack, "already registered"},
        };
        dialog.presentApplyOutcome(partial);
        QCOMPARE(summary->text(),
                 QString("Some settings were applied. Apply incomplete; setting count: 2. Correct the highlighted fields."));

        SettingsApplyOutcome persistence;
        persistence.committedSettings = AppSettings::defaults();
        SettingsApplyGlobalResult global;
        global.persistence.targetPath = "C:\\path\\airplay-settings.json";
        global.persistence.errorString = "Access is denied.";
        persistence.globalResult = global;
        dialog.presentApplyOutcome(persistence);
        QCOMPARE(summary->text(),
                 QString("Could not save C:\\path\\airplay-settings.json: Access is denied. No changes from this Apply were committed."));
    }

    void persistenceFailureDoesNotHighlightRolledBackFieldsOrSuggestEditingThem() {
        FakeRecordingPathActions actions;
        actions.openError = "Could not open recording directory";
        SettingsDialog dialog(AppSettings::defaults(), nullptr, &actions);
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        auto *receiverError = dialog.findChild<QLabel *>("receiverNameError");
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        QVERIFY(summary != nullptr);
        QVERIFY(receiver != nullptr);
        QVERIFY(receiverError != nullptr);
        QVERIFY(table != nullptr);
        QVERIFY(open != nullptr);

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        SettingsApplyGlobalResult global;
        global.persistence.targetPath = "C:\\path\\airplay-settings.json";
        global.persistence.errorString = "Access is denied.";
        outcome.globalResult = global;
        outcome.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Attempted receiver"),
             SettingsFieldStatus::ApplyFailedRolledBack, "persistence failed"},
            {SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+T"),
             SettingsFieldStatus::ApplyFailedRolledBack, "persistence failed"},
            {SettingsFieldId::recordingOutputDirectory(), QString("C:/recordings"),
             SettingsFieldStatus::ApplyFailedRolledBack, "persistence failed"},
        };
        dialog.presentApplyOutcome(outcome);

        QCOMPARE(summary->text(),
                 QString("Could not save C:\\path\\airplay-settings.json: Access is denied. No changes from this Apply were committed."));
        QVERIFY(!summary->text().contains("correct the highlighted fields"));
        QVERIFY(receiverError->isHidden());
        auto *shortcutStatus = qobject_cast<QLabel *>(table->cellWidget(3, 2));
        QVERIFY(shortcutStatus != nullptr);
        QVERIFY(shortcutStatus->isHidden());

        receiver->setText("Edited receiver");
        QVERIFY(summary->text().contains("Could not save C:\\path\\airplay-settings.json"));
        open->click();
        QVERIFY(summary->text().contains(actions.openError));
        QVERIFY(summary->text().contains("Could not save C:\\path\\airplay-settings.json"));
    }

    void persistenceFailureStillHighlightsRecoveryFailureDetails() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(summary != nullptr);
        QVERIFY(table != nullptr);

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        SettingsApplyGlobalResult global;
        global.persistence.targetPath = "C:\\path\\airplay-settings.json";
        global.persistence.errorString = "Access is denied.";
        outcome.globalResult = global;
        outcome.fieldResults = {
            {SettingsFieldId::receiverName(), QString("Attempted receiver"),
             SettingsFieldStatus::ApplyFailedRolledBack, "persistence failed"},
            {SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+T"),
             SettingsFieldStatus::RecoveryFailed, "candidate registration failed", 12345,
             "previous binding could not be restored"},
        };
        dialog.presentApplyOutcome(outcome);

        auto *shortcutStatus = qobject_cast<QLabel *>(table->cellWidget(3, 2));
        QVERIFY(shortcutStatus != nullptr);
        QVERIFY(!shortcutStatus->isHidden());
        QVERIFY(shortcutStatus->text().contains("candidate registration failed"));
        QVERIFY(shortcutStatus->text().contains("12345"));
        QVERIFY(shortcutStatus->text().contains("previous binding could not be restored"));
        QVERIFY(summary->text().contains("Could not save C:\\path\\airplay-settings.json"));
        QVERIFY(summary->text().contains("Recovery requires attention; issue count: 1."));
        QVERIFY(!summary->text().contains("correct the highlighted fields"));
        QVERIFY(!summary->text().contains("Previous setting was restored"));
    }

    void listsAllShortcutActions() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 7);
    }

    void showsRecordingSettingsInRequiredGroupOrder() {
        SettingsDialog dialog(AppSettings::defaults());

        QStringList groupTitles;
        auto *content = dialog.findChild<QScrollArea *>("settingsContentScrollArea");
        QVERIFY(content != nullptr);
        auto *layout = qobject_cast<QVBoxLayout *>(content->widget()->layout());
        QVERIFY(layout != nullptr);
        for (int index = 0; index < layout->count(); ++index) {
            if (auto *group = qobject_cast<QGroupBox *>(layout->itemAt(index)->widget())) {
                groupTitles.push_back(group->title());
            }
        }

        QCOMPARE(groupTitles,
                 QStringList({"General", "Video", "Recording", "Hotkey Binding", "Diagnostics"}));
        QVERIFY(dialog.findChild<QGroupBox *>("recordingSettingsGroup") != nullptr);
        QVERIFY(dialog.findChild<QComboBox *>("recordingFormatCombo") != nullptr);
        QVERIFY(dialog.findChild<QLineEdit *>("recordingOutputDirectoryEdit") != nullptr);
        QVERIFY(dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton") != nullptr);
        QVERIFY(dialog.findChild<QPushButton *>("openRecordingDirectoryButton") != nullptr);
        QVERIFY(dialog.findChild<QCheckBox *>("showRecordingCompletionMessageCheckBox") != nullptr);
    }

    void diagnosticsActionsHaveRequiredLayoutTextAndSignals() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *content = dialog.findChild<QScrollArea *>("settingsContentScrollArea");
        auto *group = dialog.findChild<QGroupBox *>("diagnosticsSettingsGroup");
        auto *restart = dialog.findChild<QPushButton *>("restartWithDiagnosticLoggingButton");
        auto *open = dialog.findChild<QPushButton *>("openDiagnosticLogFolderButton");
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(content != nullptr);
        auto *layout = qobject_cast<QVBoxLayout *>(content->widget()->layout());
        QVERIFY(layout != nullptr);
        QVERIFY(group != nullptr);
        QVERIFY(restart != nullptr);
        QVERIFY(open != nullptr);
        QVERIFY(buttons != nullptr);
        QCOMPARE(restart->text(), QString("Restart with Diagnostic Logging"));
        QCOMPARE(open->text(), QString("Open Log Folder"));
        QVERIFY(isAscii(group->title()));
        QVERIFY(isAscii(restart->text()));
        QVERIFY(isAscii(open->text()));
        QVERIFY(layout->indexOf(group) > layout->indexOf(dialog.findChild<QGroupBox *>("hotkeyBindingGroup")));
        QVERIFY(dialog.layout()->indexOf(content) < dialog.layout()->indexOf(buttons));

        QSignalSpy restartSpy(&dialog, &SettingsDialog::restartWithDiagnosticLoggingRequested);
        QSignalSpy openSpy(&dialog, &SettingsDialog::openDiagnosticLogFolderRequested);
        restart->click();
        open->click();
        QCOMPARE(restartSpy.count(), 1);
        QCOMPARE(openSpy.count(), 1);
    }

    void diagnosticPresentationKeepsExistingErrorsAndTracksUnappliedDrafts() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(summary != nullptr);
        QVERIFY(receiver != nullptr);
        QVERIFY(!dialog.hasUnappliedChanges());
        receiver->setText("Draft receiver");
        QVERIFY(dialog.hasUnappliedChanges());

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        SettingsApplyGlobalResult global;
        global.persistence.targetPath = "C:/settings.json";
        global.persistence.errorString = "access denied";
        outcome.globalResult = global;
        dialog.presentApplyOutcome(outcome);
        dialog.presentDiagnosticActionError(UiMessage::raw("Could not open diagnostic log folder"));

        QVERIFY(!summary->isHidden());
        QVERIFY(summary->text().contains("Could not save C:/settings.json"));
        QVERIFY(summary->text().contains("Could not open diagnostic log folder"));
        QVERIFY(isAscii(summary->text()));
    }

    void initializesRecordingSettingsControls() {
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory(QDir::fromNativeSeparators("C:/captures/AirPlay"));
        settings.setShowRecordingCompletionMessage(false);
        SettingsDialog dialog(settings);

        auto *format = dialog.findChild<QComboBox *>("recordingFormatCombo");
        auto *directory = dialog.findChild<QLineEdit *>("recordingOutputDirectoryEdit");
        auto *completion = dialog.findChild<QCheckBox *>("showRecordingCompletionMessageCheckBox");
        QVERIFY(format != nullptr);
        QVERIFY(directory != nullptr);
        QVERIFY(completion != nullptr);

        QCOMPARE(format->count(), 1);
        QCOMPARE(format->itemText(0), QString("MP4"));
        QCOMPARE(format->itemData(0).toInt(), static_cast<int>(RecordingFormat::Mp4));
        QVERIFY(directory->isReadOnly());
        QCOMPARE(directory->text(), QDir::toNativeSeparators(settings.recordingOutputDirectory()));
        QVERIFY(!completion->isChecked());
    }

    void recordingCompletionMessageDefaultsChecked() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *completion = dialog.findChild<QCheckBox *>("showRecordingCompletionMessageCheckBox");
        QVERIFY(completion != nullptr);
        QVERIFY(completion->isChecked());
    }

    void exposesAcceptedRecordingSettings() {
        AppSettings settings = AppSettings::defaults();
        SettingsDialog dialog(settings);
        auto *completion = dialog.findChild<QCheckBox *>("showRecordingCompletionMessageCheckBox");
        QVERIFY(completion != nullptr);

        completion->setChecked(false);
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().recordingFormat(), RecordingFormat::Mp4);
        QCOMPARE(dialog.settings().recordingOutputDirectory(), settings.recordingOutputDirectory());
        QVERIFY(!dialog.settings().showRecordingCompletionMessage());
    }

    void chooseRecordingDirectoryUpdatesCandidateAndPassesInitialDirectory() {
        AppSettings settings = AppSettings::defaults();
        FakeRecordingPathActions actions;
        actions.chosenDirectory = QDir::cleanPath(QDir::temp().filePath("chosen recordings"));
        SettingsDialog dialog(settings, nullptr, &actions);
        auto *button = dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton");
        auto *directory = dialog.findChild<QLineEdit *>("recordingOutputDirectoryEdit");
        QVERIFY(button != nullptr);
        QVERIFY(directory != nullptr);

        button->click();

        QCOMPARE(actions.chooseParent, &dialog);
        QCOMPARE(actions.chooseInitialDirectory, settings.recordingOutputDirectory());
        QCOMPARE(directory->text(), QDir::toNativeSeparators(actions.chosenDirectory));
        dialog.accept();
        QCOMPARE(dialog.settings().recordingOutputDirectory(),
                 QFileInfo(actions.chosenDirectory).absoluteFilePath());
    }

    void relativeRecordingDirectoryChoiceImmediatelyBecomesAbsoluteCandidate() {
        AppSettings settings = AppSettings::defaults();
        FakeRecordingPathActions actions;
        actions.chosenDirectory = QDir::fromNativeSeparators(
            "relative recordings/../relative recordings/final");
        const QString expectedAbsolute = QDir::cleanPath(
            QFileInfo(actions.chosenDirectory).absoluteFilePath());
        SettingsDialog dialog(settings, nullptr, &actions);
        auto *choose = dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton");
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *directory = dialog.findChild<QLineEdit *>("recordingOutputDirectoryEdit");
        QVERIFY(choose != nullptr);
        QVERIFY(open != nullptr);
        QVERIFY(directory != nullptr);

        choose->click();

        QCOMPARE(directory->text(), QDir::toNativeSeparators(expectedAbsolute));
        open->click();
        QCOMPARE(actions.openedDirectories, QStringList({expectedAbsolute}));
        dialog.accept();
        QCOMPARE(dialog.settings().recordingOutputDirectory(), expectedAbsolute);
    }

    void cancelledRecordingDirectoryChoiceLeavesCandidateUnchanged() {
        AppSettings settings = AppSettings::defaults();
        FakeRecordingPathActions actions;
        SettingsDialog dialog(settings, nullptr, &actions);
        auto *button = dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton");
        auto *directory = dialog.findChild<QLineEdit *>("recordingOutputDirectoryEdit");
        QVERIFY(button != nullptr);
        QVERIFY(directory != nullptr);
        const QString originalText = directory->text();

        button->click();

        QCOMPARE(directory->text(), originalText);
        dialog.accept();
        QCOMPARE(dialog.settings().recordingOutputDirectory(),
                 settings.recordingOutputDirectory());
    }

    void openRecordingDirectoryUsesCandidatePath() {
        AppSettings settings = AppSettings::defaults();
        FakeRecordingPathActions actions;
        actions.chosenDirectory = QDir::cleanPath(QDir::temp().filePath("open recordings"));
        SettingsDialog dialog(settings, nullptr, &actions);
        auto *choose = dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton");
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        QVERIFY(choose != nullptr);
        QVERIFY(open != nullptr);

        choose->click();
        open->click();

        QCOMPARE(actions.openedDirectories, QStringList({actions.chosenDirectory}));
    }

    void openRecordingDirectoryErrorUsesAlwaysVisibleErrorLabel() {
        FakeRecordingPathActions actions;
        actions.openError = "Could not open recording directory";
        SettingsDialog dialog(AppSettings::defaults(), nullptr, &actions);
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *error = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(open != nullptr);
        QVERIFY(error != nullptr);

        open->click();

        QCOMPARE(error->text(), actions.openError);
        QVERIFY(!error->isHidden());
    }

    void cachedRecordingDirectoryFailureRetranslatesWithoutRetry_data() {
        QTest::addColumn<bool>("creationFails");
        QTest::addColumn<QString>("english");
        QTest::addColumn<QString>("chinese");
        QTest::newRow("open") << false << "Could not open recording directory: %1"
            << QString::fromUtf8(u8"无法打开录制目录：%1");
        QTest::newRow("create") << true << "Could not create recording directory: %1"
            << QString::fromUtf8(u8"无法创建录制目录：%1");
    }

    void cachedRecordingDirectoryFailureRetranslatesWithoutRetry() {
        QFETCH(bool, creationFails);
        QFETCH(QString, english);
        QFETCH(QString, chinese);
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        QString directory = temporaryDirectory.filePath("recordings");
        if (creationFails) {
            QFile blocker(temporaryDirectory.filePath("blocker"));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.close();
            directory = blocker.fileName() + "/recordings";
        }
        int launchAttempts = 0;
        WindowsRecordingPathActions actions(
            [&](const QString &, const QStringList &) {
                ++launchAttempts;
                return false;
            });
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory(directory);
        SettingsDialog dialog(settings, nullptr, &actions);
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *receiver = dialog.findChild<QLineEdit *>("receiverNameEdit");
        auto *receiverError = dialog.findChild<QLabel *>("receiverNameError");
        QVERIFY(open != nullptr);
        QVERIFY(summary != nullptr);
        QVERIFY(receiver != nullptr);
        QVERIFY(receiverError != nullptr);
        receiver->setText("Unapplied receiver");
        SettingsApplyOutcome outcome;
        outcome.committedSettings = settings;
        outcome.fieldResults = {{SettingsFieldId::receiverName(), QString("Unapplied receiver"),
                                 SettingsFieldStatus::ValidationFailed, "raw validation detail"}};
        dialog.presentApplyOutcome(outcome);
        open->click();
        const QString nativeDirectory = QDir::toNativeSeparators(directory);
        QVERIFY(summary->text().contains(english.arg(nativeDirectory)));
        QCOMPARE(launchAttempts, creationFails ? 0 : 1);
        if (creationFails) {
            QVERIFY(QFile::remove(temporaryDirectory.filePath("blocker")));
        }

        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("zh-CN", QLocale("en-US")));
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&dialog, &languageChange);
        QVERIFY2(summary->text().contains(chinese.arg(nativeDirectory)),
                 qPrintable(summary->text()));
        QCOMPARE(launchAttempts, creationFails ? 0 : 1);
        if (creationFails) {
            QVERIFY(!QDir(directory).exists());
        }
        QCOMPARE(receiver->text(), QString("Unapplied receiver"));
        QVERIFY(receiverError->text().contains("raw validation detail"));
        QVERIFY(dialog.hasUnappliedChanges());

        QVERIFY(language.apply("en", QLocale("en-US")));
        QCoreApplication::sendEvent(&dialog, &languageChange);
        QVERIFY(summary->text().contains(english.arg(nativeDirectory)));
        QCOMPARE(launchAttempts, creationFails ? 0 : 1);
    }

    void cachedRawActionFailuresRemainUntranslated() {
        FakeRecordingPathActions actions;
        actions.openError = "Could not open recording directory: %1";
        const QString diagnosticError = "Could not open diagnostic log folder: %1";
        SettingsDialog dialog(AppSettings::defaults(), nullptr, &actions);
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(open != nullptr);
        QVERIFY(summary != nullptr);
        open->click();
        dialog.presentDiagnosticActionError(UiMessage::raw(diagnosticError));
        const QString expected = actions.openError + '\n' + diagnosticError;
        QCOMPARE(summary->text(), expected);

        LanguageManager language(QCoreApplication::instance());
        QEvent languageChange(QEvent::LanguageChange);
        QVERIFY(language.apply("zh-CN", QLocale("en-US")));
        QCoreApplication::sendEvent(&dialog, &languageChange);
        QCOMPARE(summary->text(), expected);
        QVERIFY(language.apply("en", QLocale("en-US")));
        QCoreApplication::sendEvent(&dialog, &languageChange);
        QCOMPARE(summary->text(), expected);
        QCOMPARE(actions.openedDirectories.size(), 1);
    }

    void successfulPathActionsClearPreviousPathErrors() {
        FakeRecordingPathActions openActions;
        openActions.openError = "Could not open recording directory";
        SettingsDialog openDialog(AppSettings::defaults(), nullptr, &openActions);
        auto *open = openDialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *openError = openDialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(open != nullptr);
        QVERIFY(openError != nullptr);
        open->click();
        QVERIFY(!openError->isHidden());
        openActions.openError.clear();
        open->click();
        QVERIFY(openError->isHidden());

        FakeRecordingPathActions chooseActions;
        chooseActions.openError = "Could not open recording directory";
        chooseActions.chosenDirectory = QDir::temp().filePath("valid recordings");
        SettingsDialog chooseDialog(AppSettings::defaults(), nullptr, &chooseActions);
        auto *chooseOpen = chooseDialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *choose = chooseDialog.findChild<QPushButton *>("chooseRecordingDirectoryButton");
        auto *chooseError = chooseDialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(chooseOpen != nullptr);
        QVERIFY(choose != nullptr);
        QVERIFY(chooseError != nullptr);
        chooseOpen->click();
        QVERIFY(!chooseError->isHidden());
        choose->click();
        QVERIFY(!chooseError->text().contains("Could not open recording directory"));
        QVERIFY(chooseError->text().contains("Unapplied change count: 1."));
    }

    void validationThenPathFailureThenOpenSuccessKeepsValidationVisible() {
        FakeRecordingPathActions actions;
        actions.openError = "Path action failed";
        SettingsDialog dialog(AppSettings::defaults(), nullptr, &actions);
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *error = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(open != nullptr);
        QVERIFY(error != nullptr);

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::receiverName(), QString("Invalid"),
                                 SettingsFieldStatus::ValidationFailed, "not allowed"}};
        dialog.presentApplyOutcome(outcome);
        QVERIFY(error->text().contains("Apply incomplete; setting count: 1."));

        open->click();
        QVERIFY(error->text().contains("Apply incomplete; setting count: 1."));
        QVERIFY(error->text().contains(actions.openError));

        actions.openError.clear();
        open->click();
        QVERIFY(error->text().contains("Apply incomplete; setting count: 1."));
        QVERIFY(!error->text().contains("Path action failed"));
        QVERIFY(!error->isHidden());
    }

    void pathFailureThenValidationThenChooseSuccessKeepsValidationVisible() {
        FakeRecordingPathActions actions;
        actions.openError = "Path action failed";
        actions.chosenDirectory = QDir::temp().filePath("valid recordings");
        SettingsDialog dialog(AppSettings::defaults(), nullptr, &actions);
        auto *open = dialog.findChild<QPushButton *>("openRecordingDirectoryButton");
        auto *choose = dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton");
        auto *error = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(open != nullptr);
        QVERIFY(choose != nullptr);
        QVERIFY(error != nullptr);

        open->click();
        QVERIFY(error->text().contains(actions.openError));

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::receiverName(), QString("Invalid"),
                                 SettingsFieldStatus::ValidationFailed, "not allowed"}};
        dialog.presentApplyOutcome(outcome);
        QVERIFY(error->text().contains("Apply incomplete; setting count: 1."));
        QVERIFY(error->text().contains(actions.openError));

        choose->click();
        QVERIFY(error->text().contains("Apply incomplete; setting count: 1."));
        QVERIFY(!error->text().contains("Path action failed"));
        QVERIFY(!error->isHidden());
    }

    void toggleVideoFitShortcutEditExists() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleVideoFit");
        QVERIFY(edit != nullptr);
    }

    void toggleVideoFitShortcutStartsWithDefault() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleVideoFit");
        QVERIFY(edit != nullptr);
        QCOMPARE(edit->keySequence(), AppSettings::defaults().shortcutFor(ShortcutAction::ToggleVideoFit));
    }

    void exposesAcceptedToggleVideoFitShortcut() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleVideoFit");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Shift+V"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleVideoFit), QKeySequence("Ctrl+Shift+V"));
    }

    void showsGeneralAndHotkeyBindingSections() {
        SettingsDialog dialog(AppSettings::defaults());

        QVERIFY(dialog.findChild<QGroupBox *>("generalSettingsGroup") != nullptr);
        QVERIFY(dialog.findChild<QGroupBox *>("hotkeyBindingGroup") != nullptr);
        QVERIFY(dialog.findChild<QLineEdit *>("receiverNameEdit") != nullptr);
        QVERIFY(dialog.findChild<QPushButton *>("resetHotkeysButton") != nullptr);
    }

    void keepsApplyAndCancelReachableAtCompactHeight() {
        SettingsDialog dialog(AppSettings::defaults());
        dialog.resize(800, 500);
        dialog.show();
        QTest::qWait(0);

        auto *content = dialog.findChild<QScrollArea *>("settingsContentScrollArea");
        auto *apply = dialog.findChild<QPushButton *>("applySettingsButton");
        auto *cancel = dialog.findChild<QPushButton *>("cancelSettingsButton");
        QVERIFY(content != nullptr);
        QVERIFY(apply != nullptr);
        QVERIFY(cancel != nullptr);

        QVERIFY(dialog.height() <= 500);
        QVERIFY(content->widgetResizable());
        QCOMPARE(content->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QVERIFY(content->verticalScrollBar()->maximum() > 0);
        QVERIFY(dialog.rect().contains(apply->mapTo(&dialog, apply->rect().center())));
        QVERIFY(dialog.rect().contains(cancel->mapTo(&dialog, cancel->rect().center())));
    }

    void shortcutRowsRemainExpandedAtCompactHeight() {
        SettingsDialog dialog(AppSettings::defaults());
        dialog.resize(800, 500);
        dialog.show();
        QTest::qWait(0);
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 7);
        QCOMPARE(table->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(table->verticalScrollBar()->maximum(), 0);
        QVERIFY(table->viewport()->rect().contains(
            table->visualItemRect(table->item(6, 0))));
    }

    void shortcutScrollingStartsAfterTenRows() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(table != nullptr);
        table->setRowCount(10);
        dialog.presentDiagnosticActionError({});
        dialog.show();
        QTest::qWait(0);
        QCOMPARE(table->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(table->verticalScrollBar()->maximum(), 0);
        const int tenRowHeight = table->height();
        table->setRowCount(11);
        dialog.presentDiagnosticActionError({});
        QTest::qWait(0);
        QCOMPARE(table->height(), tenRowHeight);
        QCOMPARE(table->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        QVERIFY(table->verticalScrollBar()->maximum() > 0);
    }

    void initialWindowFitsAvailableScreen() {
        SettingsDialog dialog(AppSettings::defaults());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        auto *screen = dialog.screen();
        auto *apply = dialog.findChild<QPushButton *>("applySettingsButton");
        auto *cancel = dialog.findChild<QPushButton *>("cancelSettingsButton");
        QVERIFY(screen != nullptr);
        QVERIFY(apply != nullptr);
        QVERIFY(cancel != nullptr);

        QVERIFY(dialog.frameGeometry().height() <= screen->availableGeometry().height());
        QVERIFY(dialog.rect().contains(apply->mapTo(&dialog, apply->rect().center())));
        QVERIFY(dialog.rect().contains(cancel->mapTo(&dialog, cancel->rect().center())));
    }

    void exposesAcceptedReceiverName() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(edit != nullptr);

        edit->setText("Desk Receiver");
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().receiverName(), QString("Desk Receiver"));
    }

    void rejectsEmptyReceiverNameOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(edit != nullptr);

        edit->setText("   ");
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.draftSettings().receiverName(), QString());
    }

    void resetHotkeysRestoresDefaultEditsWithoutSaving() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));
        SettingsDialog dialog(settings);

        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        auto *button = dialog.findChild<QPushButton *>("resetHotkeysButton");
        QVERIFY(edit != nullptr);
        QVERIFY(button != nullptr);

        QCOMPARE(edit->keySequence(), QKeySequence("Ctrl+Shift+H"));
        button->click();

        QCOMPARE(edit->keySequence(), AppSettings::defaults().shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(dialog.committedBaseline().shortcutFor(ShortcutAction::ToggleToolbar),
                 QKeySequence("Ctrl+Shift+H"));
    }

    void resetHotkeysRestoresToggleRecordingDefaultWithoutSaving() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleRecording, QKeySequence("Ctrl+Shift+R"));
        SettingsDialog dialog(settings);

        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleRecording");
        auto *button = dialog.findChild<QPushButton *>("resetHotkeysButton");
        QVERIFY(edit != nullptr);
        QVERIFY(button != nullptr);

        QCOMPARE(edit->keySequence(), QKeySequence("Ctrl+Shift+R"));
        button->click();

        QCOMPARE(edit->keySequence(),
                 AppSettings::defaults().shortcutFor(ShortcutAction::ToggleRecording));
        QCOMPARE(dialog.committedBaseline().shortcutFor(ShortcutAction::ToggleRecording),
                 QKeySequence("Ctrl+Shift+R"));
    }

    void exposesAcceptedSettings() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Shift+H"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
    }

    void rejectsDuplicateShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.draftSettings().shortcutFor(ShortcutAction::ToggleToolbar),
                 QKeySequence("Ctrl+Alt+T"));
    }

    void rejectsUnsupportedShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_NumLock)));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.draftSettings().shortcutFor(ShortcutAction::ToggleToolbar),
                 QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_NumLock)));
    }

    void rejectsMultiStepShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+K, Ctrl+C"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.draftSettings().shortcutFor(ShortcutAction::ToggleToolbar),
                 QKeySequence("Ctrl+K, Ctrl+C"));
    }

    void showsVideoSettingsSection() {
        SettingsDialog dialog(AppSettings::defaults());

        QVERIFY(dialog.findChild<QGroupBox *>("videoSettingsGroup") != nullptr);
        QVERIFY(dialog.findChild<QComboBox *>("videoResolutionCombo") != nullptr);
        QVERIFY(dialog.findChild<QComboBox *>("videoFrameRateCombo") != nullptr);
    }

    void exposesAcceptedVideoSettings() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *resolution = dialog.findChild<QComboBox *>("videoResolutionCombo");
        auto *frameRate = dialog.findChild<QComboBox *>("videoFrameRateCombo");
        QVERIFY(resolution != nullptr);
        QVERIFY(frameRate != nullptr);

        resolution->setCurrentText("720p");
        frameRate->setCurrentText("15 fps");
        dialog.accept();

        const VideoQualitySettings quality = dialog.settings().videoQuality();
        QCOMPARE(quality.resolution, VideoResolution::P720);
        QCOMPARE(quality.frameRate, VideoFrameRate::Fps15);
    }

    void initializesFromAppSettings() {
        AppSettings settings = AppSettings::defaults();
        VideoQualitySettings vqs;
        vqs.resolution = VideoResolution::P540;
        vqs.frameRate = VideoFrameRate::Fps60;
        settings.setVideoQuality(vqs);

        SettingsDialog dialog(settings);

        auto *resolution = dialog.findChild<QComboBox *>("videoResolutionCombo");
        auto *frameRate = dialog.findChild<QComboBox *>("videoFrameRateCombo");

        QCOMPARE(resolution->currentText(), "540p");
        QCOMPARE(frameRate->currentText(), "60 fps");
    }
};

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
