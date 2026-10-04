#pragma once

#include "app/UiMessage.h"

#include <QString>
#include <QUrl>

#include <functional>

struct DiagnosticLogFolderOperations {
    std::function<UiMessage(const QString &)> createDirectory;
    std::function<UiMessage(const QUrl &)> openUrl;
};

class DiagnosticLogFolderActions final {
public:
    explicit DiagnosticLogFolderActions(QString applicationDirectory,
                                        DiagnosticLogFolderOperations operations = {});

    UiMessage ensureAndOpen();

private:
    QString applicationDirectory_;
    DiagnosticLogFolderOperations operations_;
};
