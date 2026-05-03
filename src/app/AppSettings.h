#pragma once

#include <QHash>
#include <QKeySequence>
#include <QStringList>
#include <QVector>
#include "app/ShortcutAction.h"
#include "app/ShortcutBinding.h"

class AppKeySequence : public QKeySequence {
public:
    AppKeySequence() = default;
    AppKeySequence(const QKeySequence &sequence) : QKeySequence(sequence) {}

    bool isValid() const;
};

class AppSettings {
public:
    static AppSettings defaults();

    QVector<ShortcutBinding> shortcuts() const;
    AppKeySequence shortcutFor(ShortcutAction action) const;
    void setShortcut(ShortcutAction action, const QKeySequence &sequence);
    QStringList validateShortcuts() const;

private:
    QHash<int, QKeySequence> shortcuts_;
};
