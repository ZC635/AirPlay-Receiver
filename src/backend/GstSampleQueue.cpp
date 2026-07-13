#include "backend/GstSampleQueue.h"

#include <QMutexLocker>

GstSampleQueue::GstSampleQueue(qsizetype capacity)
    : m_capacity(capacity)
{
}

GstSampleQueue::~GstSampleQueue()
{
    clear();
}

bool GstSampleQueue::tryPushBorrowed(GstSample *sample) noexcept
{
    if (!sample || m_capacity <= 0) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    if (static_cast<qsizetype>(m_samples.size()) >= m_capacity) {
        return false;
    }

    GstSample *retained = gst_sample_ref(sample);
    try {
        m_samples.push_back(retained);
    } catch (...) {
        // Sample-tap callbacks cannot handle C++ exceptions. Roll back the
        // retained reference and report backpressure instead.
        gst_sample_unref(retained);
        return false;
    }
    return true;
}

GstSample *GstSampleQueue::tryPopOwned()
{
    QMutexLocker locker(&m_mutex);
    if (m_samples.empty()) {
        return nullptr;
    }

    GstSample *sample = m_samples.front();
    m_samples.pop_front();
    return sample;
}

void GstSampleQueue::clear()
{
    std::deque<GstSample *> samples;
    {
        QMutexLocker locker(&m_mutex);
        samples.swap(m_samples);
    }

    for (GstSample *sample : samples) {
        gst_sample_unref(sample);
    }
}

qsizetype GstSampleQueue::size() const
{
    QMutexLocker locker(&m_mutex);
    return static_cast<qsizetype>(m_samples.size());
}
