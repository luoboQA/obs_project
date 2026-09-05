#ifndef WGCCAPTURER_H
#define WGCCAPTURER_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <mutex>
#include <atomic>
#include <memory>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Graphics::Capture;

class WgcCapturer
{
public:
    WgcCapturer();
    ~WgcCapturer();

    void init(ID3D11Device* device);
    bool startCapture(HWND hwnd);
    void stopCapture();
    bool acquireNextFrame(ID3D11Texture2D** outTexture);

private:
    // 【线程安全设计】
    // FrameArrived 回调运行在 WGC 帧池自己的 PresentThread 上，而 WgcCapturer
    // 可能在视频线程上被 delete（切换目标/停止采集）。回调若捕获裸 this，
    // delete 后任何在途回调都会 use-after-free（本文件之前的崩溃根因）。
    // 因此所有会被回调访问的状态都放入 Impl，由 shared_ptr 持有：
    //   - 回调 lambda 捕获一份 m_impl 拷贝 → 即使 WgcCapturer 被销毁，Impl
    //     仍随回调存活，不会访问已释放内存；
    //   - 回调全程持有 impl->mutex，stopCapture 在同一把锁内置 closed 并拆除
    //     回调/关闭帧池 → 回调要么完整执行完，要么看到 closed 立即退出。
    struct Impl {
        std::mutex mutex;
        bool closed = false;
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{ nullptr };
        // WGC 的 Item.Size()/Frame.ContentSize() 均为 SizeInt32（注意不是
        // Windows::Foundation::Size），CreateFreeThreaded/Recreate 也要 SizeInt32
        winrt::Windows::Graphics::SizeInt32 lastSize{ 0, 0 }; // 帧池当前尺寸，用于 resize 检测
        ComPtr<ID3D11Texture2D> lastFrame;
        std::atomic<bool> hasNewFrame{ false };
    };
    std::shared_ptr<Impl> m_impl = std::make_shared<Impl>();

    ID3D11Device* m_d3dDevice = nullptr;

    // 以下对象仅在 startCapture（视频线程）与 stopCapture（拆除，与回调互斥）中使用，
    // 回调不再直接访问它们。
    GraphicsCaptureItem m_item{ nullptr };
    Direct3D11CaptureFramePool m_framePool{ nullptr };
    GraphicsCaptureSession m_session{ nullptr };
    winrt::event_token m_frameArrivedToken;
};

#endif // WGCCAPTURER_H
