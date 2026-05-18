#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

struct WindowStateSnapshot {
    QByteArray geometry;
    QByteArray state;
};

class WindowStateStore {
public:
    explicit WindowStateStore(QString path);

    std::optional<WindowStateSnapshot> load() const;
    bool save(const WindowStateSnapshot &snapshot) const;

private:
    QString path_;
};
