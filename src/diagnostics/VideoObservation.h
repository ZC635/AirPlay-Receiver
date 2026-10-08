#pragma once

#include "diagnostics/DiagnosticLogSink.h"
#include <array>
#include <atomic>
#include <mutex>

// IDs describe when a checkpoint was observed, never the origin of queued pixels.
// The sink must outlive this context, as with the existing receiver diagnostic sink.
class VideoObservation final {
public:
    enum Stage { Input, AppSink, SourceRejected, BridgeSample, BridgeRejected,
                 BridgeAccepted, ReceiverQueued, QtReceived, CacheCommit,
                 ImagePaint, EmptyPaint, StageCount };
    explicit VideoObservation(DiagnosticLogSink *sink) : m_sink(sink) {}
    bool active() const noexcept {
        return !m_failed.load() && m_sink && m_sink->isActive();
    }
    void serviceBegin() {
        if (!active()) return;
        std::lock_guard lock(m_mutex);
        summary("service_replaced");
        ++m_service;
        open("preconnection");
    }
    void connectionBegin() {
        if (!active()) return;
        std::lock_guard lock(m_mutex);
        summary("connection_begin");
        open("connection");
    }
    void close(const char *reason) {
        if (!active()) return;
        std::lock_guard lock(m_mutex);
        summary(reason);
        m_cycle = 0;
        m_kind = "unassigned";
        m_counts.fill(0);
    }
    void observe(Stage stage, const char *reason = "none") {
        if (!active()) return;
        std::lock_guard lock(m_mutex);
        if (++m_counts[stage] != 1) return;
        auto fields = identity();
        fields.insert("reason", QString::fromLatin1(reason));
        send(QStringLiteral("video_first_") + QString::fromLatin1(names()[stage]), std::move(fields));
    }
    // Callback is evaluated only when logging is active; callers can put state queries inside.
    template<class Snapshot> void boundary(const char *name, Snapshot snapshot) {
        if (!active()) return;
        std::lock_guard lock(m_mutex);
        auto fields = snapshot();
        const auto ids = identity();
        for (auto it = ids.cbegin(); it != ids.cend(); ++it) fields.insert(it.key(), it.value());
        fields.insert("boundary", QString::fromLatin1(name));
        send("video_boundary", std::move(fields));
    }
private:
    static const std::array<const char *, StageCount> &names() {
        static const std::array<const char *, StageCount> value = {
            "input", "appsink", "source_rejected", "bridge_sample", "bridge_rejected",
            "bridge_accepted", "receiver_queued", "qt_received", "cache_commit",
            "image_paint", "empty_paint"};
        return value;
    }
    QMap<QString, QString> identity() const {
        return {{"service_observation", QString::number(m_service)},
                {"cycle_observation", QString::number(m_cycle)},
                {"cycle_kind", QString::fromLatin1(m_kind)},
                {"attribution", "observation_time_pixel_origin_unknown"}};
    }
    void open(const char *kind) {
        m_cycle = ++m_nextCycle;
        m_kind = kind;
        m_counts.fill(0);
        send("video_cycle_begin", identity());
    }
    void summary(const char *reason) {
        // Include every stage even if the cycle had no data.
        auto fields = identity();
        fields.insert("reason", QString::fromLatin1(reason));
        for (int i = 0; i < StageCount; ++i)
            fields.insert(QString::fromLatin1(names()[i]), QString::number(m_counts[i]));
        send("video_cycle_summary", std::move(fields));
    }
    void send(QString name, QMap<QString, QString> fields) noexcept {
        if (!active()) return;
        try {
            m_sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "video_observation",
                                               std::move(name), std::move(fields), true));
        } catch (...) { m_failed.store(true); }
    }
    DiagnosticLogSink *m_sink = nullptr;
    std::atomic_bool m_failed = false;
    std::mutex m_mutex;
    quint64 m_service = 0;
    quint64 m_cycle = 0;
    quint64 m_nextCycle = 0;
    const char *m_kind = "unassigned";
    std::array<quint64, StageCount> m_counts{};
};
