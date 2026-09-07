#pragma once

#include <functional>
#include <memory>

#include <QObject>
#include <QLocale>
#include <QPointer>
#include <QString>
#include <QTranslator>
#include <QVector>

class QCoreApplication;

struct LanguageOption {
    QString id;
    QString nativeName;
    QString resourcePath;
};

class LanguageManager final : public QObject {
    Q_OBJECT

public:
    using TranslatorFactory = std::function<std::unique_ptr<QTranslator>()>;
    using TranslatorLoader = std::function<bool(QTranslator *, const QString &)>;

    explicit LanguageManager(QCoreApplication *application,
                             QObject *parent = nullptr,
                             TranslatorFactory translatorFactory = {},
                             TranslatorLoader translatorLoader = {});
    ~LanguageManager() override;

    static QVector<LanguageOption> supportedLanguages();
    static QString resolveEffectiveLanguage(const QString &selection, const QLocale &systemLocale);

    bool apply(const QString &selection, const QLocale &systemLocale = QLocale::system());
    QString selection() const;
    QString effectiveLanguage() const;

signals:
    void languageChanged(QString selection, QString effectiveLanguage);
    void translationLoadFailed(QString selection, QString resourcePath);

private:
    void clearTranslator();

    QPointer<QCoreApplication> application_;
    std::unique_ptr<QTranslator> translator_;
    TranslatorFactory translatorFactory_;
    TranslatorLoader translatorLoader_;
    QString selection_ = QStringLiteral("system");
    QString effectiveLanguage_ = QStringLiteral("en");
};
