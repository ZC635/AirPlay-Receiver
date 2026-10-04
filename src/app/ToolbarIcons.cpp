#include "app/ToolbarIcons.h"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <utility>

namespace ToolbarIcons {
namespace {

constexpr qreal IconSize = 20.0;

void line(QPainterPath &path, qreal x1, qreal y1, qreal x2, qreal y2) {
    path.moveTo(x1, y1);
    path.lineTo(x2, y2);
}

QPainterPath glyphPath(Glyph glyph) {
    QPainterPath path;
    switch (glyph) {
    case Glyph::Volume:
        path.moveTo(2.5, 8); path.lineTo(5.5, 8); path.lineTo(9.5, 4.5);
        path.lineTo(9.5, 15.5); path.lineTo(5.5, 12); path.lineTo(2.5, 12); path.closeSubpath();
        path.moveTo(12.5, 7); path.cubicTo(15, 8.5, 15, 11.5, 12.5, 13);
        path.moveTo(14.5, 4.5); path.cubicTo(18.5, 7, 18.5, 13, 14.5, 15.5);
        break;
    case Glyph::Pin:
        path.moveTo(7, 3.5); path.lineTo(13, 3.5); path.lineTo(12, 6.5);
        path.lineTo(15.5, 10); path.lineTo(15.5, 12); path.lineTo(10.5, 11);
        path.lineTo(7, 14.5); path.lineTo(6, 13.5); path.lineTo(9.5, 10);
        path.lineTo(8.5, 5); path.closeSubpath();
        line(path, 10, 11.5, 4.5, 17);
        break;
    case Glyph::AspectRatio:
        path.addRoundedRect(QRectF(2.5, 5, 15, 10), 1.5, 1.5);
        line(path, 5, 12, 5, 8); line(path, 5, 8, 8.5, 8);
        line(path, 15, 8, 15, 12); line(path, 11.5, 12, 15, 12);
        break;
    case Glyph::VideoFit:
        path.addRoundedRect(QRectF(4, 4, 12, 12), 1.2, 1.2);
        line(path, 2, 7, 2, 4); line(path, 2, 4, 5, 4);
        line(path, 15, 4, 18, 4); line(path, 18, 4, 18, 7);
        line(path, 2, 13, 2, 16); line(path, 2, 16, 5, 16);
        line(path, 15, 16, 18, 16); line(path, 18, 16, 18, 13);
        break;
    case Glyph::Record:
        path.addEllipse(QRectF(3.5, 3.5, 13, 13));
        path.addEllipse(QRectF(7, 7, 6, 6));
        break;
    case Glyph::Stop:
        path.addRoundedRect(QRectF(4, 4, 12, 12), 1.5, 1.5);
        break;
    case Glyph::Saving:
        path.moveTo(5, 3.5); path.lineTo(15, 3.5); path.lineTo(14, 6.5);
        path.lineTo(11, 9); path.lineTo(14, 11.5); path.lineTo(15, 16.5);
        path.lineTo(5, 16.5); path.lineTo(6, 11.5); path.lineTo(9, 9);
        path.lineTo(6, 6.5); path.closeSubpath();
        line(path, 6, 5.5, 14, 5.5); line(path, 6, 14.5, 14, 14.5);
        break;
    case Glyph::Fullscreen:
        line(path, 3, 7, 3, 3); line(path, 3, 3, 7, 3);
        line(path, 13, 3, 17, 3); line(path, 17, 3, 17, 7);
        line(path, 3, 13, 3, 17); line(path, 3, 17, 7, 17);
        line(path, 13, 17, 17, 17); line(path, 17, 17, 17, 13);
        break;
    case Glyph::ExitFullscreen:
        line(path, 2.5, 2.5, 7, 7); line(path, 4, 7, 7, 7); line(path, 7, 4, 7, 7);
        line(path, 17.5, 2.5, 13, 7); line(path, 13, 4, 13, 7); line(path, 13, 7, 16, 7);
        line(path, 2.5, 17.5, 7, 13); line(path, 4, 13, 7, 13); line(path, 7, 13, 7, 16);
        line(path, 17.5, 17.5, 13, 13); line(path, 13, 13, 16, 13); line(path, 13, 13, 13, 16);
        break;
    case Glyph::Settings:
        path.addEllipse(QRectF(7, 7, 6, 6));
        path.moveTo(8, 2.5); path.lineTo(12, 2.5); path.lineTo(12.5, 4.5);
        path.lineTo(14, 5.2); path.lineTo(16, 4.5); path.lineTo(18, 8);
        path.lineTo(16.5, 9.5); path.lineTo(16.5, 10.5); path.lineTo(18, 12);
        path.lineTo(16, 15.5); path.lineTo(14, 14.8); path.lineTo(12.5, 15.5);
        path.lineTo(12, 17.5); path.lineTo(8, 17.5); path.lineTo(7.5, 15.5);
        path.lineTo(6, 14.8); path.lineTo(4, 15.5); path.lineTo(2, 12);
        path.lineTo(3.5, 10.5); path.lineTo(3.5, 9.5); path.lineTo(2, 8);
        path.lineTo(4, 4.5); path.lineTo(6, 5.2); path.lineTo(7.5, 4.5); path.closeSubpath();
        break;
    }
    return path;
}

class ToolbarIconEngine final : public QIconEngine {
public:
    ToolbarIconEngine(Glyph glyph, QPalette palette) : glyph_(glyph), palette_(std::move(palette)) {}

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return renderPixmap(size, 1.0, mode, state);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        return renderPixmap(size, scale > 0.0 ? scale : 1.0, mode, state);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        const QPalette::ColorGroup group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        const QPalette::ColorRole role = mode == QIcon::Selected
                ? QPalette::HighlightedText : QPalette::ButtonText;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal scale = std::min(rect.width(), rect.height()) / IconSize;
        painter->translate(rect.center().x() + 0.5 - IconSize * scale / 2.0,
                           rect.center().y() + 0.5 - IconSize * scale / 2.0);
        painter->scale(scale, scale);
        QPen pen(palette_.color(group, role));
        pen.setWidthF(1.7);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(glyphPath(glyph_));
        painter->restore();
    }

    QIconEngine *clone() const override {
        return new ToolbarIconEngine(glyph_, palette_);
    }

private:
    QPixmap renderPixmap(const QSize &size, qreal scale, QIcon::Mode mode, QIcon::State state) {
        const QSize pixelSize(qMax(1, qRound(size.width() * scale)),
                              qMax(1, qRound(size.height() * scale)));
        QPixmap pixmap(pixelSize);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(scale);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    Glyph glyph_;
    QPalette palette_;
};

} // namespace

QIcon create(Glyph glyph, const QPalette &palette) {
    return QIcon(new ToolbarIconEngine(glyph, palette));
}

} // namespace ToolbarIcons
