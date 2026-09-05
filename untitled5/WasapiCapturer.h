#ifndef WASAPICAPTURER_H
#define WASAPICAPTURER_H

#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct AudioDeviceInfo {
    QString id;
    QString name;
};

class WasapiCapturer : public QThread
{
    Q_OBJECT
public:
    enum DeviceType { Microphone, SystemLoopback };

    explicit WasapiCapturer(QObject *parent = nullptr);
    ~WasapiCapturer();

    // 静态辅助函数：获取设备列表
    static QList<AudioDeviceInfo> getAvailableDevices(DeviceType type);

    // 设置要录制的设备ID (空字符串代表默认设备)
    void setDevice(const QString &deviceId, DeviceType type);
    void stop();

signals:
    // 发送音量电平 (0.0 - 1.0) 用于 UI 显示
    void volumeLevelChanged(float level);

    // 发送原始音频数据用于录制
    // data: PCM数据 (Float32), sampleRate: 采样率, channels: 通道数
    void audioDataReady(const QByteArray &data, int sampleRate, int channels);

    void errorOccurred(QString msg);

protected:
    void run() override;

private:
    bool initCapture();
    void cleanup();

private:
    std::atomic<bool> m_stopRequested;
    QString m_deviceId;
    DeviceType m_type;

    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<IMMDevice> m_device;
    ComPtr<IAudioClient> m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX *m_mixFormat = nullptr;
};

#endif // WASAPICAPTURER_H
