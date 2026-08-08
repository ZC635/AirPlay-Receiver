#include "diagnostics/ThirdPartyDiagnosticTranslator.h"

#include "diagnostics/DiagnosticSanitizer.h"

#include <QRegularExpression>

namespace {

DiagnosticSeverity severityForLevel(int upstreamLevel) {
    if (upstreamLevel <= 3)
        return DiagnosticSeverity::Error;
    if (upstreamLevel == 4)
        return DiagnosticSeverity::Warning;
    if (upstreamLevel <= 6)
        return DiagnosticSeverity::Info;
    return DiagnosticSeverity::Debug;
}

bool completeMatch(const QRegularExpression &expression, const QString &message,
                   QRegularExpressionMatch *result) {
    const QRegularExpressionMatch match = expression.match(message);
    if (!match.hasMatch() || match.capturedStart() != 0 || match.capturedLength() != message.size())
        return false;
    *result = match;
    return true;
}

std::optional<DiagnosticEvent> eventFor(int upstreamLevel, QString name,
                                        QMap<QString, QString> fields) {
    return makeDiagnosticEvent(severityForLevel(upstreamLevel), QStringLiteral("third_party"),
                               std::move(name), std::move(fields));
}

} // namespace

std::optional<DiagnosticEvent> ThirdPartyDiagnosticTranslator::translate(int upstreamLevel,
                                                                          QByteArrayView message) {
    if (message.size() > 512)
        return std::nullopt;
    for (const char character : message) {
        if (character == '\r' || character == '\n' || character == '\0')
            return std::nullopt;
    }

    const QString text = QString::fromUtf8(message.data(), message.size());
    QRegularExpressionMatch match;

    static const QRegularExpression userAgent(
        QStringLiteral(R"(\AClient identified as User-Agent: ([A-Za-z0-9._-]{1,64}/[A-Za-z0-9._-]{1,64})\z)"));
    if (completeMatch(userAgent, text, &match)) {
        const auto productVersion = DiagnosticSanitizer::safeProductVersion(match.captured(1));
        if (!productVersion)
            return std::nullopt;
        return eventFor(upstreamLevel, QStringLiteral("client_software"),
                        {{QStringLiteral("product"), productVersion->first},
                         {QStringLiteral("version"), productVersion->second}});
    }

    static const QRegularExpression endpoint(
        QStringLiteral(R"(\A(Local|Remote): ([0-9A-Fa-f:.]+)\z)"));
    if (completeMatch(endpoint, text, &match)) {
        const QHostAddress address(match.captured(2));
        const bool ipv4 = address.protocol() == QAbstractSocket::IPv4Protocol;
        const bool ipv6 = address.protocol() == QAbstractSocket::IPv6Protocol;
        if (!ipv4 && !ipv6)
            return std::nullopt;
        return eventFor(upstreamLevel, QStringLiteral("connection_source"),
                        {{QStringLiteral("address_family"), ipv4 ? QStringLiteral("ipv4")
                                                                  : QStringLiteral("ipv6")},
                         {QStringLiteral("endpoint"), match.captured(1).toLower()},
                         {QStringLiteral("prefix"),
                          DiagnosticSanitizer::maskedAddress(address, ipv4 ? 24 : 64)}});
    }

    static const QRegularExpression clientAuthentication(
        QStringLiteral(R"(\AClient authentication (success|failure)\z)"));
    if (completeMatch(clientAuthentication, text, &match))
        return eventFor(upstreamLevel, QStringLiteral("client_authentication"),
                        {{QStringLiteral("result"), match.captured(1)}});

    static const QRegularExpression pairing(
        QStringLiteral(R"(\APairing (setup|verify|authentication) (success|failure)\z)"));
    if (completeMatch(pairing, text, &match))
        return eventFor(upstreamLevel, QStringLiteral("pairing_result"),
                        {{QStringLiteral("phase"), match.captured(1)},
                         {QStringLiteral("result"), match.captured(2)}});

    static const QRegularExpression renderer(QStringLiteral(R"(\A(Audio|Video) renderer start\z)"));
    if (completeMatch(renderer, text, &match))
        return eventFor(upstreamLevel, QStringLiteral("renderer_started"),
                        {{QStringLiteral("renderer"), match.captured(1).toLower()}});

    static const QRegularExpression audioCodec(QStringLiteral(R"(\AAudio codec: (AAC|ALAC|PCM)\z)"));
    if (completeMatch(audioCodec, text, &match))
        return eventFor(upstreamLevel, QStringLiteral("codec_selected"),
                        {{QStringLiteral("codec"), match.captured(1).toLower()},
                         {QStringLiteral("stream_type"), QStringLiteral("audio")}});

    static const QRegularExpression videoCodec(QStringLiteral(R"(\AVideo codec: (H264|H265)\z)"));
    if (completeMatch(videoCodec, text, &match))
        return eventFor(upstreamLevel, QStringLiteral("codec_selected"),
                        {{QStringLiteral("codec"), match.captured(1).toLower()},
                         {QStringLiteral("stream_type"), QStringLiteral("video")}});

    static const QRegularExpression socketError(QStringLiteral(R"(\ASocket error: ([1-9][0-9]{0,5})\z)"));
    if (completeMatch(socketError, text, &match))
        return eventFor(upstreamLevel, QStringLiteral("socket_error"),
                        {{QStringLiteral("code"), match.captured(1)}});

    return std::nullopt;
}
