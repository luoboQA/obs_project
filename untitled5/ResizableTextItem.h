#ifndef RESIZABLETEXTITEM_H
#define RESIZABLETEXTITEM_H

#include "ResizablePixmapItem.h"
#include <QFont>
#include <QColor>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QFontDialog>
#include <QColorDialog>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>

// --- 文字属性配置对话框 ---
class TextConfigDialog : public QDialog {
    Q_OBJECT
public:
    TextConfigDialog(QString text, QFont font, QColor color, bool shadow, QWidget* parent = nullptr)
        : QDialog(parent), m_text(text), m_font(font), m_color(color), m_shadow(shadow)
    {
        setWindowTitle("Text Layer Properties");
        resize(350, 250);
        QVBoxLayout* layout = new QVBoxLayout(this);
        QFormLayout* form = new QFormLayout();

        m_editContent = new QLineEdit(text);
        form->addRow("Content:", m_editContent);

        QPushButton* btnFont = new QPushButton("Select Font...");
        connect(btnFont, &QPushButton::clicked, this, [this](){
            bool ok;
            QFont f = QFontDialog::getFont(&ok, m_font, this);
            if(ok) m_font = f;
        });
        form->addRow("Font:", btnFont);

        QPushButton* btnColor = new QPushButton("Select Color...");
        connect(btnColor, &QPushButton::clicked, this, [this](){
            QColor c = QColorDialog::getColor(m_color, this);
            if(c.isValid()) m_color = c;
        });
        form->addRow("Color:", btnColor);

        m_chkShadow = new QCheckBox("Enable Drop Shadow");
        m_chkShadow->setChecked(shadow);
        form->addRow("Effects:", m_chkShadow);

        layout->addLayout(form);
        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    QString getText() const { return m_editContent->text(); }
    QFont getFont() const { return m_font; }
    QColor getColor() const { return m_color; }
    bool getShadow() const { return m_chkShadow->isChecked(); }

private:
    QLineEdit* m_editContent;
    QCheckBox* m_chkShadow;
    QString m_text;
    QFont m_font;
    QColor m_color;
    bool m_shadow;
};

// --- 可调整文字图层 ---
class ResizableTextItem : public ResizablePixmapItem {
    Q_OBJECT
public:
    ResizableTextItem(const QString &text, int id, QGraphicsItem *parent = nullptr)
        : ResizablePixmapItem(QPixmap(), id, parent), m_text(text)
    {
        m_font.setFamily("Arial");
        m_font.setBold(true);
        m_font.setPixelSize(64); // 默认高清大字体，缩放后清晰
        m_color = Qt::white;
        m_hasShadow = true;
        regeneratePixmap();
    }

    // 双击弹出配置框
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
        TextConfigDialog dlg(m_text, m_font, m_color, m_hasShadow);
        if (dlg.exec() == QDialog::Accepted) {
            m_text = dlg.getText();
            m_font = dlg.getFont();
            m_color = dlg.getColor();
            m_hasShadow = dlg.getShadow();
            regeneratePixmap();
            // 立即通知后端更新纹理
            emit contentChanged(getId(), getPixmap().toImage());
        }
        event->accept();
    }

signals:
    void contentChanged(int id, QImage newImg);

private:
    void regeneratePixmap() {
        QFontMetrics fm(m_font);
        QRect r = fm.boundingRect(m_text);

        // 增加内边距以容纳描边或阴影
        int padding = 20;
        QPixmap pix(r.width() + padding*2, r.height() + padding*2);
        pix.fill(Qt::transparent);

        QPainter p(&pix);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setFont(m_font);

        if (m_hasShadow) {
            // 简单的投影效果：先画黑色偏移，再画彩色
            p.setPen(Qt::black);
            p.drawText(pix.rect().translated(3, 3), Qt::AlignCenter, m_text);
        }

        p.setPen(m_color);
        p.drawText(pix.rect(), Qt::AlignCenter, m_text);
        p.end();

        setPixmap(pix);
    }

private:
    QString m_text;
    QFont m_font;
    QColor m_color;
    bool m_hasShadow;
};

#endif // RESIZABLETEXTITEM_H
