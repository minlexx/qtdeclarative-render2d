/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Quick 2D Renderer module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef RECTANGLENODE_H
#define RECTANGLENODE_H

#include <private/qsgadaptationlayer_p.h>

#include <QPen>
#include <QBrush>
#include <QPixmap>

QT_BEGIN_NAMESPACE

class RectangleNode : public QSGInternalRectangleNode
{
public:
    RectangleNode();

    void setRect(const QRectF &rect);
    void setColor(const QColor &color);
    void setPenColor(const QColor &color);
    void setPenWidth(qreal width);
    void setGradientStops(const QGradientStops &stops);
    void setRadius(qreal radius);
    void setAntialiasing(bool antialiasing) { Q_UNUSED(antialiasing) }
    void setAligned(bool aligned);

    void update();

    void paint(QPainter *);

    bool isOpaque() const;
    QRectF rect() const;
private:
    void paintRectangle(QPainter *painter, const QRect &rect);
    void generateCornerPixmap();

    QRect m_rect;
    QColor m_color;
    QColor m_penColor;
    double m_penWidth;
    QGradientStops m_stops;
    double m_radius;
    QPen m_pen;
    QBrush m_brush;

    bool m_cornerPixmapIsDirty;
    QPixmap m_cornerPixmap;

    int m_devicePixelRatio;
};

QT_END_NAMESPACE

#endif // RECTANGLENODE_H
