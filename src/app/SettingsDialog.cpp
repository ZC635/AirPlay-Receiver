#include "app/SettingsDialog.h"
#include "app/ShortcutActionKey.h"

#include "platform/RecordingPathActions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

struct ShortcutRow {
    ShortcutAction action;
    const char *label;
};

constexpr ShortcutRow kShortcutRows[] = {
    {ShortcutAction::ToggleAlwaysOnTop, "Toggle always on top"},
    {ShortcutAction::VolumeUp, "Volume up"},
    {ShortcutAction::VolumeDown, "Volume down"},
    {ShortcutAction::ToggleToolbar, "Toggle toolbar"},
    {ShortcutAction::ToggleAspectRatio, "Toggle aspect ratio"},
    {ShortcutAction::ToggleVideoFit, "Toggle video fit"},
    {ShortcutAction::ToggleRecording, "Toggle recording"},
};

QString normalizedAbsolutePath(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QLabel *errorLabel(QWidget *parent, const QString &objectName) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setStyleSheet("color: #b00020;");
    label->setWordWrap(true);
    label->hide();
    return label;
}

} // namespace

SettingsDialog::SettingsDialog(const AppSettings &settings,
                               QWidget *parent,
                               RecordingPathActions *recordingPathActions)
    : QDialog(parent),
      committedBaseline_(settings),
      receiverNameEdit_(new QLineEdit(settings.receiverName(), this)),
      table_(new QTableWidget(this)),
      summaryLabel_(errorLabel(this, "settingsApplySummary")),
      ownedRecordingPathActions_(recordingPathActions == nullptr
                                     ? std::make_unique<WindowsRecordingPathActions>()
                                     : nullptr),
      recordingPathActions_(recordingPathActions != nullptr
                                ? recordingPathActions
                                : ownedRecordingPathActions_.get()) {
    setWindowTitle("Settings");

    auto *generalGroup = new QGroupBox("General", this);
    generalGroup->setObjectName("generalSettingsGroup");
    receiverNameEdit_->setObjectName("receiverNameEdit");
    auto *generalLayout = new QFormLayout(generalGroup);
    generalLayout->addRow("Receiver name", receiverNameEdit_);
    receiverNameError_ = errorLabel(generalGroup, "receiverNameError");
    generalLayout->addRow(QString(), receiverNameError_);

    auto *videoGroup = new QGroupBox("Video", this);
    videoGroup->setObjectName("videoSettingsGroup");
    const VideoQualitySettings &quality = committedBaseline_.videoQuality();
    videoResolutionCombo_ = new QComboBox(videoGroup);
    videoResolutionCombo_->setObjectName("videoResolutionCombo");
    videoResolutionCombo_->addItem("540p", static_cast<int>(VideoResolution::P540));
    videoResolutionCombo_->addItem("720p", static_cast<int>(VideoResolution::P720));
    videoResolutionCombo_->addItem("1080p", static_cast<int>(VideoResolution::P1080));
    videoResolutionCombo_->setCurrentIndex(
        videoResolutionCombo_->findData(static_cast<int>(quality.resolution)));
    videoFrameRateCombo_ = new QComboBox(videoGroup);
    videoFrameRateCombo_->setObjectName("videoFrameRateCombo");
    videoFrameRateCombo_->addItem("15 fps", static_cast<int>(VideoFrameRate::Fps15));
    videoFrameRateCombo_->addItem("30 fps", static_cast<int>(VideoFrameRate::Fps30));
    videoFrameRateCombo_->addItem("60 fps", static_cast<int>(VideoFrameRate::Fps60));
    videoFrameRateCombo_->setCurrentIndex(
        videoFrameRateCombo_->findData(static_cast<int>(quality.frameRate)));
    auto *videoLayout = new QFormLayout(videoGroup);
    videoLayout->addRow("Resolution", videoResolutionCombo_);
    videoResolutionError_ = errorLabel(videoGroup, "videoResolutionError");
    videoLayout->addRow(QString(), videoResolutionError_);
    videoLayout->addRow("Frame rate", videoFrameRateCombo_);
    videoFrameRateError_ = errorLabel(videoGroup, "videoFrameRateError");
    videoLayout->addRow(QString(), videoFrameRateError_);

    auto *recordingGroup = new QGroupBox("Recording", this);
    recordingGroup->setObjectName("recordingSettingsGroup");
    recordingFormatCombo_ = new QComboBox(recordingGroup);
    recordingFormatCombo_->setObjectName("recordingFormatCombo");
    recordingFormatCombo_->addItem("MP4", static_cast<int>(RecordingFormat::Mp4));
    recordingFormatCombo_->setCurrentIndex(recordingFormatCombo_->findData(
        static_cast<int>(committedBaseline_.recordingFormat())));
    recordingOutputDirectoryEdit_ = new QLineEdit(recordingGroup);
    recordingOutputDirectoryEdit_->setObjectName("recordingOutputDirectoryEdit");
    recordingOutputDirectoryEdit_->setReadOnly(true);
    recordingOutputDirectoryEdit_->setText(
        QDir::toNativeSeparators(committedBaseline_.recordingOutputDirectory()));
    auto *chooseDirectoryButton = new QPushButton("Choose...", recordingGroup);
    chooseDirectoryButton->setObjectName("chooseRecordingDirectoryButton");
    auto *openDirectoryButton = new QPushButton("Open", recordingGroup);
    openDirectoryButton->setObjectName("openRecordingDirectoryButton");
    auto *directoryLayout = new QHBoxLayout;
    directoryLayout->addWidget(recordingOutputDirectoryEdit_);
    directoryLayout->addWidget(chooseDirectoryButton);
    directoryLayout->addWidget(openDirectoryButton);
    showRecordingCompletionMessageCheckBox_ = new QCheckBox(
        "Show a message when recording completes", recordingGroup);
    showRecordingCompletionMessageCheckBox_->setObjectName("showRecordingCompletionMessageCheckBox");
    showRecordingCompletionMessageCheckBox_->setChecked(
        committedBaseline_.showRecordingCompletionMessage());
    auto *recordingLayout = new QFormLayout(recordingGroup);
    recordingLayout->addRow("Format", recordingFormatCombo_);
    recordingLayout->addRow("Output folder", directoryLayout);
    recordingLayout->addRow(showRecordingCompletionMessageCheckBox_);

    auto *hotkeyGroup = new QGroupBox("Hotkey Binding", this);
    hotkeyGroup->setObjectName("hotkeyBindingGroup");
    table_->setObjectName("shortcutTable");
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({"Action", "Shortcut", "Status"});
    table_->setColumnHidden(2, true);
    table_->setRowCount(static_cast<int>(std::size(kShortcutRows)));
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    for (int row = 0; row < static_cast<int>(std::size(kShortcutRows)); ++row) {
        const ShortcutRow &shortcutRow = kShortcutRows[row];
        auto *action = new QTableWidgetItem(shortcutRow.label);
        action->setFlags(action->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, 0, action);
        auto *edit = new QKeySequenceEdit(
            committedBaseline_.shortcutFor(shortcutRow.action), table_);
        edit->setObjectName(QString("shortcutEdit_%1").arg(shortcutActionKey(shortcutRow.action)));
        table_->setCellWidget(row, 1, edit);
        shortcutEdits_.insert(static_cast<int>(shortcutRow.action), edit);
        auto *status = errorLabel(table_,
                                  QString("shortcutError_%1").arg(shortcutActionKey(shortcutRow.action)));
        table_->setCellWidget(row, 2, status);
        shortcutErrorLabels_.insert(static_cast<int>(shortcutRow.action), status);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, action = shortcutRow.action]() {
            clearFieldResult(SettingsFieldId::shortcut(action));
        });
    }
    table_->horizontalHeader()->setStretchLastSection(true);
    auto *resetButton = new QPushButton("Reset to Defaults", hotkeyGroup);
    resetButton->setObjectName("resetHotkeysButton");
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        const AppSettings defaults = AppSettings::defaults();
        for (const ShortcutRow &shortcutRow : kShortcutRows) {
            shortcutEdits_.value(static_cast<int>(shortcutRow.action))->setKeySequence(
                defaults.shortcutFor(shortcutRow.action));
        }
    });
    auto *resetLayout = new QHBoxLayout;
    resetLayout->addStretch();
    resetLayout->addWidget(resetButton);
    auto *hotkeyLayout = new QVBoxLayout(hotkeyGroup);
    hotkeyLayout->addWidget(table_);
    hotkeyLayout->addLayout(resetLayout);

    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(QDialogButtonBox::Cancel);
    buttons->addButton("Apply", QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(summaryLabel_);
    layout->addWidget(generalGroup);
    layout->addWidget(videoGroup);
    layout->addWidget(recordingGroup);
    layout->addWidget(hotkeyGroup);
    layout->addWidget(buttons);

    connect(receiverNameEdit_, &QLineEdit::textChanged, this, [this] {
        clearFieldResult(SettingsFieldId::receiverName());
    });
    connect(videoResolutionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        clearFieldResult(SettingsFieldId::videoResolution());
    });
    connect(videoFrameRateCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        clearFieldResult(SettingsFieldId::videoFrameRate());
    });
    connect(recordingFormatCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        clearFieldResult(SettingsFieldId::recordingFormat());
    });
    connect(recordingOutputDirectoryEdit_, &QLineEdit::textChanged, this, [this] {
        clearFieldResult(SettingsFieldId::recordingOutputDirectory());
    });
    connect(showRecordingCompletionMessageCheckBox_, &QCheckBox::toggled, this, [this] {
        clearFieldResult(SettingsFieldId::recordingCompletionNotification());
    });
    connect(chooseDirectoryButton, &QPushButton::clicked, this, [this] {
        if (recordingPathActions_ == nullptr) {
            return;
        }
        const QString chosen = recordingPathActions_->chooseExistingDirectory(
            this, QDir::fromNativeSeparators(recordingOutputDirectoryEdit_->text()));
        if (!chosen.isEmpty()) {
            recordingOutputDirectoryEdit_->setText(
                QDir::toNativeSeparators(normalizedAbsolutePath(chosen)));
            pathActionError_.clear();
            refreshPresentation();
        }
    });
    connect(openDirectoryButton, &QPushButton::clicked, this, [this] {
        if (recordingPathActions_ == nullptr) {
            return;
        }
        pathActionError_ = recordingPathActions_->ensureAndOpenDirectory(normalizedAbsolutePath(
            QDir::fromNativeSeparators(recordingOutputDirectoryEdit_->text())));
        refreshPresentation();
    });
}

SettingsDialog::~SettingsDialog() = default;

AppSettings SettingsDialog::settings() const {
    return draftSettings();
}

AppSettings SettingsDialog::draftSettings() const {
    AppSettings draft = committedBaseline_;
    draft.setReceiverName(receiverNameEdit_->text());
    for (const ShortcutRow &shortcutRow : kShortcutRows) {
        draft.setShortcut(shortcutRow.action,
                          shortcutEdits_.value(static_cast<int>(shortcutRow.action))->keySequence());
    }
    VideoQualitySettings quality;
    quality.resolution = static_cast<VideoResolution>(videoResolutionCombo_->currentData().toInt());
    quality.frameRate = static_cast<VideoFrameRate>(videoFrameRateCombo_->currentData().toInt());
    draft.setVideoQuality(quality);
    draft.setRecordingFormat(static_cast<RecordingFormat>(recordingFormatCombo_->currentData().toInt()));
    draft.setRecordingOutputDirectory(normalizedAbsolutePath(
        QDir::fromNativeSeparators(recordingOutputDirectoryEdit_->text())));
    draft.setShowRecordingCompletionMessage(showRecordingCompletionMessageCheckBox_->isChecked());
    return draft;
}

const AppSettings &SettingsDialog::committedBaseline() const {
    return committedBaseline_;
}

void SettingsDialog::presentApplyOutcome(const SettingsApplyOutcome &outcome) {
    committedBaseline_ = outcome.committedSettings;
    fieldResults_ = outcome.fieldResults;
    globalResult_ = outcome.globalResult;
    refreshPresentation();
    if (outcome.mayClose) {
        QDialog::accept();
    }
}

void SettingsDialog::clearFieldResult(const SettingsFieldId &field) {
    const auto end = std::remove_if(fieldResults_.begin(), fieldResults_.end(), [&field](const SettingsFieldResult &result) {
        return result.field == field;
    });
    if (end != fieldResults_.end()) {
        fieldResults_.erase(end, fieldResults_.end());
    }
    refreshPresentation();
}

int SettingsDialog::unappliedChangeCount() const {
    const AppSettings draft = draftSettings();
    int changes = 0;
    for (const SettingsFieldId &field : allSettingsFields()) {
        if (settingsFieldValue(draft, field) != settingsFieldValue(committedBaseline_, field)) {
            ++changes;
        }
    }
    return changes;
}

QString SettingsDialog::fieldFailureMessage(const SettingsFieldResult &result) const {
    QString message = QString("%1 (%2): %3")
                          .arg(settingsFieldDisplayName(result.field),
                               formatSettingsFieldValue(result.attemptedValue),
                               result.reason);
    if (result.nativeErrorCode.has_value()) {
        message += QString(" (native error %1)").arg(*result.nativeErrorCode);
    }
    if (!result.recoveryError.isEmpty()) {
        message += QString(" Recovery failed: %1").arg(result.recoveryError);
    } else if (result.status == SettingsFieldStatus::ApplyFailedRolledBack) {
        message += " Previous setting was restored.";
    } else if (result.status == SettingsFieldStatus::RecoveryFailed) {
        message += " Recovery could not be confirmed.";
    }
    return message;
}

void SettingsDialog::refreshFieldErrors() {
    receiverNameError_->hide();
    videoResolutionError_->hide();
    videoFrameRateError_->hide();
    for (QLabel *label : shortcutErrorLabels_) {
        label->clear();
        label->hide();
    }
    bool showShortcutStatus = false;
    for (const SettingsFieldResult &result : fieldResults_) {
        if (!isFailureStatus(result.status)
            || (globalResult_.has_value() && result.status != SettingsFieldStatus::RecoveryFailed)) {
            continue;
        }
        QLabel *label = nullptr;
        switch (result.field.kind) {
        case SettingsFieldKind::ReceiverName: label = receiverNameError_; break;
        case SettingsFieldKind::VideoResolution: label = videoResolutionError_; break;
        case SettingsFieldKind::VideoFrameRate: label = videoFrameRateError_; break;
        case SettingsFieldKind::Shortcut:
            if (result.field.shortcutAction.has_value()) {
                label = shortcutErrorLabels_.value(static_cast<int>(*result.field.shortcutAction), nullptr);
            }
            break;
        default: break;
        }
        if (label != nullptr) {
            label->setText(fieldFailureMessage(result));
            label->show();
            showShortcutStatus = showShortcutStatus || result.field.kind == SettingsFieldKind::Shortcut;
        }
    }
    table_->setColumnHidden(2, !showShortcutStatus);
}

void SettingsDialog::refreshPresentation() {
    refreshFieldErrors();
    QStringList lines;
    if (globalResult_.has_value()) {
        const AppSettingsSaveResult &persistence = globalResult_->persistence;
        QString reason = persistence.errorString;
        if (!reason.endsWith('.')) {
            reason += '.';
        }
        lines.push_back(QString("Could not save %1: %2 No changes from this Apply were committed.")
                            .arg(persistence.targetPath, reason));
    }
    if (globalResult_.has_value()) {
        int recoveryFailures = 0;
        for (const SettingsFieldResult &result : fieldResults_) {
            recoveryFailures += result.status == SettingsFieldStatus::RecoveryFailed ? 1 : 0;
        }
        if (recoveryFailures > 0) {
            lines.push_back(recoveryFailures == 1
                ? "Recovery failure requires attention; the previous setting could not be confirmed."
                : QString("%1 recovery failures require attention; previous settings could not be confirmed.")
                      .arg(recoveryFailures));
        }
    } else {
        int failures = 0;
        bool appliedSomething = false;
        for (const SettingsFieldResult &result : fieldResults_) {
            failures += isFailureStatus(result.status) ? 1 : 0;
            appliedSomething = appliedSomething || result.status == SettingsFieldStatus::Applied
                || result.status == SettingsFieldStatus::Deferred;
        }
        if (failures > 0) {
            const QString noun = failures == 1 ? "setting was" : "settings were";
            QString line = QString("%1 %2 not applied; correct the highlighted fields.")
                               .arg(failures).arg(noun);
            if (appliedSomething) {
                line.prepend("Some settings were applied. ");
            }
            lines.push_back(line);
        } else if (const int changes = unappliedChangeCount(); changes > 0) {
            lines.push_back(changes == 1 ? "1 setting has unapplied changes."
                                        : QString("%1 settings have unapplied changes.").arg(changes));
        }
    }
    if (!pathActionError_.isEmpty()) {
        lines.push_back(pathActionError_);
    }
    if (lines.isEmpty()) {
        summaryLabel_->clear();
        summaryLabel_->hide();
    } else {
        summaryLabel_->setText(lines.join('\n'));
        summaryLabel_->show();
    }
}

void SettingsDialog::accept() {
    emit applyRequested(draftSettings());
}
