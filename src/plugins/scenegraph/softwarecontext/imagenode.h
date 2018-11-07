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

#ifndef IMAGENODE_H
#define IMAGENODE_H

#include <private/qsgadaptationlayer_p.h>
#include <private/qsgtexturematerial_p.h>

QT_BEGIN_NAMESPACE

typedef QVarLengthArray<QPainter::PixmapFragment, 16> QPixmapFragmentsArray;

struct QTileRules
{
    inline QTileRules(Qt::TileRule horizontalRule, Qt::TileRule verticalRule)
            : horizontal(horizontalRule), vertical(verticalRule) {}
    inline QTileRules(Qt::TileRule rule = Qt::StretchTile)
            : horizontal(rule), vertical(rule) {}
    Qt::TileRule horizontal;
    Qt::TileRule vertical;
};

#ifndef Q_QDOC
// For internal use only.
namespace QDrawBorderPixmap
{
    enum DrawingHint
    {
        OpaqueTopLeft = 0x0001,
        OpaqueTop = 0x0002,
        OpaqueTopRight = 0x0004,
        OpaqueLeft = 0x0008,
        OpaqueCenter = 0x0010,
        OpaqueRight = 0x0020,
        OpaqueBottomLeft = 0x0040,
        OpaqueBottom = 0x0080,
        OpaqueBottomRight = 0x0100,
        OpaqueCorners = OpaqueTopLeft | OpaqueTopRight | OpaqueBottomLeft | OpaqueBottomRight,
        OpaqueEdges = OpaqueTop | OpaqueLeft | OpaqueRight | OpaqueBottom,
        OpaqueFrame = OpaqueCorners | OpaqueEdges,
        OpaqueAll = OpaqueCenter | OpaqueFrame
    };

    Q_DECLARE_FLAGS(DrawingHints, DrawingHint)
}
#endif

namespace SoftwareContext {

void qDrawBorderPixmap(QPainter *painter, const QRect &targetRect, const QMargins &targetMargins,
                       const QPixmap &pixmap, const QRect &sourceRect,const QMargins &sourceMargins,
                       const QTileRules &rules, QDrawBorderPixmap::DrawingHints hints);

}

class ImageNode : public QSGInternalImageNode
{
public:
    ImageNode();

    void setTargetRect(const QRectF &rect);
    void setInnerTargetRect(const QRectF &rect);
    void setInnerSourceRect(const QRectF &rect);
    void setSubSourceRect(const QRectF &rect);
    void setTexture(QSGTexture *texture);
    void setMirror(bool mirror);
    void setMipmapFiltering(QSGTexture::Filtering filtering);
    void setFiltering(QSGTexture::Filtering filtering);
    void setHorizontalWrapMode(QSGTexture::WrapMode wrapMode);
    void setVerticalWrapMode(QSGTexture::WrapMode wrapMode);
    void update();

    void preprocess();

    void paint(QPainter *painter);

    QRectF rect() const;

private:
    const QPixmap &pixmap() const;

    QRectF m_targetRect;
    QRectF m_innerTargetRect;
    QRectF m_innerSourceRect;
    QRectF m_subSourceRect;

    QSGTexture *m_texture;
    QPixmap m_cachedMirroredPixmap;

    bool m_mirror;
    bool m_smooth;
    bool m_tileHorizontal;
    bool m_tileVertical;
    bool m_cachedMirroredPixmapIsDirty;
};

QT_END_NAMESPACE

#endif // IMAGENODE_H
