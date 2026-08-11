#include "diagnostics/DiagnosticSession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <memory>
#include <thread>

namespace {

QByteArray readAll(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool isInsideLogsDirectory(const QString &path, const QString &logsDirectory) {
    return QDir::fromNativeSeparators(path).startsWith(
        QDir::fromNativeSeparators(logsDirectory) + QLatin1Char('/'));
}

DiagnosticSessionOptions testOptions(const QString &appDir) {
    DiagnosticSessionOptions options;
    options.applicationDirectory = appDir;
    options.activationSource = QStringLiteral("command_argument");
    options.maxBytes = 4096;
    options.compactThresholdBytes = 3072;
    options.finalReserveBytes = 512;
    options.maxSessions = 10;
    options.now = [] {
        return QDateTime(QDate(2026, 8, 8), QTime(7, 30, 12, 418), QTimeZone::UTC);
    };
    return options;
}

struct FakeFileState {
    QByteArray bytes;
    int writes = 0;
    int closes = 0;
    int failWriteAt = 0;
    int shortWriteAt = 0;
    int partialBytesAttempted = 0;
    int rollbacks = 0;
    bool flushSucceeds = true;
    bool clearErrorOnClose = false;
    QString error;
    QString writeError = QStringLiteral("write failure");
};

class FakeSessionFile final : public DiagnosticSessionFile {
public:
    explicit FakeSessionFile(std::shared_ptr<FakeFileState> state) : m_state(std::move(state)) {}

    qint64 write(const QByteArray &bytes) override {
        ++m_state->writes;
        if (m_state->failWriteAt != 0 && m_state->writes >= m_state->failWriteAt) {
            m_state->error = m_state->writeError;
            return -1;
        }
        if (m_state->shortWriteAt != 0 && m_state->writes >= m_state->shortWriteAt) {
            m_state->error = m_state->writeError;
            const qsizetype originalSize = m_state->bytes.size();
            const int partialBytes = qMax(1, bytes.size() / 2);
            m_state->bytes.append(bytes.constData(), partialBytes);
            m_state->partialBytesAttempted += partialBytes;
            m_state->bytes.truncate(originalSize);
            ++m_state->rollbacks;
            return partialBytes;
        }
        m_state->bytes.append(bytes);
        return bytes.size();
    }
    bool flush() override {
        if (!m_state->flushSucceeds)
            m_state->error = QStringLiteral("flush failure");
        return m_state->flushSucceeds;
    }
    void close() override {
        ++m_state->closes;
        if (m_state->clearErrorOnClose)
            m_state->error.clear();
    }
    QString errorString() const override { return m_state->error; }

private:
    std::shared_ptr<FakeFileState> m_state;
};

class FakeSessionStorage final : public DiagnosticSessionStorage {
public:
    QString ensureError;
    QString exclusiveError;
    bool removeSucceeds = true;
    bool omitCreatedFromList = false;
    QVector<DiagnosticStoredFile> stored;
    std::shared_ptr<FakeFileState> file = std::make_shared<FakeFileState>();
    QStringList ensuredPaths;
    QStringList createdPaths;
    QStringList removedPaths;

    QString ensureDirectory(const QString &exactPath) override {
        ensuredPaths.append(exactPath);
        return ensureError;
    }
    std::unique_ptr<DiagnosticSessionFile> createExclusive(const QString &exactPath, QString *error) override {
        createdPaths.append(exactPath);
        if (!exclusiveError.isEmpty()) {
            *error = exclusiveError;
            return {};
        }
        return std::make_unique<FakeSessionFile>(file);
    }
    QVector<DiagnosticStoredFile> list(const QString &) override {
        QVector<DiagnosticStoredFile> result = stored;
        if (!omitCreatedFromList)
            for (const QString &path : createdPaths) {
                result.append({QFileInfo(path).fileName(),
                               QDateTime(QDate(2026, 8, 8), QTime(7, 30, 12), QTimeZone::UTC), true});
            }
        return result;
    }
    bool remove(const QString &exactPath) override {
        removedPaths.append(exactPath);
        return removeSucceeds;
    }
};

class DiagnosticSessionTest : public QObject {
    Q_OBJECT

private slots:
    void formatsOneUtcUtf8PhysicalLine();
    void destructorDoesNotInventNormalExit();
    void recordsConcurrentEventsAsCompleteLines();
    void flushesBoundaryEventsImmediately();
    void suppressesConsecutiveDuplicatesAndSummarizesCounts();
    void compactModePreservesHardLimitAndSummary();
    void retainsOnlyMatchingRegularSessionFiles();
    void usesIncrementingSuffixForCollisions();
    void reportsStorageAndHeaderFailuresWithoutFallback();
    void retentionFailureDeletesTheCurrentSession();
    void writeFailureEmitsOnceAndDisablesTheSession();
    void duplicateNoticeFailureEmitsOnceAfterUnlock();
    void compactNoticeFailureEmitsOnceAfterUnlock();
    void closeDuplicateNoticeFailureEmitsOnceAfterUnlock();
    void shortWriteEmitsOnceAndDisablesTheSession();
    void retentionIncludesCurrentWhenStorageOmitsIt();
    void collisionRetryUsesOnlyExactSentinelAndIsBounded();
    void rejectsCompactThresholdInsideSummaryReserve();
    void retentionIncludesMatchingFileWithoutReadableFilter();
    void summaryUsesTheFixedFactContract();
    void reserveKeepsFullSummaryAndBoundsDuplicateNotices();
};

void DiagnosticSessionTest::formatsOneUtcUtf8PhysicalLine() {
    QTemporaryDir dir;
    auto created = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(created.session != nullptr, qPrintable(created.error));
    created.session->record(makeDiagnosticEvent(
        DiagnosticSeverity::Info, QStringLiteral("discovery"), QStringLiteral("service_configured"),
        {{QStringLiteral("type"), QStringLiteral("_airplay._tcp")},
         {QStringLiteral("unsafe"), QStringLiteral("one\ntwo")}}, true,
        QDateTime(QDate(2026, 8, 8), QTime(7, 30, 12, 418), QTimeZone::UTC)));
    const QByteArray bytes = readAll(created.session->filePath());
    QVERIFY(bytes.contains("2026-08-08T07:30:12.418Z INFO discovery service_configured"));
    QVERIFY(bytes.contains("unsafe=one\\ntwo"));
    QCOMPARE(bytes.count('\n'), 2);
}

void DiagnosticSessionTest::destructorDoesNotInventNormalExit() {
    QTemporaryDir dir;
    QString path;
    {
        auto created = DiagnosticSession::create(testOptions(dir.path()));
        QVERIFY(created.session);
        path = created.session->filePath();
    }
    QVERIFY(!readAll(path).contains("normal_exit=yes"));
}

void DiagnosticSessionTest::recordsConcurrentEventsAsCompleteLines() {
    QTemporaryDir dir;
    auto options = testOptions(dir.path());
    options.maxBytes = 64 * 1024;
    options.compactThresholdBytes = 60 * 1024;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    std::vector<std::thread> workers;
    for (int worker = 0; worker != 8; ++worker) {
        workers.emplace_back([session = created.session.get(), worker] {
            for (int item = 0; item != 40; ++item) {
                session->record(makeDiagnosticEvent(
                    DiagnosticSeverity::Warning, QStringLiteral("worker"), QStringLiteral("item"),
                    {{QStringLiteral("worker"), QString::number(worker)},
                     {QStringLiteral("item"), QString::number(item)}}));
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();

    const QList<QByteArray> lines = readAll(created.session->filePath()).split('\n');
    QCOMPARE(lines.size() - 1, 321);
    const QRegularExpression pattern(
        QStringLiteral(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z (?:INFO session session_started activation_source=command_argument|WARN worker item item=\d+ worker=\d+)$)"));
    for (const QByteArray &line : lines) {
        if (!line.isEmpty())
            QVERIFY2(pattern.match(QString::fromUtf8(line)).hasMatch(), line.constData());
    }
}

void DiagnosticSessionTest::flushesBoundaryEventsImmediately() {
    QTemporaryDir dir;
    auto created = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(created.session, qPrintable(created.error));
    for (DiagnosticSeverity severity : {DiagnosticSeverity::Warning, DiagnosticSeverity::Error,
                                        DiagnosticSeverity::Critical}) {
        created.session->record(makeDiagnosticEvent(severity, QStringLiteral("receiver"),
            QStringLiteral("boundary"), {}, false));
        QVERIFY(readAll(created.session->filePath()).contains("receiver boundary"));
    }
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"),
        QStringLiteral("forced"), {}, true));
    QVERIFY(readAll(created.session->filePath()).contains("receiver forced"));
}

void DiagnosticSessionTest::suppressesConsecutiveDuplicatesAndSummarizesCounts() {
    QTemporaryDir dir;
    auto created = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(created.session, qPrintable(created.error));
    for (int i = 0; i < 20; ++i) {
        created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Warning,
            QStringLiteral("receiver"), QStringLiteral("failure"),
            {{QStringLiteral("reason"), QStringLiteral("socket")}}, true));
    }
    created.session->closeNormally();
    const QByteArray log = readAll(created.session->filePath());
    QCOMPARE(log.count("receiver failure"), 1);
    QVERIFY(log.contains("duplicate_events_suppressed"));
    QVERIFY(log.contains("duplicates_suppressed=19"));
    QVERIFY(log.contains("normal_exit=yes"));
}

void DiagnosticSessionTest::compactModePreservesHardLimitAndSummary() {
    QTemporaryDir dir;
    auto options = testOptions(dir.path());
    options.maxBytes = 2048;
    options.compactThresholdBytes = 1348;
    options.finalReserveBytes = 400;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    for (int i = 0; i < 200; ++i) {
        created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info,
            QStringLiteral("load"), QStringLiteral("ordinary"),
            {{QStringLiteral("value"), QString(80, QChar('x'))},
             {QStringLiteral("i"), QString::number(i)}}));
    }
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Error,
        QStringLiteral("receiver"), QStringLiteral("failure"),
        {{QStringLiteral("reason"), QStringLiteral("renderer")}}, true));
    created.session->closeNormally();
    const QFileInfo info(created.session->filePath());
    QVERIFY(info.size() <= options.maxBytes);
    const QByteArray log = readAll(info.filePath());
    QCOMPARE(log.count("compact_mode_entered"), 1);
    QVERIFY(log.contains("events_suppressed="));
    QVERIFY(log.contains("normal_exit=yes"));
}

void DiagnosticSessionTest::retainsOnlyMatchingRegularSessionFiles() {
    QTemporaryDir dir;
    const QString logs = QDir(dir.path()).filePath(QStringLiteral("logs"));
    QVERIFY(QDir().mkpath(logs));
    for (int index = 0; index != 12; ++index) {
        QFile file(QDir(logs).filePath(QStringLiteral("AirPlay-Diagnostic-2026-08-07-0700%1%2.log")
            .arg(index / 10).arg(index % 10)));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    QFile notes(QDir(logs).filePath(QStringLiteral("notes.log")));
    QVERIFY(notes.open(QIODevice::WriteOnly));
    notes.close();
    QFile renamed(QDir(logs).filePath(QStringLiteral("manual-renamed.log")));
    QVERIFY(renamed.open(QIODevice::WriteOnly));
    renamed.close();
    QVERIFY(QDir().mkpath(QDir(logs).filePath(QStringLiteral("AirPlay-Diagnostic-2026-08-07-080000.log"))));

    auto created = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(created.session, qPrintable(created.error));
    const QStringList matching = QDir(logs).entryList(
        {QStringLiteral("AirPlay-Diagnostic-*.log")}, QDir::Files);
    QCOMPARE(matching.size(), 10);
    QVERIFY(QFileInfo::exists(notes.fileName()));
    QVERIFY(QFileInfo::exists(renamed.fileName()));
    QVERIFY(QFileInfo(QDir(logs).filePath(QStringLiteral("AirPlay-Diagnostic-2026-08-07-080000.log"))).isDir());
}

void DiagnosticSessionTest::usesIncrementingSuffixForCollisions() {
    QTemporaryDir dir;
    auto first = DiagnosticSession::create(testOptions(dir.path()));
    auto second = DiagnosticSession::create(testOptions(dir.path()));
    auto third = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(first.session && second.session && third.session, "session creation failed");
    QCOMPARE(QFileInfo(first.session->filePath()).fileName(), QStringLiteral("AirPlay-Diagnostic-2026-08-08-153012.log"));
    QCOMPARE(QFileInfo(second.session->filePath()).fileName(), QStringLiteral("AirPlay-Diagnostic-2026-08-08-153012-01.log"));
    QCOMPARE(QFileInfo(third.session->filePath()).fileName(), QStringLiteral("AirPlay-Diagnostic-2026-08-08-153012-02.log"));
}

void DiagnosticSessionTest::reportsStorageAndHeaderFailuresWithoutFallback() {
    QTemporaryDir dir;
    const QString expectedLogs = QDir(dir.path()).filePath(QStringLiteral("logs"));
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->ensureError = QStringLiteral("ensure failure");
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto ensured = DiagnosticSession::create(options);
    QVERIFY(!ensured.session);
    QCOMPARE(ensured.error, QStringLiteral("ensure failure"));
    QCOMPARE(storage->ensuredPaths, QStringList{expectedLogs});
    QVERIFY(storage->createdPaths.isEmpty());

    storage = std::make_shared<FakeSessionStorage>();
    storage->exclusiveError = QStringLiteral("exclusive failure");
    options.storage = storage;
    auto exclusive = DiagnosticSession::create(options);
    QVERIFY(!exclusive.session);
    QCOMPARE(exclusive.error, QStringLiteral("exclusive failure"));
    QCOMPARE(storage->ensuredPaths, QStringList{expectedLogs});
    QVERIFY(isInsideLogsDirectory(storage->createdPaths.first(), expectedLogs));

    storage = std::make_shared<FakeSessionStorage>();
    storage->file->failWriteAt = 1;
    options.storage = storage;
    auto headerWrite = DiagnosticSession::create(options);
    QVERIFY(!headerWrite.session);
    QCOMPARE(headerWrite.error, QStringLiteral("write failure"));
    QCOMPARE(storage->removedPaths.size(), 1);
    QVERIFY(isInsideLogsDirectory(storage->removedPaths.first(), expectedLogs));

    storage = std::make_shared<FakeSessionStorage>();
    storage->file->flushSucceeds = false;
    options.storage = storage;
    auto headerFlush = DiagnosticSession::create(options);
    QVERIFY(!headerFlush.session);
    QCOMPARE(headerFlush.error, QStringLiteral("flush failure"));
    QCOMPARE(storage->removedPaths.size(), 1);
    QVERIFY(isInsideLogsDirectory(storage->removedPaths.first(), expectedLogs));
}

void DiagnosticSessionTest::retentionFailureDeletesTheCurrentSession() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->removeSucceeds = false;
    for (int index = 0; index != 10; ++index) {
        storage->stored.append({QStringLiteral("AirPlay-Diagnostic-2026-08-07-0700%1%2.log")
                                    .arg(index / 10).arg(index % 10),
                                QDateTime(QDate(2026, 8, 7), QTime(7, 0, index), QTimeZone::UTC), true});
    }
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY(!created.session);
    QCOMPARE(created.error, QStringLiteral("failed to remove old diagnostic log"));
    QCOMPARE(storage->file->closes, 1);
    QVERIFY(storage->removedPaths.size() >= 2);
    QVERIFY(isInsideLogsDirectory(storage->removedPaths.last(),
        QDir(dir.path()).filePath(QStringLiteral("logs"))));
}

void DiagnosticSessionTest::writeFailureEmitsOnceAndDisablesTheSession() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->file->failWriteAt = 2;
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    QSignalSpy failures(created.session.get(), &DiagnosticSession::writeFailed);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info,
        QStringLiteral("receiver"), QStringLiteral("first")));
    QVERIFY(!created.session->isActive());
    QCOMPARE(failures.count(), 1);
    QCOMPARE(storage->file->writes, 2);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info,
        QStringLiteral("receiver"), QStringLiteral("second")));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(storage->file->writes, 2);
}

void DiagnosticSessionTest::duplicateNoticeFailureEmitsOnceAfterUnlock() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->file->failWriteAt = 3;
    storage->file->writeError = QStringLiteral("duplicate notice failure");
    storage->file->clearErrorOnClose = true;
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    QSignalSpy failures(created.session.get(), &DiagnosticSession::writeFailed);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("same")));
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("same")));
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("next")));
    QVERIFY(!created.session->isActive());
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.at(0).at(0).toString(), QStringLiteral("duplicate notice failure"));
    QCOMPARE(storage->file->writes, 3);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("later")));
    QCOMPARE(storage->file->writes, 3);
}

void DiagnosticSessionTest::compactNoticeFailureEmitsOnceAfterUnlock() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->file->failWriteAt = 2;
    storage->file->writeError = QStringLiteral("compact notice failure");
    storage->file->clearErrorOnClose = true;
    auto options = testOptions(dir.path());
    options.compactThresholdBytes = 100;
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    QSignalSpy failures(created.session.get(), &DiagnosticSession::writeFailed);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("ordinary")));
    QVERIFY(!created.session->isActive());
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.at(0).at(0).toString(), QStringLiteral("compact notice failure"));
    QCOMPARE(storage->file->writes, 2);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("later")));
    QCOMPARE(storage->file->writes, 2);
}

void DiagnosticSessionTest::closeDuplicateNoticeFailureEmitsOnceAfterUnlock() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->file->failWriteAt = 3;
    storage->file->writeError = QStringLiteral("close duplicate notice failure");
    storage->file->clearErrorOnClose = true;
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    QSignalSpy failures(created.session.get(), &DiagnosticSession::writeFailed);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("same")));
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("same")));
    created.session->closeNormally();
    QVERIFY(!created.session->isActive());
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.at(0).at(0).toString(), QStringLiteral("close duplicate notice failure"));
    QCOMPARE(storage->file->writes, 3);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("later")));
    QCOMPARE(storage->file->writes, 3);
}

void DiagnosticSessionTest::shortWriteEmitsOnceAndDisablesTheSession() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->file->shortWriteAt = 2;
    storage->file->writeError = QStringLiteral("short write failure");
    storage->file->clearErrorOnClose = true;
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    const QByteArray headerBytes = storage->file->bytes;
    QSignalSpy failures(created.session.get(), &DiagnosticSession::writeFailed);
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("short")));
    QVERIFY(!created.session->isActive());
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.at(0).at(0).toString(), QStringLiteral("short write failure"));
    QCOMPARE(storage->file->writes, 2);
    QVERIFY(storage->file->partialBytesAttempted > 0);
    QCOMPARE(storage->file->rollbacks, 1);
    QCOMPARE(storage->file->bytes, headerBytes);
    QVERIFY(!storage->file->bytes.contains("receiver short"));
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("later")));
    QCOMPARE(storage->file->writes, 2);
}

void DiagnosticSessionTest::retentionIncludesCurrentWhenStorageOmitsIt() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->omitCreatedFromList = true;
    storage->stored = {
        {QStringLiteral("AirPlay-Diagnostic-2026-08-07-070000.log"), QDateTime(QDate(2026, 8, 7), QTime(7, 0), QTimeZone::UTC), true},
        {QStringLiteral("AirPlay-Diagnostic-2026-08-07-070001.log"), QDateTime(QDate(2026, 8, 7), QTime(7, 1), QTimeZone::UTC), true}};
    auto options = testOptions(dir.path());
    options.maxSessions = 2;
    options.storage = storage;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    QCOMPARE(storage->removedPaths.size(), 1);
    QVERIFY(storage->removedPaths.first().endsWith(QStringLiteral("AirPlay-Diagnostic-2026-08-07-070000.log")));
    QVERIFY(!storage->removedPaths.contains(created.session->filePath()));
}

void DiagnosticSessionTest::collisionRetryUsesOnlyExactSentinelAndIsBounded() {
    QTemporaryDir dir;
    auto storage = std::make_shared<FakeSessionStorage>();
    storage->exclusiveError = QStringLiteral("path does not exist");
    auto options = testOptions(dir.path());
    options.storage = storage;
    auto otherError = DiagnosticSession::create(options);
    QVERIFY(!otherError.session);
    QCOMPARE(otherError.error, QStringLiteral("path does not exist"));
    QCOMPARE(storage->createdPaths.size(), 1);

    storage = std::make_shared<FakeSessionStorage>();
    storage->exclusiveError = QStringLiteral("already exists");
    options.storage = storage;
    auto exhausted = DiagnosticSession::create(options);
    QVERIFY(!exhausted.session);
    QCOMPARE(exhausted.error, QStringLiteral("diagnostic log filename collision limit reached"));
    QVERIFY(storage->createdPaths.size() > 1);
    QVERIFY(storage->createdPaths.size() <= 10000);
}

void DiagnosticSessionTest::rejectsCompactThresholdInsideSummaryReserve() {
    QTemporaryDir dir;
    auto options = testOptions(dir.path());
    options.maxBytes = 2048;
    options.finalReserveBytes = 400;
    options.compactThresholdBytes = 1700;
    auto created = DiagnosticSession::create(options);
    QVERIFY(!created.session);
    QCOMPARE(created.error, QStringLiteral("invalid diagnostic session limits"));
}

void DiagnosticSessionTest::retentionIncludesMatchingFileWithoutReadableFilter() {
    QTemporaryDir dir;
    const QString logs = QDir(dir.path()).filePath(QStringLiteral("logs"));
    QVERIFY(QDir().mkpath(logs));
    const QString oldest = QDir(logs).filePath(QStringLiteral("AirPlay-Diagnostic-2026-08-07-070000.log"));
    for (int index = 0; index != 10; ++index) {
        QFile file(QDir(logs).filePath(QStringLiteral("AirPlay-Diagnostic-2026-08-07-07000%1.log").arg(index)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
    }
    QVERIFY(QFile::setPermissions(oldest, QFileDevice::WriteOwner));
    auto created = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(created.session, qPrintable(created.error));
    QVERIFY(!QFileInfo::exists(oldest));
    QCOMPARE(QDir(logs).entryList({QStringLiteral("AirPlay-Diagnostic-*.log")}, QDir::Files).size(), 10);
}

void DiagnosticSessionTest::summaryUsesTheFixedFactContract() {
    QTemporaryDir dir;
    auto created = DiagnosticSession::create(testOptions(dir.path()));
    QVERIFY2(created.session, qPrintable(created.error));
    const auto record = [&](DiagnosticSeverity severity, const QString &component, const QString &name,
                            QMap<QString, QString> fields = {}) {
        created.session->record(makeDiagnosticEvent(severity, component, name, std::move(fields), true));
    };
    record(DiagnosticSeverity::Info, QStringLiteral("app"), QStringLiteral("startup_completed"),
           {{QStringLiteral("result"), QStringLiteral("yes")}});
    record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("state_changed"),
           {{QStringLiteral("to"), QStringLiteral("discoverable")}});
    record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("state_changed"),
           {{QStringLiteral("to"), QStringLiteral("connected")}});
    record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("state_changed"),
           {{QStringLiteral("to"), QStringLiteral("connected")}});
    for (int i = 0; i != 2; ++i)
        record(DiagnosticSeverity::Info, QStringLiteral("discovery"), QStringLiteral("service_configured"),
               {{QStringLiteral("type"), QStringLiteral("_airplay._tcp")}});
    record(DiagnosticSeverity::Info, QStringLiteral("discovery"), QStringLiteral("service_configured"),
           {{QStringLiteral("type"), QStringLiteral("_raop._tcp")}});
    for (int i = 0; i != 3; ++i)
        record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("search_received"));
    for (int i = 0; i != 2; ++i)
        record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("send_requested"));
    for (int i = 0; i != 4; ++i)
        record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("client_request"));
    for (int i = 0; i != 2; ++i)
        record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("disconnect"));
    record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("reset"));
    for (int i = 0; i != 3; ++i)
        record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("network_changed"));
    record(DiagnosticSeverity::Warning, QStringLiteral("receiver"), QStringLiteral("warning"));
    record(DiagnosticSeverity::Error, QStringLiteral("receiver"), QStringLiteral("error"));
    record(DiagnosticSeverity::Critical, QStringLiteral("receiver"), QStringLiteral("critical"));
    record(DiagnosticSeverity::Info, QStringLiteral("third_party"), QStringLiteral("message_suppressed"));
    record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("duplicate"));
    record(DiagnosticSeverity::Info, QStringLiteral("receiver"), QStringLiteral("duplicate"));
    created.session->closeNormally();

    const QByteArray log = readAll(created.session->filePath());
    const QList<QByteArray> lines = log.split('\n');
    QByteArray summary;
    for (const QByteArray &line : lines) {
        if (line.contains("session session_summary"))
            summary = line;
    }
    QVERIFY(!summary.isEmpty());
    for (const QByteArray &field : {"startup_completed=yes", "discoverable=yes", "airplay_providers=2",
                                    "raop_providers=1", "searches=3", "send_requests=2",
                                    "client_requests=4", "connections=2", "disconnects=2", "resets=1",
                                    "network_changes=3", "warnings=1", "errors=2",
                                    "duplicates_suppressed=1", "events_suppressed=1", "compact_mode=no",
                                    "normal_exit=yes"})
        QVERIFY2(summary.contains(field), field.constData());
    QVERIFY(!summary.contains("cause="));
    QVERIFY(!summary.contains("recommend"));
    QVERIFY(!summary.contains("solution="));
    QVERIFY(log.contains("duplicate_events_suppressed"));
    QVERIFY(log.contains("count=1"));
    QVERIFY(log.contains("component=receiver"));
    QVERIFY(log.contains("event=duplicate"));
}

void DiagnosticSessionTest::reserveKeepsFullSummaryAndBoundsDuplicateNotices() {
    QTemporaryDir dir;
    auto options = testOptions(dir.path());
    options.maxBytes = 2048;
    options.compactThresholdBytes = 1000;
    options.finalReserveBytes = 600;
    auto created = DiagnosticSession::create(options);
    QVERIFY2(created.session, qPrintable(created.error));
    for (int i = 0; i != 30; ++i) {
        created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Warning,
            QStringLiteral("load"), QStringLiteral("fill"),
            {{QStringLiteral("i"), QString::number(i)}, {QStringLiteral("value"), QString(70, QLatin1Char('x'))}}));
    }
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Warning,
        QStringLiteral("receiver"), QStringLiteral("duplicate"), {{QStringLiteral("reason"), QStringLiteral("socket")}}));
    created.session->record(makeDiagnosticEvent(DiagnosticSeverity::Warning,
        QStringLiteral("receiver"), QStringLiteral("duplicate"), {{QStringLiteral("reason"), QStringLiteral("socket")}}));
    created.session->closeNormally();

    const QByteArray log = readAll(created.session->filePath());
    QVERIFY(QFileInfo(created.session->filePath()).size() <= options.maxBytes);
    qint64 endingOffset = 0;
    for (const QByteArray &line : log.split('\n')) {
        endingOffset += line.size() + 1;
        if (line.contains("duplicate_events_suppressed")) {
            QVERIFY(line.contains("count=1"));
            QVERIFY(endingOffset <= options.maxBytes - options.finalReserveBytes);
        }
    }
    const QByteArray required[] = {"startup_completed=", "discoverable=", "airplay_providers=",
                                   "raop_providers=", "searches=", "send_requests=", "client_requests=",
                                   "connections=", "disconnects=", "resets=", "network_changes=", "warnings=",
                                   "errors=", "duplicates_suppressed=", "events_suppressed=", "compact_mode=",
                                   "normal_exit=yes"};
    QByteArray summary;
    for (const QByteArray &line : log.split('\n')) {
        if (line.contains("session session_summary"))
            summary = line;
    }
    QVERIFY(!summary.isEmpty());
    for (const QByteArray &field : required)
        QVERIFY2(summary.contains(field), field.constData());
}

} // namespace

QTEST_MAIN(DiagnosticSessionTest)
#include "DiagnosticSessionTest.moc"
