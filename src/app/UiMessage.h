#pragma once

#include <QString>
#include <QStringList>

class UiMessage {
public:
    static UiMessage translated(QString context, QString source, QStringList arguments = {});
    static UiMessage raw(QString text);

    bool isEmpty() const;
    QString render() const;

private:
    QString context_;
    QString source_;
    QStringList arguments_;
    bool translatable_ = false;
};
