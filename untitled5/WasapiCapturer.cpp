#include "WasapiCapturer.h"
#include <functiondiscoverykeys_devpkey.h>
#include <QDebug>
#include <cmath>

// 定义常量，防止头文件依赖缺失
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

WasapiCapturer::WasapiCapturer(QObject *parent)
    : QThread(parent), m_stopRequested(false), m_type(Microphone)
{
}

WasapiCapturer::~WasapiCapturer() {
    stop();
    wait();
    cleanup();
}

void WasapiCapturer::stop() {
    m_stopRequested = true;
}

void WasapiCapturer::setDevice(const QString &deviceId, DeviceType type) {
    m_deviceId = deviceId;
    m_type = type;
}

void WasapiCapturer::cleanup() {
    if (m_mixFormat) {
        CoTaskMemFree(m_mixFormat);
        m_mixFormat = nullptr;
    }
    m_captureClient.Reset();
    m_audioClient.Reset();
    m_device.Reset();
    m_enumerator.Reset();
}

QList<AudioDeviceInfo> WasapiCapturer::getAvailableDevices(DeviceType type) {
    QList<AudioDeviceInfo> list;

    // 所在线程的 COM 模式已由 Qt/系统设定（主线程为 STA），显式再次
    // CoInitialize 会返回 "Cannot change thread mode after it is set."，
    // 因此不再初始化，直接使用 COM 接口。
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    if (SUCCEEDED(hr) && enumerator) {
        EDataFlow dataFlow = (type == Microphone) ? eCapture : eRender;
        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection) {
            UINT count;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                ComPtr<IMMDevice> device;
                collection->Item(i, &device);
                LPWSTR id = nullptr;
                device->GetId(&id);

                ComPtr<IPropertyStore> props;
                device->OpenPropertyStore(STGM_READ, &props);
                PROPVARIANT varName;
                PropVariantInit(&varName);
                props->GetValue(PKEY_Device_FriendlyName, &varName);

                AudioDeviceInfo info;
                info.id = QString::fromWCharArray(id);
                info.name = QString::fromWCharArray(varName.pwszVal);
                list.append(info);

                CoTaskMemFree(id);
                PropVariantClear(&varName);
            }
        }
    }

    return list;
}

bool WasapiCapturer::initCapture() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
    if (FAILED(hr)) { qDebug() << "WASAPI: Failed to create enumerator"; return false; }

    if (m_deviceId.isEmpty()) {
        EDataFlow flow = (m_type == Microphone) ? eCapture : eRender;
        hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
        qDebug() << "WASAPI: Using Default Device for" << (m_type==Microphone?"Mic":"Sys");
    } else {
        hr = m_enumerator->GetDevice(m_deviceId.toStdWString().c_str(), &m_device);
        qDebug() << "WASAPI: Using Specific Device ID:" << m_deviceId;
    }
    if (FAILED(hr)) { qDebug() << "WASAPI: Failed to get device"; return false; }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) return false;

    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr)) return false;

    // 可以在此检查格式是否支持，但通常 WASAPI 给的都是 IEEE Float

    DWORD flags = (m_type == SystemLoopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

    // 使用 SHARED 模式
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, REFTIMES_PER_SEC, 0, m_mixFormat, nullptr);
    if (FAILED(hr)) {
        qDebug() << "WASAPI: Initialize failed. HR:" << hr;
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    if (FAILED(hr)) return false;

    return true;
}

void WasapiCapturer::run() {
    // 不在此显式 CoInitialize：线程模式冲突时 Windows 会返回
    // RPC_E_CHANGED_MODE，WASAPI 依赖系统隐式初始化即可工作。

    if (!initCapture()) {
        emit errorOccurred("Failed to initialize WASAPI");
        cleanup();
        return;
    }

    m_audioClient->Start();
    m_stopRequested = false;
    qDebug() << "[Wasapi] Capture Started. Device:" << (m_type==Microphone?"Mic":"Sys")
             << "Channels:" << m_mixFormat->nChannels
             << "Rate:" << m_mixFormat->nSamplesPerSec;

    while (!m_stopRequested) {
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);

        if (SUCCEEDED(hr) && packetLength > 0) {
            BYTE *pData;
            UINT32 numFramesAvailable;
            DWORD flags;

            hr = m_captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);

            if (SUCCEEDED(hr)) {
                if (numFramesAvailable > 0) {
                    // [安全防御] 确保格式有效
                    if (!m_mixFormat || m_mixFormat->nChannels == 0) break;

                    QByteArray floatData;
                    int bytesNeeded = numFramesAvailable * m_mixFormat->nChannels * sizeof(float);

                    try {
                        floatData.resize(bytesNeeded);
                    } catch (...) {
                        m_captureClient->ReleaseBuffer(numFramesAvailable);
                        continue;
                    }

                    float* outPtr = (float*)floatData.data();
                    bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT);

                    if (m_mixFormat->wBitsPerSample == 32 && (m_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || m_mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)) {
                        if (isSilent) memset(outPtr, 0, floatData.size());
                        else memcpy(outPtr, pData, floatData.size());
                    }
                    else if (m_mixFormat->wBitsPerSample == 16) {
                        short* inPtr = (short*)pData;
                        int totalSamples = numFramesAvailable * m_mixFormat->nChannels;
                        for (int i = 0; i < totalSamples; ++i) {
                            if (isSilent) outPtr[i] = 0.0f;
                            else outPtr[i] = inPtr[i] / 32768.0f;
                        }
                    }
                    else {
                        memset(outPtr, 0, floatData.size());
                    }

                    // 计算 RMS 音量 (采样部分数据以提高性能)
                    double sum = 0;
                    int totalSamples = numFramesAvailable * m_mixFormat->nChannels;
                    int step = (totalSamples > 500) ? 5 : 1;
                    for(int i=0; i<totalSamples; i+=step) {
                        sum += outPtr[i] * outPtr[i];
                    }
                    float rms = (totalSamples > 0) ? sqrt(sum / (totalSamples/step)) : 0.0f;

                    emit volumeLevelChanged(rms);
                    emit audioDataReady(floatData, m_mixFormat->nSamplesPerSec, m_mixFormat->nChannels);
                }
                m_captureClient->ReleaseBuffer(numFramesAvailable);
            }
        } else {
            QThread::msleep(5);
        }
    }

    m_audioClient->Stop();
    cleanup();
    qDebug() << "[Wasapi] Stopped.";
}
