#pragma once

#include <qmdnsengine/abstractserver.h>

class DiagnosticLogSink;

namespace QMdnsEngine {
class Message;
}

class MdnsServerAdapter final : public QMdnsEngine::AbstractServer {
    Q_OBJECT

public:
    MdnsServerAdapter(QMdnsEngine::AbstractServer *delegate, DiagnosticLogSink *sink,
                      QObject *parent = nullptr);

    void sendMessage(const QMdnsEngine::Message &message) override;
    void sendMessageToAll(const QMdnsEngine::Message &message) override;

private slots:
    void onMessageReceived(const QMdnsEngine::Message &message);
    void onError(const QString &message);

private:
    QMdnsEngine::AbstractServer *delegate_ = nullptr;
    DiagnosticLogSink *sink_ = nullptr;
};
