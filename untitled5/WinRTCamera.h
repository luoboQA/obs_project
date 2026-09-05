#ifndef WINRTCAMERA_H
#define WINRTCAMERA_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <QObject>
#include <QMutex>
#include <atomic>
#include <memory>
#include <QString>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Capture::Frames;

class WinRTCamera : public QObject
{
    Q_OBJECT
public:
    explicit WinRTCamera(QObject *parent = nullptr);
    ~WinRTCamera();

    bool init(ID3D11Device* device);
    bool start(const QString& deviceId);
    void stop();

    ComPtr<ID3D11ShaderResourceView> getLatestFrame();

    QString getDeviceId() const { return m_currentDeviceId; }
    bool isRunning() const { return m_impl->running; }

private:
    // 【线程安全】
    // FrameArrived 回调运行在 MediaFrameReader 自己的后台线程上，而摄像头对象
    // 可能被主线程随时 stop() + delete（SystemController::cleanupCameraPool 等）。
    // 回调若捕获裸 this，对象销毁后任何在途回调都会 use-after-free（与 WgcCapturer
    // 同源的崩溃模式）。因此回调可能访问的所有状态都放入 Impl，由 shared_ptr 持有：
    //   - 回调 lambda 捕获 m_impl 的拷贝 → 即使 WinRTCamera 被销毁，Impl 仍随
    //     回调存活，不会访问已释放内存；
    //   - 纹理上传全程持 impl->mutex；stop() 先在同一把锁内置 closed 再拆除回调，
    //     回调要么完整执行完，要么看到 closed 立即退出。
    struct Impl {
        std::atomic<bool> running{ false }; // start 成功置 true
        std::atomic<bool> closed{ false };  // stop()/析构后为 true，回调据此退出
        QMutex mutex;                       // 保护纹理状态（与 getLatestFrame 共用）
        ComPtr<ID3D11Device> device;        // 回调持有自己的引用 → D3D 对象不会先于回调释放
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> copyTexture;
        ComPtr<ID3D11ShaderResourceView> currentSRV;
        std::atomic<int> frameCount{ 0 };   // 调试计数器
    };
    std::shared_ptr<Impl> m_impl = std::make_shared<Impl>();

    QString m_currentDeviceId;

    // 以下仅由 start()/stop()（调用线程）访问，回调不再直接触碰。
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    // WinRT Resources
    MediaCapture m_mediaCapture{ nullptr };
    MediaFrameReader m_frameReader{ nullptr };
    winrt::event_token m_frameToken;
};

#endif // WINRTCAMERA_H
