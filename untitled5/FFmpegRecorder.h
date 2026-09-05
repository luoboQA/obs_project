#ifndef FFMPEGRECORDER_H
#define FFMPEGRECORDER_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QMap>
#include <atomic>
#include "RecorderConfig.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
}

struct VideoFrameData {
    QImage image;        // RGBA8888 格式图像
    qint64 timestamp;    // 毫秒时间戳
};

struct AudioChunk {
    QByteArray data;     // float 格式音频数据
    int sampleRate;      // 输入采样率
    int channels;        // 输入声道数
    qint64 timestamp;    // 时间戳（当前未使用）
};


class FFmpegRecorder : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegRecorder(QObject *parent = nullptr);
    ~FFmpegRecorder();

    bool startRecord(const RecorderConfig& config);
    void stopRecord();

public slots:
    void pushVideoFrame(const QImage &img, qint64 timestamp);
    void pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels);

signals:
    void frameProcessed();
    void errorOccurred(QString msg);

protected:
    void run() override;

private:
    void initNetwork();
    bool initVideoEncoder();
    bool initAudioEncoder();
    bool initFileOutput();
    bool initStreamOutput();

    void ensureAudioSource(int id);
    bool resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels);
    void processVideo(const VideoFrameData &frameData);
    void flushEncoders();
    void closeResources();

    
    void writePacket(AVPacket* pkt, bool isVideo);
    void applyFadeEffect(float* bufferL, float* bufferR, int samples, qint64 totalEncodedSamples, bool isFadingOut, qint64 fadeOutStartSample);

private:
    RecorderConfig m_config;

    std::atomic<bool> m_isRecording;
    std::atomic<bool> m_softStopRequested;

    qint64 m_firstFrameTimestamp = -1;

    // --- 编码器 (Source) ---
    AVCodecContext *m_vCtx = nullptr;
    AVFrame *m_vFrame = nullptr;
    SwsContext *m_swsCtx = nullptr;

    AVCodecContext *m_aCtx = nullptr;
    AVFrame *m_aFrame = nullptr;
    qint64 m_totalAudioSamples = 0;

    // --- 输出 1: 本地文件 ---
    AVFormatContext *m_fmtCtxFile = nullptr;
    AVStream *m_fileVStream = nullptr;
    AVStream *m_fileAStream = nullptr;

    // --- 输出 2: 直播流 ---
    AVFormatContext *m_fmtCtxStream = nullptr;
    AVStream *m_streamVStream = nullptr;
    AVStream *m_streamAStream = nullptr;

    struct SourceContext {
        SwrContext *swrCtx = nullptr;   // 重采样上下文（输入→48kHz）
        AVAudioFifo *fifo = nullptr;    // 音频 FIFO 缓冲区
        int inSampleRate = 0;           // 输入采样率（用于检测变化）
    };
    QMap<int, SourceContext> m_audioSources;

    QMutex m_videoMutex;
    QQueue<VideoFrameData> m_videoQueue;
    QMutex m_audioMutex;
    QQueue<QPair<int, AudioChunk>> m_audioQueue;

    qint64 m_lastStreamVideoPts = 0;
    qint64 m_lastStreamVideoDts = 0;
    qint64 m_lastStreamAudioPts = 0;
    qint64 m_lastStreamAudioDts = 0;
};

#endif // FFMPEGRECORDER_H
