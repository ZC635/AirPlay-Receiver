#pragma once

#include "diagnostics/DiagnosticLogSink.h"

#include <QDateTime>
#include <QObject>
#include <QVector>

#include <functional>
#include <memory>

class DiagnosticSessionStorage;

struct DiagnosticSessionOptions {
    QString applicationDirectory;
    QString activationSource;
    qint64 maxBytes = 20 * 1024 * 1024;
    qint64 compactThresholdBytes = 19 * 1024 * 1024;
    qint64 finalReserveBytes = 64 * 1024;
    int maxSessions = 10;
    std::function<QDateTime()> now = [] { return QDateTime::currentDateTimeUtc(); };
    std::shared_ptr<DiagnosticSessionStorage> storage;
};

struct DiagnosticStoredFile {
    QString fileName;
    QDateTime lastModified;
    bool regularFile = false;
};

class DiagnosticSessionFile {
public:
    virtual ~DiagnosticSessionFile() = default;
    // All implementations must append all bytes or leave the underlying device unchanged.
    virtual qint64 write(const QByteArray &bytes) = 0;
    virtual bool flush() = 0;
    virtual void close() = 0;
    virtual QString errorString() const = 0;
};

class DiagnosticSessionStorage {
public:
    virtual ~DiagnosticSessionStorage() = default;
    virtual QString ensureDirectory(const QString &exactPath) = 0;
    virtual std::unique_ptr<DiagnosticSessionFile> createExclusive(
        const QString &exactPath, QString *error) = 0;
    virtual QVector<DiagnosticStoredFile> list(const QString &exactDirectory) = 0;
    virtual bool remove(const QString &exactPath) = 0;
};

class DiagnosticSession;

struct DiagnosticSessionCreateResult {
    std::unique_ptr<DiagnosticSession> session;
    QString error;
};

class DiagnosticSession final : public QObject, public DiagnosticLogSink {
    Q_OBJECT

public:
    static DiagnosticSessionCreateResult create(DiagnosticSessionOptions options);
    ~DiagnosticSession() override;

    void record(DiagnosticEvent event) override;
    bool isActive() const override;
    QString filePath() const;
    QString writeFailure() const;
    void closeNormally();

signals:
    void writeFailed(QString error);

private:
    DiagnosticSession(DiagnosticSessionOptions options, QString filePath,
                      std::unique_ptr<DiagnosticSessionFile> file);

    class Private;
    std::unique_ptr<Private> d;
};
