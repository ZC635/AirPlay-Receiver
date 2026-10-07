#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "app/LanguageManager.h"
#include "app/UiMessage.h"
#include "diagnostics/DiagnosticSession.h"

namespace {

constexpr const char *probeContext() {
    return "LanguageManagerTest";
}

constexpr const char *probeSource() {
    return "probe source";
}

class CapturingLanguageSink final : public DiagnosticLogSink {
public:
    explicit CapturingLanguageSink(bool active = true) : active_(active) {}
    void record(DiagnosticEvent event) override { events.append(std::move(event)); }
    bool isActive() const override { return active_; }
    QList<DiagnosticEvent> events;

private:
    bool active_;
};

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
    void lifecycleMessagesUseEmbeddedChineseCatalogue_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("chinese");
        QTest::newRow("busy") << "Settings are being applied. Try again after the current operation finishes." << QStringLiteral("正在应用设置。请在当前操作结束后重试。");
        QTest::newRow("reconciliation-unconfirmed") << "Settings were saved, but their final saved state could not be confirmed. Apply again." << QStringLiteral("设置已保存，但无法确认最终保存状态。请重新应用。");
        QTest::newRow("interrupted-saved") << "Settings application was interrupted. Saved changes are kept." << QStringLiteral("设置应用已中断。已保存的改动保留。");
        QTest::newRow("interrupted-before-save") << "Settings application was interrupted before saving." << QStringLiteral("设置应用在保存前中断。");
        QTest::newRow("timing") << "Receiver state changed. Apply again to choose when to apply receiver settings." << QStringLiteral("接收器状态已变化。请重新点击应用，选择接收器设置的应用时机。");
        QTest::newRow("not-applied") << "Receiver settings have not been applied. The saved configuration is kept for the next receiver start." << QStringLiteral("接收器设置尚未应用。已保存配置保留至下次启动接收服务。");
        QTest::newRow("ended-save") << "Receiver settings are saved for the next receiver start." << QStringLiteral("接收器设置已保存，留待下次启动接收服务。");
        QTest::newRow("prepare-unavailable") << "The receiver is unavailable." << QStringLiteral("接收器不可用。");
        QTest::newRow("prepare-not-idle") << "Receiver preparation requires an idle receiver." << QStringLiteral("准备接收器配置需要接收器处于空闲状态。");
        QTest::newRow("prepare-backend-failure") << "Receiver preparation did not complete." << QStringLiteral("接收器配置准备未完成。");
    }
    // Missing embedded translations leave these user-facing lifecycle facts in English.
    void lifecycleMessagesUseEmbeddedChineseCatalogue() {
        QFETCH(QString, source);
        QFETCH(QString, chinese);
        const auto message = UiMessage::translated("SettingsApplyCoordinator", source);
        LanguageManager manager(QCoreApplication::instance());
        QVERIFY(manager.apply("en", QLocale("en-US")));
        QCOMPARE(message.render(), source);
        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(message.render(), chinese);
        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(message.render(), source);
    }
    void startupCacheMessagesTranslateToSimplifiedChinese() {
        LanguageManager manager(QCoreApplication::instance());
        manager.apply("zh-CN");
        QCOMPARE(QCoreApplication::translate("Startup","Checking playback components…"),QStringLiteral("正在检查播放组件…"));
        QCOMPARE(QCoreApplication::translate("Startup","Updating playback component cache…"),QStringLiteral("正在更新播放组件缓存…"));
        const auto notice=QCoreApplication::translate("Startup","Playback component cache was updated, but its validation record could not be saved. The next startup may need to check it again. Reason: %1.").arg(QStringLiteral("原因"));
        QVERIFY(notice.contains(QStringLiteral("缓存已更新")));
        QVERIFY(notice.contains(QStringLiteral("原因")));
        QCOMPARE(QCoreApplication::translate("Startup","Playback component cache could not be updated. Required playback component checks passed, so AirPlay will continue starting. Reason: %1. You can exit this application, redeploy the application folder, and try again.").arg(QStringLiteral("检查超时")),
            QStringLiteral("播放组件缓存未能更新。当前必需播放组件检查已通过，AirPlay 将继续启动。原因：检查超时。可在退出本应用后重新部署应用文件夹，再重试。"));
    }
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

    void embeddedChineseCatalogTranslatesCurrentControls_data() {
        QTest::addColumn<QString>("context");
        QTest::addColumn<QString>("sourceText");
        QTest::addColumn<QString>("expected");

        QTest::newRow("toolbar volume") << "ToolbarWidget" << "Volume"
            << QString::fromUtf8(u8"音量");
        QTest::newRow("toolbar pin") << "ToolbarWidget" << "Pin"
            << QString::fromUtf8(u8"置顶");
        QTest::newRow("toolbar aspect") << "ToolbarWidget" << "Aspect"
            << QString::fromUtf8(u8"宽高比");
        QTest::newRow("toolbar fit") << "ToolbarWidget" << "Fit"
            << QString::fromUtf8(u8"适应窗口");
        QTest::newRow("toolbar settings") << "ToolbarWidget" << "Settings"
            << QString::fromUtf8(u8"设置");
        QTest::newRow("toolbar record") << "ToolbarWidget" << "Record"
            << QString::fromUtf8(u8"录制");
        QTest::newRow("toolbar stop") << "ToolbarWidget" << "Stop"
            << QString::fromUtf8(u8"停止");
        QTest::newRow("toolbar saving") << "ToolbarWidget" << "Saving..."
            << QString::fromUtf8(u8"正在保存…");
        QTest::newRow("toolbar fullscreen") << "ToolbarWidget" << "Fullscreen"
            << QString::fromUtf8(u8"全屏");
        QTest::newRow("toolbar exit fullscreen") << "ToolbarWidget" << "Exit Fullscreen"
            << QString::fromUtf8(u8"退出全屏");
        QTest::newRow("toolbar shortcut tooltip format") << "ToolbarWidget" << "%1: %2"
            << QString::fromUtf8(u8"%1：%2");
        QTest::newRow("settings hover reveal") << "SettingsDialog"
            << "Show hidden toolbar when the pointer reaches the top"
            << QString::fromUtf8(u8"工具栏隐藏时，鼠标移到顶部显示");
        QTest::newRow("settings field hover reveal") << "SettingsFields"
            << "Show hidden toolbar when the pointer reaches the top"
            << QString::fromUtf8(u8"工具栏隐藏时，鼠标移到顶部显示");
    }

    void embeddedChineseCatalogTranslatesCurrentControls() {
        QFETCH(QString, context);
        QFETCH(QString, sourceText);
        QFETCH(QString, expected);

        LanguageManager manager(QCoreApplication::instance());
        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));

        const QByteArray contextUtf8 = context.toUtf8();
        const QByteArray sourceUtf8 = sourceText.toUtf8();
        QCOMPARE(QCoreApplication::translate(contextUtf8.constData(), sourceUtf8.constData()),
                 expected);
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

    void languageApplicationsLogControlledFieldsAndSkipNoops() {
        CapturingLanguageSink sink;
        LanguageManager manager(QCoreApplication::instance());
        manager.setDiagnosticSink(&sink);

        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(sink.events.size(), 1);
        const DiagnosticEvent chinese = sink.events.constFirst();
        QCOMPARE(chinese.name, QStringLiteral("language_applied"));
        QCOMPARE(chinese.severity, DiagnosticSeverity::Info);
        QCOMPARE(chinese.fields.value("language_selection"), QStringLiteral("zh-CN"));
        QCOMPARE(chinese.fields.value("effective_language"), QStringLiteral("zh-CN"));
        QCOMPARE(chinese.fields.value("catalog_id"), QStringLiteral("airplay_zh_CN"));
        QCOMPARE(chinese.fields.value("phase"), QStringLiteral("runtime"));
        QCOMPARE(chinese.fields.value("result"), QStringLiteral("applied"));
        QVERIFY(!chinese.fields.contains("resource_path"));

        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(sink.events.size(), 1);
        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(sink.events.size(), 2);
        QCOMPARE(sink.events.constLast().fields.value("effective_language"), QStringLiteral("en"));
        QCOMPARE(sink.events.constLast().fields.value("catalog_id"), QStringLiteral("none"));
        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(sink.events.size(), 2);

        QVERIFY(manager.apply("Private language value", QLocale("en-US")));
        QCOMPARE(sink.events.size(), 3);
        QCOMPARE(sink.events.constLast().fields.value("language_selection"),
                 QStringLiteral("unsupported"));
        QVERIFY(!sink.events.constLast().fields.values().join(' ').contains("Private"));
        QVERIFY(manager.apply("system", QLocale("en-US")));
        QCOMPARE(sink.events.size(), 4);
        QCOMPARE(sink.events.constLast().fields.value("language_selection"), QStringLiteral("system"));
        QVERIFY(manager.apply("system", QLocale("en-US")));
        QCOMPARE(sink.events.size(), 4);

        manager.setDiagnosticSink(nullptr);
        QVERIFY(manager.apply("en", QLocale("zh-CN")));
        QCOMPARE(sink.events.size(), 4);
    }

    void runtimeCatalogFailuresAreFlushedAndRetriedUntilApplied() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DiagnosticSessionOptions options;
        options.applicationDirectory = directory.path();
        auto created = DiagnosticSession::create(std::move(options));
        QVERIFY2(created.session != nullptr, qPrintable(created.error));
        bool catalogAvailable = false;
        LanguageManager manager(
            QCoreApplication::instance(), nullptr,
            [] { return std::make_unique<QTranslator>(); },
            [&catalogAvailable](QTranslator *translator, const QString &resourcePath) {
                return catalogAvailable && translator->load(resourcePath);
            });
        manager.setDiagnosticSink(created.session.get());

        QVERIFY(!manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(manager.effectiveLanguage(), QStringLiteral("en"));
        QFile flushedLog(created.session->filePath());
        QVERIFY(flushedLog.open(QIODevice::ReadOnly));
        const QByteArray firstFailure = flushedLog.readAll();
        QVERIFY(firstFailure.contains("WARN language translation_load_failed"));
        QVERIFY(firstFailure.contains("language_selection=zh-CN"));
        QVERIFY(firstFailure.contains("effective_language=en"));
        QVERIFY(firstFailure.contains("catalog_id=airplay_zh_CN"));
        QVERIFY(firstFailure.contains("phase=runtime"));
        QVERIFY(firstFailure.contains("result=fallback"));
        QVERIFY(!firstFailure.contains(":/i18n/"));
        flushedLog.close();

        QVERIFY(!manager.apply("zh-CN", QLocale("en-US")));
        catalogAvailable = true;
        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(manager.effectiveLanguage(), QStringLiteral("zh-CN"));
        QVERIFY(manager.apply("zh-CN", QLocale("en-US")));
        created.session->closeNormally();

        QFile finalLog(created.session->filePath());
        QVERIFY(finalLog.open(QIODevice::ReadOnly));
        const QByteArray output = finalLog.readAll();
        QCOMPARE(output.count("WARN language translation_load_failed"), 1);
        QVERIFY(output.contains("session duplicate_events_suppressed"));
        QVERIFY(output.contains("event=translation_load_failed"));
        QVERIFY(output.contains("count=1"));
        QCOMPARE(output.count("INFO language language_applied"), 1);
        QVERIFY(output.contains("effective_language=zh-CN"));
        QVERIFY(output.contains("result=applied"));
    }

    void inactiveSinkDoesNotReceiveEventsOrChangeLanguageApply_data() {
        QTest::addColumn<bool>("catalogAvailable");
        QTest::addColumn<QString>("expectedEffective");
        QTest::newRow("successful application") << true << "zh-CN";
        QTest::newRow("English fallback") << false << "en";
    }

    void inactiveSinkDoesNotReceiveEventsOrChangeLanguageApply() {
        QFETCH(bool, catalogAvailable);
        QFETCH(QString, expectedEffective);
        CapturingLanguageSink sink(false);
        LanguageManager manager(QCoreApplication::instance(), nullptr,
                                [] { return std::make_unique<QTranslator>(); },
                                [catalogAvailable](QTranslator *translator, const QString &resourcePath) {
            return catalogAvailable && translator->load(resourcePath);
        });
        manager.setDiagnosticSink(&sink);

        QCOMPARE(manager.apply("zh-CN", QLocale("en-US")), catalogAvailable);
        QCOMPARE(manager.selection(), QStringLiteral("zh-CN"));
        QCOMPARE(manager.effectiveLanguage(), expectedEffective);
        QVERIFY(sink.events.isEmpty());
    }
};

QTEST_GUILESS_MAIN(LanguageManagerTest)
#include "LanguageManagerTest.moc"
