#include <QtTest/QtTest>

#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "app/SettingsDialog.h"
#include "app/SettingsApplyTypes.h"
#include "backend/VideoQualitySettings.h"
#include "platform/RecordingPathActions.h"

namespace {
class FakeRecordingPathActions final : public RecordingPathActions {
public:
    QString chooseExistingDirectory(QWidget *parent,
                                    const QString &initialDirectory) override {
        chooseParent = parent;
        chooseInitialDirectory = initialDirectory;
        return chosenDirectory;
    }

    QString ensureAndOpenDirectory(const QString &directory) override {
        openedDirectories.push_back(directory);
        return openError;
    }

    QString revealFile(const QString &filePath) override {
        revealedFiles.push_back(filePath);
        return revealError;
    }

    QWidget *chooseParent = nullptr;
    QString chooseInitialDirectory;
    QString chosenDirectory;
    QStringList openedDirectories;
    QStringList revealedFiles;
    QString openError;
    QString revealError;
};
}

class SettingsDialogTest : public QObject {
    Q_OBJECT

private slots:
    void summaryAppearsAboveEverySettingsGroup() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        auto *layout = qobject_cast<QVBoxLayout *>(dialog.layout());
        QVERIFY(summary != nullptr);
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

        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                                 QKeySequence("Ctrl+Shift+T"),
                                 SettingsFieldStatus::ApplyFailedRolledBack,
                                 "already registered"}};
        dialog.presentApplyOutcome(outcome);

        auto *status = qobject_cast<QLabel *>(table->cellWidget(3, 2));
        QVERIFY(status != nullptr);
        QVERIFY(status->wordWrap());
        QVERIFY(status->text().contains("Toggle toolbar"));
        QVERIFY(status->text().contains("Ctrl+Shift+T"));
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
        QVERIFY(summary->text().contains("1 setting has unapplied changes."));
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
                 QString("Some settings were applied. 2 settings were not applied; correct the highlighted fields."));

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

    void listsAllShortcutActions() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 7);
    }

    void showsRecordingSettingsInRequiredGroupOrder() {
        SettingsDialog dialog(AppSettings::defaults());

        QStringList groupTitles;
        auto *layout = qobject_cast<QVBoxLayout *>(dialog.layout());
        QVERIFY(layout != nullptr);
        for (int index = 0; index < layout->count(); ++index) {
            if (auto *group = qobject_cast<QGroupBox *>(layout->itemAt(index)->widget())) {
                groupTitles.push_back(group->title());
            }
        }

        QCOMPARE(groupTitles,
                 QStringList({"General", "Video", "Recording", "Hotkey Binding"}));
        QVERIFY(dialog.findChild<QGroupBox *>("recordingSettingsGroup") != nullptr);
        QVERIFY(dialog.findChild<QComboBox *>("recordingFormatCombo") != nullptr);
        QVERIFY(dialog.findChild<QLineEdit *>("recordingOutputDirectoryEdit") != nullptr);
        QVERIFY(dialog.findChild<QPushButton *>("chooseRecordingDirectoryButton") != nullptr);
        QVERIFY(dialog.findChild<QPushButton *>("openRecordingDirectoryButton") != nullptr);
        QVERIFY(dialog.findChild<QCheckBox *>("showRecordingCompletionMessageCheckBox") != nullptr);
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
        QVERIFY(chooseError->text().contains("1 setting has unapplied changes."));
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
        QVERIFY(error->text().contains("1 setting was not applied"));

        open->click();
        QVERIFY(error->text().contains("1 setting was not applied"));
        QVERIFY(error->text().contains(actions.openError));

        actions.openError.clear();
        open->click();
        QVERIFY(error->text().contains("1 setting was not applied"));
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
        QVERIFY(error->text().contains("1 setting was not applied"));
        QVERIFY(error->text().contains(actions.openError));

        choose->click();
        QVERIFY(error->text().contains("1 setting was not applied"));
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
