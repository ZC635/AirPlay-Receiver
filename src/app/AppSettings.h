#pragma once

#include <QHash>
#include <QKeySequence>
#include <QString>
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
    int volume() const;
    void setVolume(int value);
    QString receiverName() const;
    void setReceiverName(QString name);
    bool aspectRatioLock() const;
    void setAspectRatioLock(bool enabled);
    QStringList validateGeneral() const;
    QStringList validateShortcuts() const;

private:
    QHash<int, QKeySequence> shortcuts_;
    int volume_ = 100;
    QString receiverName_ = "AirPlay Receiver";
    bool aspectRatioLock_ = false;
};
