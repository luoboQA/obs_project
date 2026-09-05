#ifndef RESIZABLEPIXMAPITEM_H
#define RESIZABLEPIXMAPITEM_H

#include <QGraphicsObject>
#include <QPixmap>
#include <QPen>
#include <QGraphicsLineItem>

class ResizablePixmapItem : public QGraphicsObject
{
    Q_OBJECT
public:
    explicit ResizablePixmapItem(const QPixmap &pixmap, int id, QGraphicsItem *parent = nullptr);
    ~ResizablePixmapItem();

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;
    QRectF getImageRect() const { return m_rect; }
    void setPixmap(const QPixmap &pixmap);
    int getId() const { return m_id; } // 获取ID
    // 允许外部代码直接修改尺寸 (用于分辨率自适应)
    void setRect(const QRectF &rect);
    const QPixmap& getPixmap() const { return m_pixmap; }

signals:
    void geometryChanged(int id, QRect rect);

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;

private:
    bool isInResizeArea(const QPointF &pos) const;
    void ensureGuideLinesExist();
    void hideGuideLines();
    void triggerFeedback();

private:
    int m_id;
    QPixmap m_pixmap;
    QRectF m_rect;
    QRectF m_initialRect;
    QPointF m_startPos;
    bool m_isResizing;
    const qreal m_handleSize = 12.0;
    QGraphicsLineItem *m_vLine;
    QGraphicsLineItem *m_hLine;
    bool m_snappedX;
    bool m_snappedY;
    const qreal SNAP_THRESHOLD = 20.0;
};
#endif
