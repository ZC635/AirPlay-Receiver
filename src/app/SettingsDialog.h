#pragma once

#include "app/SettingsApplyTypes.h"

#include <QDialog>
#include <QHash>

#include <memory>

class QComboBox;
class QCheckBox;
class QEvent;
class QLabel;
class QKeySequenceEdit;
class QLineEdit;
class QPushButton;
class QGroupBox;
class QDialogButtonBox;
class QTableWidgetItem;
class QTableWidget;
class RecordingPathActions;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings &settings,
                            QWidget *parent = nullptr,
                            RecordingPathActions *recordingPathActions = nullptr);
    ~SettingsDialog() override;

    AppSettings settings() const;
    AppSettings draftSettings() const;
    const AppSettings &committedBaseline() const;
    void presentApplyOutcome(const SettingsApplyOutcome &outcome);
    bool hasUnappliedChanges() const;
    void presentDiagnosticActionError(UiMessage error);

signals:
    void applyRequested(AppSettings draft);
    void restartWithDiagnosticLoggingRequested();
    void openDiagnosticLogFolderRequested();

public slots:
    void accept() override;

protected:
    void changeEvent(QEvent *event) override;

private:
    void clearFieldResult(const SettingsFieldId &field);
    void retranslateUi();
    void repopulateLanguageCombo();
    void refreshPresentation();
    void refreshFieldErrors();
    int unappliedChangeCount() const;
    QString fieldFailureMessage(const SettingsFieldResult &result) const;

    AppSettings committedBaseline_;
    QLineEdit *receiverNameEdit_;
    QComboBox *languageCombo_;
    QTableWidget *table_;
    QGroupBox *generalGroup_;
    QGroupBox *videoGroup_;
    QGroupBox *recordingGroup_;
    QGroupBox *hotkeyGroup_;
    QGroupBox *diagnosticsGroup_;
    QLabel *summaryLabel_;
    QLabel *receiverNameLabel_;
    QLabel *languageLabel_;
    QLabel *videoResolutionLabel_;
    QLabel *videoFrameRateLabel_;
    QLabel *recordingFormatLabel_;
    QLabel *recordingOutputFolderLabel_;
    QLabel *receiverNameError_;
    QLabel *videoResolutionError_;
    QLabel *videoFrameRateError_;
    QHash<int, QKeySequenceEdit *> shortcutEdits_;
    QHash<int, QTableWidgetItem *> shortcutActionItems_;
    QHash<int, QLabel *> shortcutErrorLabels_;
    QComboBox *videoResolutionCombo_;
    QComboBox *videoFrameRateCombo_;
    QComboBox *recordingFormatCombo_;
    QLineEdit *recordingOutputDirectoryEdit_;
    QCheckBox *showRecordingCompletionMessageCheckBox_;
    QCheckBox *toolbarHoverRevealCheckBox_;
    QPushButton *chooseDirectoryButton_;
    QPushButton *openDirectoryButton_;
    QPushButton *resetButton_;
    QPushButton *restartWithDiagnosticLoggingButton_;
    QPushButton *openDiagnosticLogFolderButton_;
    QDialogButtonBox *buttons_;
    QPushButton *cancelButton_;
    QPushButton *applyButton_;
    std::unique_ptr<RecordingPathActions> ownedRecordingPathActions_;
    RecordingPathActions *recordingPathActions_;
    QVector<SettingsFieldResult> fieldResults_;
    std::optional<SettingsApplyGlobalResult> globalResult_;
    UiMessage pathActionError_;
    UiMessage diagnosticActionError_;
};
