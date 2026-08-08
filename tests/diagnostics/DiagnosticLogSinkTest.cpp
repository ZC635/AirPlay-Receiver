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
};

QTEST_GUILESS_MAIN(DiagnosticLogSinkTest)
#include "DiagnosticLogSinkTest.moc"
