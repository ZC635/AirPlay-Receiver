#pragma once
#include "platform/FileSystemPath.h"
#include <QDir>
#include <QFile>
#include <QString>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
class HiddenFileLease {
public:
    explicit HiddenFileLease(QString path)
        : m_path(std::move(path))
    {
    }

    bool makeVisible(QString *error)
    {
#ifdef Q_OS_WIN
        const QString ioPath = FileSystemPath::forIo(m_path);
        if (ioPath.isEmpty() || ioPath.contains(QChar(0))) {
            if (error) *error = QStringLiteral("Invalid owned placeholder path: %1").arg(m_path);
            return false;
        }
        const std::wstring nativePath = ioPath.toStdWString();
        m_originalAttributes = GetFileAttributesW(nativePath.c_str());
        if (m_originalAttributes == INVALID_FILE_ATTRIBUTES) {
            if (error) {
                *error = QStringLiteral("Could not read owned placeholder attributes for %1 (Windows error %2)")
                             .arg(m_path).arg(GetLastError());
            }
            return false;
        }
        DWORD visibleAttributes = m_originalAttributes & ~FILE_ATTRIBUTE_HIDDEN;
        if (visibleAttributes == 0) visibleAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(nativePath.c_str(), visibleAttributes)) {
            if (error) {
                *error = QStringLiteral("Could not make owned placeholder visible for writer %1 (Windows error %2)")
                             .arg(m_path).arg(GetLastError());
            }
            return false;
        }
        m_changed = true;
#else
        if (!QFile::exists(m_path)) {
            if (error) *error = QStringLiteral("Owned placeholder is missing: %1").arg(m_path);
            return false;
        }
#endif
        return true;
    }

    bool restore(QString *error)
    {
#ifdef Q_OS_WIN
        if (!m_changed) return true;
        const QString ioPath = FileSystemPath::forIo(m_path);
        if (ioPath.isEmpty() || ioPath.contains(QChar(0))) {
            if (error) *error = QStringLiteral("Invalid owned placeholder path: %1").arg(m_path);
            return false;
        }
        const std::wstring nativePath = ioPath.toStdWString();
        if (!SetFileAttributesW(nativePath.c_str(), m_originalAttributes)) {
            if (error) {
                *error = QStringLiteral("Could not restore HIDDEN on owned recording file %1 (Windows error %2)")
                             .arg(m_path).arg(GetLastError());
            }
            return false;
        }
        m_changed = false;
#else
        Q_UNUSED(error);
#endif
        return true;
    }

    ~HiddenFileLease()
    {
        QString ignored;
        restore(&ignored);
    }

private:
    QString m_path;
#ifdef Q_OS_WIN
    DWORD m_originalAttributes = INVALID_FILE_ATTRIBUTES;
    bool m_changed = false;
#endif
};

