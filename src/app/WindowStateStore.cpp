#include "app/WindowStateStore.h"

#include <QFile>
#include <QSaveFile>
#include <QDataStream>

#include <utility>

namespace {
constexpr quint32 kMagic = 0x41505753; // APWS
constexpr quint16 kVersion = 1;
}

WindowStateStore::WindowStateStore(QString path)
    : path_(std::move(path)) {}

std::optional<WindowStateSnapshot> WindowStateStore::load() const {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint16 version = 0;
    QByteArray geometry;
    QByteArray state;
    stream >> magic >> version >> geometry >> state;
    if (stream.status() != QDataStream::Ok || magic != kMagic || version != kVersion || geometry.isEmpty()) {
        return std::nullopt;
    }

    return WindowStateSnapshot{geometry, state};
}

bool WindowStateStore::save(const WindowStateSnapshot &snapshot) const {
    if (snapshot.geometry.isEmpty()) {
        return false;
    }

    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kMagic << kVersion << snapshot.geometry << snapshot.state;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    return file.commit();
}
