#include "app/LanguageManager.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

#include <utility>

namespace {

[[maybe_unused]] constexpr auto kTranslationLoadProbe =
    QT_TRANSLATE_NOOP("LanguageManager", "Translation loaded");

QString normalizedSelection(const QString &selection) {
    const QString normalized = selection.trimmed();
    return normalized.isEmpty() ? QStringLiteral("system") : normalized;
}

} // namespace

LanguageManager::LanguageManager(QCoreApplication *application,
                                 QObject *parent,
                                 TranslatorFactory translatorFactory,
                                 TranslatorLoader translatorLoader)
    : QObject(parent),
      application_(application),
      translatorFactory_(std::move(translatorFactory)),
      translatorLoader_(std::move(translatorLoader)) {
    if (!translatorFactory_) {
        translatorFactory_ = [] { return std::make_unique<QTranslator>(); };
    }
    if (!translatorLoader_) {
        translatorLoader_ = [](QTranslator *translator, const QString &resourcePath) {
            return translator->load(resourcePath);
        };
    }
}

LanguageManager::~LanguageManager() {
    clearTranslator();
}

QVector<LanguageOption> LanguageManager::supportedLanguages() {
    return {
        {QStringLiteral("en"), QStringLiteral("English"), {}},
        {QStringLiteral("zh-CN"), QString::fromUtf8(u8"简体中文"),
         QStringLiteral(":/i18n/airplay_zh_CN.qm")},
    };
}

QString LanguageManager::resolveEffectiveLanguage(const QString &selection,
                                                  const QLocale &systemLocale) {
    const QString requestedSelection = normalizedSelection(selection);
    if (requestedSelection == QStringLiteral("en")) {
        return QStringLiteral("en");
    }
    if (requestedSelection == QStringLiteral("zh-CN")) {
        return QStringLiteral("zh-CN");
    }
    if (requestedSelection != QStringLiteral("system")) {
        return QStringLiteral("en");
    }

    if (systemLocale.language() != QLocale::Chinese) {
        return QStringLiteral("en");
    }

    if (systemLocale.script() == QLocale::SimplifiedHanScript) {
        return QStringLiteral("zh-CN");
    }
    if (systemLocale.script() == QLocale::TraditionalHanScript) {
        return QStringLiteral("en");
    }

    if (systemLocale.script() == QLocale::AnyScript) {
        if (systemLocale.territory() == QLocale::China
            || systemLocale.territory() == QLocale::Singapore) {
            return QStringLiteral("zh-CN");
        }
    }

    return QStringLiteral("en");
}

bool LanguageManager::apply(const QString &selection, const QLocale &systemLocale) {
    const QString requestedSelection = normalizedSelection(selection);
    QString nextEffectiveLanguage = resolveEffectiveLanguage(requestedSelection, systemLocale);

    if (requestedSelection == selection_ && nextEffectiveLanguage == effectiveLanguage_) {
        return true;
    }

    bool applied = true;
    std::unique_ptr<QTranslator> nextTranslator;
    if (nextEffectiveLanguage == QStringLiteral("zh-CN")) {
        QString resourcePath;
        for (const LanguageOption &option : supportedLanguages()) {
            if (option.id == nextEffectiveLanguage) {
                resourcePath = option.resourcePath;
                break;
            }
        }

        nextTranslator = translatorFactory_();
        if (!application_ || !nextTranslator || !translatorLoader_(nextTranslator.get(), resourcePath)) {
            nextTranslator.reset();
            nextEffectiveLanguage = QStringLiteral("en");
            applied = false;
            emit translationLoadFailed(requestedSelection, resourcePath);
        }
    }

    clearTranslator();

    if (application_ && nextTranslator) {
        application_->installTranslator(nextTranslator.get());
        translator_ = std::move(nextTranslator);
    }

    const bool changed = requestedSelection != selection_
        || nextEffectiveLanguage != effectiveLanguage_;
    selection_ = requestedSelection;
    effectiveLanguage_ = nextEffectiveLanguage;
    if (changed) {
        emit languageChanged(selection_, effectiveLanguage_);
    }

    return applied;
}

QString LanguageManager::selection() const {
    return selection_;
}

QString LanguageManager::effectiveLanguage() const {
    return effectiveLanguage_;
}

void LanguageManager::clearTranslator() {
    if (application_ && translator_) {
        application_->removeTranslator(translator_.get());
    }
    translator_.reset();
}
