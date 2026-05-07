#include "platform/MdnsPublisher.h"

#include <QByteArray>

#if AIRPLAY_WITH_UXPLAY
#include <qmdnsengine/hostname.h>
#include <qmdnsengine/provider.h>
#include <qmdnsengine/server.h>
#include <qmdnsengine/service.h>
#endif

static QByteArray uppercaseHex(const QByteArray &data) {
    return data.toHex().toUpper();
}

struct MdnsPublisher::Private {
#if AIRPLAY_WITH_UXPLAY
    QMdnsEngine::Server *server = nullptr;
    QMdnsEngine::Hostname *hostname = nullptr;
    QMdnsEngine::Provider *raopProvider = nullptr;
    QMdnsEngine::Provider *airplayProvider = nullptr;
#endif
};

MdnsPublisher::MdnsPublisher(QObject *parent)
    : QObject(parent), d(new Private) {}

MdnsPublisher::~MdnsPublisher() {
    stop();
}

QMap<QByteArray, QByteArray> MdnsPublisher::parseTxtRecord(const char *txt, int length, bool *ok) {
    QMap<QByteArray, QByteArray> result;
    if (!txt || length <= 0) {
        if (ok) *ok = false;
        return result;
    }

    const unsigned char *data = reinterpret_cast<const unsigned char *>(txt);
    const unsigned char *end = data + length;

    while (data < end) {
        int entryLen = static_cast<int>(*data);
        data++;
        if (entryLen <= 0 || data + entryLen > end) {
            break;
        }

        QByteArray entry(reinterpret_cast<const char *>(data), entryLen);
        int equalsPos = entry.indexOf('=');
        if (equalsPos >= 0) {
            QByteArray key = entry.left(equalsPos);
            QByteArray value = entry.mid(equalsPos + 1);
            result.insert(key, value);
        } else {
            result.insert(entry, QByteArray());
        }

        data += entryLen;
    }

    if (ok) *ok = true;
    return result;
}

QList<MdnsPublisher::ServiceDefinition> MdnsPublisher::buildServiceDefinitions(
    const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
    const char *raopTxt, int raopTxtLength,
    const char *airplayTxt, int airplayTxtLength,
    bool *ok) {
    QList<ServiceDefinition> defs;

    bool raopOk = false;
    QMap<QByteArray, QByteArray> raopAttrs = parseTxtRecord(raopTxt, raopTxtLength, &raopOk);
    if (!raopOk) {
        if (ok) *ok = false;
        return defs;
    }

    bool airplayOk = false;
    QMap<QByteArray, QByteArray> airplayAttrs = parseTxtRecord(airplayTxt, airplayTxtLength, &airplayOk);
    if (!airplayOk) {
        if (ok) *ok = false;
        return defs;
    }

    const QByteArray raopName = uppercaseHex(hardwareAddress) + "@" + receiverName.toUtf8();

    ServiceDefinition raopDef;
    raopDef.type = QByteArrayLiteral("_raop._tcp.local.");
    raopDef.name = raopName;
    raopDef.port = port;
    raopDef.attributes = raopAttrs;
    defs.append(raopDef);

    ServiceDefinition airplayDef;
    airplayDef.type = QByteArrayLiteral("_airplay._tcp.local.");
    airplayDef.name = receiverName.toUtf8();
    airplayDef.port = port;
    airplayDef.attributes = airplayAttrs;
    defs.append(airplayDef);

    if (ok) *ok = true;
    return defs;
}

bool MdnsPublisher::publish(const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
                            const char *raopTxt, int raopTxtLength,
                            const char *airplayTxt, int airplayTxtLength) {
    bool ok = false;
    QList<ServiceDefinition> defs = buildServiceDefinitions(
        receiverName, hardwareAddress, port,
        raopTxt, raopTxtLength,
        airplayTxt, airplayTxtLength, &ok);
    if (!ok || defs.size() != 2) {
        return false;
    }

#if AIRPLAY_WITH_UXPLAY
    stop();

    d->server = new QMdnsEngine::Server(this);
    if (!d->server) {
        return false;
    }

    d->hostname = new QMdnsEngine::Hostname(d->server, this);

    const auto &raopDef = defs.at(0);
    QMdnsEngine::Service raopService;
    raopService.setName(raopDef.name);
    raopService.setType(raopDef.type);
    raopService.setPort(raopDef.port);
    QMap<QByteArray, QByteArray> raopAttrs = raopDef.attributes;
    raopService.setAttributes(raopAttrs);

    d->raopProvider = new QMdnsEngine::Provider(d->server, d->hostname, this);
    d->raopProvider->update(raopService);

    const auto &airplayDef = defs.at(1);
    QMdnsEngine::Service airplayService;
    airplayService.setName(airplayDef.name);
    airplayService.setType(airplayDef.type);
    airplayService.setPort(airplayDef.port);
    QMap<QByteArray, QByteArray> airplayAttrs = airplayDef.attributes;
    airplayService.setAttributes(airplayAttrs);

    d->airplayProvider = new QMdnsEngine::Provider(d->server, d->hostname, this);
    d->airplayProvider->update(airplayService);
#endif

#if AIRPLAY_WITH_UXPLAY
    return true;
#else
    return false;
#endif
}

void MdnsPublisher::stop() {
#if AIRPLAY_WITH_UXPLAY
    delete d->raopProvider;
    d->raopProvider = nullptr;
    delete d->airplayProvider;
    d->airplayProvider = nullptr;
    delete d->hostname;
    d->hostname = nullptr;
    delete d->server;
    d->server = nullptr;
#endif
}
