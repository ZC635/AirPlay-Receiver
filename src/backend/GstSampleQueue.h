#pragma once

#include <QMutex>
#include <QtTypes>

#include <gst/gst.h>

#include <deque>

class GstSampleQueue
{
public:
    explicit GstSampleQueue(qsizetype capacity);

    // Callers must stop all concurrent operations before destruction.
    ~GstSampleQueue();

    GstSampleQueue(const GstSampleQueue &) = delete;
    GstSampleQueue &operator=(const GstSampleQueue &) = delete;
    GstSampleQueue(GstSampleQueue &&) = delete;
    GstSampleQueue &operator=(GstSampleQueue &&) = delete;

    // Retains one reference on success. Full and invalid queues return immediately.
    bool tryPushBorrowed(GstSample *sample) noexcept;

    // Transfers the queue-owned reference to the caller, or returns nullptr.
    GstSample *tryPopOwned();

    // Clears samples present at this call's lock linearization point. Concurrent
    // producers may successfully add later samples after that point.
    void clear();
    qsizetype size() const;

private:
    const qsizetype m_capacity;
    mutable QMutex m_mutex;
    std::deque<GstSample *> m_samples;
};
