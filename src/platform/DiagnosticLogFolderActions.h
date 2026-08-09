#pragma once

#include <QString>
#include <QUrl>

#include <functional>

struct DiagnosticLogFolderOperations {
    std::function<QString(const QString &)> createDirectory;
    std::function<QString(const QUrl &)> openUrl;
};

class DiagnosticLogFolderActions final {
public:
    explicit DiagnosticLogFolderActions(QString applicationDirectory,
                                        DiagnosticLogFolderOperations operations = {});

    QString ensureAndOpen();

private:
    QString applicationDirectory_;
    DiagnosticLogFolderOperations operations_;
};
