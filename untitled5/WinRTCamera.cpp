#include "WinRTCamera.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <QDebug>
#include <QtConcurrent>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Devices.h>
#include <MemoryBuffer.h>

// 引入必要的命名空间
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Devices;
using namespace winrt::Windows::Graphics::Imaging; // 引入 Imaging 命名空间方便转换

struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

// IID 定义
static const GUID IID_IDirect3DDxgiInterfaceAccess_Local =
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };
struct IDirect3DDxgiInterfaceAccess_Local : public IUnknown
{ virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **p) = 0; };

WinRTCamera::WinRTCamera(QObject *parent) : QObject(parent) {}
WinRTCamera::~WinRTCamera() { stop(); }

bool WinRTCamera::init(ID3D11Device* device) {
    if (!device) return false;
    m_impl->device = device; // WRL ComPtr 持有自身引用
    m_impl->device->GetImmediateContext(m_impl->context.ReleaseAndGetAddressOf());

    ComPtr<IDXGIDevice> dxgiDevice;
    m_impl->device.As(&dxgiDevice);
    winrt::com_ptr<IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (inspectable) {
        m_winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    }
    return true;
}

bool WinRTCamera::start(const QString& deviceId) {
    if (m_impl->running) stop();
    m_currentDeviceId = deviceId;
    m_impl->frameCount = 0;

    try {
        m_mediaCapture = MediaCapture();

        // 不捕获 this：Failed 事件可能在对象销毁后才派发，回调只使用参数
        m_mediaCapture.Failed([](MediaCapture const&, MediaCaptureFailedEventArgs const& args) {
            qWarning() << "[WinRTCamera] CRITICAL ERROR:" << (int)args.Code() << QString::fromStdWString(args.Message().c_str());
        });

        MediaCaptureInitializationSettings settings;
        settings.VideoDeviceId(deviceId.toStdWString());
        settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
        settings.StreamingCaptureMode(StreamingCaptureMode::Video);
        settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);

        m_mediaCapture.InitializeAsync(settings).get();

        // --- 格式协商 ---
        auto deviceController = m_mediaCapture.VideoDeviceController();
        auto allProperties = deviceController.GetAvailableMediaStreamProperties(MediaStreamType::VideoRecord);

        VideoEncodingProperties bestFormat = nullptr;
        int bestScore = -1;

        qDebug() << "=== Scanning Formats ===";

        for (auto prop : allProperties) {
            auto videoProp = prop.as<VideoEncodingProperties>();
            if (!videoProp) continue;

            auto subtype = videoProp.Subtype();
            int w = videoProp.Width();
            int h = videoProp.Height();
            double fps = (double)videoProp.FrameRate().Numerator() / videoProp.FrameRate().Denominator();

            int score = 0;

            // 格式评分调整：BGRA8 优先级最高
            // 这样系统会优先选择无需转换的格式，性能最好且颜色一定正确
            bool isBgra = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Bgra8());
            bool isNV12 = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Nv12());
            bool isYUY2 = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Yuy2());
            bool isMJPG = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Mjpg());

            if (isBgra) score += 3000;      // BGRA 直接可用
            else if (isNV12) score += 2000; // NV12 次之
            else if (isYUY2) score += 1000; // YUY2
            else if (isMJPG) score += 500;  // MJPG 需要解码
            else continue;

            // 分辨率
            if (w == 1920 && h == 1080) score += 100;
            else if (w == 1280 && h == 720) score += 80;

            // 帧率
            if (std::abs(fps - 30.0) < 1.0) score += 50;
            if (std::abs(fps - 60.0) < 1.0) score += 60;

            if (score > bestScore) {
                bestScore = score;
                bestFormat = videoProp;
            }
        }
        qDebug() << "=======================================";

        if (bestFormat) {
            qDebug() << "[WinRTCamera] Setting Format:"
                     << QString::fromStdWString(bestFormat.Subtype().c_str())
                     << bestFormat.Width() << "x" << bestFormat.Height();
            deviceController.SetMediaStreamPropertiesAsync(MediaStreamType::VideoRecord, bestFormat).get();
        }

        auto frameSources = m_mediaCapture.FrameSources();
        MediaFrameSource selectedSource = nullptr;
        for (auto pair : frameSources) {
            auto source = pair.Value();
            if (source.Info().SourceKind() == MediaFrameSourceKind::Color) { selectedSource = source; break; }
        }
        if (!selectedSource) { qDebug() << "[WinRTCamera] No Color Source"; return false; }

        m_frameReader = m_mediaCapture.CreateFrameReaderAsync(selectedSource).get();

        {
            // 上一会话 stop() 曾置 closed=true，重启前在锁内复位（与遗留回调互斥）
            QMutexLocker locker(&m_impl->mutex);
            m_impl->closed = false;
        }

        // 【线程安全】回调跑在 MediaFrameReader 自己的后台线程上，绝不能捕获裸
        // this：摄像头对象可能随时被主线程 stop()+delete（cleanupCameraPool），
        // 在途回调访问已释放对象就是崩溃（0xDDDD... use-after-free）。因此捕获
        // impl 的 shared_ptr 拷贝：回调期间 Impl 一定存活；stop() 先置 closed
        // 再拆除回调，二者通过 impl->mutex 互斥，见 stop()。
        auto impl = m_impl; // shared_ptr 拷贝：延长状态对象生命周期到回调结束
        m_frameToken = m_frameReader.FrameArrived([impl](MediaFrameReader const& sender, MediaFrameArrivedEventArgs const&) {
            if (impl->closed) return; // 已停止，忽略迟到事件（原子快速路径）
            int c = ++impl->frameCount;

            try {
                auto frameRef = sender.TryAcquireLatestFrame();
                if (!frameRef) return;
                auto videoFrame = frameRef.VideoMediaFrame();
                if (!videoFrame) return;

                auto inputBitmap = videoFrame.SoftwareBitmap();
                if (!inputBitmap) return;

                // 强制颜色空间转换
                // 无论摄像头给的是 NV12 还是 YUY2，统统转成 BGRA8
                // SoftwareBitmap::Convert 非常高效（使用 WIC/SIMD 指令集）
                SoftwareBitmap bitmapToUpload = nullptr;
                if (inputBitmap.BitmapPixelFormat() != BitmapPixelFormat::Bgra8 ||
                    inputBitmap.BitmapAlphaMode() != BitmapAlphaMode::Premultiplied)
                {
                    bitmapToUpload = SoftwareBitmap::Convert(
                        inputBitmap,
                        BitmapPixelFormat::Bgra8,
                        BitmapAlphaMode::Premultiplied // 确保 Alpha 通道正确
                        );
                } else {
                    bitmapToUpload = inputBitmap;
                }

                // 锁定缓冲区
                auto buffer = bitmapToUpload.LockBuffer(BitmapBufferAccessMode::Read);
                auto reference = buffer.CreateReference();
                auto byteAccess = reference.as<IMemoryBufferByteAccess>();

                uint8_t* pData = nullptr;
                uint32_t capacity = 0;
                if (SUCCEEDED(byteAccess->GetBuffer(&pData, &capacity)) && pData) {

                    int w = bitmapToUpload.PixelWidth();
                    int h = bitmapToUpload.PixelHeight();

                    // 调试信息
                    if (c == 1) qDebug() << "[WinRTCamera] Uploading BGRA8:" << w << "x" << h;

                    QMutexLocker locker(&impl->mutex);
                    // 拿到锁后再次确认：与 stop() 的拆除互斥；stop() 一旦置
                    // closed，这里就不再往已重置的纹理上传
                    if (impl->closed) return;

                    // 3. 统一创建 BGRA8 纹理
                    // 因为数据已经在 CPU 端转好了，GPU 这里只需要最标准的 RGB 纹理
                    if (!impl->copyTexture || !impl->currentSRV) {
                        D3D11_TEXTURE2D_DESC desc = {};
                        desc.Width = w;
                        desc.Height = h;
                        desc.MipLevels = 1;
                        desc.ArraySize = 1;
                        desc.SampleDesc.Count = 1;
                        desc.Usage = D3D11_USAGE_DEFAULT;
                        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        desc.CPUAccessFlags = 0;
                        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // 【关键】始终为 BGRA

                        if (FAILED(impl->device->CreateTexture2D(&desc, nullptr, impl->copyTexture.ReleaseAndGetAddressOf()))) return;

                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = desc.Format;
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MostDetailedMip = 0;
                        srvDesc.Texture2D.MipLevels = 1;

                        impl->device->CreateShaderResourceView(impl->copyTexture.Get(), &srvDesc, impl->currentSRV.ReleaseAndGetAddressOf());
                    }

                    // 4. 上传数据
                    // BGRA8 的 Pitch 固定为 Width * 4
                    int rowPitch = w * 4;
                    impl->context->UpdateSubresource(impl->copyTexture.Get(), 0, nullptr, pData, rowPitch, 0);
                }

            } catch (...) {}
        });

        MediaFrameReaderStartStatus status = m_frameReader.StartAsync().get();

        if (status == MediaFrameReaderStartStatus::Success) {
            impl->running = true;
            return true;
        }

    } catch (...) {
        qWarning() << "[WinRTCamera] Start Failed Exception";
    }
    return false;
}

void WinRTCamera::stop() {
    m_impl->running = false;
    // 1) 与在途 FrameArrived 回调互斥：置 closed 并释放纹理。回调的上传段
    //    全程持 impl->mutex（见注册处），若回调正在上传，这里会等它完成；
    //    之后任何迟到的回调只会看到 closed==true 直接返回 —— 两条路径都
    //    不会再访问已被释放的 WinRTCamera 成员。
    {
        QMutexLocker locker(&m_impl->mutex);
        m_impl->closed = true;
        m_impl->copyTexture.Reset();
        m_impl->currentSRV.Reset();
    }
    // 2) 注销回调并关闭 reader/capture。注意不持有 impl->mutex 做这些调用，
    //    避免与 MediaFrameReader 事件派发线程（可能持事件锁再进入回调）构成
    //    ABBA 死锁。
    try {
        if (m_frameReader) {
            m_frameReader.FrameArrived(m_frameToken);
            m_frameReader.StopAsync();
            m_frameReader.Close();
            m_frameReader = nullptr;
        }
        if (m_mediaCapture) { m_mediaCapture.Close(); m_mediaCapture = nullptr; }
    } catch (...) {}
}

ComPtr<ID3D11ShaderResourceView> WinRTCamera::getLatestFrame() {
    QMutexLocker locker(&m_impl->mutex);
    return m_impl->currentSRV; // ComPtr 拷贝：返回后即使被 Reset 也仍持有引用
}
