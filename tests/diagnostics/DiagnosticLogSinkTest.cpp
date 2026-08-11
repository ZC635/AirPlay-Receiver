#include <QtTest>

#include "support/CollectingDiagnosticLogSink.h"

class DiagnosticLogSinkTest final : public QObject {
    Q_OBJECT

private slots:
    void nullSinkAcceptsEventsWithoutState() {
        auto &first = nullDiagnosticLogSink();
        auto &second = nullDiagnosticLogSink();
        QCOMPARE(&first, &second);
        first.record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup",
                                         "startup_completed", {{"result", "yes"}}, true));
        QVERIFY(!first.isActive());
    }

    void eventFactoryUsesUtcAndMarksBoundaries() {
        const QDateTime local(QDate(2026, 8, 8), QTime(15, 30), QTimeZone("Asia/Shanghai"));
        const auto event = makeDiagnosticEvent(DiagnosticSeverity::Warning, "receiver", "failure",
                                               {{"reason", "renderer_init"}}, true, local);
        QCOMPARE(event.timeUtc.timeSpec(), Qt::UTC);
        QCOMPARE(event.timeUtc, local.toUTC());
        QCOMPARE(event.component, QString("receiver"));
        QCOMPARE(event.name, QString("failure"));
        QVERIFY(event.flushImmediately);
    }

    void collectingSinkRetainsEventsAndSupportsQueries() {
        CollectingSink sink;
        sink.record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "started",
                                        {{"result", "yes"}}));
        sink.record(makeDiagnosticEvent(DiagnosticSeverity::Warning, "receiver", "failure",
                                        {{"reason", "renderer_init"}}));

        QCOMPARE(countEvents(sink, "failure"), 1);
        QVERIFY(hasEvent(sink, "failure"));
        QVERIFY(!hasEvent(sink, "missing"));

        const auto failure = findEvent(sink, "failure");
        QVERIFY(failure.severity == DiagnosticSeverity::Warning);
        QCOMPARE(failure.fields.value("reason"), QString("renderer_init"));
        QCOMPARE(joinedFields(sink), QString("yes|renderer_init"));
    }
};

QTEST_GUILESS_MAIN(DiagnosticLogSinkTest)
#include "DiagnosticLogSinkTest.moc"
