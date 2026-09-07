#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QSignalSpy>

#include <memory>

#include "app/LanguageManager.h"

namespace {

constexpr const char *probeContext() {
    return "LanguageManagerTest";
}

constexpr const char *probeSource() {
    return "probe source";
}

} // namespace

class ProbeTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation)
        Q_UNUSED(n)
        if (qstrcmp(context, probeContext()) == 0
            && qstrcmp(sourceText, probeSource()) == 0) {
            return QStringLiteral("probe translation");
        }
        return {};
    }
};

class LanguageManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void resolveEffectiveLanguage_data() {
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("systemLocale");
        QTest::addColumn<QString>("expected");

        QTest::newRow("system English") << "system" << "en-US" << "en";
        QTest::newRow("system China") << "system" << "zh-CN" << "zh-CN";
        QTest::newRow("system Singapore") << "system" << "zh-SG" << "zh-CN";
        QTest::newRow("system simplified script") << "system" << "zh-Hans" << "zh-CN";
        QTest::newRow("simplified script overrides Hong Kong") << "system" << "zh-Hans-HK" << "zh-CN";
        QTest::newRow("traditional script overrides China") << "system" << "zh-Hant-CN" << "en";
        QTest::newRow("traditional script overrides Singapore") << "system" << "zh-Hant-SG" << "en";
        QTest::newRow("system Taiwan") << "system" << "zh-TW" << "en";
        QTest::newRow("system French") << "system" << "fr-FR" << "en";
        QTest::newRow("explicit English") << "en" << "zh-CN" << "en";
        QTest::newRow("explicit simplified Chinese") << "zh-CN" << "en-US" << "zh-CN";
        QTest::newRow("unsupported selection") << "fr" << "zh-CN" << "en";
        QTest::newRow("underscore identifier is unsupported") << "zh_CN" << "zh-CN" << "en";
        QTest::newRow("uppercase identifier is unsupported") << "ZH-CN" << "zh-CN" << "en";
    }

    void resolveEffectiveLanguage() {
        QFETCH(QString, selection);
        QFETCH(QString, systemLocale);
        QFETCH(QString, expected);

        QCOMPARE(LanguageManager::resolveEffectiveLanguage(selection, QLocale(systemLocale)), expected);
    }

    void supportedLanguagesDescribeCatalog() {
        const QVector<LanguageOption> languages = LanguageManager::supportedLanguages();

        QCOMPARE(languages.size(), 2);
        QCOMPARE(languages.at(0).id, QString("en"));
        QCOMPARE(languages.at(0).nativeName, QString("English"));
        QCOMPARE(languages.at(0).resourcePath, QString());
        QCOMPARE(languages.at(1).id, QString("zh-CN"));
        QCOMPARE(languages.at(1).nativeName, QString::fromUtf8(u8"简体中文"));
        QCOMPARE(languages.at(1).resourcePath, QString(":/i18n/airplay_zh_CN.qm"));
    }

    void applyFallsBackWhenChineseTranslatorResourceIsMissing() {
        LanguageManager manager(
            QCoreApplication::instance(),
            nullptr,
            [] { return std::make_unique<QTranslator>(); },
            [](QTranslator *, const QString &) { return false; });
        QSignalSpy loadFailures(&manager, &LanguageManager::translationLoadFailed);

        QVERIFY(!manager.apply("zh-CN", QLocale("en-US")));

        QCOMPARE(loadFailures.count(), 1);
        QCOMPARE(manager.selection(), QString("zh-CN"));
        QCOMPARE(manager.effectiveLanguage(), QString("en"));
        QCOMPARE(loadFailures.at(0).at(0).toString(), QString("zh-CN"));
        QCOMPARE(loadFailures.at(0).at(1).toString(), QString(":/i18n/airplay_zh_CN.qm"));
    }

    void applyLoadsEmbeddedChineseResourceAndTranslatesProbe() {
        LanguageManager manager(QCoreApplication::instance());

        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(manager.effectiveLanguage(), QString("zh-CN"));
        QCOMPARE(QCoreApplication::translate("LanguageManager", "Translation loaded"),
                 QString::fromUtf8(u8"翻译已加载"));

        QVERIFY(manager.apply("en", QLocale("en-US")));
        QCOMPARE(QCoreApplication::translate("LanguageManager", "Translation loaded"),
                 QString("Translation loaded"));
    }

    void destroyManagerRemovesInstalledTranslator() {
        auto *application = QCoreApplication::instance();
        const auto factory = [] { return std::make_unique<ProbeTranslator>(); };
        const auto loader = [](QTranslator *, const QString &) { return true; };

        {
            LanguageManager manager(application, nullptr, factory, loader);
            QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
            QCOMPARE(QCoreApplication::translate(probeContext(), probeSource()),
                     QString("probe translation"));
        }

        QCOMPARE(QCoreApplication::translate(probeContext(), probeSource()),
                 QString("probe source"));
    }

    void applyWithNullApplicationFailsSafelyAndKeepsEnglishFallback() {
        LanguageManager manager(nullptr);
        QSignalSpy loadFailures(&manager, &LanguageManager::translationLoadFailed);

        QVERIFY(!manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(loadFailures.count(), 1);
        QCOMPARE(manager.selection(), QString("zh-CN"));
        QCOMPARE(manager.effectiveLanguage(), QString("en"));

        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(manager.selection(), QString("en"));
        QCOMPARE(manager.effectiveLanguage(), QString("en"));
    }

    void applyOnlySignalsWhenSelectionOrEffectiveLanguageChanges() {
        LanguageManager manager(QCoreApplication::instance());
        QSignalSpy languageChanges(&manager, &LanguageManager::languageChanged);

        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(languageChanges.count(), 1);
        QCOMPARE(manager.selection(), QString("en"));
        QCOMPARE(manager.effectiveLanguage(), QString("en"));

        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(languageChanges.count(), 1);

        QVERIFY(manager.apply("system", QLocale("en-US")));
        QCOMPARE(languageChanges.count(), 2);
        QCOMPARE(languageChanges.at(1).at(0).toString(), QString("system"));
        QCOMPARE(languageChanges.at(1).at(1).toString(), QString("en"));

        QVERIFY(manager.apply("system", QLocale("en-US")));
        QCOMPARE(languageChanges.count(), 2);
    }
};

QTEST_GUILESS_MAIN(LanguageManagerTest)
#include "LanguageManagerTest.moc"
