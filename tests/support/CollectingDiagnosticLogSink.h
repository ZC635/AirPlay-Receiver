#pragma once

#include "diagnostics/DiagnosticLogSink.h"

#include <QStringList>
#include <QVector>

#include <algorithm>

class CollectingDiagnosticLogSink final : public DiagnosticLogSink {
public:
    void record(DiagnosticEvent event) override { events.append(std::move(event)); }
    bool isActive() const override { return true; }

    QVector<DiagnosticEvent> events;
};

using CollectingSink = CollectingDiagnosticLogSink;

inline int countEvents(const CollectingSink &sink, const QString &name) {
    return std::count_if(sink.events.cbegin(), sink.events.cend(),
                         [&](const DiagnosticEvent &event) { return event.name == name; });
}

inline bool hasEvent(const CollectingSink &sink, const QString &name) {
    return countEvents(sink, name) > 0;
}

inline DiagnosticEvent findEvent(const CollectingSink &sink, const QString &name) {
    const auto it = std::find_if(sink.events.cbegin(), sink.events.cend(),
                                 [&](const DiagnosticEvent &event) { return event.name == name; });
    return it == sink.events.cend() ? DiagnosticEvent{} : *it;
}

inline QString joinedFields(const CollectingSink &sink) {
    QStringList values;
    for (const auto &event : sink.events) {
        values.append(event.fields.values());
    }
    return values.join('|');
}
