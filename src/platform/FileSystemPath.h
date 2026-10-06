#pragma once

#include <QString>

namespace FileSystemPath {

// Logical absolute path for transaction identity, returned paths and display.
// Extended Windows disk/UNC input is folded into the same logical identity.
QString absolute(const QString &path);

// Native file-I/O boundary. Windows disk/UNC paths use an extended UTF-16 path;
// unsupported Windows device namespaces return empty. Other platforms use the
// logical absolute path. This representation is not for UI or shell commands.
QString forIo(const QString &path);

} // namespace FileSystemPath
