#include "ResizablePixmapItem.h"
#include <QPainter>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QApplication>
#include <cmath>

ResizablePixmapItem::ResizablePixmapItem(const QPixmap &pixmap, int id, QGraphicsItem *parent)
    : QGraphicsObject(parent)
    , m_id(id)
    , m_pixmap(pixmap)
    , m_isResizing(false)
    , m_vLine(nullptr), m_hLine(nullptr)
    , m_snappedX(false), m_snappedY(false)
{
    m_rect = m_pixmap.rect();
    setAcceptHoverEvents(true);
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
}

ResizablePixmapItem::~ResizablePixmapItem() {
    if (m_vLine && m_vLine->scene()) m_vLine->scene()->removeItem(m_vLine);
    delete m_vLine;
    if (m_hLine && m_hLine->scene()) m_hLine->scene()->removeItem(m_hLine);
    delete m_hLine;
}

void ResizablePixmapItem::setPixmap(const QPixmap &pixmap) {
    m_pixmap = pixmap;
    // 不重置 m_rect，保持当前的大小和位置，只更新内容
    update();
}

void ResizablePixmapItem::setRect(const QRectF &rect)
{
    if (m_rect == rect) return;
    prepareGeometryChange();
    m_rect = rect;
    update();
}

void ResizablePixmapItem::ensureGuideLinesExist() {
    if (!scene()) return;
    if (!m_vLine) { m_vLine = new QGraphicsLineItem(); QPen p(Qt::green, 2, Qt::DashLine); m_vLine->setPen(p); m_vLine->setZValue(100); scene()->addItem(m_vLine); m_vLine->hide(); }
    if (!m_hLine) { m_hLine = new QGraphicsLineItem(); QPen p(Qt::green, 2, Qt::DashLine); m_hLine->setPen(p); m_hLine->setZValue(100); scene()->addItem(m_hLine); m_hLine->hide(); }
}

void ResizablePixmapItem::hideGuideLines() { if (m_vLine) m_vLine->hide(); if (m_hLine) m_hLine->hide(); m_snappedX=false; m_snappedY=false; }
QRectF ResizablePixmapItem::boundingRect() const { return m_rect.adjusted(-5, -5, 5, 5); }
bool ResizablePixmapItem::isInResizeArea(const QPointF &pos) const { return (pos.x() >= m_rect.right() - m_handleSize && pos.x() <= m_rect.right() && pos.y() >= m_rect.bottom() - m_handleSize && pos.y() <= m_rect.bottom()); }
void ResizablePixmapItem::triggerFeedback() { QApplication::beep(); }

void ResizablePixmapItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option); Q_UNUSED(widget);

    // 绘制时拉伸 pixmap 到 m_rect
    painter->drawPixmap(m_rect.toRect(), m_pixmap);

    if (isSelected()) {
        QPen pen(Qt::blue, 2, Qt::DashLine);
        painter->setPen(pen); painter->setBrush(Qt::NoBrush);
        painter->drawRect(m_rect);
        painter->setBrush(Qt::blue); painter->setPen(Qt::NoPen);
        painter->drawRect(m_rect.right() - m_handleSize, m_rect.bottom() - m_handleSize, m_handleSize, m_handleSize);
    }
}

void ResizablePixmapItem::hoverMoveEvent(QGraphicsSceneHoverEvent *event) {
    if (isSelected() && isInResizeArea(event->pos())) setCursor(Qt::SizeFDiagCursor); else setCursor(Qt::ArrowCursor);
    QGraphicsObject::hoverMoveEvent(event);
}

void ResizablePixmapItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (isSelected() && isInResizeArea(event->pos())) {
            m_isResizing = true; m_startPos = event->pos(); m_initialRect = m_rect; event->accept(); hideGuideLines();
        } else {
            QGraphicsObject::mousePressEvent(event);
        }
    }
}

void ResizablePixmapItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (m_isResizing) {
        QPointF diff = event->pos() - m_startPos;
        qreal newW = qMax(50.0, m_initialRect.width() + diff.x());
        qreal newH = qMax(50.0, m_initialRect.height() + diff.y());
        if (scene()) {
            QRectF sceneRect = scene()->sceneRect(); QPointF pos = this->pos();
            if (pos.x() + newW > sceneRect.right()) newW = sceneRect.right() - pos.x();
            if (pos.y() + newH > sceneRect.bottom()) newH = sceneRect.bottom() - pos.y();
        }
        prepareGeometryChange(); m_rect = QRectF(0, 0, newW, newH);
        emit geometryChanged(m_id, QRect(this->x(), this->y(), m_rect.width(), m_rect.height()));
        update();
    } else {
        QGraphicsObject::mouseMoveEvent(event);
    }
}

void ResizablePixmapItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    m_isResizing = false; hideGuideLines(); QGraphicsObject::mouseReleaseEvent(event);
}

QVariant ResizablePixmapItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemPositionChange && scene()) {
        QPointF newPos = value.toPointF();
        QRectF sceneRect = scene()->sceneRect();
        QRectF myRect = m_rect;
        qreal x = newPos.x(); qreal y = newPos.y(); qreal w = myRect.width(); qreal h = myRect.height();
        ensureGuideLinesExist();

        bool showV = false; qreal targetX = x; qreal lineX = 0;
        if (std::abs((x + w/2) - sceneRect.center().x()) < SNAP_THRESHOLD) { targetX = sceneRect.center().x() - w/2; lineX = sceneRect.center().x(); showV = true; }
        else if (std::abs(x - sceneRect.left()) < SNAP_THRESHOLD) { targetX = sceneRect.left(); lineX = sceneRect.left(); showV = true; }
        else if (std::abs((x + w) - sceneRect.right()) < SNAP_THRESHOLD) { targetX = sceneRect.right() - w; lineX = sceneRect.right(); showV = true; }

        bool showH = false; qreal targetY = y; qreal lineY = 0;
        if (std::abs((y + h/2) - sceneRect.center().y()) < SNAP_THRESHOLD) { targetY = sceneRect.center().y() - h/2; lineY = sceneRect.center().y(); showH = true; }
        else if (std::abs(y - sceneRect.top()) < SNAP_THRESHOLD) { targetY = sceneRect.top(); lineY = sceneRect.top(); showH = true; }
        else if (std::abs((y + h) - sceneRect.bottom()) < SNAP_THRESHOLD) { targetY = sceneRect.bottom() - h; lineY = sceneRect.bottom(); showH = true; }

        if (showV && m_vLine) { m_vLine->setLine(lineX, sceneRect.top(), lineX, sceneRect.bottom()); m_vLine->setVisible(true); } else if (m_vLine) m_vLine->setVisible(false);
        if (showH && m_hLine) { m_hLine->setLine(sceneRect.left(), lineY, sceneRect.right(), lineY); m_hLine->setVisible(true); } else if (m_hLine) m_hLine->setVisible(false);

        if ((showV && !m_snappedX) || (showH && !m_snappedY)) triggerFeedback();
        m_snappedX = showV; m_snappedY = showH;

        if (targetX < sceneRect.left()) targetX = sceneRect.left();
        if (targetY < sceneRect.top()) targetY = sceneRect.top();
        if (targetX + w > sceneRect.right()) targetX = sceneRect.right() - w;
        if (targetY + h > sceneRect.bottom()) targetY = sceneRect.bottom() - h;

        emit geometryChanged(m_id, QRect(targetX, targetY, w, h));
        return QPointF(targetX, targetY);
    }
    if (change == ItemSceneChange) hideGuideLines();
    return QGraphicsObject::itemChange(change, value);
}
