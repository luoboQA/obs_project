#ifndef DXGICAPTURER_H
#define DXGICAPTURER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QMap>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "WgcCapturer.h"
#include "WinRTCamera.h"

using Microsoft::WRL::ComPtr;

enum class CaptureSourceType { Monitor, Window };

struct CaptureTarget {
    CaptureSourceType type = CaptureSourceType::Monitor;
    int monitorIndex = 0;
    HWND windowHandle = nullptr;
    bool operator!=(const CaptureTarget& other) const {
        return type != other.type || monitorIndex != other.monitorIndex || windowHandle != other.windowHandle;
    }
};

enum class OverlayType { Image, Camera };

struct OverlayData {
    int id = 0;
    OverlayType type = OverlayType::Image;
    QImage image; // 存储图片或文字的像素数据
    WinRTCamera* camera = nullptr;
    QRect rect;
    bool textureDirty = false; // 标记是否需要上传到GPU
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};

class DxGiCapturer : public QObject
{
    Q_OBJECT
public:
    explicit DxGiCapturer(QObject *parent = nullptr);
    ~DxGiCapturer();

    static QStringList getMonitorNames();
    static QMap<size_t, QString> getWindowList();

    bool init();
    ID3D11Device* getD3DDevice() const { return m_d3dDevice.Get(); }

    void setCaptureTarget(const CaptureTarget& target);
    void setPreviewEnabled(bool enabled);

    // 在同一个线程调用时可以直接使用，但为了安全建议都通过 Slot 调用
    void addOverlay(int id, const QImage& img, const QRect& rect);
    void addCameraOverlay(int id, WinRTCamera* camera, const QRect& rect);

public slots:
    // --- 必须声明为 Slots 才能被 SystemController invokeMethod 调用 ---
    void updateRenderOrder(QList<int> order);
    void updateOverlayImage(int id, const QImage& img);
    void updateOverlayGeometry(int id, const QRect& rect);
    void removeOverlay(int id);

    void requestSnapshot();
    void stopCapture();
    void startCapture();
    void onUiFrameProcessed();
    void startRecording();
    void stopRecording();

signals:
    void initFinished(bool success);
    void frameCaptured(const QImage &img, qint64 timestamp);
    void frameCapturedForPreview(const QImage &img);
    void snapshotCaptured(const QImage &img);
    void captureStopped();
    void errorOccurred(QString msg);

private:
    void cleanup();
    void resetCaptureState();
    bool initDevice(IDXGIAdapter* adapter = nullptr);
    bool initShaders();
    bool initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl);
    void renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground = false);
    void processPendingUpdates();
    void updateTextureFromImage(OverlayData* data);
    void preciseSleep(double seconds);

private:
    std::atomic<bool> m_running{false};
    std::atomic<int> m_snapshotRequested{0};
    std::atomic<bool> m_previewEnabled{true};
    std::atomic<bool> m_isRecordingMP4{false};
    std::atomic<int> m_uiInFlight{0};

    QMutex m_targetMutex;
    CaptureTarget m_currentTarget;
    CaptureTarget m_pendingTarget;
    bool m_targetChanged = false;

    // 采集生命周期锁：startCapture() 每轮迭代持锁，cleanup()（析构路径）也要
    // 先拿到它才释放捕获资源 → 析构与采集循环不会并发使用 m_wgcCapturer 等对象。
    // 用 QRecursiveMutex（Qt 6 已无 QMutex::Recursive）：采集循环持锁期间可能经
    // initDuplication() → initDevice() → cleanup() 再次加锁（同线程，需可重入）。
    QRecursiveMutex m_teardownMutex;

    WgcCapturer* m_wgcCapturer = nullptr;

    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<IDXGIOutputDuplication> m_deskDupl;
    LUID m_currentAdapterLuid = { 0, 0 };

    // Resources
    ComPtr<ID3D11Texture2D> m_currentMonitorTexture;
    ComPtr<ID3D11Texture2D> m_compositionTexture;
    ComPtr<ID3D11RenderTargetView> m_compositionRTV;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    ComPtr<ID3D11ShaderResourceView> m_cachedSRV;
    ComPtr<ID3D11Texture2D> m_capturedTexture;

    // Shaders
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;

    int m_width = 0; int m_height = 0;
    QMutex m_dataMutex;
    QMap<int, OverlayData*> m_overlays;
    QList<int> m_pendingRemovals;
    QList<int> m_renderOrder; // 渲染顺序 ID 列表
};

#endif // DXGICAPTURER_H
