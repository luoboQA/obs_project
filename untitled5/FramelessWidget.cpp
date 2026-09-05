#include "FramelessWidget.h"
#include <QStyleOption>
#include <QPainter>

enum Direction {
    None = 0, Left = 1, Top = 2, Right = 4, Bottom = 8,
    TopLeft = 3, TopRight = 6, BottomLeft = 9, BottomRight = 12
};

FramelessWidget::FramelessWidget(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint);
    setMouseTracking(true);
    setAttribute(Qt::WA_StyledBackground);
}

void FramelessWidget::setResizable(bool enable) {
    if (m_isResizable == enable) return;
    m_isResizable = enable;
    if (m_isResizable) {
        setMinimumSize(800, 600); // 限制最小尺寸
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    } else {
        setFixedSize(this->size());
        setCursor(Qt::ArrowCursor);
    }
}

void FramelessWidget::setMaximizable(bool enable) {
    m_isMaximizable = enable;
}

void FramelessWidget::setTitleBarHeight(int height) {
    m_titleBarHeight = height;
}

int FramelessWidget::getRegion(const QPoint &pos) {
    if (m_isMaximizedState) return Direction::None;
    int w = width(); int h = height();
    bool isTop = (pos.y() <= m_cornerWidth);
    bool isBottom = (pos.y() >= h - m_cornerWidth);
    bool isLeft = (pos.x() <= m_cornerWidth);
    bool isRight = (pos.x() >= w - m_cornerWidth);

    if (isTop && isLeft) return Direction::TopLeft;
    if (isTop && isRight) return Direction::TopRight;
    if (isBottom && isLeft) return Direction::BottomLeft;
    if (isBottom && isRight) return Direction::BottomRight;

    if (pos.x() <= m_borderWidth) return Direction::Left;
    if (pos.x() >= w - m_borderWidth) return Direction::Right;
    if (pos.y() <= m_borderWidth) return Direction::Top;
    if (pos.y() >= h - m_borderWidth) return Direction::Bottom;

    return Direction::None;
}

void FramelessWidget::updateCursorShape(const QPoint &pos) {
    if (m_isMaximizedState || !m_isResizable) {
        if (cursor().shape() != Qt::ArrowCursor) setCursor(Qt::ArrowCursor);
        return;
    }
    int dir = getRegion(pos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    switch (dir) {
    case Direction::TopLeft: case Direction::BottomRight: shape = Qt::SizeFDiagCursor; break;
    case Direction::TopRight: case Direction::BottomLeft: shape = Qt::SizeBDiagCursor; break;
    case Direction::Left: case Direction::Right: shape = Qt::SizeHorCursor; break;
    case Direction::Top: case Direction::Bottom: shape = Qt::SizeVerCursor; break;
    default: shape = Qt::ArrowCursor; break;
    }
    if (cursor().shape() != shape) setCursor(shape);
}

void FramelessWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_isPressed = true;
        m_dragPos = event->pos();
        if (m_isResizable && !m_isMaximizedState) {
            m_resizeDir = getRegion(event->pos());
            if (m_resizeDir != Direction::None) return;
        }
        if (getRegion(event->pos()) == Direction::None && event->pos().y() <= m_titleBarHeight) {
            m_isDragWindow = true;
        }
    }
    QWidget::mousePressEvent(event);
}

void FramelessWidget::mouseMoveEvent(QMouseEvent *event) {
    if (!m_isPressed) {
        updateCursorShape(event->pos());
        return;
    }
    if (m_isResizable && m_resizeDir != Direction::None && !m_isMaximizedState) {
        QRect rect = geometry();
        QPoint gp = event->globalPos();
        QSize minSize = minimumSize();
        if (m_resizeDir & Direction::Left) {
            int delta = gp.x() - rect.left();
            if (rect.width() - delta > minSize.width()) rect.setLeft(gp.x());
        } else if (m_resizeDir & Direction::Right) {
            int delta = gp.x() - rect.right();
            if (rect.width() + delta > minSize.width()) rect.setRight(gp.x());
        }
        if (m_resizeDir & Direction::Top) {
            int delta = gp.y() - rect.top();
            if (rect.height() - delta > minSize.height()) rect.setTop(gp.y());
        } else if (m_resizeDir & Direction::Bottom) {
            int delta = gp.y() - rect.bottom();
            if (rect.height() + delta > minSize.height()) rect.setBottom(gp.y());
        }
        setGeometry(rect);
        return;
    }
    if (m_isDragWindow && !m_isMaximizedState) {
        move(event->globalPos() - m_dragPos);
    }
}

void FramelessWidget::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    m_isPressed = false; m_isDragWindow = false; m_resizeDir = Direction::None;
    updateCursorShape(mapFromGlobal(QCursor::pos()));
}

void FramelessWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (m_isMaximizable && event->button() == Qt::LeftButton) {
        if (getRegion(event->pos()) == Direction::None && event->pos().y() <= m_titleBarHeight) {
            toggleMaximized();
        }
    }
}

void FramelessWidget::changeEvent(QEvent *event) {
    if (event->type() == QEvent::WindowStateChange) {
        if (windowState() & Qt::WindowMaximized) {
            m_isMaximizedState = true;
            m_restoreRect = geometry();
        } else if (windowState() & Qt::WindowNoState) {
            m_isMaximizedState = false;
        }
    }
    QWidget::changeEvent(event);
}

void FramelessWidget::paintEvent(QPaintEvent *event) {
    QStyleOption opt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    opt.initFrom(this);
#else
    opt.init(this);
#endif
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    QWidget::paintEvent(event);
}

void FramelessWidget::onMinimizeRequested() { showMinimized(); }
void FramelessWidget::onMaximizeRequested() { toggleMaximized(); }
void FramelessWidget::onCloseRequested() { close(); }

void FramelessWidget::toggleMaximized() {
    if (!m_isMaximizable) return;
    if (m_isMaximizedState) {
        setGeometry(m_restoreRect);
        m_isMaximizedState = false;
    } else {
        QScreen *screen = QGuiApplication::screenAt(geometry().center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        m_restoreRect = geometry();
        setGeometry(screen->availableGeometry());
        m_isMaximizedState = true;
    }
}
