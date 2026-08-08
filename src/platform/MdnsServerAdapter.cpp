#include "platform/MdnsServerAdapter.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QMap>
#include <QString>
#include <qmdnsengine/dns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>

#include "diagnostics/DiagnosticEvent.h"
#include "diagnostics/DiagnosticLogSink.h"
#include "diagnostics/DiagnosticSanitizer.h"

namespace {

QString addressFamily(const QHostAddress &address) {
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return QStringLiteral("ipv4");
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return QStringLiteral("ipv6");
    return QStringLiteral("unknown");
}

QString recordType(quint16 type) {
    switch (type) {
    case QMdnsEngine::PTR: return QStringLiteral("PTR");
    case QMdnsEngine::SRV: return QStringLiteral("SRV");
    case QMdnsEngine::TXT: return QStringLiteral("TXT");
    case QMdnsEngine::A: return QStringLiteral("A");
    case QMdnsEngine::AAAA: return QStringLiteral("AAAA");
    default: return QStringLiteral("other");
    }
}

QString serviceName(const QByteArray &name) {
    const QByteArray lower = name.toLower();
    if (lower == QByteArrayLiteral("_airplay._tcp.local."))
        return QStringLiteral("_airplay._tcp");
    if (lower == QByteArrayLiteral("_raop._tcp.local."))
        return QStringLiteral("_raop._tcp");
    return {};
}

} // namespace

MdnsServerAdapter::MdnsServerAdapter(QMdnsEngine::AbstractServer *delegate,
                                     DiagnosticLogSink *sink, QObject *parent)
    : QMdnsEngine::AbstractServer(parent), delegate_(delegate), sink_(sink) {
    if (!delegate_)
        return;
    connect(delegate_, &QMdnsEngine::AbstractServer::messageReceived,
            this, &MdnsServerAdapter::onMessageReceived);
    connect(delegate_, &QMdnsEngine::AbstractServer::error, this, &MdnsServerAdapter::onError);
}

void MdnsServerAdapter::sendMessage(const QMdnsEngine::Message &message) {
    if (sink_) {
        sink_->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("discovery"),
                                          QStringLiteral("send_requested"),
                                          {{QStringLiteral("scope"), QStringLiteral("unicast")},
                                           {QStringLiteral("address_family"), addressFamily(message.address())}}));
    }
    if (delegate_)
        delegate_->sendMessage(message);
}

void MdnsServerAdapter::sendMessageToAll(const QMdnsEngine::Message &message) {
    if (sink_) {
        sink_->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("discovery"),
                                          QStringLiteral("send_requested"),
                                          {{QStringLiteral("scope"), QStringLiteral("multicast_all")},
                                           {QStringLiteral("address_family"), addressFamily(message.address())}}));
    }
    if (delegate_)
        delegate_->sendMessageToAll(message);
}

void MdnsServerAdapter::onMessageReceived(const QMdnsEngine::Message &message) {
    if (sink_) {
        const int prefix = message.address().protocol() == QAbstractSocket::IPv6Protocol ? 64 : 24;
        for (const QMdnsEngine::Query &query : message.queries()) {
            const QString queryName = serviceName(query.name());
            if (queryName.isEmpty())
                continue;
            sink_->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("discovery"),
                                              QStringLiteral("search_received"),
                                              {{QStringLiteral("query"), queryName},
                                               {QStringLiteral("type"), recordType(query.type())},
                                               {QStringLiteral("source"),
                                                DiagnosticSanitizer::maskedAddress(message.address(), prefix)}}));
        }
    }
    emit messageReceived(message);
}

void MdnsServerAdapter::onError(const QString &message) {
    if (!sink_)
        return;
    sink_->record(makeDiagnosticEvent(DiagnosticSeverity::Error, QStringLiteral("discovery"),
                                      QStringLiteral("error"),
                                      {{QStringLiteral("reason"), QStringLiteral("qmdns_engine")},
                                       {QStringLiteral("message"),
                                        DiagnosticSanitizer::sanitizeText(message).left(256)}}, true));
}
