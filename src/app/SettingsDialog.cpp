#include "app/SettingsDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
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
};
}

SettingsDialog::SettingsDialog(const AppSettings &settings, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle("Settings");

    auto *table = new QTableWidget(this);
    table->setObjectName("shortcutTable");
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Action", "Shortcut"});
    table->setRowCount(static_cast<int>(std::size(kShortcutRows)));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);

    for (int row = 0; row < static_cast<int>(std::size(kShortcutRows)); ++row) {
        const ShortcutRow &shortcutRow = kShortcutRows[row];
        table->setItem(row, 0, new QTableWidgetItem(shortcutRow.label));
        table->setItem(row, 1, new QTableWidgetItem(settings.shortcutFor(shortcutRow.action).toString(QKeySequence::NativeText)));
    }

    table->horizontalHeader()->setStretchLastSection(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(table);
    layout->addWidget(buttons);
}
