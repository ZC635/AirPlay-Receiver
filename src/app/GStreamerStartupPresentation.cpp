#include "app/GStreamerStartupPresentation.h"
#include "app/UiMessage.h"
#include <QApplication>
#include <QCloseEvent>
#include <QEventLoop>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>
namespace {
QString text(const char *source, QStringList args = {}) {
    return UiMessage::translated(QStringLiteral("Startup"), QString::fromUtf8(source), std::move(args)).render();
}
QString failureText(CacheFailureReason reason) {
    switch(reason) {
    case CacheFailureReason::Busy: return text(QT_TRANSLATE_NOOP("Startup", "another application is updating the cache"));
    case CacheFailureReason::DirectoryNotWritable: return text(QT_TRANSLATE_NOOP("Startup", "the application folder cannot be written to"));
    case CacheFailureReason::TempUnavailable: return text(QT_TRANSLATE_NOOP("Startup", "temporary storage is unavailable"));
    case CacheFailureReason::Timeout: return text(QT_TRANSLATE_NOOP("Startup", "the component check timed out"));
    case CacheFailureReason::InputChanged: return text(QT_TRANSLATE_NOOP("Startup", "application files changed during the check"));
    case CacheFailureReason::RecordSaveFailed: return text(QT_TRANSLATE_NOOP("Startup", "the validation record could not be saved"));
    case CacheFailureReason::PrecommitCleanupFailed: return text(QT_TRANSLATE_NOOP("Startup", "temporary files could not be cleaned up"));
    case CacheFailureReason::OwnershipUnknown: return text(QT_TRANSLATE_NOOP("Startup", "cache file ownership could not be verified"));
    case CacheFailureReason::UnsupportedPublish: return text(QT_TRANSLATE_NOOP("Startup", "the cache could not be replaced safely"));
    case CacheFailureReason::ScanFailed:
    case CacheFailureReason::ValidationFailed:
    case CacheFailureReason::None: return text(QT_TRANSLATE_NOOP("Startup", "the playback component cache could not be verified"));
    }
    return {};
}
class PreparationWindow final : public QWidget {
public:
    explicit PreparationWindow(GStreamerStartupCache &cache) : cache_(cache) {
        setObjectName(QStringLiteral("gstreamerStartupStatus"));
        setWindowTitle(QStringLiteral("AirPlay"));
        auto *layout = new QVBoxLayout(this);
        label = new QLabel(text(QT_TRANSLATE_NOOP("Startup", "Checking playback components…")), this);
        layout->addWidget(label);
        setMinimumWidth(320);
    }
    QLabel *label;
protected:
    void closeEvent(QCloseEvent *event) override { cache_.cancel(); event->accept(); }
    bool eventFilter(QObject *object, QEvent *event) override {
        if(object == qApp && event->type() == QEvent::Quit) { cache_.cancel(); return true; }
        return QWidget::eventFilter(object,event);
    }
private:
    GStreamerStartupCache &cache_;
};
}
GStreamerCacheResult GStreamerStartupPresentation::prepare(GStreamerStartupCache &cache,
                                                         const GStreamerCacheRequest &request) {
    GStreamerCacheResult result;
    const bool quitOnLastWindowClosed = qApp->quitOnLastWindowClosed();
    qApp->setQuitOnLastWindowClosed(false);
    {
        PreparationWindow status(cache);
        qApp->installEventFilter(&status);
        QEventLoop loop;
        bool finished = false;
        const auto completion = QObject::connect(&cache, &GStreamerStartupCache::finished, &loop,
            [&](const GStreamerCacheResult &value) { result=value;finished=true;loop.quit(); });
        const auto progress = QObject::connect(&cache, &GStreamerStartupCache::stageChanged, &status,
            [&](CacheWorkerStage stage) {
                if(stage == CacheWorkerStage::Scan || stage == CacheWorkerStage::Verify || stage == CacheWorkerStage::PrepareCommit)
                    status.label->setText(text(QT_TRANSLATE_NOOP("Startup", "Updating playback component cache…")));
            });
        status.show();
        if(cache.start(request)) {
            // Cancellation completion owns native cleanup; keep Qt signal delivery alive.
            while(!finished) loop.exec();
        }
        QObject::disconnect(completion);
        QObject::disconnect(progress);
        status.hide();
        qApp->removeEventFilter(&status);
    }
    qApp->setQuitOnLastWindowClosed(quitOnLastWindowClosed);
    return result;
}
void GStreamerStartupPresentation::showCacheNoticeOnce(QWidget *owner, const GStreamerCacheResult &result) {
    if(noticeShown_ || !owner || result.cancelled || result.readinessState != ReadinessState::Ready) return;
    const char *source = nullptr;
    if(result.cacheState == CacheState::Updated && result.recordState == RecordState::NotSaved)
        source = QT_TRANSLATE_NOOP("Startup", "Playback component cache was updated, but its validation record could not be saved. The next startup may need to check it again. Reason: %1.");
    else if(result.cacheState == CacheState::RecoveryFailed || result.cacheState == CacheState::RecoverySkipped)
        source = QT_TRANSLATE_NOOP("Startup", "Playback component cache could not be updated. Required playback component checks passed, so AirPlay will continue starting. Reason: %1. You can exit this application, redeploy the application folder, and try again.");
    if(!source) return;
    noticeShown_=true;
    const QString message=text(source,{failureText(result.failureReason)});
    QTimer::singleShot(0,owner,[owner,message] {
        auto *warning = new QMessageBox(QMessageBox::Warning,
            text(QT_TRANSLATE_NOOP("Startup", "Playback component cache")),message,QMessageBox::Ok,owner);
        warning->setObjectName(QStringLiteral("gstreamerCacheNotice"));
        warning->setAttribute(Qt::WA_DeleteOnClose);
        warning->setWindowModality(Qt::NonModal);
        warning->show();
    });
}
