#pragma once

#include <QObject>

#include <functional>
#include <memory>

struct DiagnosticRestartOperations {
    std::function<bool(const QString &, const QStringList &, QString *)> startDetached;
    std::function<QString()> createToken;
};

class DiagnosticRestartCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit DiagnosticRestartCoordinator(DiagnosticRestartOperations operations = {},
                                         QObject *parent = nullptr);
    ~DiagnosticRestartCoordinator() override;

    bool begin(const QString &executablePath, qint64 parentPid);

signals:
    void childReady();
    void failed(QString error);

private:
    void fail(QString error);
    void finishReady();
    void acceptConnection();
    void readConnection();
    void cleanup();

    class Private;
    std::unique_ptr<Private> d;
};
