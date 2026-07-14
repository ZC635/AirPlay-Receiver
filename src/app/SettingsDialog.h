#pragma once

#include "app/AppSettings.h"

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

public slots:
    void accept() override;

private:
    void refreshErrorLabel();

    AppSettings settings_;
    QLineEdit *receiverNameEdit_;
    QTableWidget *table_;
    QLabel *errorLabel_;
    QHash<int, QKeySequenceEdit *> shortcutEdits_;
    QComboBox *videoResolutionCombo_;
    QComboBox *videoFrameRateCombo_;
    QComboBox *recordingFormatCombo_;
    QLineEdit *recordingOutputDirectoryEdit_;
    QCheckBox *showRecordingCompletionMessageCheckBox_;
    std::unique_ptr<RecordingPathActions> ownedRecordingPathActions_;
    RecordingPathActions *recordingPathActions_;
    QString validationError_;
    QString pathActionError_;
};
