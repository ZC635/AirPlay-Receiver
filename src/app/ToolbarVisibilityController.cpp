#include "app/ToolbarVisibilityController.h"
#include "diagnostics/DiagnosticLogSink.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QWidget>
#include <algorithm>

namespace {
bool contains(const QWidget *widget, const QPoint &position) {
    return QRect(widget->mapToGlobal(QPoint()), widget->size()).contains(position);
}
}

ToolbarVisibilityController::ToolbarVisibilityController(QWidget *content, QWidget *toolbar, QObject *parent)
    : QObject(parent), content_(content), toolbar_(toolbar), diagnosticSink_(&nullDiagnosticLogSink()),
      lastPosition_(QCursor::pos()) {
    cursorTimer_.setInterval(25);
    cursorTimer_.setTimerType(Qt::PreciseTimer);
    connect(&cursorTimer_, &QTimer::timeout, this, &ToolbarVisibilityController::evaluateCursor);
    qApp->installEventFilter(this);
}

ToolbarVisibilityController::~ToolbarVisibilityController() {
    qApp->removeEventFilter(this);
}

void ToolbarVisibilityController::setDiagnosticSink(DiagnosticLogSink *sink) {
    diagnosticSink_ = sink ? sink : &nullDiagnosticLogSink();
}

void ToolbarVisibilityController::receiverStateChanged(ReceiverState state) {
    if (receiverState_ == state) return;
    receiverState_ = state;
    preservedVisibilityPosition_.reset();
    baselineVisible_ = state != ReceiverState::Connected;
    temporaryVisible_ = false;
    suppressUntilLeave_ = false;
    publishVisibility("receiver_state");
    updateTimer();
}

void ToolbarVisibilityController::setHoverRevealEnabled(bool enabled) {
    hoverEnabled_ = enabled;
    if (!enabled) {
        temporaryVisible_ = false;
        preservedVisibilityPosition_.reset();
    }
    publishVisibility("preference");
    updateTimer();
}

void ToolbarVisibilityController::toggleManually() {
    preservedVisibilityPosition_.reset();
    baselineVisible_ = !isVisible();
    temporaryVisible_ = false;
    suppressUntilLeave_ = !baselineVisible_ && inTopBand(lastPosition_);
    publishVisibility("manual");
    updateTimer();
}

void ToolbarVisibilityController::toggleManually(const QPoint &globalPosition) {
    // Sample hotkey-time position without first altering the visible state that
    // the user's toggle acts on (native surfaces may not send mouse events).
    lastPosition_ = globalPosition;
    toggleManually();
}

void ToolbarVisibilityController::preserveVisibilityUntilPointerMoves(const QPoint &globalPosition) {
    // Window geometry alone must not reveal or hide the toolbar.
    preservedVisibilityPosition_ = globalPosition;
}

bool ToolbarVisibilityController::isVisible() const {
    return baselineVisible_ || temporaryVisible_;
}

bool ToolbarVisibilityController::inTopBand(const QPoint &position) const {
    if (!content_ || !toolbar_) return false;
    int height = toolbar_->height();
    if (toolbar_->isHidden() && toolbar_->sizeHint().height() > 0) {
        height = toolbar_->sizeHint().height();
    }
    return QRect(content_->mapToGlobal(QPoint()),
                 QSize(content_->width(), std::min(height, content_->height()))).contains(position);
}

bool ToolbarVisibilityController::ownsControl(const QWidget *widget) const {
    if (!toolbar_ || !widget) return false;
    // A toolbar parent is not enough: dialogs/tool windows are separate UI.
    for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
        if (ancestor == toolbar_) return true;
        if (ancestor->isWindow() && ancestor->windowType() != Qt::Popup) return false;
    }
    return false;
}

bool ToolbarVisibilityController::inInteractiveRegion(const QPoint &position) const {
    if (inTopBand(position)) return true;
    if (!toolbar_) return false;
    if (toolbar_->isVisible() && contains(toolbar_, position)) return true;
    for (auto *widget : toolbar_->findChildren<QWidget *>()) {
        if (widget->isVisible() && ownsControl(widget) && contains(widget, position)) return true;
    }
    return false;
}

bool ToolbarVisibilityController::hoverWindowActive() const {
    if (!content_ || !content_->isVisible() || QApplication::activeModalWidget()
        || QGuiApplication::applicationState() != Qt::ApplicationActive) return false;
    if (auto *popup = QApplication::activePopupWidget()) return ownsControl(popup);
    return content_->window()->isActiveWindow();
}

void ToolbarVisibilityController::evaluatePointer(const QPoint &globalPosition, bool active) {
    if (evaluating_) return;
    QScopedValueRollback<bool> guard(evaluating_, true);
    lastPosition_ = globalPosition;
    if (preservedVisibilityPosition_) {
        if (hoverEnabled_ && active && globalPosition == *preservedVisibilityPosition_) {
            return;
        }
        preservedVisibilityPosition_.reset();
    }
    const bool atTop = inTopBand(globalPosition);
    if (!atTop) suppressUntilLeave_ = false;
    if (!hoverEnabled_ || !active || baselineVisible_) {
        temporaryVisible_ = false;
    } else if (temporaryVisible_) {
        temporaryVisible_ = inInteractiveRegion(globalPosition);
    } else {
        temporaryVisible_ = atTop && !suppressUntilLeave_;
    }
    publishVisibility(!active ? "deactivated" : temporaryVisible_ ? "hover_enter" : "hover_leave");
}

void ToolbarVisibilityController::publishVisibility(const char *reason) {
    const bool visible = isVisible();
    if (visible == publishedVisible_) return;
    publishedVisible_ = visible;
    QScopedValueRollback<bool> guard(evaluating_, true);
    if (diagnosticSink_->isActive()) {
        const auto flag = [](bool value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); };
        diagnosticSink_->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("ui"),
            QStringLiteral("toolbar_visibility_changed"),
            {{QStringLiteral("visible"), flag(visible)},
             {QStringLiteral("baseline_visible"), flag(baselineVisible_)},
             {QStringLiteral("temporary_visible"), flag(temporaryVisible_)},
             {QStringLiteral("hover_enabled"), flag(hoverEnabled_)},
             {QStringLiteral("suppressed"), flag(suppressUntilLeave_)},
             {QStringLiteral("preserved"), flag(preservedVisibilityPosition_.has_value())},
             {QStringLiteral("reason"), QString::fromLatin1(reason)}}));
    }
    emit visibilityChanged(visible);
}

void ToolbarVisibilityController::updateTimer() {
    if (hoverEnabled_ && !baselineVisible_ && hoverWindowActive()) {
        if (!cursorTimer_.isActive()) cursorTimer_.start();
    } else {
        cursorTimer_.stop();
    }
}

void ToolbarVisibilityController::evaluateCursor() {
    evaluatePointer(QCursor::pos(), hoverWindowActive());
    updateTimer();
}

bool ToolbarVisibilityController::eventFilter(QObject *watched, QEvent *event) {
    if (evaluating_ || !content_ || !toolbar_) return false;
    auto *widget = qobject_cast<QWidget *>(watched);
    const bool relevant = widget && (widget->window() == content_->window() || ownsControl(widget));
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
        if (relevant) {
            evaluatePointer(static_cast<QMouseEvent *>(event)->globalPosition().toPoint(), hoverWindowActive());
            updateTimer();
        }
        break;
    case QEvent::Enter:
    case QEvent::Leave:
        if (relevant) evaluateCursor();
        break;
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationActivate:
    case QEvent::ApplicationDeactivate:
        // Qt updates activeWindow after some activation notifications. The queued
        // pass also recognizes an owned popup taking focus from the main window.
        if (event->type() == QEvent::ApplicationDeactivate) evaluatePointer(QCursor::pos(), false);
        QTimer::singleShot(0, this, &ToolbarVisibilityController::evaluateCursor);
        break;
    case QEvent::Hide:
    case QEvent::Close:
        if (relevant && widget != content_->window()) evaluateCursor();
        break;
    case QEvent::Show:
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::LayoutRequest:
    case QEvent::LanguageChange:
        if (relevant) QTimer::singleShot(0, this, &ToolbarVisibilityController::evaluateCursor);
        break;
    default:
        break;
    }
    return false;
}
