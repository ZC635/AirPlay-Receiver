#include "app/SettingsDialog.h"

#include "platform/WindowsHotkeyService.h"

#include <QDialogButtonBox>
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
};

QString keyFor(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToggleAlwaysOnTop: return "toggleAlwaysOnTop";
    case ShortcutAction::VolumeUp: return "volumeUp";
    case ShortcutAction::VolumeDown: return "volumeDown";
    case ShortcutAction::ToggleToolbar: return "toggleToolbar";
    case ShortcutAction::ToggleAspectRatio: return "toggleAspectRatio";
    case ShortcutAction::ToggleVideoFit: return "toggleVideoFit";
    }
    return {};
}
}

SettingsDialog::SettingsDialog(const AppSettings &settings, QWidget *parent)
    : QDialog(parent),
      settings_(settings),
      receiverNameEdit_(new QLineEdit(settings.receiverName(), this)),
      table_(new QTableWidget(this)),
      errorLabel_(new QLabel(this)) {
    setWindowTitle("Settings");

    auto *generalGroup = new QGroupBox("General", this);
    generalGroup->setObjectName("generalSettingsGroup");
    receiverNameEdit_->setObjectName("receiverNameEdit");

    auto *generalLayout = new QFormLayout(generalGroup);
    generalLayout->addRow("Receiver name", receiverNameEdit_);

    auto *hotkeyGroup = new QGroupBox("Hotkey Binding", this);
    hotkeyGroup->setObjectName("hotkeyBindingGroup");

    table_->setObjectName("shortcutTable");
    table_->setColumnCount(2);
    table_->setHorizontalHeaderLabels({"Action", "Shortcut"});
    table_->setRowCount(static_cast<int>(std::size(kShortcutRows)));
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);

    for (int row = 0; row < static_cast<int>(std::size(kShortcutRows)); ++row) {
        const ShortcutRow &shortcutRow = kShortcutRows[row];
        auto *labelItem = new QTableWidgetItem(shortcutRow.label);
        labelItem->setFlags(labelItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, 0, labelItem);

        auto *edit = new QKeySequenceEdit(settings.shortcutFor(shortcutRow.action), table_);
        edit->setObjectName(QString("shortcutEdit_%1").arg(keyFor(shortcutRow.action)));
        table_->setCellWidget(row, 1, edit);
        shortcutEdits_.insert(static_cast<int>(shortcutRow.action), edit);
    }

    table_->horizontalHeader()->setStretchLastSection(true);

    auto *resetButton = new QPushButton("Reset to Defaults", hotkeyGroup);
    resetButton->setObjectName("resetHotkeysButton");
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        const AppSettings defaults = AppSettings::defaults();
        for (const ShortcutRow &shortcutRow : kShortcutRows) {
            auto *edit = shortcutEdits_.value(static_cast<int>(shortcutRow.action), nullptr);
            if (edit != nullptr) {
                edit->setKeySequence(defaults.shortcutFor(shortcutRow.action));
            }
        }
    });

    auto *resetLayout = new QHBoxLayout;
    resetLayout->addStretch();
    resetLayout->addWidget(resetButton);

    auto *hotkeyLayout = new QVBoxLayout(hotkeyGroup);
    hotkeyLayout->addWidget(table_);
    hotkeyLayout->addLayout(resetLayout);

    errorLabel_->setObjectName("settingsErrorLabel");
    errorLabel_->setStyleSheet("color: #b00020;");
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();

    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(QDialogButtonBox::Cancel);
    buttons->addButton("Apply", QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(generalGroup);
    layout->addWidget(hotkeyGroup);
    layout->addWidget(errorLabel_);
    layout->addWidget(buttons);
}

AppSettings SettingsDialog::settings() const {
    return settings_;
}

void SettingsDialog::accept() {
    AppSettings candidate = settings_;
    candidate.setReceiverName(receiverNameEdit_->text());
    for (const ShortcutRow &shortcutRow : kShortcutRows) {
        auto *edit = shortcutEdits_.value(static_cast<int>(shortcutRow.action), nullptr);
        if (edit != nullptr) {
            candidate.setShortcut(shortcutRow.action, edit->keySequence());
        }
    }

    QStringList errors = candidate.validateGeneral();
    errors.append(candidate.validateShortcuts());
    for (const ShortcutRow &shortcutRow : kShortcutRows) {
        const QKeySequence sequence = candidate.shortcutFor(shortcutRow.action);
        if (sequence.count() > 1) {
            errors.push_back(QString("Shortcut for %1 must use a single key combination")
                                 .arg(shortcutRow.label));
            continue;
        }
        if (!sequence.isEmpty() && !WindowsHotkeyService::toNativeHotkey(sequence).has_value()) {
            errors.push_back(QString("Unsupported shortcut for %1: %2")
                                 .arg(shortcutRow.label, sequence.toString(QKeySequence::NativeText)));
        }
    }
    if (!errors.isEmpty()) {
        errorLabel_->setText(errors.join('\n'));
        errorLabel_->show();
        return;
    }

    errorLabel_->clear();
    errorLabel_->hide();
    settings_ = candidate;
    QDialog::accept();
}
