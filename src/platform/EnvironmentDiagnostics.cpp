#include "platform/EnvironmentDiagnostics.h"

#include "diagnostics/DiagnosticSanitizer.h"

#include <QHostAddress>
#include <QRegularExpression>

namespace {

QString statusText(DiagnosticFactStatus status) {
    switch (status) {
    case DiagnosticFactStatus::Available: return QStringLiteral("available");
    case DiagnosticFactStatus::TimedOut: return QStringLiteral("timed_out");
    case DiagnosticFactStatus::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

bool ascii(const QString &value) {
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() > 0x7e)
            return false;
    }
    return true;
}

QString enumValue(const DiagnosticFact &fact, const QStringList &allowed) {
    if (fact.status != DiagnosticFactStatus::Available)
        return statusText(fact.status);
    return allowed.contains(fact.value) ? fact.value : QStringLiteral("unavailable");
}

QString systemValue(const DiagnosticFact &fact) {
    if (fact.status != DiagnosticFactStatus::Available)
        return statusText(fact.status);
    const QString sanitized = DiagnosticSanitizer::sanitizeText(fact.value);
    return ascii(sanitized) && !sanitized.isEmpty() ? sanitized : QStringLiteral("unavailable");
}

QString profileValue(const DiagnosticFact &fact) {
    if (fact.status != DiagnosticFactStatus::Available)
        return statusText(fact.status);
    static const QRegularExpression allowed(
        QStringLiteral("\\A(?:domain|private|public)=(?:on|off)(?:,(?:domain|private|public)=(?:on|off))*\\z"));
    return allowed.match(fact.value).hasMatch() ? fact.value : QStringLiteral("unavailable");
}

QString safePrefix(QString prefix) {
    if (prefix == QStringLiteral("public_ipv4") || prefix == QStringLiteral("public_ipv6"))
        return prefix;
    const QStringList parts = prefix.split(QLatin1Char('/'));
    bool validLength = false;
    const int length = parts.size() == 2 ? parts.at(1).toInt(&validLength) : 0;
    if (!validLength || parts.size() != 2 || QString::number(length) != parts.at(1))
        return QStringLiteral("unavailable");

    const QStringList ipv4 = parts.at(0).split(QLatin1Char('.'));
    if (ipv4.contains(QStringLiteral("xxx"))) {
        if (length < 0 || length > 32 || ipv4.size() != 4)
            return QStringLiteral("unavailable");
        QStringList reconstructed;
        for (const QString &part : ipv4) {
            if (part == QStringLiteral("xxx")) {
                reconstructed.append(QStringLiteral("0"));
                continue;
            }
            bool numeric = false;
            const int value = part.toInt(&numeric);
            if (!numeric || value < 0 || value > 255 || QString::number(value) != part)
                return QStringLiteral("unavailable");
            reconstructed.append(part);
        }
        const QHostAddress address(reconstructed.join(QLatin1Char('.')));
        if (!address.isNull() && DiagnosticSanitizer::maskedAddress(address, length).compare(
                prefix, Qt::CaseInsensitive) == 0)
            return prefix;
        return QStringLiteral("unavailable");
    }

    const QStringList ipv6 = parts.at(0).split(QLatin1Char(':'));
    if (ipv6.contains(QStringLiteral("xxxx"))) {
        if (length < 0 || length > 64 || ipv6.size() != length / 16 + 1 ||
            ipv6.constLast().compare(QStringLiteral("xxxx"), Qt::CaseInsensitive) != 0)
            return QStringLiteral("unavailable");
        QStringList reconstructed;
        for (qsizetype index = 0; index + 1 < ipv6.size(); ++index) {
            static const QRegularExpression hex(QStringLiteral("\\A[0-9a-f]{4}\\z"),
                                                QRegularExpression::CaseInsensitiveOption);
            if (!hex.match(ipv6.at(index)).hasMatch())
                return QStringLiteral("unavailable");
            reconstructed.append(ipv6.at(index));
        }
        while (reconstructed.size() < 8)
            reconstructed.append(QStringLiteral("0000"));
        const QHostAddress address(reconstructed.join(QLatin1Char(':')));
        if (!address.isNull() && DiagnosticSanitizer::maskedAddress(address, length).compare(
                prefix, Qt::CaseInsensitive) == 0)
            return prefix;
        return QStringLiteral("unavailable");
    }

    const QHostAddress address(parts.value(0));
    if (address.isNull())
        return QStringLiteral("unavailable");
    return DiagnosticSanitizer::maskedAddress(address, length);
}

DiagnosticFact timedOutIfExpired(QDeadlineTimer deadline, DiagnosticFact value) {
    return deadline.hasExpired() ? DiagnosticFact::timedOut() : value;
}

template<typename T>
DiagnosticValue<T> timedOutIfExpired(QDeadlineTimer deadline, DiagnosticValue<T> value) {
    return deadline.hasExpired() ? DiagnosticValue<T>::timedOut() : value;
}

} // namespace

DiagnosticFact DiagnosticFact::available(QString value) {
    return {DiagnosticFactStatus::Available, std::move(value)};
}

DiagnosticFact DiagnosticFact::unavailable() { return {}; }

DiagnosticFact DiagnosticFact::timedOut() { return {DiagnosticFactStatus::TimedOut, {}}; }

bool shouldCollectEnvironmentDiagnostics(bool diagnosticSessionActive) {
    return diagnosticSessionActive;
}

EnvironmentSnapshot EnvironmentDiagnostics::collect(const EnvironmentDiagnosticProviders &providers,
                                                    int totalTimeoutMs) {
    QDeadlineTimer deadline(qMax(0, totalTimeoutMs));
    EnvironmentSnapshot snapshot;
    const auto fact = [&deadline](const auto &provider) {
        if (deadline.hasExpired() || !provider)
            return DiagnosticFact::timedOut();
        return timedOutIfExpired(deadline, provider(deadline));
    };
    snapshot.operatingSystem = fact(providers.operatingSystem);
    snapshot.cpuArchitecture = fact(providers.cpuArchitecture);
    snapshot.processElevation = fact(providers.processElevation);
    if (deadline.hasExpired() || !providers.network) {
        snapshot.network = DiagnosticValue<NetworkEnvironmentFact>::timedOut();
    } else {
        snapshot.network = timedOutIfExpired(deadline, providers.network(deadline));
    }
    return snapshot;
}

QList<DiagnosticEvent> EnvironmentDiagnostics::events(const EnvironmentSnapshot &snapshot) {
    QMap<QString, QString> snapshotFields{
        {QStringLiteral("operating_system"), systemValue(snapshot.operatingSystem)},
        {QStringLiteral("cpu_architecture"), systemValue(snapshot.cpuArchitecture)},
        {QStringLiteral("process_elevation"), enumValue(snapshot.processElevation,
            {QStringLiteral("elevated"), QStringLiteral("not_elevated")})},
        {QStringLiteral("network"), statusText(snapshot.network.status)},
    };
    if (snapshot.network.status == DiagnosticFactStatus::Available) {
        snapshotFields.insert(QStringLiteral("network_category"), enumValue(
            snapshot.network.value.category,
            {QStringLiteral("domain"), QStringLiteral("private"), QStringLiteral("public")}));
        snapshotFields.insert(QStringLiteral("firewall_profiles"),
                              profileValue(snapshot.network.value.firewallProfiles));
        snapshotFields.insert(QStringLiteral("firewall_rule"), enumValue(
            snapshot.network.value.executableFirewallRule,
            {QStringLiteral("confirmed"), QStringLiteral("denied"), QStringLiteral("absent"),
             QStringLiteral("not_read")}));
    }
    QList<DiagnosticEvent> result;
    result.append(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("environment"),
                                      QStringLiteral("environment_snapshot"), snapshotFields, true));
    if (snapshot.network.status != DiagnosticFactStatus::Available)
        return result;

    for (const NetworkAdapterFact &adapter : snapshot.network.value.adapters) {
        QStringList prefixes;
        for (const QString &prefix : adapter.prefixes)
            prefixes.append(safePrefix(prefix));
        const QString type = QStringList{QStringLiteral("ethernet"), QStringLiteral("wifi"),
                                         QStringLiteral("tunnel"), QStringLiteral("loopback"),
                                         QStringLiteral("other")}.contains(adapter.type)
            ? adapter.type : QStringLiteral("other");
        const QString physical = QStringList{QStringLiteral("physical"), QStringLiteral("virtual"),
                                             QStringLiteral("unavailable")}.contains(adapter.physicalClassification)
            ? adapter.physicalClassification : QStringLiteral("unavailable");
        result.append(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("environment"),
                                          QStringLiteral("environment_adapter"),
                                          {{QStringLiteral("index"), QString::number(adapter.sessionIndex)},
                                           {QStringLiteral("type"), type},
                                           {QStringLiteral("enabled"), adapter.enabled ? QStringLiteral("yes") : QStringLiteral("no")},
                                           {QStringLiteral("up"), adapter.up ? QStringLiteral("yes") : QStringLiteral("no")},
                                           {QStringLiteral("physical_class"), physical},
                                           {QStringLiteral("route_metric"), QString::number(adapter.routeMetric)},
                                           {QStringLiteral("default_route"), adapter.ownsDefaultRoute ? QStringLiteral("yes") : QStringLiteral("no")},
                                           {QStringLiteral("prefixes"), prefixes.join(QLatin1Char(','))}}, true));
    }
    return result;
}
