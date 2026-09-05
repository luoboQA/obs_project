#include "FFmpegRecorder.h"
#include <QDebug>
#include <QDateTime>
#include <QCoreApplication>
#include <algorithm>

static void printError(const QString& tag, int errCode) {
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errCode, errBuf, AV_ERROR_MAX_STRING_SIZE);
    qCritical() << "[FFmpegError]" << tag << ":" << errBuf << "(Code:" << errCode << ")";
}

FFmpegRecorder::FFmpegRecorder(QObject *parent) : QThread(parent) {
    m_isRecording = false;
    m_softStopRequested = false;
    this->moveToThread(this);
    initNetwork();
}

FFmpegRecorder::~FFmpegRecorder() {
    if (isRunning()) {
        m_softStopRequested = true;
        wait();
    }
}

void FFmpegRecorder::initNetwork() {
    avformat_network_init();
}

bool FFmpegRecorder::startRecord(const RecorderConfig& config) {
    if (isRunning()) return false;
    m_lastStreamVideoPts = 0; m_lastStreamVideoDts = 0;
    m_lastStreamAudioPts = 0; m_lastStreamAudioDts = 0;

    m_config = config;

    if (m_config.savePath.isEmpty()) {
        m_config.savePath = "REC_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4";
    }

    m_config.width -= (m_config.width % 2);
    m_config.height -= (m_config.height % 2);

    if (!m_config.enableFileRecord && !m_config.enableStreaming) {
        qWarning() << "No output selected (File or Stream).";
        return false;
    }

    { QMutexLocker l(&m_videoMutex); m_videoQueue.clear(); }
    { QMutexLocker l(&m_audioMutex); m_audioQueue.clear(); }

    m_softStopRequested = false;
    m_firstFrameTimestamp = -1;
    start();
    return true;
}

void FFmpegRecorder::stopRecord() {
    if (isRunning()) {
        m_softStopRequested = true;
    }
}

void FFmpegRecorder::pushVideoFrame(const QImage &img, qint64 timestamp) {
    if (!m_isRecording || m_softStopRequested) return;
    QMutexLocker locker(&m_videoMutex);
    if (m_videoQueue.size() > 150) return;

    VideoFrameData data;
    if (img.format() != QImage::Format_RGBA8888) data.image = img.convertToFormat(QImage::Format_RGBA8888);
    else data.image = img.copy();
    data.timestamp = timestamp;
    m_videoQueue.enqueue(data);
}

void FFmpegRecorder::pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    if (!m_isRecording || m_softStopRequested) return;
    if (m_firstFrameTimestamp == -1) return;

    QMutexLocker locker(&m_audioMutex);
    AudioChunk chunk;
    chunk.data = data;
    chunk.sampleRate = sampleRate;
    chunk.channels = channels;
    chunk.timestamp = 0;
    m_audioQueue.enqueue(qMakePair(sourceId, chunk));
}

void FFmpegRecorder::ensureAudioSource(int id) {
    if (!m_audioSources.contains(id)) {
        SourceContext ctx;
        ctx.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 1);
        m_audioSources.insert(id, ctx);
    }
}

void FFmpegRecorder::applyFadeEffect(float* bufferL, float* bufferR, int samples,
                                     qint64 totalEncodedSamples, bool isFadingOut, qint64 fadeOutStartSample)
{
    int sampleRate = m_config.audioSampleRate;
    qint64 fadeInSamples = (qint64)(m_config.fadeInDurationMs / 1000.0 * sampleRate);
    qint64 fadeOutSamples = (qint64)(m_config.fadeOutDurationMs / 1000.0 * sampleRate);

    for (int i = 0; i < samples; ++i) {
        qint64 currentPos = totalEncodedSamples + i;
        float gain = 1.0f;
        if (currentPos < fadeInSamples) gain = (float)currentPos / (float)fadeInSamples;
        if (isFadingOut) {
            qint64 samplesSinceStop = currentPos - fadeOutStartSample;
            if (samplesSinceStop >= 0) {
                if (samplesSinceStop < fadeOutSamples) gain *= (1.0f - (float)samplesSinceStop / (float)fadeOutSamples);
                else gain = 0.0f;
            }
        }
        bufferL[i] *= gain; bufferR[i] *= gain;
    }
}

void FFmpegRecorder::run() {
    qDebug() << "Recorder Thread Start. File:" << m_config.enableFileRecord << "Stream:" << m_config.enableStreaming;

    if (!initVideoEncoder()) { emit errorOccurred("Video Encoder Init Failed"); closeResources(); return; }
    if (!initAudioEncoder()) { emit errorOccurred("Audio Encoder Init Failed"); closeResources(); return; }

    bool fileOk = true, streamOk = true;
    if (m_config.enableFileRecord) {
        if (!initFileOutput()) { emit errorOccurred("File Output Init Failed"); fileOk = false; }
    }
    if (m_config.enableStreaming) {
        if (!initStreamOutput()) { emit errorOccurred("Streaming Init Failed"); streamOk = false; }
    }

    if (!fileOk && !streamOk) { closeResources(); return; }

    ensureAudioSource(0); ensureAudioSource(1);
    m_isRecording = true;
    m_totalAudioSamples = 0;
    qint64 lastVideoPts = 0;

    bool isFadingOut = false;
    qint64 fadeOutStartSample = 0;
    qint64 finalAudioSampleEnd = -1;

    int frameSize = m_aCtx->frame_size;
    float *mixBufferL = new float[frameSize];
    float *mixBufferR = new float[frameSize];
    float *tmpL = new float[frameSize];
    float *tmpR = new float[frameSize];
    void *tmpData[2] = { tmpL, tmpR };

    while (true) {
        QCoreApplication::processEvents();

        // Video
        VideoFrameData vFrameData;
        bool hasVideo = false;
        {
            QMutexLocker locker(&m_videoMutex);
            if (!m_videoQueue.isEmpty()) { vFrameData = m_videoQueue.dequeue(); hasVideo = true; }
        }
        if (hasVideo) {
            emit frameProcessed();
            processVideo(vFrameData);
            lastVideoPts = vFrameData.timestamp;
        }

        // Stop Logic
        if (m_softStopRequested && !isFadingOut) {
            isFadingOut = true;
            fadeOutStartSample = m_totalAudioSamples;
            qint64 samplesNeeded = (qint64)(m_config.fadeOutDurationMs / 1000.0 * m_config.audioSampleRate);
            finalAudioSampleEnd = fadeOutStartSample + samplesNeeded;
            qDebug() << "Stopping... Fading out.";
        }
        if (isFadingOut && m_totalAudioSamples >= finalAudioSampleEnd && !hasVideo && m_videoQueue.isEmpty()) break;

        // Audio Fifo
        while(true) {
            QPair<int, AudioChunk> chunk;
            bool hasAudio = false;
            {
                QMutexLocker locker(&m_audioMutex);
                if (!m_audioQueue.isEmpty()) { chunk = m_audioQueue.dequeue(); hasAudio = true; }
            }
            if (hasAudio) resampleToFifo(chunk.first, chunk.second.data, chunk.second.sampleRate, chunk.second.channels);
            else break;
        }

        // Audio Encoding
        if (m_firstFrameTimestamp != -1) {
            qint64 targetSamples = isFadingOut ? finalAudioSampleEnd : av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});
            while (m_totalAudioSamples < targetSamples) {
                memset(mixBufferL, 0, frameSize * sizeof(float)); memset(mixBufferR, 0, frameSize * sizeof(float));
                for (auto key : m_audioSources.keys()) {
                    SourceContext &ctx = m_audioSources[key];
                    if (!ctx.fifo) continue;
                    if (av_audio_fifo_size(ctx.fifo) >= frameSize) {
                        av_audio_fifo_read(ctx.fifo, tmpData, frameSize);
                        for (int i = 0; i < frameSize; ++i) { mixBufferL[i] += tmpL[i]; mixBufferR[i] += tmpR[i]; }
                    }
                }
                applyFadeEffect(mixBufferL, mixBufferR, frameSize, m_totalAudioSamples, isFadingOut, fadeOutStartSample);
                for (int i = 0; i < frameSize; ++i) {
                    mixBufferL[i] = std::clamp(mixBufferL[i], -1.0f, 1.0f);
                    mixBufferR[i] = std::clamp(mixBufferR[i], -1.0f, 1.0f);
                }

                av_frame_make_writable(m_aFrame);
                memcpy(m_aFrame->data[0], mixBufferL, frameSize * sizeof(float));
                memcpy(m_aFrame->data[1], mixBufferR, frameSize * sizeof(float));
                m_aFrame->pts = av_rescale_q(m_totalAudioSamples, {1, m_aCtx->sample_rate}, m_aCtx->time_base);
                m_totalAudioSamples += frameSize;

                if (avcodec_send_frame(m_aCtx, m_aFrame) >= 0) {
                    AVPacket *pkt = av_packet_alloc();
                    while (avcodec_receive_packet(m_aCtx, pkt) >= 0) {
                        writePacket(pkt, false);
                        av_packet_unref(pkt);
                    }
                    av_packet_free(&pkt);
                }
            }
        }
        if (!hasVideo && !m_softStopRequested) QThread::msleep(1);
    }

    delete[] mixBufferL; delete[] mixBufferR; delete[] tmpL; delete[] tmpR;

    flushEncoders();
    if (m_fmtCtxFile) av_write_trailer(m_fmtCtxFile);
    if (m_fmtCtxStream) av_write_trailer(m_fmtCtxStream);

    closeResources();
    m_isRecording = false;
    qDebug() << "Recording/Streaming Finished.";
}

// --- 初始化文件输出 ---
bool FFmpegRecorder::initFileOutput() {
    int ret = avformat_alloc_output_context2(&m_fmtCtxFile, nullptr, nullptr, m_config.savePath.toStdString().c_str());
    if (!m_fmtCtxFile || ret < 0) return false;

    m_fileVStream = avformat_new_stream(m_fmtCtxFile, nullptr);
    avcodec_parameters_from_context(m_fileVStream->codecpar, m_vCtx);
    m_fileVStream->time_base = m_vCtx->time_base;

    m_fileAStream = avformat_new_stream(m_fmtCtxFile, nullptr);
    avcodec_parameters_from_context(m_fileAStream->codecpar, m_aCtx);
    m_fileAStream->time_base = m_aCtx->time_base;

    if (!(m_fmtCtxFile->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_fmtCtxFile->pb, m_config.savePath.toStdString().c_str(), AVIO_FLAG_WRITE) < 0) return false;
    }

    if (avformat_write_header(m_fmtCtxFile, nullptr) < 0) return false;
    qDebug() << "File Output Init OK:" << m_config.savePath;
    return true;
}

// --- 初始化直播输出 ---
bool FFmpegRecorder::initStreamOutput() {
    QString fullUrl = m_config.rtmpUrl;
    if (!m_config.streamKey.isEmpty()) {
        if (!fullUrl.endsWith("/")) fullUrl += "/";
        fullUrl += m_config.streamKey;
    }

    qDebug() << "Initializing Stream:" << fullUrl;

    // 强制指定 flv 封装格式
    int ret = avformat_alloc_output_context2(&m_fmtCtxStream, nullptr, "flv", fullUrl.toStdString().c_str());
    if (!m_fmtCtxStream || ret < 0) {
        printError("Stream Alloc Context", ret);
        return false;
    }

    // 直播输出(网络流)不可 seek:停止推流时 FLV muxer 无法回写 duration/filesize 字段,
    // 会打印 "Failed to update header with correct duration/filesize" 警告。
    // 直播场景不需要这两个字段,直接禁用(等价于 ffmpeg 推流的 -flvflags no_duration_filesize)
    av_opt_set(m_fmtCtxStream->priv_data, "flvflags", "no_duration_filesize", 0);

    // --- 复制视频流 ---
    m_streamVStream = avformat_new_stream(m_fmtCtxStream, nullptr);
    avcodec_parameters_from_context(m_streamVStream->codecpar, m_vCtx);
    m_streamVStream->time_base = m_vCtx->time_base;
    m_streamVStream->codecpar->codec_tag = 0; // 消除警告

    // --- 复制音频流 ---
    m_streamAStream = avformat_new_stream(m_fmtCtxStream, nullptr);
    avcodec_parameters_from_context(m_streamAStream->codecpar, m_aCtx);
    m_streamAStream->time_base = m_aCtx->time_base;
    m_streamAStream->codecpar->codec_tag = 0;

    // --- 设置 RTMP 传输参数 ---
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtmp_transport", "tcp", 0); // 强制 TCP
    av_dict_set(&options, "rtmp_buffer", "1000", 0);   // 1000ms 缓冲
    av_dict_set(&options, "buffer_size", "4096000", 0);// Socket 发送缓冲区 4MB
    av_dict_set(&options, "rw_timeout", "5000000", 0); // 读写超时 5秒
    av_dict_set(&options, "tcp_nodelay", "1", 0);      // 禁用 Nagle 算法，降低延迟

    if (!(m_fmtCtxStream->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&m_fmtCtxStream->pb, fullUrl.toStdString().c_str(), AVIO_FLAG_WRITE, nullptr, &options);
        if (ret < 0) {
            printError("Stream Connect Failed", ret);
            av_dict_free(&options);
            return false;
        }
    }

    ret = avformat_write_header(m_fmtCtxStream, &options);
    av_dict_free(&options);

    if (ret < 0) {
        printError("Stream Write Header", ret);
        return false;
    }

    qDebug() << "Stream Output Init OK.";
    return true;
}

// --- 多路分发 ---
void FFmpegRecorder::writePacket(AVPacket* pkt, bool isVideo) {
    // 获取源时间基准
    AVRational srcTb = isVideo ? m_vCtx->time_base : m_aCtx->time_base;

    // 1. 写入文件 (本地录制通常比较宽容，保持原样)
    if (m_fmtCtxFile) {
        AVPacket* filePkt = av_packet_clone(pkt);
        if (filePkt) {
            AVStream* outStream = isVideo ? m_fileVStream : m_fileAStream;
            av_packet_rescale_ts(filePkt, srcTb, outStream->time_base);
            filePkt->stream_index = outStream->index;
            av_interleaved_write_frame(m_fmtCtxFile, filePkt);
            av_packet_unref(filePkt);
            av_packet_free(&filePkt);
        }
    }

    // 2. 写入直播流 (增加严格检查)
    if (m_fmtCtxStream) {
        AVPacket* streamPkt = av_packet_clone(pkt);
        if (streamPkt) {
            AVStream* outStream = isVideo ? m_streamVStream : m_streamAStream;
            av_packet_rescale_ts(streamPkt, srcTb, outStream->time_base);
            streamPkt->stream_index = outStream->index;

            // --- 时间戳单调性强制矫正 ---
            // RTMP 服务器非常讨厌时间戳回退，这会导致立即断开 (-10053)
            qint64* lastPts = isVideo ? &m_lastStreamVideoPts : &m_lastStreamAudioPts;
            qint64* lastDts = isVideo ? &m_lastStreamVideoDts : &m_lastStreamAudioDts;

            if (streamPkt->dts < *lastDts) {
                streamPkt->dts = *lastDts + 1; // 强制递增
            }
            if (streamPkt->pts < streamPkt->dts) {
                streamPkt->pts = streamPkt->dts; // PTS 必须 >= DTS
            }
            // 更新记录
            *lastDts = streamPkt->dts;
            *lastPts = streamPkt->pts;

            // 执行写入
            int ret = av_interleaved_write_frame(m_fmtCtxStream, streamPkt);

            if (ret < 0) {
                char errBuf[128] = {0};
                av_strerror(ret, errBuf, 128);
                qWarning() << "Stream Write Error:" << ret << "(" << errBuf << ")";

                // 如果是 -10053 (WSAECONNABORTED) 或 Broken Pipe，通常意味着连接已死
                // 此时应停止尝试写入直播流，避免阻塞或崩溃，但保留本地录制
                if (ret == -10053 || ret == AVERROR(EPIPE)) {
                    qCritical() << "Connection lost. Stopping Stream Output.";
                    if (!(m_fmtCtxStream->oformat->flags & AVFMT_NOFILE)) {
                        avio_closep(&m_fmtCtxStream->pb);
                    }
                    avformat_free_context(m_fmtCtxStream);
                    m_fmtCtxStream = nullptr; // 置空，后续不再进入此逻辑
                }
            }
            av_packet_unref(streamPkt);
            av_packet_free(&streamPkt);
        }
    }
}

// --- Video Process ---
void FFmpegRecorder::processVideo(const VideoFrameData &frameData) {
    if (m_firstFrameTimestamp == -1) m_firstFrameTimestamp = frameData.timestamp;
    if (frameData.image.isNull()) return;

    if (!m_swsCtx) {
        AVPixelFormat inputFmt = (frameData.image.format() == QImage::Format_RGBA8888) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
        m_swsCtx = sws_getContext(frameData.image.width(), frameData.image.height(), inputFmt,
                                  m_vCtx->width, m_vCtx->height, m_vCtx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    }
    const uint8_t *srcSlice[] = { frameData.image.constBits() };
    int srcStride[] = { (int)frameData.image.bytesPerLine() };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, frameData.image.height(), m_vFrame->data, m_vFrame->linesize);

    qint64 diffMs = frameData.timestamp;
    m_vFrame->pts = av_rescale_q(diffMs * 1000, {1, 1000000}, m_vCtx->time_base);

    if (avcodec_send_frame(m_vCtx, m_vFrame) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(m_vCtx, pkt) >= 0) {
            writePacket(pkt, true); // 发送视频包
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
}

void FFmpegRecorder::flushEncoders() {
    if (!m_vCtx || !m_aCtx) return;
    AVPacket *pkt = av_packet_alloc();

    avcodec_send_frame(m_vCtx, nullptr);
    while(avcodec_receive_packet(m_vCtx, pkt) >= 0) {
        writePacket(pkt, true);
        av_packet_unref(pkt);
    }

    avcodec_send_frame(m_aCtx, nullptr);
    while(avcodec_receive_packet(m_aCtx, pkt) >= 0) {
        writePacket(pkt, false);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void FFmpegRecorder::closeResources() {
    if (m_vFrame) { av_frame_free(&m_vFrame); m_vFrame = nullptr; }
    if (m_aFrame) { av_frame_free(&m_aFrame); m_aFrame = nullptr; }
    if (m_vCtx) { avcodec_free_context(&m_vCtx); m_vCtx = nullptr; }
    if (m_aCtx) { avcodec_free_context(&m_aCtx); m_aCtx = nullptr; }

    if (m_fmtCtxFile) {
        if (!(m_fmtCtxFile->oformat->flags & AVFMT_NOFILE)) avio_closep(&m_fmtCtxFile->pb);
        avformat_free_context(m_fmtCtxFile);
        m_fmtCtxFile = nullptr;
    }

    if (m_fmtCtxStream) {
        if (!(m_fmtCtxStream->oformat->flags & AVFMT_NOFILE)) avio_closep(&m_fmtCtxStream->pb);
        avformat_free_context(m_fmtCtxStream);
        m_fmtCtxStream = nullptr;
    }

    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    for(auto &k : m_audioSources.keys()) {
        if (m_audioSources[k].swrCtx) swr_free(&m_audioSources[k].swrCtx);
        if (m_audioSources[k].fifo) av_audio_fifo_free(m_audioSources[k].fifo);
    }
    m_audioSources.clear();
}

// 视频编码器初始化（保持不变）
bool FFmpegRecorder::initVideoEncoder() {
    auto tryOpenCodec = [&](QString codecName) -> bool {
        const AVCodec *codec = avcodec_find_encoder_by_name(codecName.toStdString().c_str());
        if (!codec) return false;

        if (m_vCtx) avcodec_free_context(&m_vCtx);
        m_vCtx = avcodec_alloc_context3(codec);

        m_vCtx->width = m_config.width;
        m_vCtx->height = m_config.height;
        m_vCtx->time_base = {1, 1000000};
        m_vCtx->framerate = {m_config.fps, 1};
        m_vCtx->gop_size = m_config.fps * 2;
        m_vCtx->max_b_frames = 0;
        m_vCtx->bit_rate = m_config.videoBitrate;

        // --- 强制生成全局头部信息 (SPS/PPS) ---
        // FLV (RTMP) 和 MP4 容器都需要这个标志，否则服务器无法解析 H264 Config
        m_vCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (codecName.contains("nvenc") || codecName.contains("qsv")) {
            m_vCtx->pix_fmt = AV_PIX_FMT_NV12;
        } else {
            m_vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        }

        if (codecName == "libx264") {
            av_opt_set(m_vCtx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(m_vCtx->priv_data, "tune", "zerolatency", 0);
        }

        return (avcodec_open2(m_vCtx, codec, nullptr) >= 0);
    };

    if (tryOpenCodec(m_config.encoderName)) goto success;

    // 降级尝试
    if (m_config.encoderName != "libx264" && tryOpenCodec("libx264")) {
        m_config.encoderName="libx264";
        goto success;
    }

    return false;

success:
    if (m_vFrame) av_frame_free(&m_vFrame);
    m_vFrame = av_frame_alloc();
    m_vFrame->width = m_config.width;
    m_vFrame->height = m_config.height;
    m_vFrame->format = m_vCtx->pix_fmt;
    av_frame_get_buffer(m_vFrame, 32);
    return true;
}

// 替换 FFmpegRecorder.cpp 中的 initAudioEncoder
bool FFmpegRecorder::initAudioEncoder() {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return false;

    m_aCtx = avcodec_alloc_context3(codec);
    m_aCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_aCtx->bit_rate = m_config.audioBitrate;
    m_aCtx->sample_rate = m_config.audioSampleRate;

    AVChannelLayout layout; av_channel_layout_default(&layout, 2);
    av_channel_layout_copy(&m_aCtx->ch_layout, &layout);
    av_channel_layout_uninit(&layout);

    m_aCtx->time_base = {1, m_aCtx->sample_rate};

    // --- 音频也需要全局头部 (AAC Config) ---
    m_aCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(m_aCtx, codec, nullptr) < 0) return false;

    m_aFrame = av_frame_alloc();
    m_aFrame->nb_samples = m_aCtx->frame_size;
    m_aFrame->format = m_aCtx->sample_fmt;
    av_channel_layout_copy(&m_aFrame->ch_layout, &m_aCtx->ch_layout);
    m_aFrame->sample_rate = m_aCtx->sample_rate;
    av_frame_get_buffer(m_aFrame, 0);

    return true;
}

// 重采样逻辑（保持不变）
bool FFmpegRecorder::resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    ensureAudioSource(sourceId);
    SourceContext &ctx = m_audioSources[sourceId];
    if (data.isEmpty()) return true;
    if (!ctx.swrCtx || ctx.inSampleRate != sampleRate) {
        if (ctx.swrCtx) swr_free(&ctx.swrCtx);
        AVChannelLayout outLayout; av_channel_layout_copy(&outLayout, &m_aCtx->ch_layout);
        AVChannelLayout inLayout; av_channel_layout_default(&inLayout, channels);
        swr_alloc_set_opts2(&ctx.swrCtx, &outLayout, AV_SAMPLE_FMT_FLTP, m_aCtx->sample_rate,
                            &inLayout, AV_SAMPLE_FMT_FLT, sampleRate, 0, nullptr);
        swr_init(ctx.swrCtx);
        ctx.inSampleRate = sampleRate;
        av_channel_layout_uninit(&inLayout); av_channel_layout_uninit(&outLayout);
    }
    int inputSamples = static_cast<int>(data.size() / (channels * sizeof(float))); // qsizetype -> int (C4267)
    int maxOutSamples = av_rescale_rnd(swr_get_delay(ctx.swrCtx, sampleRate) + inputSamples, m_aCtx->sample_rate, sampleRate, AV_ROUND_UP);
    uint8_t **outData = nullptr;
    av_samples_alloc_array_and_samples(&outData, nullptr, 2, maxOutSamples, AV_SAMPLE_FMT_FLTP, 0);
    const uint8_t *inData[] = { (const uint8_t*)data.constData() };
    int converted = swr_convert(ctx.swrCtx, outData, maxOutSamples, inData, inputSamples);
    if (converted > 0) {
        if (av_audio_fifo_space(ctx.fifo) < converted) {
            int requiredSize = av_audio_fifo_size(ctx.fifo) + converted;
            if (av_audio_fifo_realloc(ctx.fifo, requiredSize) < 0) {
                av_freep(&outData[0]);
                av_freep(&outData);
                return false; // FIFO 扩容失败(内存不足),丢弃本次转换结果
            }
        }
        av_audio_fifo_write(ctx.fifo, (void**)outData, converted);
    }
    if (outData) { av_freep(&outData[0]); av_freep(&outData); }
    return true;
}
