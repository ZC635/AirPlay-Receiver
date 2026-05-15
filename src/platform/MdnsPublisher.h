#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QScopedPointer>
#include <QString>

#include "platform/MdnsPublishing.h"

class MdnsPublisher : public QObject, public MdnsPublishing {
    Q_OBJECT

public:
    struct ServiceDefinition {
        QByteArray name;
        QByteArray type;
        quint16 port = 0;
        QMap<QByteArray, QByteArray> attributes;
    };

    explicit MdnsPublisher(QObject *parent = nullptr);
    ~MdnsPublisher() override;

    bool publish(const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
                 const char *raopTxt, int raopTxtLength,
                 const char *airplayTxt, int airplayTxtLength) override;
    void stop() override;

    static QMap<QByteArray, QByteArray> parseTxtRecord(const char *txt, int length, bool *ok = nullptr);
    static QList<ServiceDefinition> buildServiceDefinitions(
        const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
        const char *raopTxt, int raopTxtLength,
        const char *airplayTxt, int airplayTxtLength,
        bool *ok = nullptr);

private:
    struct Private;
    QScopedPointer<Private> d;
};
