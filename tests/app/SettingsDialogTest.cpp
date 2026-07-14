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

        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
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
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(open != nullptr);
        QVERIFY(error != nullptr);

        open->click();

        QCOMPARE(error->text(), actions.openError);
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

        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
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

        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().receiverName(), QString("Desk Receiver"));
    }

    void rejectsEmptyReceiverNameOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(edit != nullptr);

        edit->setText("   ");
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("Receiver name"));
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
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
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
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleRecording),
                 QKeySequence("Ctrl+Shift+R"));
    }

    void exposesAcceptedSettings() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Shift+H"));
        dialog.accept();

        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
    }

    void rejectsDuplicateShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("Duplicate shortcut"));
    }

    void rejectsUnsupportedShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_NumLock)));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("Unsupported shortcut"));
    }

    void rejectsMultiStepShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+K, Ctrl+C"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("single key combination"));
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
