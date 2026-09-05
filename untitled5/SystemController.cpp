#include "SystemController.h"
#include "EventBus.h"
#include "Logger.h"
#include <QDebug>
#include <QDateTime>
#include <QMediaDevices>
#include <QCameraDevice>
#include <roapi.h>

SystemController::SystemController(QObject *parent) : QObject(parent) {
    m_recorder = new FFmpegRecorder(nullptr);
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);
    connect(m_durationTimer, &QTimer::timeout, this, &SystemController::onDurationTimer);
    LOG_INFO("SystemController Created.");
}

SystemController::~SystemController() {
    LOG_INFO("SystemController Destroying...");
    if (m_recorder) { m_recorder->stopRecord(); m_recorder->wait(); delete m_recorder; m_recorder = nullptr; }

    stopVideoThread();
    cleanupCameraPool();

    if (m_micCapturer) { m_micCapturer->stop(); m_micCapturer->wait(); delete m_micCapturer; }
    if (m_sysCapturer) { m_sysCapturer->stop(); m_sysCapturer->wait(); delete m_sysCapturer; }
    LOG_INFO("SystemController Destroyed.");
}

// 信号连接（UI → Controller）
void SystemController::initialize() {
    LOG_INFO("SystemController::initialize() Start");
    EventBus* bus = EventBus::instance();
    connect(bus, &EventBus::cmd_changeSource, this, &SystemController::onChangeSource);
    connect(bus, &EventBus::cmd_addCamera, this, &SystemController::onAddCamera);
    connect(bus, &EventBus::cmd_toggleRecording, this, &SystemController::onToggleRecording);
    connect(bus, &EventBus::cmd_updateConfig, this, &SystemController::onUpdateConfig);
    connect(bus, &EventBus::cmd_refreshDevices, this, &SystemController::onRefreshDevices);
    connect(bus, &EventBus::cmd_takeSnapshot, this, &SystemController::onTakeSnapshot);
    connect(bus, &EventBus::cmd_changeAudioDevice, this, &SystemController::onChangeAudioDevice);
    connect(bus, &EventBus::cmd_uiFrameProcessed, this, &SystemController::onUiFrameProcessed);
    connect(bus, &EventBus::cmd_overlayGeometryChanged, this, &SystemController::onOverlayGeometryChanged);
    connect(bus, &EventBus::cmd_toggleMicCapture, this, &SystemController::onToggleMicCapture);

    // 连接删除信号，并清理活动图层记录
    connect(bus, &EventBus::cmd_removeOverlay, this, [this](int id){
        // 1. 通知后台渲染线程删除 (invokeMethod 依赖 main.cpp 中的注册)
        if (m_capturer) QMetaObject::invokeMethod(m_capturer, "removeOverlay", Qt::QueuedConnection, Q_ARG(int, id));

        // 2. 从活动图层列表中移除，允许重新添加
        QMutexLocker locker(&m_poolMutex); // 保护 m_activeLayers
        for (int i = 0; i < m_activeLayers.size(); ++i) {
            if (m_activeLayers[i]->itemId == id) {
                delete m_activeLayers[i]; // 删除 Context 对象
                m_activeLayers.removeAt(i);
                LOG_INFO(QString("Camera Layer %1 removed from active list.").arg(id));
                break;
            }
        }
    });

    // 连接排序信号,更新渲染顺序 (QList<int> 需要已在 main.cpp 中注册元类型)
    connect(bus, &EventBus::cmd_updateRenderOrder, this, [this](QList<int> order){
        if (m_capturer) QMetaObject::invokeMethod(m_capturer, "updateRenderOrder", Qt::QueuedConnection, Q_ARG(QList<int>, order));
    });
    // 启动视频捕获线程
    startVideoThread();
    LOG_INFO("SystemController::initialize() Return");
}

// 视频捕获线程
void SystemController::startVideoThread() {
    if (m_videoThread) return;
    LOG_INFO("Starting Video Thread...");
    m_videoThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_videoThread);

    // 线程启动时初始化捕获器
    connect(m_videoThread, &QThread::started, m_capturer, [this]() {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr)) CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (m_capturer->init()) {
            m_capturer->startCapture();
        } else {
            EventBus::instance()->fireError("Failed to initialize DXGI Capturer");
        }
    });

    // 跨线程连接（从 UI 线程到捕获线程）
    connect(EventBus::instance(), &EventBus::data_overlayUpdate, m_capturer, &DxGiCapturer::updateOverlayImage, Qt::QueuedConnection);
    connect(EventBus::instance(), &EventBus::cmd_overlayGeometryChanged, m_capturer, &DxGiCapturer::updateOverlayGeometry, Qt::QueuedConnection);

    // 从捕获线程到主线程
    connect(m_capturer, &DxGiCapturer::initFinished, this, &SystemController::onCapturerInitFinished, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::frameCaptured, this, &SystemController::onVideoFrameCaptured, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::frameCapturedForPreview, EventBus::instance(), &EventBus::data_previewFrame, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::snapshotCaptured, this, &SystemController::onSnapshotCaptured, Qt::QueuedConnection);
    
    // 线程清理
    connect(m_videoThread, &QThread::finished, m_capturer, &QObject::deleteLater);
    connect(m_videoThread, &QThread::finished, this, [](){ RoUninitialize(); });

    m_videoThread->start();
}

void SystemController::stopVideoThread() {
    if (m_capturer) QMetaObject::invokeMethod(m_capturer, "stopCapture", Qt::BlockingQueuedConnection);
    if (m_videoThread) {
        m_videoThread->quit();
        m_videoThread->wait(3000);
        delete m_videoThread;
        m_videoThread = nullptr;
        m_capturer = nullptr;
    }
}

void SystemController::onCapturerInitFinished(bool success) {
    if (success) {
        onRefreshDevices();      // 刷新设备列表
        restartAudio(false, ""); // 启动系统音频捕获
        prewarmAllCameras();     // 预热所有摄像头
    }
}

void SystemController::prewarmAllCameras() {
    if (!m_capturer) return;
    QMetaObject::invokeMethod(m_capturer, [this]() {
        const auto cameras = QMediaDevices::videoInputs();
        ID3D11Device* rawDevice = m_capturer->getD3DDevice();
        if (!rawDevice) return;
        QMutexLocker locker(&m_poolMutex);
        for (const auto& info : cameras) {
            QString deviceId = QString::fromUtf8(info.id());
            if (m_cameraPool.contains(deviceId)) continue;
            WinRTCamera* cam = new WinRTCamera();
            if (cam->init(rawDevice) && cam->start(deviceId)) {
                m_cameraPool.insert(deviceId, cam);
            } else { delete cam; }
        }
    });
}

void SystemController::cleanupCameraPool() {
    QMutexLocker locker(&m_poolMutex);
    for (auto cam : m_cameraPool) { cam->stop(); delete cam; }
    m_cameraPool.clear();
    for (auto ctx : m_activeLayers) delete ctx;
    m_activeLayers.clear();
}

void SystemController::onAddCamera(QVariant deviceVariant) {
    QCameraDevice info = deviceVariant.value<QCameraDevice>();
    QString deviceId = QString::fromUtf8(info.id());

    // 防止同一个摄像头重复添加到同一个图层ID（如果有逻辑需要），
    // 但这里主要检查是否已经在 activeLayers 中
    {
        QMutexLocker locker(&m_poolMutex);
        for (auto ctx : m_activeLayers) {
            if (ctx->deviceId == deviceId) {
                LOG_INFO("Camera already added.");
                return;
            }
        }
    }

    // 从池中获取或创建摄像头
    WinRTCamera* targetCam = nullptr;
    { QMutexLocker locker(&m_poolMutex); if (m_cameraPool.contains(deviceId)) targetCam = m_cameraPool.value(deviceId); }

    QRect defaultRect(50, 50, 320, 240);
    int id = m_nextItemId++;

    auto setupLayer = [=](WinRTCamera* cam) {
        m_capturer->addCameraOverlay(id, cam, defaultRect);

        CameraContext* ctx = new CameraContext;
        ctx->itemId = id; ctx->deviceId = deviceId; ctx->camera = cam;

        // 必须加入列表，否则无法追踪
        {
            QMutexLocker l(&m_poolMutex);
            m_activeLayers.append(ctx);
        }

        EventBus::instance()->fireCameraAdded(id, defaultRect);
    };

    if (targetCam) {
        setupLayer(targetCam); // 从池中直接使用
    } else if (m_capturer) {
        // 在捕获器线程中创建（访问 D3D 设备）
        QMetaObject::invokeMethod(m_capturer, [=]() {
            ID3D11Device* device = m_capturer->getD3DDevice();
            WinRTCamera* cam = new WinRTCamera();
            if (cam->init(device) && cam->start(deviceId)) {
                { QMutexLocker l(&m_poolMutex); m_cameraPool.insert(deviceId, cam); }
                QMetaObject::invokeMethod(this, [=]() { setupLayer(cam); });
            } else { delete cam; EventBus::instance()->fireError("Failed to open camera"); }
        });
    }
}

void SystemController::onRefreshDevices() {
    // 1. 获取显示器列表
    QStringList monitors = DxGiCapturer::getMonitorNames();
    QList<QPair<QString, QVariant>> srcList;
    for(int i=0; i<monitors.size(); ++i) srcList.append({ "[Monitor] "+monitors[i], QVariant::fromValue<QList<QVariant>>({0, i}) });
    
    // 2. 获取窗口列表
    QMap<size_t, QString> windows = DxGiCapturer::getWindowList();
    QMapIterator<size_t, QString> i(windows);
    while (i.hasNext()) { i.next(); srcList.append({ "[Window] " + i.value(), QVariant::fromValue<QList<QVariant>>({1, (qulonglong)i.key()}) }); }
    EventBus::instance()->fireSourceListUpdated(srcList);

    // 3. 获取音频设备
    QList<AudioDeviceInfo> mics = WasapiCapturer::getAvailableDevices(WasapiCapturer::Microphone);
    QStringList micNames, micIds; for(auto &d : mics) { micNames << d.name; micIds << d.id; }
    EventBus::instance()->fireAudioDeviceListUpdated(true, micNames, micIds);

    QList<AudioDeviceInfo> speakers = WasapiCapturer::getAvailableDevices(WasapiCapturer::SystemLoopback);
    QStringList sysNames, sysIds; for(auto &d : speakers) { sysNames << d.name; sysIds << d.id; }
    EventBus::instance()->fireAudioDeviceListUpdated(false, sysNames, sysIds);
}

void SystemController::onChangeSource(int type, size_t id) {
    if (!m_capturer) return;
    CaptureTarget target; target.type = (type == 0) ? CaptureSourceType::Monitor : CaptureSourceType::Window;
    if (type == 0) target.monitorIndex = (int)id; else target.windowHandle = (HWND)id;
    m_capturer->setCaptureTarget(target);
}
void SystemController::onToggleRecording(bool start) {
    if (start) {
        if (m_config.savePath.isEmpty()) m_config.savePath = "Rec_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4";
        QMetaObject::invokeMethod(m_capturer, "startRecording", Qt::QueuedConnection);
        if (m_recorder->startRecord(m_config)) { m_recordElapsed.restart(); m_durationTimer->start(); EventBus::instance()->fireRecordingStateChanged(true, m_config.savePath); }
    } else {
        m_recorder->stopRecord(); QMetaObject::invokeMethod(m_capturer, "stopRecording", Qt::QueuedConnection);
        m_durationTimer->stop(); EventBus::instance()->fireRecordingStateChanged(false, ""); EventBus::instance()->fireDurationUpdate("00:00:00");
    }
}
void SystemController::onUpdateConfig(RecorderConfig cfg) { m_config = cfg; }
void SystemController::onTakeSnapshot() { if(m_capturer) m_capturer->requestSnapshot(); }
void SystemController::onChangeAudioDevice(bool isMic, QString deviceId) { if(isMic) { m_currentMicId=deviceId; if(m_isMicEnabled) restartAudio(true, deviceId); } else { m_currentSysId=deviceId; restartAudio(false, deviceId); } }
void SystemController::onToggleMicCapture(bool active) { m_isMicEnabled=active; if(active) restartAudio(true, m_currentMicId); else { if(m_micCapturer) {m_micCapturer->stop(); delete m_micCapturer; m_micCapturer=nullptr;} EventBus::instance()->fireAudioLevel(true, 0); } }
void SystemController::onUiFrameProcessed() { if(m_capturer) QMetaObject::invokeMethod(m_capturer, "onUiFrameProcessed", Qt::QueuedConnection); }
void SystemController::onOverlayGeometryChanged(int id, QRect rect) { if(m_capturer) m_capturer->updateOverlayGeometry(id, rect); }
void SystemController::onVideoFrameCaptured(const QImage& img, qint64 ts) { m_recorder->pushVideoFrame(img, ts); }
void SystemController::onSnapshotCaptured(const QImage& img) { QString f="Snap.png"; img.save(f); EventBus::instance()->fireSnapshotTaken(f); }
void SystemController::onDurationTimer() { qint64 s = m_recordElapsed.elapsed()/1000; EventBus::instance()->fireDurationUpdate(QString("%1:%2:%3").arg(s/3600,2,10,QChar('0')).arg((s%3600)/60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'))); }

void SystemController::restartAudio(bool isMic, const QString& devId) {
    WasapiCapturer** ptr = isMic ? &m_micCapturer : &m_sysCapturer;
    if(*ptr) { (*ptr)->stop(); (*ptr)->wait(); delete *ptr; *ptr=nullptr; }
    *ptr = new WasapiCapturer(this); (*ptr)->setDevice(devId, isMic?WasapiCapturer::Microphone:WasapiCapturer::SystemLoopback);
    connect(*ptr, &WasapiCapturer::volumeLevelChanged, this, [isMic](float l){EventBus::instance()->fireAudioLevel(isMic, l);});
    connect(*ptr, &WasapiCapturer::audioDataReady, m_recorder, [this, isMic](QByteArray d, int r, int c){ if(m_recorder) m_recorder->pushAudioData(isMic?1:0, d, r, c); });
    (*ptr)->start();
}
