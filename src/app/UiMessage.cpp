#include "UiMessage.h"

#include <QCoreApplication>

#include <utility>

UiMessage UiMessage::translated(QString context, QString source, QStringList arguments) {
    UiMessage message;
    message.context_ = std::move(context);
    message.source_ = std::move(source);
    message.arguments_ = std::move(arguments);
    message.translatable_ = true;
    return message;
}

UiMessage UiMessage::raw(QString text) {
    UiMessage message;
    message.source_ = std::move(text);
    return message;
}

bool UiMessage::isEmpty() const {
    return source_.isEmpty();
}

QString UiMessage::render() const {
    QString result = source_;
    if (translatable_) {
        const QByteArray contextUtf8 = context_.toUtf8();
        const QByteArray sourceUtf8 = source_.toUtf8();
        result = QCoreApplication::translate(contextUtf8.constData(), sourceUtf8.constData());
    }

    for (const QString &argument : arguments_) {
        result = result.arg(argument);
    }
    return result;
}
