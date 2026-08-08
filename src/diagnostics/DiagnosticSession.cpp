#include "diagnostics/DiagnosticSession.h"

#include "diagnostics/DiagnosticSanitizer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>

namespace {

class QFileDiagnosticSessionFile final : public DiagnosticSessionFile {
public:
    explicit QFileDiagnosticSessionFile(QString path) : m_file(std::move(path)) {}

    bool open() { return m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly); }
    qint64 write(const QByteArray &bytes) override { return m_file.write(bytes); }
    bool flush() override { return m_file.flush(); }
    void close() override { m_file.close(); }
    QString errorString() const override { return m_file.errorString(); }

private:
    QFile m_file;
};

class LocalDiagnosticSessionStorage final : public DiagnosticSessionStorage {
public:
    QString ensureDirectory(const QString &exactPath) override {
        return QDir().mkpath(exactPath) ? QString() : QStringLiteral("failed to create diagnostics directory");
    }

    std::unique_ptr<DiagnosticSessionFile> createExclusive(
        const QString &exactPath, QString *error) override {
        auto file = std::make_unique<QFileDiagnosticSessionFile>(exactPath);
        if (file->open())
            return file;
        if (error)
            *error = file->errorString();
        return {};
    }

    QVector<DiagnosticStoredFile> list(const QString &exactDirectory) override {
        QVector<DiagnosticStoredFile> files;
        const QFileInfoList entries = QDir(exactDirectory).entryInfoList(
            QDir::Files | QDir::NoSymLinks | QDir::Readable | QDir::Hidden);
        files.reserve(entries.size());
        for (const QFileInfo &entry : entries)
            files.append({entry.fileName(), entry.lastModified(), entry.isFile() && !entry.isSymLink()});
        return files;
    }

    bool remove(const QString &exactPath) override { return QFile::remove(exactPath); }
};

QString severityName(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Debug: return QStringLiteral("DEBUG");
    case DiagnosticSeverity::Info: return QStringLiteral("INFO");
    case DiagnosticSeverity::Warning: return QStringLiteral("WARN");
    case DiagnosticSeverity::Error: return QStringLiteral("ERROR");
    case DiagnosticSeverity::Critical: return QStringLiteral("CRITICAL");
    }
    return QStringLiteral("INFO");
}

QString asciiToken(QStringView text) {
    const QString sanitized = DiagnosticSanitizer::sanitizeText(text);
    QString result;
    result.reserve(sanitized.size());
    for (const QChar character : sanitized) {
        if (character.isLetterOrNumber() && character.unicode() <= 0x7f) {
            result.append(character);
        } else if (character == QLatin1Char('_') || character == QLatin1Char('-') ||
                   character == QLatin1Char('.')) {
            result.append(character);
        } else {
            result.append(QLatin1Char('_'));
        }
    }
    return result.isEmpty() ? QStringLiteral("unknown") : result;
}

QString escapeFieldValue(QStringView text) {
    const QString sanitized = DiagnosticSanitizer::sanitizeText(text);
    QString result;
    result.reserve(sanitized.size());
    for (qsizetype i = 0; i < sanitized.size(); ++i) {
        const QChar character = sanitized.at(i);
        if (character.unicode() > 0x7e) {
            result.append(QStringLiteral("\\u%1").arg(static_cast<ushort>(character.unicode()), 4, 16, QLatin1Char('0')));
        } else if (character == QLatin1Char('\\')) {
            const bool sanitizerEscape = i + 1 < sanitized.size() &&
                (sanitized.at(i + 1) == QLatin1Char('n') || sanitized.at(i + 1) == QLatin1Char('r') ||
                 sanitized.at(i + 1) == QLatin1Char('t') || sanitized.at(i + 1) == QLatin1Char('x'));
            result.append(sanitizerEscape ? QStringLiteral("\\") : QStringLiteral("\\\\"));
        } else if (character == QLatin1Char(' ')) {
            result.append(QStringLiteral("\\s"));
        } else if (character == QLatin1Char('=')) {
            result.append(QStringLiteral("\\="));
        } else if (character == QLatin1Char('"')) {
            result.append(QStringLiteral("\\\""));
        } else if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            result.append(QStringLiteral("\\x%1").arg(static_cast<ushort>(character.unicode()), 2, 16, QLatin1Char('0')));
        } else {
            result.append(character);
        }
    }
    return result;
}

QMap<QString, QString> sanitizedFields(const QMap<QString, QString> &fields) {
    QMap<QString, QString> result;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        const QString name = DiagnosticSanitizer::sanitizeFieldName(it.key());
        if (!name.isEmpty())
            result.insert(name, escapeFieldValue(it.value()));
    }
    return result;
}

QString formattedLine(const DiagnosticEvent &event, const QMap<QString, QString> &fields) {
    const QDateTime timestamp = event.timeUtc.isValid() ? event.timeUtc.toUTC() : QDateTime::currentDateTimeUtc();
    QString line = timestamp.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
    line += QLatin1Char(' ') + severityName(event.severity);
    line += QLatin1Char(' ') + asciiToken(event.component);
    line += QLatin1Char(' ') + asciiToken(event.name);
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        line += QLatin1Char(' ') + it.key() + QLatin1Char('=') + it.value();
    return line + QLatin1Char('\n');
}

bool isBoundary(DiagnosticSeverity severity) {
    return severity == DiagnosticSeverity::Warning || severity == DiagnosticSeverity::Error ||
           severity == DiagnosticSeverity::Critical;
}

bool isSessionFileName(const QString &name) {
    static const QRegularExpression expression(
        QStringLiteral(R"(^AirPlay-Diagnostic-\d{4}-\d{2}-\d{2}-\d{6}(?:-\d{2,})?\.log$)"));
    return expression.match(name).hasMatch();
}

} // namespace

class DiagnosticSession::Private {
public:
    DiagnosticSessionOptions options;
    QString path;
    std::unique_ptr<DiagnosticSessionFile> file;
    mutable QMutex mutex;
    bool active = true;
    bool closed = false;
    bool failureEmitted = false;
    bool compact = false;
    qint64 bytesWritten = 0;
    qint64 compactOrSizeSuppressed = 0;
    qint64 duplicateSuppressed = 0;
    qint64 safeMessageSuppressed = 0;
    qint64 warnings = 0;
    qint64 errors = 0;
    qint64 criticals = 0;
    qint64 startupCompleted = 0;
    qint64 receiverDiscoverable = 0;
    qint64 receiverConnected = 0;
    qint64 serviceTypes = 0;
    qint64 searchReceived = 0;
    qint64 sendRequested = 0;
    qint64 clientRequest = 0;
    qint64 disconnects = 0;
    qint64 resets = 0;
    qint64 networkChanged = 0;
    QString duplicateKey;
    QString duplicateComponent;
    QString duplicateEvent;
    qint64 pendingDuplicates = 0;
};

DiagnosticSession::DiagnosticSession(DiagnosticSessionOptions options, QString filePath,
                                     std::unique_ptr<DiagnosticSessionFile> file)
    : d(std::make_unique<Private>()) {
    d->options = std::move(options);
    d->path = std::move(filePath);
    d->file = std::move(file);
}

DiagnosticSession::~DiagnosticSession() {
    QMutexLocker lock(&d->mutex);
    if (d->file)
        d->file->close();
    d->active = false;
    d->closed = true;
}

DiagnosticSessionCreateResult DiagnosticSession::create(DiagnosticSessionOptions options) {
    if (options.applicationDirectory.isEmpty())
        return {{}, QStringLiteral("application directory is empty")};
    if (options.maxBytes <= 0 || options.compactThresholdBytes < 0 || options.finalReserveBytes < 0 ||
        options.compactThresholdBytes > options.maxBytes || options.finalReserveBytes >= options.maxBytes ||
        options.maxSessions <= 0) {
        return {{}, QStringLiteral("invalid diagnostic session limits")};
    }
    if (!options.storage)
        options.storage = std::make_shared<LocalDiagnosticSessionStorage>();

    const QString logsDirectory = QDir(options.applicationDirectory).filePath(QStringLiteral("logs"));
    const QString directoryError = options.storage->ensureDirectory(logsDirectory);
    if (!directoryError.isEmpty())
        return {{}, directoryError};

    const QDateTime now = options.now ? options.now() : QDateTime::currentDateTimeUtc();
    const QString stem = QStringLiteral("AirPlay-Diagnostic-") + now.toLocalTime().toString(
        QStringLiteral("yyyy-MM-dd-HHmmss"));
    QString path;
    std::unique_ptr<DiagnosticSessionFile> file;
    QString createError;
    for (int suffix = 0; ; ++suffix) {
        const QString name = suffix == 0 ? stem + QStringLiteral(".log") :
            stem + QStringLiteral("-%1.log").arg(suffix, 2, 10, QLatin1Char('0'));
        path = QDir(logsDirectory).filePath(name);
        createError.clear();
        file = options.storage->createExclusive(path, &createError);
        if (file)
            break;
        if (!createError.contains(QStringLiteral("exist"), Qt::CaseInsensitive))
            return {{}, createError.isEmpty() ? QStringLiteral("failed to create diagnostic log") : createError};
    }

    QVector<DiagnosticStoredFile> retained;
    for (const DiagnosticStoredFile &entry : options.storage->list(logsDirectory)) {
        if (entry.regularFile && isSessionFileName(entry.fileName))
            retained.append(entry);
    }
    std::sort(retained.begin(), retained.end(), [](const DiagnosticStoredFile &left,
                                                     const DiagnosticStoredFile &right) {
        return left.lastModified == right.lastModified ? left.fileName < right.fileName :
                                                        left.lastModified < right.lastModified;
    });
    while (retained.size() > options.maxSessions) {
        const QString oldPath = QDir(logsDirectory).filePath(retained.front().fileName);
        if (!options.storage->remove(oldPath)) {
            file->close();
            options.storage->remove(path);
            return {{}, QStringLiteral("failed to remove old diagnostic log")};
        }
        retained.removeFirst();
    }

    auto session = std::unique_ptr<DiagnosticSession>(new DiagnosticSession(std::move(options), path, std::move(file)));
    DiagnosticEvent started = makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("session"),
        QStringLiteral("session_started"), {{QStringLiteral("activation_source"), session->d->options.activationSource}},
        true, now.toUTC());
    const QByteArray line = formattedLine(started, sanitizedFields(started.fields)).toUtf8();
    if (line.size() > session->d->options.maxBytes || session->d->file->write(line) != line.size() ||
        !session->d->file->flush()) {
        const QString error = session->d->file->errorString();
        session->d->file->close();
        session->d->options.storage->remove(path);
        return {{}, error.isEmpty() ? QStringLiteral("failed to write diagnostic session header") : error};
    }
    session->d->bytesWritten = line.size();
    return {std::move(session), {}};
}

bool DiagnosticSession::isActive() const {
    QMutexLocker lock(&d->mutex);
    return d->active;
}

QString DiagnosticSession::filePath() const {
    QMutexLocker lock(&d->mutex);
    return d->path;
}

void DiagnosticSession::record(DiagnosticEvent event) {
    QString failure;
    {
        QMutexLocker lock(&d->mutex);
        if (!d->active || d->closed)
            return;

        if (event.severity == DiagnosticSeverity::Warning)
            ++d->warnings;
        else if (event.severity == DiagnosticSeverity::Error)
            ++d->errors;
        else if (event.severity == DiagnosticSeverity::Critical)
            ++d->criticals;
        if (event.name == QStringLiteral("startup_completed") && event.fields.value(QStringLiteral("result")) == QStringLiteral("yes"))
            ++d->startupCompleted;
        if (event.name == QStringLiteral("state_changed")) {
            if (event.fields.value(QStringLiteral("state")) == QStringLiteral("discoverable"))
                ++d->receiverDiscoverable;
            if (event.fields.value(QStringLiteral("state")) == QStringLiteral("connected"))
                ++d->receiverConnected;
        }
        const QString serviceType = event.fields.value(QStringLiteral("type"));
        if (event.name == QStringLiteral("service_configured") &&
            (serviceType == QStringLiteral("_airplay._tcp") || serviceType == QStringLiteral("_raop._tcp")))
            ++d->serviceTypes;
        if (event.name == QStringLiteral("search_received")) ++d->searchReceived;
        if (event.name == QStringLiteral("send_requested")) ++d->sendRequested;
        if (event.name == QStringLiteral("client_request")) ++d->clientRequest;
        if (event.name == QStringLiteral("disconnect")) ++d->disconnects;
        if (event.name == QStringLiteral("reset")) ++d->resets;
        if (event.name == QStringLiteral("network_changed")) ++d->networkChanged;
        if (event.component == QStringLiteral("third_party") && event.name == QStringLiteral("message_suppressed"))
            ++d->safeMessageSuppressed;

        const QMap<QString, QString> fields = sanitizedFields(event.fields);
        const QString component = asciiToken(event.component);
        const QString name = asciiToken(event.name);
        const QByteArray line = formattedLine(event, fields).toUtf8();
        QString key = severityName(event.severity) + QLatin1Char('\x1f') + component +
            QLatin1Char('\x1f') + name;
        for (auto it = fields.cbegin(); it != fields.cend(); ++it)
            key += QLatin1Char('\x1f') + it.key() + QLatin1Char('=') + it.value();

        auto fail = [&] {
            d->active = false;
            d->file->close();
            if (!d->failureEmitted) {
                d->failureEmitted = true;
                failure = d->file->errorString();
                if (failure.isEmpty())
                    failure = QStringLiteral("diagnostic log write failed");
            }
        };
        auto writeLine = [&](const QByteArray &bytes, bool flush) {
            if (d->file->write(bytes) != bytes.size()) {
                fail();
                return false;
            }
            d->bytesWritten += bytes.size();
            if (flush && !d->file->flush()) {
                fail();
                return false;
            }
            return true;
        };
        auto writeDuplicateNotice = [&] {
            if (d->pendingDuplicates == 0 || !d->active)
                return;
            DiagnosticEvent notice = makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("session"),
                QStringLiteral("duplicate_events_suppressed"),
                {{QStringLiteral("duplicates_suppressed"), QString::number(d->pendingDuplicates)},
                 {QStringLiteral("component"), d->duplicateComponent},
                 {QStringLiteral("event"), d->duplicateEvent}}, true, event.timeUtc);
            const QByteArray noticeLine = formattedLine(notice, sanitizedFields(notice.fields)).toUtf8();
            if (d->bytesWritten + noticeLine.size() <= d->options.maxBytes)
                writeLine(noticeLine, true);
            else
                d->compactOrSizeSuppressed += d->pendingDuplicates;
            d->pendingDuplicates = 0;
        };

        if (key == d->duplicateKey) {
            ++d->pendingDuplicates;
            ++d->duplicateSuppressed;
            return;
        }
        writeDuplicateNotice();
        if (!d->active)
            return;
        d->duplicateKey = key;
        d->duplicateComponent = component;
        d->duplicateEvent = name;

        if (!d->compact && d->bytesWritten + line.size() > d->options.compactThresholdBytes) {
            d->compact = true;
            DiagnosticEvent notice = makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("session"),
                QStringLiteral("compact_mode_entered"), {}, true, event.timeUtc);
            const QByteArray noticeLine = formattedLine(notice, {}).toUtf8();
            if (d->bytesWritten + noticeLine.size() <= d->options.maxBytes)
                writeLine(noticeLine, true);
        }
        if (!d->active)
            return;
        if (d->compact && (event.severity == DiagnosticSeverity::Debug || event.severity == DiagnosticSeverity::Info)) {
            ++d->compactOrSizeSuppressed;
            return;
        }
        const qint64 ordinaryLimit = d->options.maxBytes - d->options.finalReserveBytes;
        if (d->bytesWritten + line.size() > ordinaryLimit) {
            ++d->compactOrSizeSuppressed;
            return;
        }
        writeLine(line, event.flushImmediately || isBoundary(event.severity));
    }
    if (!failure.isEmpty())
        emit writeFailed(failure);
}

void DiagnosticSession::closeNormally() {
    QString failure;
    {
        QMutexLocker lock(&d->mutex);
        if (!d->active || d->closed)
            return;
        auto fail = [&] {
            d->active = false;
            d->file->close();
            if (!d->failureEmitted) {
                d->failureEmitted = true;
                failure = d->file->errorString();
                if (failure.isEmpty())
                    failure = QStringLiteral("diagnostic log write failed");
            }
        };
        auto writeLine = [&](const QByteArray &bytes) {
            if (d->file->write(bytes) != bytes.size()) {
                fail();
                return false;
            }
            d->bytesWritten += bytes.size();
            return true;
        };
        if (d->pendingDuplicates > 0) {
            DiagnosticEvent notice = makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("session"),
                QStringLiteral("duplicate_events_suppressed"),
                {{QStringLiteral("duplicates_suppressed"), QString::number(d->pendingDuplicates)},
                 {QStringLiteral("component"), d->duplicateComponent},
                 {QStringLiteral("event"), d->duplicateEvent}}, true, d->options.now().toUTC());
            const QByteArray line = formattedLine(notice, sanitizedFields(notice.fields)).toUtf8();
            if (d->bytesWritten + line.size() <= d->options.maxBytes)
                writeLine(line);
            else
                d->compactOrSizeSuppressed += d->pendingDuplicates;
            d->pendingDuplicates = 0;
        }
        if (!d->active)
            return;
        DiagnosticEvent summary = makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("session"),
            QStringLiteral("session_summary"),
            {{QStringLiteral("client_requests"), QString::number(d->clientRequest)},
             {QStringLiteral("critical_events"), QString::number(d->criticals)},
             {QStringLiteral("disconnects"), QString::number(d->disconnects)},
             {QStringLiteral("duplicates_suppressed"), QString::number(d->duplicateSuppressed)},
             {QStringLiteral("errors"), QString::number(d->errors)},
             {QStringLiteral("events_suppressed"), QString::number(d->compactOrSizeSuppressed + d->safeMessageSuppressed)},
             {QStringLiteral("network_changed"), QString::number(d->networkChanged)},
             {QStringLiteral("normal_exit"), QStringLiteral("yes")},
             {QStringLiteral("receiver_connected"), QString::number(d->receiverConnected)},
             {QStringLiteral("receiver_discoverable"), QString::number(d->receiverDiscoverable)},
             {QStringLiteral("resets"), QString::number(d->resets)},
             {QStringLiteral("search_received"), QString::number(d->searchReceived)},
             {QStringLiteral("send_requested"), QString::number(d->sendRequested)},
             {QStringLiteral("service_types"), QString::number(d->serviceTypes)},
             {QStringLiteral("startup_completed"), QString::number(d->startupCompleted)},
             {QStringLiteral("warnings"), QString::number(d->warnings)}}, true, d->options.now().toUTC());
        QByteArray line = formattedLine(summary, sanitizedFields(summary.fields)).toUtf8();
        const qint64 available = d->options.maxBytes - d->bytesWritten;
        if (line.size() > available) {
            summary.fields = {{QStringLiteral("events_suppressed"), QString::number(d->compactOrSizeSuppressed + d->safeMessageSuppressed)},
                              {QStringLiteral("normal_exit"), QStringLiteral("yes")}};
            line = formattedLine(summary, sanitizedFields(summary.fields)).toUtf8();
        }
        if (line.size() <= d->options.maxBytes - d->bytesWritten)
            writeLine(line);
        else
            fail();
        if (d->active && !d->file->flush())
            fail();
        if (d->file)
            d->file->close();
        d->closed = true;
        d->active = false;
    }
    if (!failure.isEmpty())
        emit writeFailed(failure);
}
