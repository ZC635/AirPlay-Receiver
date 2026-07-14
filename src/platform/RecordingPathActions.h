#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

class RecordingPathActions {
public:
    virtual ~RecordingPathActions() = default;

    virtual QString chooseExistingDirectory(QWidget *parent,
                                            const QString &initialDirectory) = 0;
    virtual QString ensureAndOpenDirectory(const QString &directory) = 0;
    virtual QString revealFile(const QString &filePath) = 0;
};

class WindowsRecordingPathActions final : public RecordingPathActions {
public:
    using ProcessLauncher = std::function<bool(const QString &, const QStringList &)>;

    explicit WindowsRecordingPathActions(ProcessLauncher processLauncher = {});

    QString chooseExistingDirectory(QWidget *parent,
                                    const QString &initialDirectory) override;
    QString ensureAndOpenDirectory(const QString &directory) override;
    QString revealFile(const QString &filePath) override;

private:
    ProcessLauncher processLauncher_;
};
