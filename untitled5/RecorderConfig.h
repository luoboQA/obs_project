#ifndef RECORDERCONFIG_H
#define RECORDERCONFIG_H

#include <QString>

struct RecorderConfig {
    // --- 视频参数 ---
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int videoBitrate = 6000000; // 6 Mbps
    QString encoderName = "h264_nvenc"; // h264_nvenc, h264_qsv, libx264

    // --- 本地录制参数 ---
    bool enableFileRecord = true; // 是否保存本地文件
    QString savePath = "";

    // --- 直播参数 ---
    bool enableStreaming = false;       // 是否开启直播
    QString rtmpUrl = "";               // 推流地址 (例如 rtmp://live.twitch.tv/app/)
    QString streamKey = "";             // 推流码 (例如 live_xxxx_xxxx)

    // --- 音频参数 ---
    int audioSampleRate = 44100;
    int audioBitrate = 128000;
    int audioChannels = 2;
    int fadeInDurationMs = 300;
    int fadeOutDurationMs = 500;

    // --- 窗口行为参数 ---
    bool windowResizable = false;    // 默认锁定
    bool windowMaximizable = false;  // 默认禁用最大化
};

#endif // RECORDERCONFIG_H

