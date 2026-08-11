#include <QtTest>

#include "diagnostics/DiagnosticSanitizer.h"
#include "diagnostics/ThirdPartyDiagnosticTranslator.h"

class ThirdPartyDiagnosticTranslatorTest final : public QObject {
    Q_OBJECT

private slots:
    void translatesReviewedSafePatterns();
    void translatesAllReviewedFixedPatterns();
    void codecEventsUseOnlyNonSensitiveEnumeratedFields();
    void rejectsRawSecretsAndPayloads();
    void rejectsMultilineOversizedAndUnknownMessages();
};

void ThirdPartyDiagnosticTranslatorTest::translatesReviewedSafePatterns() {
    const auto ua = ThirdPartyDiagnosticTranslator::translate(
        6, "Client identified as User-Agent: AirPlay/550.10");
    QVERIFY(ua.has_value());
    QCOMPARE(ua->name, QString("client_software"));
    const QMap<QString, QString> expectedUserAgent = {{"product", "AirPlay"}, {"version", "550.10"}};
    QCOMPARE(ua->fields, expectedUserAgent);
    QCOMPARE(ua->severity, DiagnosticSeverity::Info);

    const auto remote = ThirdPartyDiagnosticTranslator::translate(6, "Remote: 192.168.1.88");
    QVERIFY(remote.has_value());
    QCOMPARE(remote->name, QString("connection_source"));
    QCOMPARE(remote->fields.value("address_family"), QString("ipv4"));
    QVERIFY(!remote->fields.value("prefix").contains(".88"));

    const auto upstreamLocal = ThirdPartyDiagnosticTranslator::translate(6, "Local : 192.168.1.10");
    QVERIFY(upstreamLocal.has_value());
    QCOMPARE(upstreamLocal->name, QString("connection_source"));
    QCOMPARE(upstreamLocal->fields.value("endpoint"), QString("local"));
    QCOMPARE(upstreamLocal->fields.value("address_family"), QString("ipv4"));
    QVERIFY(!upstreamLocal->fields.value("prefix").contains(".10"));

    const auto pairing = ThirdPartyDiagnosticTranslator::translate(6, "Client authentication success");
    QVERIFY(pairing.has_value());
    QCOMPARE(pairing->fields.value("result"), QString("success"));
}

void ThirdPartyDiagnosticTranslatorTest::translatesAllReviewedFixedPatterns() {
    const auto local = ThirdPartyDiagnosticTranslator::translate(7, "Local: fd12:3456:789a:bcde::42");
    QVERIFY(local.has_value());
    QCOMPARE(local->fields.value("address_family"), QString("ipv6"));
    QCOMPARE(local->severity, DiagnosticSeverity::Debug);

    const auto setup = ThirdPartyDiagnosticTranslator::translate(6, "Pairing setup success");
    QVERIFY(setup.has_value());
    const QMap<QString, QString> expectedSetup = {{"phase", "setup"}, {"result", "success"}};
    QCOMPARE(setup->fields, expectedSetup);

    const auto verify = ThirdPartyDiagnosticTranslator::translate(4, "Pairing verify failure");
    QVERIFY(verify.has_value());
    const QMap<QString, QString> expectedVerify = {{"phase", "verify"}, {"result", "failure"}};
    QCOMPARE(verify->fields, expectedVerify);
    QCOMPARE(verify->severity, DiagnosticSeverity::Warning);

    const auto authentication = ThirdPartyDiagnosticTranslator::translate(3, "Pairing authentication success");
    QVERIFY(authentication.has_value());
    const QMap<QString, QString> expectedAuthentication = {
        {"phase", "authentication"}, {"result", "success"}};
    QCOMPARE(authentication->fields, expectedAuthentication);

    const auto audio = ThirdPartyDiagnosticTranslator::translate(6, "Audio renderer start");
    QVERIFY(audio.has_value());
    const QMap<QString, QString> expectedAudio = {{"renderer", "audio"}};
    QCOMPARE(audio->fields, expectedAudio);

    const auto video = ThirdPartyDiagnosticTranslator::translate(6, "Video renderer start");
    QVERIFY(video.has_value());
    const QMap<QString, QString> expectedVideo = {{"renderer", "video"}};
    QCOMPARE(video->fields, expectedVideo);

    const auto codec = ThirdPartyDiagnosticTranslator::translate(6, "Video codec: H264");
    QVERIFY(codec.has_value());
    const QMap<QString, QString> expectedCodec = {{"codec", "h264"}, {"stream_type", "video"}};
    QCOMPARE(codec->fields, expectedCodec);

    const auto socket = ThirdPartyDiagnosticTranslator::translate(3, "Socket error: 104");
    QVERIFY(socket.has_value());
    QCOMPARE(socket->name, QString("socket_error"));
    const QMap<QString, QString> expectedSocket = {{"code", "104"}};
    QCOMPARE(socket->fields, expectedSocket);
    QCOMPARE(socket->severity, DiagnosticSeverity::Error);
}

void ThirdPartyDiagnosticTranslatorTest::codecEventsUseOnlyNonSensitiveEnumeratedFields() {
    const auto codec = ThirdPartyDiagnosticTranslator::translate(6, "Video codec: H264");
    QVERIFY(codec.has_value());
    for (auto it = codec->fields.cbegin(); it != codec->fields.cend(); ++it)
        QVERIFY(!DiagnosticSanitizer::isSensitiveField(it.key()));
    QCOMPARE(codec->fields.value("stream_type"), QString("video"));
    QVERIFY(QStringList({"h264", "h265"}).contains(codec->fields.value("codec")));
}

void ThirdPartyDiagnosticTranslatorTest::rejectsRawSecretsAndPayloads() {
    const QStringList denied = {
        "eiv:\n00 11 22", "ekey:\naa bb", "16 byte aeskey: deadbeef",
        "request header: Authorization: Bearer secret",
        "X-Apple-Session-ID has changed: was:abc now:def",
        "raop_rtp_mirror h264 SPS+PPS header:\n00 00 01",
        "<plist><key>title</key><string>Private Song</string></plist>"
    };
    for (const QString &message : denied) {
        QVERIFY(!ThirdPartyDiagnosticTranslator::translate(7, message.toUtf8()).has_value());
        QVERIFY(!ThirdPartyDiagnosticTranslator::translate(6, message.toUtf8()).has_value());
    }
}

void ThirdPartyDiagnosticTranslatorTest::rejectsMultilineOversizedAndUnknownMessages() {
    QVERIFY(!ThirdPartyDiagnosticTranslator::translate(6, "Local: 192.168.1.8\nbody").has_value());
    QVERIFY(!ThirdPartyDiagnosticTranslator::translate(6, QByteArray(513, 'x')).has_value());
    QVERIFY(!ThirdPartyDiagnosticTranslator::translate(6, QByteArray("Audio renderer start\0body", 25)).has_value());
    QVERIFY(!ThirdPartyDiagnosticTranslator::translate(3, "unexpected socket failure 104").has_value());
}

QTEST_GUILESS_MAIN(ThirdPartyDiagnosticTranslatorTest)
#include "ThirdPartyDiagnosticTranslatorTest.moc"
