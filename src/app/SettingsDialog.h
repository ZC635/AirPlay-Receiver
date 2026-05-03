#pragma once

#include "app/AppSettings.h"

#include <QDialog>

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings &settings, QWidget *parent = nullptr);
};
