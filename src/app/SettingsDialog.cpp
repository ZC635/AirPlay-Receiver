#include "app/SettingsDialog.h"
#include "app/LanguageManager.h"
#include "app/ShortcutActionKey.h"
#include "app/ShortcutActionText.h"

#include "platform/RecordingPathActions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace {

class SettingsComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *event) override {
        event->ignore();
    }
};

struct ShortcutRow {
    ShortcutAction action;
};

constexpr ShortcutRow kShortcutRows[] = {
    {ShortcutAction::ToggleAlwaysOnTop},
    {ShortcutAction::VolumeUp},
    {ShortcutAction::VolumeDown},
    {ShortcutAction::ToggleToolbar},
    {ShortcutAction::ToggleAspectRatio},
    {ShortcutAction::ToggleVideoFit},
    {ShortcutAction::ToggleRecording},
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
      languageCombo_(new SettingsComboBox(this)),
      table_(new QTableWidget(this)),
      summaryLabel_(errorLabel(this, "settingsApplySummary")),
      ownedRecordingPathActions_(recordingPathActions == nullptr
                                     ? std::make_unique<WindowsRecordingPathActions>()
                                     : nullptr),
      recordingPathActions_(recordingPathActions != nullptr
                                ? recordingPathActions
                                : ownedRecordingPathActions_.get()) {
    generalGroup_ = new QGroupBox(this);
    generalGroup_->setObjectName("generalSettingsGroup");
    receiverNameEdit_->setObjectName("receiverNameEdit");
    receiverNameLabel_ = new QLabel(generalGroup_);
    receiverNameLabel_->setObjectName("receiverNameLabel");
    auto *generalLayout = new QFormLayout(generalGroup_);
    generalLayout->addRow(receiverNameLabel_, receiverNameEdit_);
    receiverNameError_ = errorLabel(generalGroup_, "receiverNameError");
    generalLayout->addRow(QString(), receiverNameError_);
    languageLabel_ = new QLabel(generalGroup_);
    languageLabel_->setObjectName("languageLabel");
    languageCombo_->setParent(generalGroup_);
    languageCombo_->setObjectName("languageCombo");
    generalLayout->addRow(languageLabel_, languageCombo_);

    videoGroup_ = new QGroupBox(this);
    videoGroup_->setObjectName("videoSettingsGroup");
    const VideoQualitySettings &quality = committedBaseline_.videoQuality();
    videoResolutionCombo_ = new SettingsComboBox(videoGroup_);
    videoResolutionCombo_->setObjectName("videoResolutionCombo");
    videoResolutionCombo_->addItem("540p", static_cast<int>(VideoResolution::P540));
    videoResolutionCombo_->addItem("720p", static_cast<int>(VideoResolution::P720));
    videoResolutionCombo_->addItem("1080p", static_cast<int>(VideoResolution::P1080));
    videoResolutionCombo_->setCurrentIndex(
        videoResolutionCombo_->findData(static_cast<int>(quality.resolution)));
    videoFrameRateCombo_ = new SettingsComboBox(videoGroup_);
    videoFrameRateCombo_->setObjectName("videoFrameRateCombo");
    videoFrameRateCombo_->addItem("15 fps", static_cast<int>(VideoFrameRate::Fps15));
    videoFrameRateCombo_->addItem("30 fps", static_cast<int>(VideoFrameRate::Fps30));
    videoFrameRateCombo_->addItem("60 fps", static_cast<int>(VideoFrameRate::Fps60));
    videoFrameRateCombo_->setCurrentIndex(
        videoFrameRateCombo_->findData(static_cast<int>(quality.frameRate)));
    auto *videoLayout = new QFormLayout(videoGroup_);
    videoResolutionLabel_ = new QLabel(videoGroup_);
    videoResolutionLabel_->setObjectName("videoResolutionLabel");
    videoLayout->addRow(videoResolutionLabel_, videoResolutionCombo_);
    videoResolutionError_ = errorLabel(videoGroup_, "videoResolutionError");
    videoLayout->addRow(QString(), videoResolutionError_);
    videoFrameRateLabel_ = new QLabel(videoGroup_);
    videoFrameRateLabel_->setObjectName("videoFrameRateLabel");
    videoLayout->addRow(videoFrameRateLabel_, videoFrameRateCombo_);
    videoFrameRateError_ = errorLabel(videoGroup_, "videoFrameRateError");
    videoLayout->addRow(QString(), videoFrameRateError_);

    recordingGroup_ = new QGroupBox(this);
    recordingGroup_->setObjectName("recordingSettingsGroup");
    recordingFormatCombo_ = new SettingsComboBox(recordingGroup_);
    recordingFormatCombo_->setObjectName("recordingFormatCombo");
    recordingFormatCombo_->addItem("MP4", static_cast<int>(RecordingFormat::Mp4));
    recordingFormatCombo_->setCurrentIndex(recordingFormatCombo_->findData(
        static_cast<int>(committedBaseline_.recordingFormat())));
    recordingOutputDirectoryEdit_ = new QLineEdit(recordingGroup_);
    recordingOutputDirectoryEdit_->setObjectName("recordingOutputDirectoryEdit");
    recordingOutputDirectoryEdit_->setReadOnly(true);
    recordingOutputDirectoryEdit_->setText(
        QDir::toNativeSeparators(committedBaseline_.recordingOutputDirectory()));
    chooseDirectoryButton_ = new QPushButton(recordingGroup_);
    chooseDirectoryButton_->setObjectName("chooseRecordingDirectoryButton");
    openDirectoryButton_ = new QPushButton(recordingGroup_);
    openDirectoryButton_->setObjectName("openRecordingDirectoryButton");
    auto *directoryLayout = new QHBoxLayout;
    directoryLayout->addWidget(recordingOutputDirectoryEdit_);
    directoryLayout->addWidget(chooseDirectoryButton_);
    directoryLayout->addWidget(openDirectoryButton_);
    showRecordingCompletionMessageCheckBox_ = new QCheckBox(
        recordingGroup_);
    showRecordingCompletionMessageCheckBox_->setObjectName("showRecordingCompletionMessageCheckBox");
    showRecordingCompletionMessageCheckBox_->setChecked(
        committedBaseline_.showRecordingCompletionMessage());
    auto *recordingLayout = new QFormLayout(recordingGroup_);
    recordingFormatLabel_ = new QLabel(recordingGroup_);
    recordingFormatLabel_->setObjectName("recordingFormatLabel");
    recordingLayout->addRow(recordingFormatLabel_, recordingFormatCombo_);
    recordingOutputFolderLabel_ = new QLabel(recordingGroup_);
    recordingOutputFolderLabel_->setObjectName("recordingOutputFolderLabel");
    recordingLayout->addRow(recordingOutputFolderLabel_, directoryLayout);
    recordingLayout->addRow(showRecordingCompletionMessageCheckBox_);

    hotkeyGroup_ = new QGroupBox(this);
    hotkeyGroup_->setObjectName("hotkeyBindingGroup");
    table_->setObjectName("shortcutTable");
    table_->setColumnCount(3);
    for (int column = 0; column < table_->columnCount(); ++column) {
        table_->setHorizontalHeaderItem(column, new QTableWidgetItem);
    }
    table_->setColumnHidden(2, true);
    table_->setRowCount(static_cast<int>(std::size(kShortcutRows)));
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    for (int row = 0; row < static_cast<int>(std::size(kShortcutRows)); ++row) {
        const ShortcutRow &shortcutRow = kShortcutRows[row];
        auto *action = new QTableWidgetItem;
        action->setFlags(action->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, 0, action);
        shortcutActionItems_.insert(static_cast<int>(shortcutRow.action), action);
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
    resetButton_ = new QPushButton(hotkeyGroup_);
    resetButton_->setObjectName("resetHotkeysButton");
    connect(resetButton_, &QPushButton::clicked, this, [this]() {
        const AppSettings defaults = AppSettings::defaults();
        for (const ShortcutRow &shortcutRow : kShortcutRows) {
            shortcutEdits_.value(static_cast<int>(shortcutRow.action))->setKeySequence(
                defaults.shortcutFor(shortcutRow.action));
        }
    });
    auto *resetLayout = new QHBoxLayout;
    resetLayout->addStretch();
    resetLayout->addWidget(resetButton_);
    auto *hotkeyLayout = new QVBoxLayout(hotkeyGroup_);
    hotkeyLayout->addWidget(table_);
    hotkeyLayout->addLayout(resetLayout);

    diagnosticsGroup_ = new QGroupBox(this);
    diagnosticsGroup_->setObjectName("diagnosticsSettingsGroup");
    restartWithDiagnosticLoggingButton_ = new QPushButton(diagnosticsGroup_);
    restartWithDiagnosticLoggingButton_->setObjectName("restartWithDiagnosticLoggingButton");
    openDiagnosticLogFolderButton_ = new QPushButton(diagnosticsGroup_);
    openDiagnosticLogFolderButton_->setObjectName("openDiagnosticLogFolderButton");
    auto *diagnosticsLayout = new QVBoxLayout(diagnosticsGroup_);
    diagnosticsLayout->addWidget(restartWithDiagnosticLoggingButton_);
    diagnosticsLayout->addWidget(openDiagnosticLogFolderButton_);

    buttons_ = new QDialogButtonBox(this);
    cancelButton_ = buttons_->addButton(QString(), QDialogButtonBox::RejectRole);
    cancelButton_->setObjectName("cancelSettingsButton");
    applyButton_ = buttons_->addButton(QString(), QDialogButtonBox::AcceptRole);
    applyButton_->setObjectName("applySettingsButton");
    connect(buttons_, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->addWidget(summaryLabel_);
    contentLayout->addWidget(generalGroup_);
    contentLayout->addWidget(videoGroup_);
    contentLayout->addWidget(recordingGroup_);
    contentLayout->addWidget(hotkeyGroup_);
    contentLayout->addWidget(diagnosticsGroup_);
    contentLayout->addStretch();

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName("settingsContentScrollArea");
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidget(content);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(scrollArea, 1);
    layout->addWidget(buttons_);

    connect(receiverNameEdit_, &QLineEdit::textChanged, this, [this] {
        clearFieldResult(SettingsFieldId::receiverName());
    });
    connect(languageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        clearFieldResult(SettingsFieldId::language());
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
    connect(chooseDirectoryButton_, &QPushButton::clicked, this, [this] {
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
    connect(openDirectoryButton_, &QPushButton::clicked, this, [this] {
        if (recordingPathActions_ == nullptr) {
            return;
        }
        pathActionError_ = recordingPathActions_->ensureAndOpenDirectory(normalizedAbsolutePath(
            QDir::fromNativeSeparators(recordingOutputDirectoryEdit_->text())));
        refreshPresentation();
    });
    connect(restartWithDiagnosticLoggingButton_, &QPushButton::clicked, this, [this] {
        diagnosticActionError_.clear();
        refreshPresentation();
        emit restartWithDiagnosticLoggingRequested();
    });
    connect(openDiagnosticLogFolderButton_, &QPushButton::clicked, this, [this] {
        diagnosticActionError_.clear();
        refreshPresentation();
        emit openDiagnosticLogFolderRequested();
    });

    retranslateUi();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QDialog::changeEvent(event);
}

void SettingsDialog::retranslateUi() {
    setWindowTitle(tr("Settings"));
    generalGroup_->setTitle(tr("General"));
    videoGroup_->setTitle(tr("Video"));
    recordingGroup_->setTitle(tr("Recording"));
    hotkeyGroup_->setTitle(tr("Hotkey Binding"));
    diagnosticsGroup_->setTitle(tr("Diagnostics"));

    receiverNameLabel_->setText(tr("Receiver name"));
    languageLabel_->setText(tr("Language"));
    videoResolutionLabel_->setText(tr("Resolution"));
    videoFrameRateLabel_->setText(tr("Frame rate"));
    recordingFormatLabel_->setText(tr("Format"));
    recordingOutputFolderLabel_->setText(tr("Output folder"));
    showRecordingCompletionMessageCheckBox_->setText(tr("Show a message when recording completes"));

    chooseDirectoryButton_->setText(tr("Choose..."));
    openDirectoryButton_->setText(tr("Open"));
    resetButton_->setText(tr("Reset to Defaults"));
    restartWithDiagnosticLoggingButton_->setText(tr("Restart with Diagnostic Logging"));
    openDiagnosticLogFolderButton_->setText(tr("Open Log Folder"));
    cancelButton_->setText(tr("Cancel"));
    applyButton_->setText(tr("Apply"));

    table_->horizontalHeaderItem(0)->setText(tr("Action"));
    table_->horizontalHeaderItem(1)->setText(tr("Shortcut"));
    table_->horizontalHeaderItem(2)->setText(tr("Status"));
    for (const ShortcutRow &shortcutRow : kShortcutRows) {
        shortcutActionItems_.value(static_cast<int>(shortcutRow.action))->setText(
            shortcutActionDisplayName(shortcutRow.action));
    }

    repopulateLanguageCombo();
    refreshPresentation();
}

void SettingsDialog::repopulateLanguageCombo() {
    QString selected = languageCombo_->currentData().toString();
    if (selected.isEmpty()) {
        selected = committedBaseline_.language();
    }

    const QSignalBlocker blocker(languageCombo_);
    languageCombo_->clear();
    languageCombo_->addItem(tr("System Default"), QStringLiteral("system"));
    for (const LanguageOption &option : LanguageManager::supportedLanguages()) {
        languageCombo_->addItem(option.nativeName, option.id);
    }
    const int index = languageCombo_->findData(selected);
    languageCombo_->setCurrentIndex(index >= 0 ? index : languageCombo_->findData(QStringLiteral("system")));
}

AppSettings SettingsDialog::settings() const {
    return draftSettings();
}

AppSettings SettingsDialog::draftSettings() const {
    AppSettings draft = committedBaseline_;
    draft.setReceiverName(receiverNameEdit_->text());
    draft.setLanguage(languageCombo_->currentData().toString());
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

bool SettingsDialog::hasUnappliedChanges() const {
    return unappliedChangeCount() > 0;
}

void SettingsDialog::presentDiagnosticActionError(QString error) {
    diagnosticActionError_ = std::move(error);
    refreshPresentation();
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
    const QString reason = result.userReason.isEmpty() ? result.reason : result.userReason.render();
    const QString recoveryError = result.userRecoveryError.isEmpty()
        ? result.recoveryError : result.userRecoveryError.render();
    QString message = tr("%1 (%2): %3")
                          .arg(settingsFieldDisplayName(result.field),
                               formatSettingsFieldValue(result.attemptedValue),
                               reason);
    if (result.nativeErrorCode.has_value()) {
        message += tr(" (native error %1)").arg(*result.nativeErrorCode);
    }
    if (!result.userRecoveryError.isEmpty()) {
        message += QStringLiteral(" ") + recoveryError;
    } else if (!recoveryError.isEmpty()) {
        message += tr(" Recovery failed: %1").arg(recoveryError);
    } else if (result.status == SettingsFieldStatus::ApplyFailedRolledBack) {
        message += tr(" Previous setting was restored.");
    } else if (result.status == SettingsFieldStatus::RecoveryFailed) {
        message += tr(" Recovery could not be confirmed.");
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
    table_->resizeRowsToContents();
    constexpr int maximumVisibleRows = 10;
    int rowsHeight = 0;
    for (int row = 0; row < std::min(table_->rowCount(), maximumVisibleRows); ++row) {
        rowsHeight += table_->rowHeight(row);
    }
    table_->setVerticalScrollBarPolicy(table_->rowCount() > maximumVisibleRows
                                          ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    table_->setFixedHeight(table_->horizontalHeader()->height() + rowsHeight
                          + 2 * table_->frameWidth()
                          + table_->horizontalScrollBar()->sizeHint().height());
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
        lines.push_back(tr("Could not save %1: %2 No changes from this Apply were committed.")
                            .arg(persistence.targetPath, reason));
    }
    if (globalResult_.has_value()) {
        int recoveryFailures = 0;
        for (const SettingsFieldResult &result : fieldResults_) {
            recoveryFailures += result.status == SettingsFieldStatus::RecoveryFailed ? 1 : 0;
        }
        if (recoveryFailures > 0) {
            lines.push_back(tr("Recovery requires attention; issue count: %n. Previous settings could not be confirmed.",
                               nullptr, recoveryFailures));
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
            QString line = tr("Apply incomplete; setting count: %n. Correct the highlighted fields.",
                              nullptr, failures);
            if (appliedSomething) {
                line.prepend(tr("Some settings were applied. "));
            }
            lines.push_back(line);
        } else if (const int changes = unappliedChangeCount(); changes > 0) {
            lines.push_back(tr("Unapplied change count: %n.", nullptr, changes));
        }
    }
    if (!pathActionError_.isEmpty()) {
        lines.push_back(pathActionError_);
    }
    if (!diagnosticActionError_.isEmpty()) {
        lines.push_back(diagnosticActionError_);
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
