#include <QtTest>

#include "diagnostics/DiagnosticSanitizer.h"

class DiagnosticSanitizerTest final : public QObject {
    Q_OBJECT

private slots:
    void sensitiveTextNeverSurvives();
    void sensitiveTextNeverSurvives_data();
    void maskedAddressPreservesOnlyPrivatePrefixes();
    void controlCharactersAreEscaped();
    void sensitiveFieldNamesAreRejected();
    void safeClientModelAcceptsOnlyRestrictedCharacters();
    void safeProductVersionRequiresOneCompleteToken();
};

void DiagnosticSanitizerTest::sensitiveTextNeverSurvives() {
    QFETCH(QString, input);
    QFETCH(QStringList, forbidden);

    const QString output = DiagnosticSanitizer::sanitizeText(input);
    QVERIFY(!output.contains('\n'));
    QVERIFY(!output.contains('\r'));
    for (const QString &token : forbidden)
        QVERIFY(!output.contains(token, Qt::CaseInsensitive));
}

void DiagnosticSanitizerTest::sensitiveTextNeverSurvives_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QStringList>("forbidden");

    QTest::newRow("ipv4") << "peer=192.168.4.37 public=8.8.8.8"
                           << QStringList{"192.168.4.37", "8.8.8.8"};
    QTest::newRow("ipv6") << "fe80::abcd:1234:5678:9abc" << QStringList{"abcd", "1234"};
    QTest::newRow("path") << "C:\\Users\\Alice\\Videos\\capture.mp4"
                           << QStringList{"Alice", "capture.mp4"};
    QTest::newRow("mac-device") << "AA:BB:CC:DD:EE:FF deviceid=11:22:33:44:55:66"
                                 << QStringList{"AA:BB", "11:22"};
    QTest::newRow("headers") << "Authorization: Bearer secret\r\nX-Session-ID: abc\nFAKE event"
                              << QStringList{"secret", "abc", "FAKE event"};
    QTest::newRow("keys") << "eiv=001122 ekey=aabbcc aeskey=deadbeef public_key=cafe"
                           << QStringList{"001122", "aabbcc", "deadbeef", "cafe"};
}

void DiagnosticSanitizerTest::maskedAddressPreservesOnlyPrivatePrefixes() {
    QCOMPARE(DiagnosticSanitizer::maskedAddress(QHostAddress("192.168.1.37"), 24),
             QString("192.168.1.xxx/24"));
    QCOMPARE(DiagnosticSanitizer::maskedAddress(QHostAddress("fd12:3456:789a:bcde::42"), 64),
             QString("fd12:3456:789a:bcde:xxxx/64"));
    QCOMPARE(DiagnosticSanitizer::maskedAddress(QHostAddress("8.8.8.8"), 24),
             QString("public_ipv4"));
    QCOMPARE(DiagnosticSanitizer::maskedAddress(QHostAddress("2001:4860:4860::8888"), 64),
             QString("public_ipv6"));
}

void DiagnosticSanitizerTest::controlCharactersAreEscaped() {
    QCOMPARE(DiagnosticSanitizer::sanitizeText(QStringLiteral("left\n\r\t\x01right")),
             QString("left\\n\\r\\t\\x01right"));
}

void DiagnosticSanitizerTest::sensitiveFieldNamesAreRejected() {
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("ssid")));
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("client_name")));
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("X-Session-ID")));
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("media_metadata")));
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("request_headers")));
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("remote_address")));
    QVERIFY(DiagnosticSanitizer::isSensitiveField(QStringLiteral("device_uuid")));
    QVERIFY(!DiagnosticSanitizer::isSensitiveField(QStringLiteral("renderer_count")));
    QVERIFY(DiagnosticSanitizer::sanitizeFieldName(QStringLiteral("device_id")).isEmpty());
    QCOMPARE(DiagnosticSanitizer::sanitizeFieldName(QStringLiteral("Renderer Count")),
             QString("renderer_count"));
}

void DiagnosticSanitizerTest::safeClientModelAcceptsOnlyRestrictedCharacters() {
    QVERIFY(DiagnosticSanitizer::safeClientModel(QStringLiteral("iPhone15,3")) ==
            std::optional<QString>(QString("iPhone15,3")));
    QVERIFY(!DiagnosticSanitizer::safeClientModel(QStringLiteral("Alice's iPhone")).has_value());
    QVERIFY(!DiagnosticSanitizer::safeClientModel(QString(65, QLatin1Char('a'))).has_value());
}

void DiagnosticSanitizerTest::safeProductVersionRequiresOneCompleteToken() {
    const auto expected = std::optional<QPair<QString, QString>>(
        qMakePair(QString("AirPlay"), QString("550.10")));
    QVERIFY(DiagnosticSanitizer::safeProductVersion(QStringLiteral("AirPlay/550.10")) == expected);
    QVERIFY(!DiagnosticSanitizer::safeProductVersion(QStringLiteral("AirPlay/550.10 (Alice)")).has_value());
    QVERIFY(!DiagnosticSanitizer::safeProductVersion(QStringLiteral("Air Play/550.10")).has_value());
}

QTEST_GUILESS_MAIN(DiagnosticSanitizerTest)
#include "DiagnosticSanitizerTest.moc"
