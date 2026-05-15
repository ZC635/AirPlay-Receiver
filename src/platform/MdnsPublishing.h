#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

class MdnsPublishing {
public:
    virtual ~MdnsPublishing() = default;

    virtual bool publish(const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
                         const char *raopTxt, int raopTxtLength,
                         const char *airplayTxt, int airplayTxtLength) = 0;
    virtual void stop() = 0;
};
