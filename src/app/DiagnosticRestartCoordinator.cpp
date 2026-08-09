#include "app/DiagnosticRestartCoordinator.h"

#include "diagnostics/DiagnosticSanitizer.h"

#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QUuid>

#include <memory>

namespace {

constexpr int kHandshakeTimeoutMs = 10000;
constexpr qsizetype kMaximumLineBytes = 1024;

QString safeFailure(QString text) {
    text = DiagnosticSanitizer::sanitizeText(text);
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.left(kMaximumLineBytes).trimmed();
}

} // namespace

class DiagnosticRestartCoordinator::Private {
public:
    explicit Private(QObject *owner) : timer(owner) {}

    DiagnosticRestartOperations operations;
    QLocalServer server;
    QLocalSocket *socket = nullptr;
    QTimer timer;
    QString token;
    QByteArray input;
    bool active = false;
    bool terminal = false;
};

DiagnosticRestartCoordinator::DiagnosticRestartCoordinator(DiagnosticRestartOperations operations,
                                                           QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this)) {
    d->operations = std::move(operations);
    if (!d->operations.startDetached) {
        d->operations.startDetached = [](const QString &executablePath, const QStringList &arguments,
                                         QString *) {
            return QProcess::startDetached(executablePath, arguments,
                                           QFileInfo(executablePath).absolutePath());
        };
    }
    if (!d->operations.createToken) {
        d->operations.createToken = [] {
            return QStringLiteral("AirPlay-Diagnostic-Ready-") +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
        };
    }
    d->timer.setSingleShot(true);
    d->timer.setInterval(kHandshakeTimeoutMs);
    connect(&d->server, &QLocalServer::newConnection, this,
            &DiagnosticRestartCoordinator::acceptConnection);
    connect(&d->timer, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("Diagnostic restart timed out waiting for the child process."));
    });
}

DiagnosticRestartCoordinator::~DiagnosticRestartCoordinator() {
    cleanup();
}

bool DiagnosticRestartCoordinator::begin(const QString &executablePath, qint64 parentPid) {
    if (d->active || d->terminal) {
        return false;
    }
    if (executablePath.isEmpty() || parentPid <= 0) {
        fail(QStringLiteral("Diagnostic restart could not be started."));
        return false;
    }

    d->token = d->operations.createToken();
    if (d->token.isEmpty() || !d->server.listen(d->token)) {
        fail(QStringLiteral("Diagnostic restart could not create its private handoff channel."));
        return false;
    }
    d->active = true;
    d->timer.start();
    const QStringList arguments = {
        QStringLiteral("--diagnostic-log"),
        QStringLiteral("--diagnostic-parent-pid=%1").arg(parentPid),
        QStringLiteral("--diagnostic-ready-token=%1").arg(d->token),
    };
    QString startError;
    if (!d->operations.startDetached(executablePath, arguments, &startError)) {
        fail(QStringLiteral("Diagnostic restart could not start the child process."));
        return false;
    }
    return true;
}

void DiagnosticRestartCoordinator::acceptConnection() {
    if (!d->active) {
        while (QLocalSocket *extra = d->server.nextPendingConnection())
            extra->deleteLater();
        return;
    }
    if (d->socket != nullptr) {
        while (QLocalSocket *extra = d->server.nextPendingConnection())
            extra->deleteLater();
        fail(QStringLiteral("Diagnostic restart received an invalid child response."));
        return;
    }
    d->socket = d->server.nextPendingConnection();
    if (d->socket == nullptr) {
        fail(QStringLiteral("Diagnostic restart received an invalid child response."));
        return;
    }
    connect(d->socket, &QLocalSocket::readyRead, this, &DiagnosticRestartCoordinator::readConnection);
    connect(d->socket, &QLocalSocket::disconnected, this, [this] {
        if (d->active)
            fail(QStringLiteral("Diagnostic restart received an incomplete child response."));
    });
}

void DiagnosticRestartCoordinator::readConnection() {
    if (!d->active || d->socket == nullptr)
        return;
    d->input.append(d->socket->readAll());
    if (d->input.size() > kMaximumLineBytes) {
        fail(QStringLiteral("Diagnostic restart received an oversized child response."));
        return;
    }
    const int newline = d->input.indexOf('\n');
    if (newline < 0)
        return;
    if (newline != d->input.size() - 1) {
        fail(QStringLiteral("Diagnostic restart received an invalid child response."));
        return;
    }
    if (d->input == QByteArrayLiteral("READY\n")) {
        finishReady();
        return;
    }
    if (d->input.startsWith(QByteArrayLiteral("ERROR\t"))) {
        const QString childError = QString::fromUtf8(d->input.mid(6, d->input.size() - 7));
        fail(safeFailure(childError).isEmpty()
                 ? QStringLiteral("Diagnostic child could not initialize logging.")
                 : safeFailure(childError));
        return;
    }
    fail(QStringLiteral("Diagnostic restart received an invalid child response."));
}

void DiagnosticRestartCoordinator::finishReady() {
    if (!d->active || d->terminal)
        return;
    d->terminal = true;
    cleanup();
    emit childReady();
}

void DiagnosticRestartCoordinator::fail(QString error) {
    if (d->terminal)
        return;
    d->terminal = true;
    cleanup();
    emit failed(safeFailure(std::move(error)));
}

void DiagnosticRestartCoordinator::cleanup() {
    d->active = false;
    d->timer.stop();
    if (d->socket != nullptr) {
        d->socket->disconnect(this);
        d->socket->close();
        d->socket->deleteLater();
        d->socket = nullptr;
    }
    d->server.close();
}
