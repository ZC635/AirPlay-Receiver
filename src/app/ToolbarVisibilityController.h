#pragma once

#include "backend/ReceiverState.h"
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <optional>

class QWidget;

// Owns toolbar policy, independently of the window's normal/fullscreen mode.
class ToolbarVisibilityController final : public QObject {
    Q_OBJECT
public:
    ToolbarVisibilityController(QWidget *content, QWidget *toolbar, QObject *parent = nullptr);
    ~ToolbarVisibilityController() override;
    void receiverStateChanged(ReceiverState state);
    void setHoverRevealEnabled(bool enabled);
    void toggleManually();
    void toggleManually(const QPoint &globalPosition);
    void preserveTemporaryRevealUntilPointerMoves(const QPoint &globalPosition);
    bool isVisible() const;
    void evaluatePointer(const QPoint &globalPosition, bool active);

signals:
    void visibilityChanged(bool visible);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool inTopBand(const QPoint &position) const;
    bool inInteractiveRegion(const QPoint &position) const;
    bool ownsControl(const QWidget *widget) const;
    bool hoverWindowActive() const;
    void evaluateCursor();
    void publishVisibility();
    void updateTimer();

    QPointer<QWidget> content_;
    QPointer<QWidget> toolbar_;
    QTimer cursorTimer_;
    std::optional<ReceiverState> receiverState_;
    QPoint lastPosition_;
    std::optional<QPoint> preservedRevealPosition_;
    bool baselineVisible_ = true;
    bool temporaryVisible_ = false;
    bool hoverEnabled_ = true;
    bool suppressUntilLeave_ = false;
    bool publishedVisible_ = true;
    bool evaluating_ = false;
};
