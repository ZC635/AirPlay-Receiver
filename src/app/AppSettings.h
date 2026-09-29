#pragma once

#include <QHash>
#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <QVector>
#include "app/ShortcutAction.h"
#include "app/ShortcutBinding.h"
#include "backend/RecordingTypes.h"
#include "backend/VideoQualitySettings.h"

class AppKeySequence : public QKeySequence {
public:
    AppKeySequence() = default;
    AppKeySequence(const QKeySequence &sequence) : QKeySequence(sequence) {}

    bool isValid() const;
};

class AppSettings {
public:
    static AppSettings defaults();

    QVector<ShortcutBinding> shortcuts() const;
    AppKeySequence shortcutFor(ShortcutAction action) const;
    void setShortcut(ShortcutAction action, const QKeySequence &sequence);
    int volume() const;
    void setVolume(int value);
    QString receiverName() const;
    void setReceiverName(QString name);
    QString language() const;
    void setLanguage(QString language);
    bool aspectRatioLock() const;
    void setAspectRatioLock(bool enabled);
    bool videoFitMode() const;
    void setVideoFitMode(bool enabled);
    VideoQualitySettings videoQuality() const;
    void setVideoQuality(VideoQualitySettings quality);
    RecordingFormat recordingFormat() const;
    void setRecordingFormat(RecordingFormat format);
    QString recordingOutputDirectory() const;
    void setRecordingOutputDirectory(QString path);
    bool showRecordingCompletionMessage() const;
    void setShowRecordingCompletionMessage(bool enabled);
    QStringList validateGeneral() const;
    QStringList validateShortcuts() const;

private:
    QHash<int, QKeySequence> shortcuts_;
    int volume_ = 100;
    QString receiverName_ = "AirPlay Receiver";
    QString language_ = QStringLiteral("system");
    bool aspectRatioLock_ = true;
    bool videoFitMode_ = true;
    VideoQualitySettings videoQuality_;
    RecordingFormat recordingFormat_ = RecordingFormat::Mp4;
    QString recordingOutputDirectory_;
    bool showRecordingCompletionMessage_ = true;
};
