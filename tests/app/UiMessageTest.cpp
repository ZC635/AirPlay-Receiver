#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QTranslator>

#include "app/UiMessage.h"

namespace {

class PrefixTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);

        if (QString::fromUtf8(context) == QStringLiteral("UiMessageTest")) {
            const QString source = QString::fromUtf8(sourceText);
            if (source == QStringLiteral("Pair: %1/%2")) {
                return QStringLiteral("中:%2/%1");
            }
            return QStringLiteral("中:") + source;
        }
        return {};
    }
};

} // namespace

class UiMessageTest : public QObject {
    Q_OBJECT

private slots:
    void translatedMessageRendersAtRenderTime() {
        const UiMessage message = UiMessage::translated(
            QStringLiteral("UiMessageTest"), QStringLiteral("Failure: %1"), {QStringLiteral("raw")});

        QCOMPARE(message.render(), QStringLiteral("Failure: raw"));
        PrefixTranslator translator;
        QCoreApplication::installTranslator(&translator);
        const QString rendered = message.render();
        QCoreApplication::removeTranslator(&translator);

        QCOMPARE(rendered, QStringLiteral("中:Failure: raw"));
        QCOMPARE(message.render(), QStringLiteral("Failure: raw"));
    }

    void translatedMessageSubstitutesMultipleArgumentsAfterTranslation() {
        const UiMessage message = UiMessage::translated(
            QStringLiteral("UiMessageTest"), QStringLiteral("Pair: %1/%2"),
            {QStringLiteral("first"), QStringLiteral("second")});
        PrefixTranslator translator;
        QCoreApplication::installTranslator(&translator);
        const QString rendered = message.render();
        QCoreApplication::removeTranslator(&translator);

        QCOMPARE(rendered, QStringLiteral("中:second/first"));
    }

    void rawMessageRendersUnchanged() {
        const UiMessage message = UiMessage::raw(QStringLiteral("GStreamer error"));
        PrefixTranslator translator;
        QCoreApplication::installTranslator(&translator);
        const QString rendered = message.render();
        QCoreApplication::removeTranslator(&translator);

        QCOMPARE(rendered, QStringLiteral("GStreamer error"));
    }
};

QTEST_MAIN(UiMessageTest)
#include "UiMessageTest.moc"
