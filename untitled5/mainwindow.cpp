#include "MainWindow.h"
#include "SettingsDialog.h"
#include "ResizablePixmapItem.h"
#include "ResizableTextItem.h"
#include "AddSourceDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QTimer>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QInputDialog>
#include "Logger.h"
#include <QPainter>

MainWindow::MainWindow(QWidget *parent) : FramelessWidget(parent)
{
    setupUi();

    setFixedSize(1280, 720);
    setTitleBarHeight(40);
    applyWindowConfig();
    m_lastSceneSize = QSize(0, 0);

    EventBus* bus = EventBus::instance();
    connect(bus, &EventBus::data_previewFrame, this, &MainWindow::updatePreview);
    connect(bus, &EventBus::data_overlayUpdate, this, &MainWindow::updateOverlay);
    connect(bus, &EventBus::data_audioLevel, this, &MainWindow::updateAudioLevel);

    connect(bus, &EventBus::state_recordingChanged, this, &MainWindow::onRecordingStateChanged);
    connect(bus, &EventBus::state_durationUpdated, this, &MainWindow::onDurationUpdate);
    connect(bus, &EventBus::state_sourceListUpdated, this, &MainWindow::onSourceListUpdated);
    connect(bus, &EventBus::state_audioDeviceListUpdated, this, &MainWindow::onAudioDeviceListUpdated);
    connect(bus, &EventBus::state_snapshotTaken, this, &MainWindow::onSnapshotTaken);
    connect(bus, &EventBus::state_error, this, &MainWindow::onError);
    connect(bus, &EventBus::state_cameraAdded, this, &MainWindow::onCameraAdded);

    QTimer::singleShot(500, this, [this](){
        LOG_INFO("UI Safe-Init: Refreshing Cameras & Devices...");
        this->refreshCameras();
        EventBus::instance()->sendCommandRefreshDevices();
    });
}

MainWindow::~MainWindow() {}

void MainWindow::refreshCameras() {}

void MainWindow::applyWindowConfig() {
    setResizable(m_config.windowResizable);
    setMaximizable(m_config.windowMaximizable);
    if (m_btnMax) m_btnMax->setEnabled(m_config.windowMaximizable);
}

void MainWindow::setupUi() {
    setStyleSheet(R"(
        QWidget { background-color: #2b2b2b; color: #ffffff; font-family: 'Segoe UI'; }
        QGroupBox { border: 1px solid #444; margin-top: 10px; border-radius: 4px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 3px; }
        QLabel { color: #cccccc; }
        QPushButton { background-color: #3e3e3e; border: 1px solid #555; border-radius: 4px; padding: 5px 10px; }
        QPushButton:hover { background-color: #505050; border-color: #666; }
        QPushButton:pressed { background-color: #2d2d2d; }
        QComboBox { background-color: #333; border: 1px solid #555; padding: 4px; border-radius: 3px; }
        QProgressBar { border: 1px solid #555; border-radius: 3px; text-align: center; background: #1a1a1a; }
        QListWidget { background: #222; border: 1px solid #444; outline: none; }
        QListWidget::item { padding: 5px; border-bottom: 1px solid #333; }
        QListWidget::item:selected { background: #448aff; color: white; }
        QPushButton#RecBtn { background-color: #2a2a2a; border: 1px solid #444; font-weight: bold; }
    )");

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    setupTitleBar(rootLayout);

    QWidget* contentWidget = new QWidget();
    rootLayout->addWidget(contentWidget);

    QVBoxLayout* mainVLayout = new QVBoxLayout(contentWidget);
    mainVLayout->setContentsMargins(10, 10, 10, 10);
    mainVLayout->setSpacing(10);

    // --- Top Control ---
    QHBoxLayout *topLayout = new QHBoxLayout();
    QGroupBox *grpVideo = new QGroupBox("Primary Source");
    QHBoxLayout *hboxSrc = new QHBoxLayout(grpVideo);
    hboxSrc->addWidget(new QLabel("Capture:"));
    m_comboSources = new QComboBox();
    m_comboSources->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_comboSources, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSourceComboChanged);
    hboxSrc->addWidget(m_comboSources);
    QPushButton* btnRefresh = new QPushButton("⟳"); btnRefresh->setFixedWidth(30);
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onBtnRefreshClicked);
    hboxSrc->addWidget(btnRefresh);
    topLayout->addWidget(grpVideo, 1);

    QGroupBox *grpAudio = new QGroupBox("Audio Mixer");
    QHBoxLayout *hboxAudio = new QHBoxLayout(grpAudio);
    hboxAudio->addWidget(new QLabel("Sys:"));
    m_comboSysAudio = new QComboBox(); m_comboSysAudio->setMinimumWidth(120);
    connect(m_comboSysAudio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSysComboChanged);
    hboxAudio->addWidget(m_comboSysAudio);
    m_progressSys = new QProgressBar(); m_progressSys->setFixedWidth(60);
    hboxAudio->addWidget(m_progressSys);
    hboxAudio->addSpacing(10);
    m_chkMixMic = new QCheckBox("Mic");
    connect(m_chkMixMic, &QCheckBox::toggled, this, &MainWindow::onChkMixMicToggled);
    hboxAudio->addWidget(m_chkMixMic);
    m_comboMic = new QComboBox(); m_comboMic->setMinimumWidth(120); m_comboMic->setEnabled(false);
    connect(m_comboMic, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onMicComboChanged);
    hboxAudio->addWidget(m_comboMic);
    m_progressMic = new QProgressBar(); m_progressMic->setFixedWidth(60); m_progressMic->setEnabled(false);
    hboxAudio->addWidget(m_progressMic);
    topLayout->addWidget(grpAudio, 2);
    mainVLayout->addLayout(topLayout);

    // --- Center ---
    QHBoxLayout *centerLayout = new QHBoxLayout();
    m_scene = new QGraphicsScene(this);
    m_view = new QGraphicsView(m_scene);
    m_view->setRenderHint(QPainter::Antialiasing);
    m_view->setRenderHint(QPainter::SmoothPixmapTransform);
    m_view->setBackgroundBrush(Qt::black);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    centerLayout->addWidget(m_view, 1);

    QPixmap defaultBg(1920, 1080); defaultBg.fill(Qt::black);
    m_bgItem = new QGraphicsPixmapItem(defaultBg);
    m_scene->addItem(m_bgItem);

    QGroupBox *grpLayers = new QGroupBox("Sources / Layers");
    grpLayers->setFixedWidth(280);
    QVBoxLayout *layerLayout = new QVBoxLayout(grpLayers);
    m_layerList = new QListWidget();
    connect(m_layerList, &QListWidget::itemSelectionChanged, this, &MainWindow::onLayerSelectionChanged);
    layerLayout->addWidget(m_layerList);

    QGridLayout *layerBtnGrid = new QGridLayout();
    m_btnAddSource = new QPushButton("Add Source (+)");
    connect(m_btnAddSource, &QPushButton::clicked, this, &MainWindow::onBtnAddSourceClicked);
    m_btnLayerUp = new QPushButton("Up ▲");
    connect(m_btnLayerUp, &QPushButton::clicked, this, &MainWindow::onBtnLayerUpClicked);
    m_btnLayerDown = new QPushButton("Down ▼");
    connect(m_btnLayerDown, &QPushButton::clicked, this, &MainWindow::onBtnLayerDownClicked);
    m_btnLayerDel = new QPushButton("Remove 🗑");
    connect(m_btnLayerDel, &QPushButton::clicked, this, &MainWindow::onBtnLayerDelClicked);
    layerBtnGrid->addWidget(m_btnAddSource, 0, 0, 1, 3);
    layerBtnGrid->addWidget(m_btnLayerUp, 1, 0);
    layerBtnGrid->addWidget(m_btnLayerDown, 1, 1);
    layerBtnGrid->addWidget(m_btnLayerDel, 1, 2);
    layerLayout->addLayout(layerBtnGrid);
    centerLayout->addWidget(grpLayers, 0);
    mainVLayout->addLayout(centerLayout);

    // --- Bottom ---
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_btnSettings = new QPushButton("⚙ Settings");
    connect(m_btnSettings, &QPushButton::clicked, this, &MainWindow::onBtnSettingsClicked);
    btnLayout->addWidget(m_btnSettings);
    m_lblDuration = new QLabel("00:00:00");
    m_lblDuration->setStyleSheet("color: #ff5555; font-weight: bold; font-size: 16px; margin-right: 15px;");
    m_lblDuration->setVisible(false);
    btnLayout->addWidget(m_lblDuration);
    QPushButton *btnSnap = new QPushButton("Snapshot");
    connect(btnSnap, &QPushButton::clicked, this, &MainWindow::onBtnSnapshotClicked);
    btnLayout->addWidget(btnSnap);
    m_btnRecord = new QPushButton("● Start Recording");
    m_btnRecord->setObjectName("RecBtn");
    m_btnRecord->setMinimumHeight(36);
    connect(m_btnRecord, &QPushButton::clicked, this, &MainWindow::onBtnRecordClicked);
    btnLayout->addWidget(m_btnRecord);
    mainVLayout->addLayout(btnLayout);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_view && m_scene) {
        QRectF rect = m_scene->sceneRect();
        if (rect.width() > 1 && rect.height() > 1) m_view->fitInView(rect, Qt::KeepAspectRatio);
    }
}

// --- 添加源逻辑 ---
void MainWindow::onBtnAddSourceClicked() {
    AddSourceDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        SourceSelection sel = dlg.getSelection();

        // 1. 添加图片
        if (sel.addImage && !sel.imagePath.isEmpty()) {
            QPixmap pix(sel.imagePath);
            if (!pix.isNull()) {
                int id = m_nextImageId++;
                ResizablePixmapItem *item = new ResizablePixmapItem(pix, id);
                item->setPos(50, 50);
                m_scene->addItem(item);
                m_overlayItems.insert(id, item);

                addLayerToList(id, QFileInfo(sel.imagePath).fileName(), "[IMG]");

                connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);

                // 创建时必须立即发送几何信息和像素数据
                EventBus::instance()->sendOverlayGeometryChanged(id, QRect(50, 50, pix.width(), pix.height()));
                EventBus::instance()->fireOverlayUpdate(id, pix.toImage());
            }
        }

        // 2. 添加文字
        if (sel.addText && !sel.textContent.isEmpty()) {
            int id = m_nextImageId++;
            ResizableTextItem *item = new ResizableTextItem(sel.textContent, id);
            item->setPos(100, 100);
            m_scene->addItem(item);
            m_overlayItems.insert(id, item);

            addLayerToList(id, sel.textContent, "[TXT]");

            connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
            // 监听双击修改文字后，更新后台纹理
            connect(item, &ResizableTextItem::contentChanged, [this](int id, QImage img){
                EventBus::instance()->fireOverlayUpdate(id, img);
            });

            // 创建时立即发送
            QRectF r = item->getImageRect();
            EventBus::instance()->sendOverlayGeometryChanged(id, r.toRect());
            EventBus::instance()->fireOverlayUpdate(id, item->getPixmap().toImage());
        }

        // 3. 添加摄像头 (异步)
        if (sel.addCamera && sel.cameraDevice.isValid()) {
            EventBus::instance()->sendCommandAddCamera(sel.cameraDevice);
        }

        updateZOrder();
    }
}

void MainWindow::onCameraAdded(int id, QRect rect) {
    QPixmap placeholder(rect.size());
    placeholder.fill(QColor(0, 0, 0, 0));
    QPainter p(&placeholder);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 150), 4, Qt::DashLine));
    p.drawRect(0, 0, rect.width(), rect.height());
    p.setPen(Qt::white);
    p.drawText(placeholder.rect(), Qt::AlignCenter, "CAMERA " + QString::number(id));
    p.end();

    ResizablePixmapItem *item = new ResizablePixmapItem(placeholder, id);
    item->setPos(rect.topLeft());
    item->setZValue(100);
    m_scene->addItem(item);
    m_overlayItems.insert(id, item);

    connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
    addLayerToList(id, QString("Camera %1").arg(id), "[CAM]");
    updateZOrder();
}

void MainWindow::addLayerToList(int id, QString name, QString prefix) {
    QListWidgetItem *item = new QListWidgetItem(QString("%1 %2").arg(prefix).arg(name));
    item->setData(Qt::UserRole, id);
    m_layerList->insertItem(0, item);
    m_layerList->setCurrentItem(item);
}

// --- Z轴同步 ---
void MainWindow::updateZOrder() {
    QList<int> renderOrder;
    int count = m_layerList->count();

    // UI 列表中，index 0 是最上面的元素 (Covering others)
    // GraphicsScene ZValue 越大越在上面
    // Backend 渲染列表：通常按顺序绘制，后绘制的覆盖先绘制的

    // 因此我们需要倒序遍历列表来生成从底到顶的渲染顺序
    for (int i = 0; i < count; ++i) {
        // 列表最下面的是最底层
        QListWidgetItem *item = m_layerList->item(count - 1 - i);
        int id = item->data(Qt::UserRole).toInt();

        if (m_overlayItems.contains(id)) {
            // Scene ZValue: Base(10) + i
            m_overlayItems[id]->setZValue(10 + i);
            renderOrder.append(id);
        }
    }

    EventBus::instance()->sendCommandUpdateRenderOrder(renderOrder);
}

void MainWindow::onBtnLayerUpClicked() {
    int row = m_layerList->currentRow();
    if (row > 0) {
        QListWidgetItem *item = m_layerList->takeItem(row);
        m_layerList->insertItem(row - 1, item);
        m_layerList->setCurrentRow(row - 1);
        updateZOrder();
    }
}

void MainWindow::onBtnLayerDownClicked() {
    int row = m_layerList->currentRow();
    if (row < m_layerList->count() - 1) {
        QListWidgetItem *item = m_layerList->takeItem(row);
        m_layerList->insertItem(row + 1, item);
        m_layerList->setCurrentRow(row + 1);
        updateZOrder();
    }
}

void MainWindow::onBtnLayerDelClicked() {
    int row = m_layerList->currentRow();
    if (row >= 0) {
        QListWidgetItem *item = m_layerList->takeItem(row);
        int id = item->data(Qt::UserRole).toInt();
        delete item;

        if (m_overlayItems.contains(id)) {
            m_scene->removeItem(m_overlayItems[id]);
            delete m_overlayItems[id];
            m_overlayItems.remove(id);
        }

        EventBus::instance()->sendCommandRemoveOverlay(id);
        updateZOrder();
    }
}

void MainWindow::onLayerSelectionChanged() {
    for(auto item : m_overlayItems) item->setSelected(false);
    QList<QListWidgetItem*> selected = m_layerList->selectedItems();
    if (!selected.isEmpty()) {
        int id = selected.first()->data(Qt::UserRole).toInt();
        if (m_overlayItems.contains(id)) m_overlayItems[id]->setSelected(true);
    }
}

void MainWindow::updatePreview(QImage img) {
    if (img.isNull() || img.width() <= 1) return;
    if (m_lastSceneSize.isValid() && !m_lastSceneSize.isEmpty() && m_lastSceneSize != img.size()) {
        qreal scaleX = (qreal)img.width() / m_lastSceneSize.width();
        qreal scaleY = (qreal)img.height() / m_lastSceneSize.height();
        for (auto item : m_overlayItems) {
            QPointF oldPos = item->pos();
            QRectF oldRect = item->getImageRect();
            qreal newX = oldPos.x() * scaleX;
            qreal newY = oldPos.y() * scaleY;
            qreal newW = oldRect.width() * scaleX;
            qreal newH = oldRect.height() * scaleY;
            item->setPos(newX, newY);
            item->setRect(QRectF(0, 0, newW, newH));
            EventBus::instance()->sendOverlayGeometryChanged(item->getId(), QRect(newX, newY, newW, newH));
        }
    }
    m_lastSceneSize = img.size();
    if (m_bgItem) m_bgItem->setPixmap(QPixmap::fromImage(img));
    m_scene->setSceneRect(img.rect());
    if (m_view) m_view->fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
    EventBus::instance()->sendUiFrameProcessed();
}

void MainWindow::setupTitleBar(QVBoxLayout* rootLayout) {
    m_titleBarWidget = new QWidget();
    m_titleBarWidget->setFixedHeight(40);
    m_titleBarWidget->setStyleSheet("background-color: #1f1f1f; border-bottom: 1px solid #333;");
    QHBoxLayout *header = new QHBoxLayout(m_titleBarWidget);
    header->setContentsMargins(15, 0, 5, 0);
    m_lblTitle = new QLabel("OBS Studio (Qt Enterprise Edition)");
    m_lblTitle->setStyleSheet("font-weight: bold; font-size: 13px; color: #ddd;");
    header->addWidget(m_lblTitle);
    header->addStretch();
    m_btnMin = new QPushButton("_"); m_btnMin->setFixedSize(30, 30);
    connect(m_btnMin, &QPushButton::clicked, this, &FramelessWidget::onMinimizeRequested);
    header->addWidget(m_btnMin);
    m_btnMax = new QPushButton("□"); m_btnMax->setFixedSize(30, 30); m_btnMax->setEnabled(m_config.windowMaximizable);
    connect(m_btnMax, &QPushButton::clicked, this, &FramelessWidget::onMaximizeRequested);
    header->addWidget(m_btnMax);
    m_btnClose = new QPushButton("✕"); m_btnClose->setFixedSize(30, 30);
    connect(m_btnClose, &QPushButton::clicked, this, &FramelessWidget::onCloseRequested);
    header->addWidget(m_btnClose);
    rootLayout->addWidget(m_titleBarWidget);
}

void MainWindow::onBtnSettingsClicked() {
    SettingsDialog dlg(m_config, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_config = dlg.getConfig();
        EventBus::instance()->sendCommandUpdateConfig(m_config);
        applyWindowConfig();
    }
}

void MainWindow::onBtnRefreshClicked() { this->refreshCameras(); EventBus::instance()->sendCommandRefreshDevices(); }
void MainWindow::onBtnRecordClicked() { EventBus::instance()->sendCommandToggleRecording(!m_isRecording); }
void MainWindow::onChkMixMicToggled(bool checked) { m_comboMic->setEnabled(checked); m_progressMic->setEnabled(checked); EventBus::instance()->sendCommandToggleMicCapture(checked); }
void MainWindow::onBtnSnapshotClicked() { EventBus::instance()->sendCommandSnapshot(); }
void MainWindow::onSourceComboChanged(int index) { if (index < 0) return; QList<QVariant> data = m_comboSources->itemData(index).value<QList<QVariant>>(); if (data.size() == 2) EventBus::instance()->sendCommandChangeSource(data[0].toInt(), (size_t)data[1].toULongLong()); }
void MainWindow::onMicComboChanged(int index) { if (index < 0) return; EventBus::instance()->sendCommandChangeAudioDevice(true, m_comboMic->itemData(index).toString()); }
void MainWindow::onSysComboChanged(int index) { if (index < 0) return; EventBus::instance()->sendCommandChangeAudioDevice(false, m_comboSysAudio->itemData(index).toString()); }
void MainWindow::onItemGeometryChanged(int id, QRect rect) { EventBus::instance()->sendOverlayGeometryChanged(id, rect); }
void MainWindow::updateOverlay(int id, QImage img) { if (!img.isNull() && m_overlayItems.contains(id)) m_overlayItems[id]->setPixmap(QPixmap::fromImage(img)); }
void MainWindow::updateAudioLevel(bool isMic, float level) { int val = static_cast<int>(level * 100); if(val>100)val=100; if (isMic) m_progressMic->setValue(val); else m_progressSys->setValue(val); }
void MainWindow::onRecordingStateChanged(bool isRecording, QString msg) { m_isRecording = isRecording; if (isRecording) { m_btnRecord->setText("■ Stop"); m_btnRecord->setStyleSheet("background-color: #880000;"); m_lblDuration->setVisible(true); } else { m_btnRecord->setText("● Record"); m_btnRecord->setStyleSheet("background-color: #2a2a2a;"); if(!msg.isEmpty()) QMessageBox::information(this,"Saved",msg); } }
void MainWindow::onDurationUpdate(QString timeStr) { m_lblDuration->setText(timeStr); }
void MainWindow::onSourceListUpdated(QList<QPair<QString, QVariant>> sources) { m_comboSources->blockSignals(true); m_comboSources->clear(); for(auto& pair:sources) m_comboSources->addItem(pair.first, pair.second); m_comboSources->blockSignals(false); }
void MainWindow::onAudioDeviceListUpdated(bool isMic, QStringList names, QStringList ids) { QComboBox* box=isMic?m_comboMic:m_comboSysAudio; box->blockSignals(true); box->clear(); for(int i=0;i<names.size();++i) box->addItem(names[i], ids[i]); box->blockSignals(false); }
void MainWindow::onSnapshotTaken(QString path) { QMessageBox::information(this, "Snap", "Saved: "+path); }
void MainWindow::onError(QString msg) { QMessageBox::critical(this, "Error", msg); }
void MainWindow::onBtnLoadImageClicked() {}
void MainWindow::onBtnAddCamClicked() {}
