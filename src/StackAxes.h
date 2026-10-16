#pragma once

#include "scopeone/ExperimentDocument.h"

#include <QChar>
#include <QList>

// Where the planes of an image lie along its channel, Z and time axes. The axes are named by
// the letters C, Z and T, and a plane number counts the planes in the order of the shape.
namespace scopeone::ui::stack
{
    using scopeone::core::StackShape;

    inline int planeIndex(const StackShape& shape, int c, int z, int t)
    {
        int index = 0;
        for (const QChar letter : shape.order)
        {
            const int position = letter == QLatin1Char('C') ? c : (letter == QLatin1Char('Z') ? z : t);
            index = index * shape.axisSize(letter) + position;
        }
        return index;
    }

    inline void coordinates(const StackShape& shape, int index, int& c, int& z, int& t)
    {
        c = z = t = 0;
        for (int i = shape.order.size() - 1; i >= 0; --i)
        {
            const QChar letter = shape.order.at(i);
            const int size = shape.axisSize(letter);
            int& position = letter == QLatin1Char('C') ? c : (letter == QLatin1Char('Z') ? z : t);
            position = index % size;
            index /= size;
        }
    }

    inline int& coordinate(int& c, int& z, int& t, QChar axis)
    {
        return axis == QLatin1Char('C') ? c : (axis == QLatin1Char('Z') ? z : t);
    }

    // The axes that have more than one step, in the order channel, Z, time
    inline QList<QChar> activeAxes(const StackShape& shape)
    {
        QList<QChar> axes;
        for (const QChar letter : QStringLiteral("CZT"))
        {
            if (shape.axisSize(letter) > 1)
            {
                axes.append(letter);
            }
        }
        return axes;
    }

    inline QString axisName(QChar axis, bool plain = false)
    {
        if (plain)
        {
            return QStringLiteral("Slices");
        }
        return axis == QLatin1Char('C') ? QStringLiteral("Channels")
                                        : (axis == QLatin1Char('Z') ? QStringLiteral("Slices (Z)") : QStringLiteral("Frames (T)"));
    }

    // The planes that lie along an axis, with the other axes where the given plane has them
    inline QList<int> planesAlong(const StackShape& shape, QChar axis, int index)
    {
        int c, z, t;
        coordinates(shape, index, c, z, t);
        QList<int> planes;
        for (int step = 0; step < shape.axisSize(axis); ++step)
        {
            coordinate(c, z, t, axis) = step;
            planes.append(planeIndex(shape, c, z, t));
        }
        return planes;
    }

    // A name for the position of a plane, such as C1 Z3 T5, with the axes of one step left out
    inline QString positionText(const StackShape& shape, int index)
    {
        int c, z, t;
        coordinates(shape, index, c, z, t);
        QStringList parts;
        if (shape.channels > 1) parts << QStringLiteral("C%1").arg(c + 1);
        if (shape.slices > 1) parts << QStringLiteral("Z%1").arg(z + 1);
        if (shape.frames > 1) parts << QStringLiteral("T%1").arg(t + 1);
        return parts.join(QLatin1Char(' '));
    }
}
