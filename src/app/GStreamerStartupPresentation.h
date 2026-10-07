#pragma once
#include "platform/GStreamerStartupCache.h"
class QWidget;
class GStreamerStartupPresentation {
public:
    GStreamerCacheResult prepare(GStreamerStartupCache &, const GStreamerCacheRequest &);
    void showCacheNoticeOnce(QWidget *, const GStreamerCacheResult &);
private:
    bool noticeShown_ = false;
};
