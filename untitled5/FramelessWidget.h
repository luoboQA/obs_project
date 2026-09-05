#ifndef FRAMELESSWIDGET_H
#define FRAMELESSWIDGET_H

#include <QWidget>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QApplication>

class FramelessWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FramelessWidget(QWidget *parent = nullptr);
    virtual ~FramelessWidget() {}

    void setResizable(bool enable);
    void setMaximizable(bool enable);
    void setTitleBarHeight(int height);

public slots:
    void onMinimizeRequested();
    void onMaximizeRequested();
    void onCloseRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateCursorShape(const QPoint &pos);
    int getRegion(const QPoint &pos);
    void toggleMaximized();

protected:
    bool m_isResizable = false;
    bool m_isMaximizable = false;
    int m_titleBarHeight = 30;
    int m_borderWidth = 6;
    int m_cornerWidth = 20;
    bool m_isMaximizedState = false;

private:
    bool m_isPressed = false;
    bool m_isDragWindow = false;
    int m_resizeDir = 0;
    QPoint m_dragPos;
    QRect m_restoreRect;
};

#endif // FRAMELESSWIDGET_H
