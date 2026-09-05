#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QMap>
#include <QVariant>
#include <QtConcurrent>
#include <QSet>
#include <QMutex>

#include "DxGiCapturer.h"
#include "WinRTCamera.h" // 替换 MfCamera
#include "WasapiCapturer.h"
#include "FFmpegRecorder.h"
#include "RecorderConfig.h"

struct CameraContext {
    int itemId;           // 图层 ID（唯一标识）
    QString deviceId;     // 摄像头设备 ID
    WinRTCamera* camera;  // 摄像头实例指针
};

class SystemController : public QObject
{
    Q_OBJECT
public:
    explicit SystemController(QObject *parent = nullptr);
    ~SystemController();
    void initialize();

private slots:
    // 捕获器(D3D环境)准备好后，启动资源预热
    void onCapturerInitFinished(bool success);

    void onChangeSource(int type, size_t id);
    void onAddCamera(QVariant deviceVariant);
    void onToggleRecording(bool start);
    void onUpdateConfig(RecorderConfig cfg);
    void onRefreshDevices();
    void onTakeSnapshot();
    void onChangeAudioDevice(bool isMic, QString deviceId);
    void onToggleMicCapture(bool active);
    void onUiFrameProcessed();
    void onOverlayGeometryChanged(int id, QRect rect);
    void onVideoFrameCaptured(const QImage& img, qint64 timestamp);
    void onSnapshotCaptured(const QImage& img);
    void onDurationTimer();

private:
    void startVideoThread();
    void stopVideoThread();
    void restartAudio(bool isMic, const QString& deviceId);

    // 核心优化：预热所有摄像头
    void prewarmAllCameras();
    void cleanupCameraPool();

/*
主线程（GUI）
    ├── SystemController
    ├── FFmpegRecorder
    ├── WasapiCapturer (麦克风)
    ├── WasapiCapturer (系统音频)
    ├── QTimer (时长更新)
    └── 信号槽(EventBus)通信

视频线程 (m_videoThread)
    └── DxGiCapturer
        ├── DXGI 屏幕捕获
        ├── WinRTCamera (摄像头池)
        └── 覆盖层(Overlay)渲染
*/    
private:
    QThread* m_videoThread = nullptr;           // 视频捕获工作线程（真正的线程）
    DxGiCapturer* m_capturer = nullptr;         // DXGI 屏幕/窗口捕获器（运行在 m_videoThread ）
    FFmpegRecorder* m_recorder = nullptr;       // 录制器（运行在主线程SystemController）
    WasapiCapturer* m_micCapturer = nullptr;    // 麦克风捕获器（运行在主线程SystemController）
    WasapiCapturer* m_sysCapturer = nullptr;    // 系统音频捕获器（运行在主线程SystemController）

    QList<CameraContext*> m_activeLayers; // 活动中的图层列表

    // 资源池：[DeviceId] -> [Running WinRTCamera Instance]
    // 这些实例在程序生命周期内一直存活并运行
    QMap<QString, WinRTCamera*> m_cameraPool;
    QMutex m_poolMutex; // 保护摄像头池的互斥锁

    int m_nextItemId = 100; //图层 ID 自增计数器（从 100 开始）
    RecorderConfig m_config; // 当前配置
    QTimer* m_durationTimer; // 录制时长更新定时器（每秒）
    QElapsedTimer m_recordElapsed; // 录制耗时计时器
    QString m_currentMicId; // 当前麦克风设备 ID
    QString m_currentSysId; // 当前系统音频设备 ID
    bool m_isMicEnabled = false; // 麦克风是否启用
};
#endif // SYSTEMCONTROLLER_H
