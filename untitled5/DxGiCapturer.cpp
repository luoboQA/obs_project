#include "DxGiCapturer.h"
#include "Logger.h"
#include <d3d11.h>
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <dwmapi.h>
#include <QElapsedTimer>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")

#define SAFE_RELEASE(p) { if(p) { (p)->Release(); (p)=nullptr; } }

// ID3D11Multithread interface definition
static const GUID IID_ID3D11Multithread_Local = { 0x9B7E4E00, 0x342C, 0x4106, { 0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 } };
struct ID3D11Multithread_Local : public IUnknown {
    virtual void STDMETHODCALLTYPE Enter(void) = 0;
    virtual void STDMETHODCALLTYPE Leave(void) = 0;
    virtual BOOL STDMETHODCALLTYPE SetMultithreadProtected(BOOL bMtProtect) = 0;
    virtual BOOL STDMETHODCALLTYPE GetMultithreadProtected(void) = 0;
};

const double TARGET_FPS = 30.0;
const double TARGET_FRAME_TIME_SEC = 1.0 / TARGET_FPS;
static long long s_recordedFrameCount = 0;

struct Vertex { float x, y, z; float u, v; };

// Shader source code
const char* shaderCode =
    "Texture2D shaderTexture : register(t0);"
    "SamplerState sampleType : register(s0);"
    "struct VOut { float4 position : SV_POSITION; float2 tex : TEXCOORD0; };"
    "VOut VShader(float4 position : POSITION, float2 tex : TEXCOORD0) { VOut output; output.position = position; output.tex = tex; return output; }"
    "float4 PShader(VOut input) : SV_TARGET { return shaderTexture.Sample(sampleType, input.tex); }";

HWND GetRootWindow(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* map = reinterpret_cast<QMap<size_t, QString>*>(lParam);
    if (IsWindowVisible(hwnd) && GetWindowTextLength(hwnd) > 0) {
        int cloaked = 0;
        HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (SUCCEEDED(hr) && cloaked) return TRUE;
        RECT rect; GetWindowRect(hwnd, &rect);
        if ((rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0) {
            TCHAR title[256]; GetWindowText(hwnd, title, 256);
            QString str = QString::fromWCharArray(title);
            if (str != "Program Manager" && str != "Settings" && str != "Microsoft Text Input Application") map->insert((size_t)hwnd, str);
        }
    }
    return TRUE;
}

QMap<size_t, QString> DxGiCapturer::getWindowList() { QMap<size_t, QString> list; EnumWindows(EnumWindowsProc, (LPARAM)&list); return list; }

DxGiCapturer::DxGiCapturer(QObject *parent) : QObject(parent), m_running(false), m_snapshotRequested(0) {
    m_previewEnabled = true;
    m_isRecordingMP4 = false;
    m_uiInFlight = 0;
    m_width = 0; m_height = 0;
    m_wgcCapturer = nullptr;
    m_targetChanged = false;
}

DxGiCapturer::~DxGiCapturer() {
    stopCapture();
    // cleanup() 内部通过 m_teardownMutex 与采集循环互斥：会等待循环结束当前
    // 迭代后再释放资源，无需（也不再）轮询 m_running 忙等。
    cleanup();
}

void DxGiCapturer::setPreviewEnabled(bool enabled) { m_previewEnabled = enabled; if (!enabled) m_uiInFlight = 0; }
void DxGiCapturer::startRecording() { m_isRecordingMP4 = true; s_recordedFrameCount = 0; }
void DxGiCapturer::stopRecording() { m_isRecordingMP4 = false; }
void DxGiCapturer::onUiFrameProcessed() { m_uiInFlight = 0; }

QStringList DxGiCapturer::getMonitorNames() {
    QStringList list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return list;
    ComPtr<IDXGIAdapter1> adapter; UINT adapterIndex = 0; int globalMonitorIndex = 0;
    while (factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        ComPtr<IDXGIOutput> output; UINT outputIndex = 0;
        while (adapter->EnumOutputs(outputIndex++, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC desc; output->GetDesc(&desc);
            int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            list.append(QString("Monitor %1: %2x%3").arg(globalMonitorIndex++).arg(w).arg(h));
        }
    }
    return list;
}

void DxGiCapturer::cleanup() {
    // 与采集循环每轮迭代持有的 m_teardownMutex 互斥：确保释放 D3D/WGC 资源时
    // 采集线程已不再访问它们（循环内持锁后会复检 m_running 再退出，见 startCapture）。
    QMutexLocker teardownLock(&m_teardownMutex);
    resetCaptureState();
    m_vertexShader.Reset(); m_pixelShader.Reset(); m_inputLayout.Reset();
    m_vertexBuffer.Reset(); m_samplerState.Reset(); m_blendState.Reset(); m_rasterizerState.Reset();
    QMutexLocker locker(&m_dataMutex);
    for(auto data : m_overlays) { SAFE_RELEASE(data->texture); SAFE_RELEASE(data->srv); delete data; }
    m_overlays.clear();
    if (m_d3dContext) { m_d3dContext->ClearState(); m_d3dContext->Flush(); }
    m_d3dContext.Reset(); m_d3dDevice.Reset(); m_currentAdapterLuid = {0, 0};
}

void DxGiCapturer::resetCaptureState() {
    if (m_wgcCapturer) { delete m_wgcCapturer; m_wgcCapturer = nullptr; }
    m_deskDupl.Reset(); m_currentMonitorTexture.Reset(); m_cachedSRV.Reset();
    m_capturedTexture.Reset(); m_compositionTexture.Reset(); m_compositionRTV.Reset(); m_stagingTexture.Reset();
}

void DxGiCapturer::stopCapture() { m_running = false; }

bool DxGiCapturer::initShaders() {
    if (!m_d3dDevice) return false;
    HMODULE hCompiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!hCompiler) return false;
    pD3DCompile fnD3DCompile = (pD3DCompile)GetProcAddress(hCompiler, "D3DCompile");
    if (!fnD3DCompile) { FreeLibrary(hCompiler); return false; }

    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errorBlob = nullptr;
    HRESULT hr = fnD3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VShader", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if(FAILED(hr)) { if(errorBlob) errorBlob->Release(); FreeLibrary(hCompiler); return false; }
    m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    m_d3dDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    vsBlob->Release();

    hr = fnD3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PShader", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if(FAILED(hr)) { if(errorBlob) errorBlob->Release(); FreeLibrary(hCompiler); return false; }
    m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release(); FreeLibrary(hCompiler);

    D3D11_BUFFER_DESC vbDesc = { sizeof(Vertex) * 6, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    m_d3dDevice->CreateBuffer(&vbDesc, nullptr, &m_vertexBuffer);

    D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    m_d3dDevice->CreateSamplerState(&sampDesc, &m_samplerState);

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
    m_d3dDevice->CreateBlendState(&blendDesc, &m_blendState);

    D3D11_RASTERIZER_DESC rsDesc = {}; rsDesc.CullMode = D3D11_CULL_NONE; rsDesc.FillMode = D3D11_FILL_SOLID;
    m_d3dDevice->CreateRasterizerState(&rsDesc, &m_rasterizerState);
    return true;
}

bool DxGiCapturer::initDevice(IDXGIAdapter* specificAdapter) {
    DXGI_ADAPTER_DESC desc = {}; if (specificAdapter) specificAdapter->GetDesc(&desc);
    if (m_d3dDevice) {
        if (!specificAdapter) return true;
        if (m_currentAdapterLuid.LowPart == desc.AdapterLuid.LowPart && m_currentAdapterLuid.HighPart == desc.AdapterLuid.HighPart) return true;
    }
    cleanup();
    if (specificAdapter) m_currentAdapterLuid = desc.AdapterLuid; else m_currentAdapterLuid = { 0, 0 };
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT numFeatureLevels = ARRAYSIZE(featureLevels); D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(specificAdapter, specificAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, featureLevels, numFeatureLevels, D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), &featureLevel, m_d3dContext.ReleaseAndGetAddressOf());
    if (FAILED(hr) && !specificAdapter) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, numFeatureLevels, D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), &featureLevel, m_d3dContext.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Multithread_Local> multi;
        if (SUCCEEDED(m_d3dDevice->QueryInterface(IID_ID3D11Multithread_Local, (void**)multi.GetAddressOf()))) multi->SetMultithreadProtected(TRUE);
        return initShaders();
    }
    return false;
}

bool DxGiCapturer::init() { return initDevice(nullptr); }

bool DxGiCapturer::initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl) {
    ComPtr<IDXGIFactory1> factory; if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;
    ComPtr<IDXGIAdapter1> adapter; UINT adapterIndex = 0; int currentGlobalIndex = 0;
    while (factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        ComPtr<IDXGIOutput> output; UINT outputIndex = 0;
        while (adapter->EnumOutputs(outputIndex++, &output) != DXGI_ERROR_NOT_FOUND) {
            if (currentGlobalIndex == monitorIndex) {
                ComPtr<IDXGIAdapter> targetAdapter; adapter.As(&targetAdapter);
                if (!initDevice(targetAdapter.Get())) return false;
                ComPtr<IDXGIOutput1> output1;
                if (SUCCEEDED(output.As(&output1))) return SUCCEEDED(output1->DuplicateOutput(m_d3dDevice.Get(), &outDupl));
                return false;
            }
            currentGlobalIndex++;
        }
    }
    return false;
}

void DxGiCapturer::setCaptureTarget(const CaptureTarget& target) {
    QMutexLocker locker(&m_targetMutex);
    if (m_currentTarget != target) { m_pendingTarget = target; m_targetChanged = true; }
}

void DxGiCapturer::preciseSleep(double targetSeconds) {
    if (targetSeconds <= 0) return;
    double targetMs = targetSeconds * 1000.0;
    QElapsedTimer t; t.start();
    if (targetMs > 2.0) QThread::msleep((unsigned long)(targetMs - 1.0));
    while (t.nsecsElapsed() / 1000000000.0 < targetSeconds) {}
}

void DxGiCapturer::addOverlay(int id, const QImage& img, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex);
    OverlayData* data = new OverlayData;
    data->id = id;
    data->type = OverlayType::Image;
    data->image = img.copy(); // Deep copy
    data->rect = rect;
    data->textureDirty = true;
    data->texture = nullptr;
    data->srv = nullptr;
    m_overlays.insert(id, data);

    // Auto-add to top of render order if not exists
    if (!m_renderOrder.contains(id)) m_renderOrder.append(id);
}

void DxGiCapturer::addCameraOverlay(int id, WinRTCamera* camera, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex);
    OverlayData* data = new OverlayData;
    data->id = id;
    data->type = OverlayType::Camera;
    data->camera = camera;
    data->rect = rect;
    data->texture = nullptr; // Camera uses its own SRV
    data->srv = nullptr;
    m_overlays.insert(id, data);
    if (!m_renderOrder.contains(id)) m_renderOrder.append(id);
}

void DxGiCapturer::removeOverlay(int id) {
    QMutexLocker locker(&m_dataMutex);
    // Queue for safe removal in render thread
    if (!m_pendingRemovals.contains(id)) m_pendingRemovals.append(id);
    m_renderOrder.removeAll(id);
}

void DxGiCapturer::updateOverlayGeometry(int id, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex);
    if (m_overlays.contains(id)) m_overlays[id]->rect = rect;
}

void DxGiCapturer::updateOverlayImage(int id, const QImage& img) {
    QMutexLocker locker(&m_dataMutex);
    if (m_overlays.contains(id)) {
        OverlayData* data = m_overlays[id];
        // Only update if it's an Image type
        if (data->type == OverlayType::Image) {
            data->image = img.copy();
            data->textureDirty = true;
        }
    }
}

void DxGiCapturer::updateRenderOrder(QList<int> order) {
    QMutexLocker locker(&m_dataMutex);
    m_renderOrder = order;
}

void DxGiCapturer::requestSnapshot() { m_snapshotRequested = 1; }

// Internal: Create/Update D3D Texture from QImage
void DxGiCapturer::updateTextureFromImage(OverlayData* data) {
    if (!m_d3dDevice || data->image.isNull()) return;

    // Convert format to BGRA8 (WinRT/DirectX standard)
    QImage converted = data->image;
    if (converted.format() != QImage::Format_ARGB32_Premultiplied &&
        converted.format() != QImage::Format_ARGB32) {
        converted = converted.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    int w = converted.width();
    int h = converted.height();

    // Release old if size changed
    if (data->texture) {
        D3D11_TEXTURE2D_DESC d;
        data->texture->GetDesc(&d);
        if (d.Width != (UINT)w || d.Height != (UINT)h) {
            SAFE_RELEASE(data->texture);
            SAFE_RELEASE(data->srv);
        }
    }

    // Create Texture if needed
    if (!data->texture) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Qt ARGB32 maps to this usually, or use R8G8B8A8 and swizzle
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(m_d3dDevice->CreateTexture2D(&desc, nullptr, &data->texture))) return;
        m_d3dDevice->CreateShaderResourceView(data->texture, nullptr, &data->srv);
    }

    // Upload Data
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_d3dContext->Map(data->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        const uchar* bits = converted.constBits();
        // Handle pitch mismatch
        if (converted.bytesPerLine() == (int)map.RowPitch) {
            memcpy(map.pData, bits, converted.height() * converted.bytesPerLine());
        } else {
            for (int y = 0; y < h; ++y) {
                memcpy((uchar*)map.pData + y * map.RowPitch,
                       bits + y * converted.bytesPerLine(),
                       w * 4);
            }
        }
        m_d3dContext->Unmap(data->texture, 0);
    }
    data->textureDirty = false;
}

// Internal: Clean up removed overlays
void DxGiCapturer::processPendingUpdates() {
    QMutexLocker locker(&m_dataMutex);
    for(int id : m_pendingRemovals) {
        if(m_overlays.contains(id)) {
            OverlayData* data = m_overlays[id];
            SAFE_RELEASE(data->texture);
            SAFE_RELEASE(data->srv);
            delete data;
            m_overlays.remove(id);
        }
    }
    m_pendingRemovals.clear();

    for(auto data : m_overlays) {
        if(data->type == OverlayType::Image && data->textureDirty) {
            updateTextureFromImage(data);
        }
    }
}

void DxGiCapturer::renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground) {
    if (!srv || !m_d3dDevice || !m_vertexBuffer || !m_d3dContext) return;
    float left, right, top, bottom;
    if (isBackground) { left = -1.0f; right = 1.0f; top = 1.0f; bottom = -1.0f; }
    else {
        float w = (float)m_width; float h = (float)m_height; if (w <= 1 || h <= 1) return;
        left = (float)rect.x() / w * 2.0f - 1.0f; right = (float)(rect.x() + rect.width()) / w * 2.0f - 1.0f;
        top = 1.0f - (float)rect.y() / h * 2.0f; bottom = 1.0f - (float)(rect.y() + rect.height()) / h * 2.0f;
    }
    Vertex vertices[] = { { left, top, 0.0f, 0.0f, 0.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { left, bottom, 0.0f, 0.0f, 1.0f }, { left, bottom, 0.0f, 0.0f, 1.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { right, bottom, 0.0f, 1.0f, 1.0f } };
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_d3dContext->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, vertices, sizeof(vertices));
        m_d3dContext->Unmap(m_vertexBuffer.Get(), 0);
        UINT stride = sizeof(Vertex); UINT offset = 0;
        m_d3dContext->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
        m_d3dContext->PSSetShaderResources(0, 1, &srv);
        m_d3dContext->Draw(6, 0);
    }
}

void DxGiCapturer::startCapture() {
    if (m_running) return;
    m_running = true; m_uiInFlight = 0;
    { QMutexLocker locker(&m_targetMutex); m_currentTarget = m_pendingTarget; m_targetChanged = true; }
    if (!m_d3dDevice) { if (!initDevice(nullptr)) { emit errorOccurred("Failed to initialize Graphics Device"); emit initFinished(false); return; } }
    emit initFinished(true);

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {}; ComPtr<IDXGIResource> desktopResource;
    QElapsedTimer loopTimer; loopTimer.start(); double nextFrameTargetTime = loopTimer.nsecsElapsed() / 1000000000.0;

    try {
        while (m_running) {
            QCoreApplication::processEvents();

            // 采集生命周期锁：整轮迭代持有，与 ~DxGiCapturer::cleanup() 互斥
            // （析构线程要等本迭代结束才能释放捕获资源）。
            // 若 stopCapture() 刚在 processEvents() 中被调用（BlockingQueued
            // 槽），持锁后立即退出 —— 避免 cleanup() 已释放资源后本迭代继续
            // 使用 m_wgcCapturer / m_deskDupl 等对象。
            QMutexLocker teardownLock(&m_teardownMutex);
            if (!m_running) break;

            bool needSwitch = false; { QMutexLocker locker(&m_targetMutex); if (m_targetChanged) { m_currentTarget = m_pendingTarget; m_targetChanged = false; needSwitch = true; } }
            if (needSwitch) resetCaptureState();

            bool isMonitor = (m_currentTarget.type == CaptureSourceType::Monitor);
            bool isWindow = (m_currentTarget.type == CaptureSourceType::Window);
            bool initialized = false;

            if (isMonitor) { if (!m_deskDupl) { if (initDuplication(m_currentTarget.monitorIndex, m_deskDupl)) initialized=true; else { QThread::msleep(200); continue; } } else initialized=true; }
            else if (isWindow) { if (!m_wgcCapturer) { if (!IsWindow(m_currentTarget.windowHandle)) { QThread::msleep(500); continue; } HWND root = GetRootWindow(m_currentTarget.windowHandle); m_wgcCapturer = new WgcCapturer(); m_wgcCapturer->init(m_d3dDevice.Get()); if(m_wgcCapturer->startCapture(root)) initialized=true; else { delete m_wgcCapturer; m_wgcCapturer=nullptr; QThread::msleep(200); continue; } } else initialized=true; }
            if (!initialized) { QThread::msleep(50); continue; }

            bool captureSuccess = false; bool accessLost = false;
            if (isMonitor && m_deskDupl) {
                desktopResource.Reset(); HRESULT hr = m_deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
                if (SUCCEEDED(hr)) {
                    ComPtr<ID3D11Texture2D> rawTex; if (desktopResource) hr = desktopResource.As(&rawTex);
                    if (SUCCEEDED(hr) && rawTex) {
                        D3D11_TEXTURE2D_DESC desc; rawTex->GetDesc(&desc);
                        if (desc.Width > 0 && desc.Height > 0) {
                            if (!m_currentMonitorTexture || desc.Width != m_width || desc.Height != m_height) {
                                m_currentMonitorTexture.Reset(); m_cachedSRV.Reset(); m_stagingTexture.Reset(); m_width = desc.Width; m_height = desc.Height;
                                D3D11_TEXTURE2D_DESC cacheDesc = desc; cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; cacheDesc.MiscFlags = 0; cacheDesc.CPUAccessFlags = 0; cacheDesc.Usage = D3D11_USAGE_DEFAULT;
                                m_d3dDevice->CreateTexture2D(&cacheDesc, nullptr, &m_currentMonitorTexture);
                                m_d3dDevice->CreateShaderResourceView(m_currentMonitorTexture.Get(), nullptr, &m_cachedSRV);
                            }
                            if(m_currentMonitorTexture) { m_d3dContext->CopyResource(m_currentMonitorTexture.Get(), rawTex.Get()); captureSuccess = true; }
                        }
                    }
                    m_deskDupl->ReleaseFrame();
                } else if (hr == DXGI_ERROR_ACCESS_LOST) accessLost = true;
            } else if (isWindow && m_wgcCapturer) {
                ID3D11Texture2D* wgcTex = nullptr;
                if (m_wgcCapturer->acquireNextFrame(&wgcTex)) {
                    if (wgcTex) {
                        D3D11_TEXTURE2D_DESC d; wgcTex->GetDesc(&d); m_width = d.Width; m_height = d.Height;
                        m_capturedTexture.Attach(wgcTex); m_cachedSRV.Reset();
                        m_d3dDevice->CreateShaderResourceView(m_capturedTexture.Get(), nullptr, &m_cachedSRV);
                        captureSuccess = true;
                    }
                } else QThread::msleep(1);
            }
            if (accessLost) { resetCaptureState(); QThread::msleep(100); continue; }

            if (m_d3dDevice) {
                bool needRecreate = false; int renderW = (m_width > 0) ? m_width : 1280; int renderH = (m_height > 0) ? m_height : 720;
                if (!m_compositionTexture || !m_stagingTexture) needRecreate = true;
                else { D3D11_TEXTURE2D_DESC d; m_compositionTexture->GetDesc(&d); if(d.Width!=(UINT)renderW || d.Height!=(UINT)renderH) needRecreate=true; }

                if (needRecreate) {
                    m_compositionTexture.Reset(); m_compositionRTV.Reset(); m_stagingTexture.Reset();
                    D3D11_TEXTURE2D_DESC compDesc = {}; compDesc.Width = renderW; compDesc.Height = renderH; compDesc.MipLevels = 1; compDesc.ArraySize = 1; compDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; compDesc.SampleDesc.Count = 1; compDesc.Usage = D3D11_USAGE_DEFAULT; compDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    m_d3dDevice->CreateTexture2D(&compDesc, nullptr, &m_compositionTexture);
                    m_d3dDevice->CreateRenderTargetView(m_compositionTexture.Get(), nullptr, &m_compositionRTV);
                    D3D11_TEXTURE2D_DESC stageDesc = compDesc; stageDesc.Usage = D3D11_USAGE_STAGING; stageDesc.BindFlags = 0; stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    m_d3dDevice->CreateTexture2D(&stageDesc, nullptr, &m_stagingTexture);
                    D3D11_VIEWPORT vp = {0, 0, (float)renderW, (float)renderH, 0, 1}; m_d3dContext->RSSetViewports(1, &vp); m_width = renderW; m_height = renderH;
                }

                float clear[] = {0, 0, 0, 1}; m_d3dContext->ClearRenderTargetView(m_compositionRTV.Get(), clear);
                m_d3dContext->OMSetRenderTargets(1, m_compositionRTV.GetAddressOf(), nullptr);
                m_d3dContext->RSSetState(m_rasterizerState.Get());
                m_d3dContext->IASetInputLayout(m_inputLayout.Get());
                m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_d3dContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
                m_d3dContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);
                m_d3dContext->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

                float blendFactor[] = {0,0,0,0};
                m_d3dContext->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffff);

                // 1. Draw Background (Desktop/Window Capture)
                if (m_cachedSRV) {
                    renderTexture(m_cachedSRV.Get(), QRect(0, 0, m_width, m_height), true);
                }

                // 2. Process Updates (Removals/Texture Uploads)
                processPendingUpdates();

                // 3. Draw Overlays in Z-Order
                {
                    QMutexLocker locker(&m_dataMutex);
                    // 遍历渲染列表（从下到上）
                    // 只有在这里的 id 才会被绘制，所以删除后不会出现幽灵画面
                    for(int id : m_renderOrder) {
                        if (!m_overlays.contains(id)) continue;
                        OverlayData* data = m_overlays[id];

                        // Skip if rect is empty or hidden
                        if (data->rect.isEmpty()) continue;

                        if (data->type == OverlayType::Image && data->srv) {
                            renderTexture(data->srv, data->rect, false);
                        }
                        else if (data->type == OverlayType::Camera && data->camera) {
                            auto camSRV = data->camera->getLatestFrame();
                            if (camSRV) {
                                renderTexture(camSRV.Get(), data->rect, false);
                            }
                        }
                    }
                }

                if (m_stagingTexture) {
                    m_d3dContext->CopyResource(m_stagingTexture.Get(), m_compositionTexture.Get()); m_d3dContext->Flush();
                    D3D11_MAPPED_SUBRESOURCE mapInfo;
                    if (SUCCEEDED(m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapInfo)) && mapInfo.pData) {
                        QImage deepCopy;
                        if (mapInfo.RowPitch == (UINT)m_width*4) deepCopy = QImage((uchar*)mapInfo.pData, m_width, m_height, QImage::Format_ARGB32_Premultiplied).copy();
                        else {
                            deepCopy = QImage(m_width, m_height, QImage::Format_ARGB32_Premultiplied);
                            for (int y=0; y<m_height; ++y) memcpy(deepCopy.bits() + y*m_width*4, (uchar*)mapInfo.pData + y*mapInfo.RowPitch, m_width*4);
                        }
                        m_d3dContext->Unmap(m_stagingTexture.Get(), 0);
                        if (m_isRecordingMP4) { emit frameCaptured(deepCopy, (qint64)(s_recordedFrameCount * 1000.0 / TARGET_FPS)); s_recordedFrameCount++; }
                        if (m_previewEnabled && m_uiInFlight == 0) { m_uiInFlight = 1; emit frameCapturedForPreview(deepCopy); }
                        if (m_snapshotRequested.exchange(0) == 1) emit snapshotCaptured(deepCopy);
                    }
                }
            }
            nextFrameTargetTime += TARGET_FRAME_TIME_SEC;
            double now = loopTimer.nsecsElapsed() / 1000000000.0;
            double sleepTime = nextFrameTargetTime - now;
            if (sleepTime > 0) preciseSleep(sleepTime); else nextFrameTargetTime = now;
        }
    } catch (...) {}
    emit captureStopped();
}
