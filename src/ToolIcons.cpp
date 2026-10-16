#include "ToolIcons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

namespace scopeone::ui
{
    namespace
    {
        // Draws one icon on a square of 20 units, which the painter has scaled to the pixmap
        void drawTool(QPainter& painter, ToolIconKind kind, const QColor& color)
        {
            QPen pen(color);
            pen.setWidthF(1.5);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);

            // Small filled squares mark the corners and ends that can be dragged
            const auto handle = [&painter, &color](const QPointF& center)
            {
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawRect(QRectF(center.x() - 1.6, center.y() - 1.6, 3.2, 3.2));
                painter.restore();
            };

            switch (kind)
            {
            case ToolIconKind::Select:
            {
                // The arrow of a mouse pointer
                QPolygonF arrow;
                arrow << QPointF(5.0, 3.0) << QPointF(5.0, 15.5) << QPointF(8.2, 12.3)
                      << QPointF(10.6, 17.2) << QPointF(12.6, 16.2) << QPointF(10.2, 11.4)
                      << QPointF(14.6, 11.4);
                painter.drawPolygon(arrow);
                break;
            }
            case ToolIconKind::Rectangle:
                painter.drawRect(QRectF(3.0, 5.0, 14.0, 10.0));
                break;
            case ToolIconKind::Ellipse:
                painter.drawEllipse(QRectF(3.0, 4.5, 14.0, 11.0));
                break;
            case ToolIconKind::Polygon:
            {
                QPolygonF shape;
                shape << QPointF(4.0, 8.0) << QPointF(9.0, 3.5) << QPointF(16.0, 6.5)
                      << QPointF(15.0, 15.5) << QPointF(6.5, 16.0);
                painter.drawPolygon(shape);
                for (const QPointF& corner : shape)
                {
                    handle(corner);
                }
                break;
            }
            case ToolIconKind::Freehand:
            {
                // A closed curve drawn by hand
                QPainterPath curve;
                curve.moveTo(4.0, 11.0);
                curve.cubicTo(3.0, 5.0, 9.0, 3.0, 11.0, 6.0);
                curve.cubicTo(13.0, 8.5, 17.5, 5.5, 16.5, 11.5);
                curve.cubicTo(16.0, 16.0, 10.0, 17.5, 7.5, 15.0);
                curve.cubicTo(5.5, 13.5, 6.0, 12.5, 4.0, 11.0);
                painter.drawPath(curve);
                break;
            }
            case ToolIconKind::Wand:
            {
                // A stick with sparks at the tip
                painter.drawLine(QPointF(3.5, 16.5), QPointF(11.0, 9.0));
                painter.drawLine(QPointF(14.0, 3.0), QPointF(14.0, 8.0));
                painter.drawLine(QPointF(11.5, 5.5), QPointF(16.5, 5.5));
                painter.drawLine(QPointF(12.2, 3.7), QPointF(15.8, 7.3));
                painter.drawLine(QPointF(15.8, 3.7), QPointF(12.2, 7.3));
                break;
            }
            case ToolIconKind::Line:
                painter.drawLine(QPointF(4.0, 16.0), QPointF(16.0, 4.0));
                handle(QPointF(4.0, 16.0));
                handle(QPointF(16.0, 4.0));
                break;
            case ToolIconKind::Point:
                // A cross hair with a dot in the middle
                painter.drawLine(QPointF(10.0, 3.0), QPointF(10.0, 17.0));
                painter.drawLine(QPointF(3.0, 10.0), QPointF(17.0, 10.0));
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawEllipse(QPointF(10.0, 10.0), 2.0, 2.0);
                painter.restore();
                break;
            case ToolIconKind::Open:
            {
                // A folder with a tab
                QPainterPath folder;
                folder.moveTo(2.5, 5.0);
                folder.lineTo(8.0, 5.0);
                folder.lineTo(9.8, 7.0);
                folder.lineTo(17.5, 7.0);
                folder.lineTo(17.5, 15.5);
                folder.lineTo(2.5, 15.5);
                folder.closeSubpath();
                painter.drawPath(folder);
                break;
            }
            case ToolIconKind::Live:
            {
                // A play triangle in a ring
                painter.drawEllipse(QRectF(2.5, 2.5, 15.0, 15.0));
                QPolygonF triangle;
                triangle << QPointF(8.2, 6.6) << QPointF(8.2, 13.4) << QPointF(13.6, 10.0);
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawPolygon(triangle);
                painter.restore();
                break;
            }
            case ToolIconKind::Snap:
            {
                // A camera
                painter.drawRoundedRect(QRectF(2.5, 6.0, 15.0, 10.0), 1.5, 1.5);
                painter.drawLine(QPointF(7.0, 6.0), QPointF(8.2, 3.8));
                painter.drawLine(QPointF(8.2, 3.8), QPointF(11.8, 3.8));
                painter.drawLine(QPointF(11.8, 3.8), QPointF(13.0, 6.0));
                painter.drawEllipse(QPointF(10.0, 11.0), 3.0, 3.0);
                break;
            }
            case ToolIconKind::Contrast:
            {
                // A circle with one half filled
                painter.drawEllipse(QRectF(2.5, 2.5, 15.0, 15.0));
                QPainterPath half;
                half.moveTo(10.0, 2.5);
                half.arcTo(QRectF(2.5, 2.5, 15.0, 15.0), 90.0, 180.0);
                half.closeSubpath();
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawPath(half);
                painter.restore();
                break;
            }
            }
        }

        QPixmap render(ToolIconKind kind, const QColor& color, qreal devicePixelRatio, int size)
        {
            QPixmap pixmap(QSize(size, size) * devicePixelRatio);
            pixmap.setDevicePixelRatio(devicePixelRatio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.scale(size / 20.0, size / 20.0);
            drawTool(painter, kind, color);
            return pixmap;
        }
    }

    QIcon toolIcon(ToolIconKind kind, const QPalette& palette, qreal devicePixelRatio, int size)
    {
        QIcon icon;
        icon.addPixmap(render(kind, palette.color(QPalette::Active, QPalette::ButtonText), devicePixelRatio, size),
                       QIcon::Normal);
        icon.addPixmap(render(kind, palette.color(QPalette::Disabled, QPalette::ButtonText), devicePixelRatio, size),
                       QIcon::Disabled);
        return icon;
    }
}
