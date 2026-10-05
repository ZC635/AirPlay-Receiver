#pragma once

#include "diagnostics/DiagnosticSession.h"

namespace DiagnosticTestSupport {

struct FileState {
    QByteArray bytes;
    QString ensureError;
    bool failWrites = false;
    bool failFlush = false;
    int writes = 0;
    int failWriteAt = 0;
    int closes = 0;
};

class MemoryFile final : public DiagnosticSessionFile {
public:
    explicit MemoryFile(std::shared_ptr<FileState> state) : state_(std::move(state)) {}
    qint64 write(const QByteArray &bytes) override {
        ++state_->writes;
        if (state_->failWrites || (state_->failWriteAt > 0 && state_->writes >= state_->failWriteAt))
            return -1;
        state_->bytes += bytes;
        return bytes.size();
    }
    bool flush() override { return !state_->failFlush; }
    void close() override { ++state_->closes; }
    QString errorString() const override { return QStringLiteral("disk full"); }
private:
    std::shared_ptr<FileState> state_;
};

class MemoryStorage final : public DiagnosticSessionStorage {
public:
    std::shared_ptr<FileState> file = std::make_shared<FileState>();
    QString ensureDirectory(const QString &) override { return file->ensureError; }
    std::unique_ptr<DiagnosticSessionFile> createExclusive(const QString &, QString *) override {
        return std::make_unique<MemoryFile>(file);
    }
    QVector<DiagnosticStoredFile> list(const QString &) override { return {}; }
    bool remove(const QString &) override { return true; }
};

} // namespace DiagnosticTestSupport
