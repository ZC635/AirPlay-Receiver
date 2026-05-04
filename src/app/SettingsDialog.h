#pragma once

#include "app/AppSettings.h"

#include <QDialog>
#include <QHash>

class QLabel;
class QKeySequenceEdit;
class QLineEdit;
class QTableWidget;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings &settings, QWidget *parent = nullptr);
    AppSettings settings() const;

public slots:
    void accept() override;

private:
    AppSettings settings_;
    QLineEdit *receiverNameEdit_;
    QTableWidget *table_;
    QLabel *errorLabel_;
    QHash<int, QKeySequenceEdit *> shortcutEdits_;
};
