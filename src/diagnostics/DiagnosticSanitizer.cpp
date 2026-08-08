#include "diagnostics/DiagnosticSanitizer.h"

#include <QRegularExpression>

namespace {

QString normalizedFieldName(QStringView name) {
    QString normalized;
    normalized.reserve(name.size());
    bool previousUnderscore = false;
    for (const QChar character : name) {
        if (character.isLetterOrNumber() && character.unicode() <= 0x7f) {
            normalized.append(character.toLower());
            previousUnderscore = false;
        } else if (!normalized.isEmpty() && !previousUnderscore) {
            normalized.append(QLatin1Char('_'));
            previousUnderscore = true;
        }
    }
    while (normalized.endsWith(QLatin1Char('_')))
        normalized.chop(1);
    return normalized;
}

bool isPrivateIpv4(quint32 address) {
    const quint8 first = static_cast<quint8>(address >> 24);
    const quint8 second = static_cast<quint8>(address >> 16);
    return first == 10 || (first == 172 && second >= 16 && second <= 31) ||
           (first == 192 && second == 168);
}

bool isPrivateIpv6(const Q_IPV6ADDR &address) {
    const bool uniqueLocal = (address.c[0] & 0xfeU) == 0xfcU;
    const bool linkLocal = address.c[0] == 0xfeU && (address.c[1] & 0xc0U) == 0x80U;
    return uniqueLocal || linkLocal;
}

QString replaceAddresses(QString text, const QRegularExpression &expression, int prefixLength) {
    QRegularExpressionMatchIterator matches = expression.globalMatch(text);
    QList<QPair<int, int>> replacements;
    QStringList values;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        QHostAddress address(match.captured(0));
        if (address.protocol() == QAbstractSocket::IPv4Protocol ||
            address.protocol() == QAbstractSocket::IPv6Protocol) {
            replacements.append(qMakePair(match.capturedStart(), match.capturedLength()));
            values.append(DiagnosticSanitizer::maskedAddress(address, prefixLength));
        }
    }
    for (qsizetype index = replacements.size() - 1; index >= 0; --index)
        text.replace(replacements.at(index).first, replacements.at(index).second, values.at(index));
    return text;
}

QString escapeControls(QStringView value) {
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (character == QLatin1Char('\n')) {
            escaped.append(QStringLiteral("\\n"));
        } else if (character == QLatin1Char('\r')) {
            escaped.append(QStringLiteral("\\r"));
        } else if (character == QLatin1Char('\t')) {
            escaped.append(QStringLiteral("\\t"));
        } else if (code < 0x20 || code == 0x7f) {
            escaped.append(QStringLiteral("\\x%1").arg(code, 2, 16, QLatin1Char('0')));
        } else {
            escaped.append(character);
        }
    }
    return escaped;
}

} // namespace

QString DiagnosticSanitizer::sanitizeFieldName(QStringView name) {
    const QString sanitized = normalizedFieldName(name);
    if (sanitized.isEmpty() || sanitized.size() > 64 || isSensitiveField(sanitized))
        return {};
    return sanitized;
}

QString DiagnosticSanitizer::sanitizeText(QStringView value) {
    QString text = value.toString();
    static const QRegularExpression sensitiveMultiline(
        QStringLiteral(R"(\b(?:eiv|ekey|aesiv|aeskey|iv|ecdh(?:[_-]?secret)?|password|ssid|authorization|cookie|session(?:[_-]?id)?|token|(?:public|private)?[_-]?key|header|auth(?:entication)?|pairing|secret)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if ((text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) &&
        sensitiveMultiline.match(text).hasMatch()) {
        return QStringLiteral("[redacted_multiline]");
    }

    static const QRegularExpression path(
        QStringLiteral(R"((?:[A-Za-z]:[\\/]|\\\\)[^\s,;]*)"));
    static const QRegularExpression mac(
        QStringLiteral(R"(\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b)"));
    static const QRegularExpression headerValue(
        QStringLiteral(R"(\b(?:(?:request\s+)?headers?|authorization|proxy-authorization|(?:x-)?(?:apple-)?session-id|cookie|set-cookie)\s*:\s*[^\r\n]*)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression secret(
        QStringLiteral(R"(\b(?:eiv|ekey|aeskey|public_key|private_key|key|token|password|secret|authorization|(?:x-)?(?:apple-)?session(?:[_-]id)?|deviceid|device_id|username|user|computer|hostname|client_name|ssid)\s*[:=]\s*[^\r\n,;]*)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ipv6(
        QStringLiteral(R"((?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f:]+(?![0-9A-Fa-f:]))"));
    static const QRegularExpression ipv4(QStringLiteral(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)"));

    text.replace(path, QStringLiteral("[path]"));
    text.replace(mac, QStringLiteral("[mac]"));
    text = replaceAddresses(text, ipv6, 64);
    text = replaceAddresses(text, ipv4, 24);
    text.replace(headerValue, QStringLiteral("[redacted]"));
    text.replace(secret, QStringLiteral("[redacted]"));
    return escapeControls(text);
}

QString DiagnosticSanitizer::maskedAddress(const QHostAddress &address, int prefixLength) {
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 raw = address.toIPv4Address();
        if (!isPrivateIpv4(raw))
            return QStringLiteral("public_ipv4");

        const int prefix = qBound(0, prefixLength, 24);
        const int octets = prefix / 8;
        QStringList parts;
        for (int index = 0; index < 4; ++index) {
            if (index < octets)
                parts.append(QString::number((raw >> (24 - index * 8)) & 0xffU));
            else
                parts.append(QStringLiteral("xxx"));
        }
        return parts.join(QLatin1Char('.')) + QLatin1Char('/') + QString::number(prefix);
    }

    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR raw = address.toIPv6Address();
        if (!isPrivateIpv6(raw))
            return QStringLiteral("public_ipv6");

        const int prefix = qBound(0, prefixLength, 64);
        const int groups = prefix / 16;
        QStringList parts;
        for (int index = 0; index < groups; ++index) {
            const quint16 group = static_cast<quint16>((raw.c[index * 2] << 8) | raw.c[index * 2 + 1]);
            parts.append(QStringLiteral("%1").arg(group, 4, 16, QLatin1Char('0')));
        }
        parts.append(QStringLiteral("xxxx"));
        return parts.join(QLatin1Char(':')) + QLatin1Char('/') + QString::number(prefix);
    }

    return QStringLiteral("unknown_address");
}

std::optional<QString> DiagnosticSanitizer::safeClientModel(QStringView model) {
    static const QRegularExpression allowed(QStringLiteral(R"(\A[A-Za-z0-9,._-]{1,64}\z)"));
    const QString value = model.toString();
    if (!allowed.match(value).hasMatch())
        return std::nullopt;
    return value;
}

std::optional<QPair<QString, QString>> DiagnosticSanitizer::safeProductVersion(QStringView userAgent) {
    static const QRegularExpression allowed(
        QStringLiteral(R"(\A([A-Za-z0-9._-]{1,64})/([A-Za-z0-9._-]{1,64})\z)"));
    const QRegularExpressionMatch match = allowed.match(userAgent.toString());
    if (!match.hasMatch())
        return std::nullopt;
    return qMakePair(match.captured(1), match.captured(2));
}

bool DiagnosticSanitizer::isSensitiveField(QStringView fieldName) {
    const QString field = normalizedFieldName(fieldName);
    if (field == QStringLiteral("address_family"))
        return false;
    static const QStringList exact = {
        QStringLiteral("ssid"), QStringLiteral("username"), QStringLiteral("user"),
        QStringLiteral("computer"), QStringLiteral("hostname"), QStringLiteral("path"),
        QStringLiteral("file"), QStringLiteral("filename"), QStringLiteral("address"),
        QStringLiteral("ip"), QStringLiteral("ip_address"), QStringLiteral("mac"),
        QStringLiteral("mac_address"), QStringLiteral("client_name"), QStringLiteral("device_id"),
        QStringLiteral("deviceid"), QStringLiteral("metadata"), QStringLiteral("media"),
        QStringLiteral("headers"), QStringLiteral("header"), QStringLiteral("request_body"),
        QStringLiteral("response_body")};
    if (exact.contains(field))
        return true;

    static const QStringList fragments = {
        QStringLiteral("user"), QStringLiteral("computer"), QStringLiteral("host"),
        QStringLiteral("path"), QStringLiteral("file"), QStringLiteral("address"),
        QStringLiteral("device"), QStringLiteral("header"), QStringLiteral("body"),
        QStringLiteral("request"), QStringLiteral("response"), QStringLiteral("session"),
        QStringLiteral("auth"), QStringLiteral("pairing"), QStringLiteral("encrypt"),
        QStringLiteral("key"), QStringLiteral("token"), QStringLiteral("secret"),
        QStringLiteral("password"), QStringLiteral("cookie"), QStringLiteral("metadata"),
        QStringLiteral("media")};
    for (const QString &fragment : fragments) {
        if (field.contains(fragment))
            return true;
    }
    return false;
}
