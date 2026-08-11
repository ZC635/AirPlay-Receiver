#pragma once

#include <QHostAddress>
#include <QPair>
#include <QString>
#include <QStringView>

#include <optional>

class DiagnosticSanitizer {
public:
    static QString sanitizeFieldName(QStringView name);
    static QString sanitizeText(QStringView value);
    static QString maskedAddress(const QHostAddress &address, int prefixLength);
    static std::optional<QString> safeClientModel(QStringView model);
    static std::optional<QPair<QString, QString>> safeProductVersion(QStringView userAgent);
    static bool isSensitiveField(QStringView fieldName);
};
