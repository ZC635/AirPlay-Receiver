#pragma once

#include "app/SettingsApplyTypes.h"

#include <QDialog>
#include <QHash>

#include <memory>

class QComboBox;
class QCheckBox;
class QLabel;
class QKeySequenceEdit;
class QLineEdit;
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
    void presentDiagnosticActionError(QString error);

signals:
    void applyRequested(AppSettings draft);
    void restartWithDiagnosticLoggingRequested();
    void openDiagnosticLogFolderRequested();

public slots:
    void accept() override;

private:
    void clearFieldResult(const SettingsFieldId &field);
    void refreshPresentation();
    void refreshFieldErrors();
    int unappliedChangeCount() const;
    QString fieldFailureMessage(const SettingsFieldResult &result) const;

    AppSettings committedBaseline_;
    QLineEdit *receiverNameEdit_;
    QTableWidget *table_;
    QLabel *summaryLabel_;
    QLabel *receiverNameError_;
    QLabel *videoResolutionError_;
    QLabel *videoFrameRateError_;
    QHash<int, QKeySequenceEdit *> shortcutEdits_;
    QHash<int, QLabel *> shortcutErrorLabels_;
    QComboBox *videoResolutionCombo_;
    QComboBox *videoFrameRateCombo_;
    QComboBox *recordingFormatCombo_;
    QLineEdit *recordingOutputDirectoryEdit_;
    QCheckBox *showRecordingCompletionMessageCheckBox_;
    std::unique_ptr<RecordingPathActions> ownedRecordingPathActions_;
    RecordingPathActions *recordingPathActions_;
    QVector<SettingsFieldResult> fieldResults_;
    std::optional<SettingsApplyGlobalResult> globalResult_;
    QString pathActionError_;
    QString diagnosticActionError_;
};
