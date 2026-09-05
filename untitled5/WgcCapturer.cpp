#include "WgcCapturer.h"

// ============================================================================
// 1. 只包含 C++/WinRT 核心库
// ============================================================================
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <QDebug>

// ============================================================================
// 2. 仅包含创建 D3D 设备所需的 Interop 头文件
// ============================================================================
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// ============================================================================
// 3. 手动定义所有必要的 GUID 和接口
//    彻底断绝 "Dependency Hell" (依赖地狱)
// ============================================================================

// [GUID 1] IGraphicsCaptureItemInterop (用于 CreateForWindow)
// {3628E81B-3CAC-4C60-B7F4-23CE0E0C3356}
static const GUID IID_IGraphicsCaptureItemInterop_Local =
    { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };

// [GUID 2] IDirect3DDxgiInterfaceAccess (用于获取 D3D 纹理)
// {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1}
static const GUID IID_IDirect3DDxgiInterfaceAccess_Local =
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };

// [GUID 3] IGraphicsCaptureItem (用于 WinRT CaptureItem 对象)
// {79C3F95B-31F7-4EC2-A464-632EF5D30760}
static const GUID IID_IGraphicsCaptureItem_Local =
    { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };

// 接口定义：IGraphicsCaptureItemInterop
struct IGraphicsCaptureItemInterop_Local : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void **result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void **result) = 0;
};

// 接口定义：IDirect3DDxgiInterfaceAccess
struct IDirect3DDxgiInterfaceAccess_Local : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **p) = 0;
};

// ============================================================================
// 实现部分
// ============================================================================

WgcCapturer::WgcCapturer() {}

WgcCapturer::~WgcCapturer() {
    stopCapture();
}

void WgcCapturer::init(ID3D11Device* device) {
    m_d3dDevice = device;

    if (m_d3dDevice) {
        com_ptr<IDXGIDevice> dxgiDevice;
        dxgiDevice.attach(nullptr);
        m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());

        com_ptr<IInspectable> inspectable;

        // 创建 WinRT D3D 设备
        HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
        if (SUCCEEDED(hr)) {
            m_impl->device = inspectable.as<IDirect3DDevice>();
        } else {
            qWarning() << "[WgcCapturer] Failed to create WinRT D3D device. HR:" << hr;
        }
    }
}

bool WgcCapturer::startCapture(HWND hwnd) {
    if (!m_d3dDevice || !m_impl->device) return false;

    if (!IsWindow(hwnd)) {
        qWarning() << "[WgcCapturer] Invalid HWND.";
        return false;
    }

    if (IsIconic(hwnd)) {
        qWarning() << "[WgcCapturer] Window is minimized, cannot capture.";
        return false;
    }

    try {
        // 1. 获取 Activation Factory
        auto activation_factory = get_activation_factory<GraphicsCaptureItem>();

        // 2. 获取 Interop 接口 (使用我们手动定义的 Local 版)
        com_ptr<IUnknown> unknownFactory = activation_factory.as<IUnknown>();
        com_ptr<IGraphicsCaptureItemInterop_Local> interopFactory;

        HRESULT hr = unknownFactory->QueryInterface(IID_IGraphicsCaptureItemInterop_Local, interopFactory.put_void());

        if (FAILED(hr) || !interopFactory) {
            qWarning() << "[WgcCapturer] Failed to get IGraphicsCaptureItemInterop. HR:" << hr;
            return false;
        }

        // 3. 通过 HWND 创建 Item
        com_ptr<IInspectable> inspectableItem;

        // 使用 IID_IGraphicsCaptureItem_Local
        hr = interopFactory->CreateForWindow(
            hwnd,
            IID_IGraphicsCaptureItem_Local,
            inspectableItem.put_void()
            );

        if (FAILED(hr)) {
            qWarning() << "[WgcCapturer] CreateForWindow Failed. HR:" << hr;
            return false;
        }

        m_item = inspectableItem.as<GraphicsCaptureItem>();
        if (!m_item) return false;

        // 4. 创建 FramePool
        m_impl->lastSize = m_item.Size();
        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_impl->device,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1,
            m_impl->lastSize);

        // 5. 注册帧到达回调
        // 【线程安全】回调在 WGC 帧池自己的 PresentThread 上触发，这里绝不能捕获
        // 裸 this：WgcCapturer 可能随时被 delete（切换采集目标/停止），在途回调
        // 访问已释放对象就是崩溃（0xDDDD... use-after-free）。因此捕获 impl 的
        // shared_ptr 拷贝，回调期间 Impl 一定存活；stopCapture 在 impl->mutex
        // 内置 closed 并拆除回调，二者互斥，见 stopCapture()。
        auto impl = m_impl; // shared_ptr 拷贝：延长状态对象生命周期到回调结束
        m_frameArrivedToken = m_framePool.FrameArrived([impl](Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->closed) return; // 采集已停止，忽略迟到的帧事件

            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            auto surface = frame.Surface();

            // 使用我们手动定义的 Access 接口
            com_ptr<IDirect3DDxgiInterfaceAccess_Local> access;
            com_ptr<IUnknown> unknownSurface = surface.as<IUnknown>();

            HRESULT hr = unknownSurface->QueryInterface(IID_IDirect3DDxgiInterfaceAccess_Local, access.put_void());

            if (SUCCEEDED(hr) && access) {
                com_ptr<ID3D11Texture2D> tex;
                // 获取真正的 D3D11 纹理
                access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());

                if (tex) {
                    // 安全地将 winrt::com_ptr 转移给 WRL::ComPtr
                    impl->lastFrame = tex.get();
                    impl->hasNewFrame = true;

                    // 处理尺寸变化：用帧池上次的尺寸做对比（等价于查询 item 的当前
                    // Size，但避免在回调里触碰 m_item / this）
                    auto contentSize = frame.ContentSize();
                    if (contentSize.Width != impl->lastSize.Width || contentSize.Height != impl->lastSize.Height) {
                        sender.Recreate(
                            impl->device,
                            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                            1,
                            contentSize);
                        impl->lastSize = contentSize;
                    }
                }
            }
        });

        // 6. 启动会话
        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(true);
        m_session.StartCapture();

        qDebug() << "[WgcCapturer] Session Started OK.";
        return true;

    } catch (winrt::hresult_error const& ex) {
        qWarning() << "[WgcCapturer] WinRT Exception:" << QString::fromStdWString(ex.message().c_str())
                   << "HR:" << (int)ex.code();
        return false;
    } catch (...) {
        qWarning() << "[WgcCapturer] Unknown Exception.";
        return false;
    }
}

void WgcCapturer::stopCapture() {
    // 1) 与 FrameArrived 回调互斥：置 closed 并清空帧缓冲。回调全程持有
    //    impl->mutex（见注册处），因此若回调正在执行，这里会等它完成；
    //    之后任何迟到的回调拿到锁只会看到 closed==true 直接返回 ——
    //    两条路径都不会再访问已被释放的 WgcCapturer 成员。
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->closed = true;
        m_impl->lastFrame.Reset();
        m_impl->hasNewFrame = false;
        m_item = nullptr;
    }

    // 2) 注销回调并关闭帧池/会话。注意不持有 impl->mutex 做这些调用：
    //    避免与帧池 PresentThread 的事件派发（其内部可能持事件锁再进入
    //    回调 → 回调又要拿 impl->mutex）构成 ABBA 死锁。
    if (m_framePool) {
        m_framePool.FrameArrived(m_frameArrivedToken);
        m_framePool.Close();
        m_framePool = nullptr;
    }
    if (m_session) {
        m_session.Close();
        m_session = nullptr;
    }
}

bool WgcCapturer::acquireNextFrame(ID3D11Texture2D** outTexture) {
    if (!m_impl->hasNewFrame) return false;

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->lastFrame) {
        m_impl->lastFrame.CopyTo(outTexture);
        m_impl->hasNewFrame = false;
        return true;
    }
    return false;
}
