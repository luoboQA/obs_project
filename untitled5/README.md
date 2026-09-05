# 学习指南：Qt + FFmpeg 录屏/推流桌面应用（迷你 OBS）

> 项目为 Windows 专用（依赖 DXGI / WASAPI / WinRT），只能在 Windows + MSVC 环境下编译运行。

---

## 目录

- [一、项目速览](#一项目速览)
- [二、编译与运行](#二编译与运行)
- [三、总体架构与线程模型](#三总体架构与线程模型)
- [四、数据流三大管线](#四数据流三大管线)
- [五、Qt 桌面（上位机）开发知识对照](#五qt-桌面上位机开发知识对照)
- [六、FFmpeg 音视频知识对照](#六ffmpeg-音视频知识对照)
- [七、直播推流使用说明](#七直播推流使用说明)

---

## 一、项目速览

### 1.1 功能清单（对照界面按钮看代码）

| 功能 | 实现文件 | 一句话原理 |
|---|---|---|
| 录制整个屏幕 | `DxGiCapturer.cpp` | DXGI Desktop Duplication 抓屏 |
| 录制某个窗口 | `DxGiCapturer.cpp` + `WgcCapturer.cpp` | WinRT Windows.Graphics.Capture |
| 麦克风 / 系统声音 | `WasapiCapturer.cpp` | WASAPI 共享模式采集（系统声音用 Loopback 回环） |
| 摄像头画面作为图层 | `WinRTCamera.cpp` | WinRT MediaCapture + MediaFrameReader |
| 图片 / 文字叠加层 | `mainwindow.cpp` + `ResizablePixmapItem.cpp` | GPU 纹理合成 + 图层管理 |
| 录制本地 MP4 / RTMP 直播推流 | `FFmpegRecorder.cpp` | FFmpeg：H.264 + AAC，双路输出 |
| 截图快照 | `DxGiCapturer.cpp`（requestSnapshot） | 合成帧直接存 PNG |
| 无边框窗口、图层上下移、拖拽缩放、电平表、设置 | `FramelessWidget.cpp` / `mainwindow.cpp` / `ResizablePixmapItem.cpp` / `SettingsDialog.cpp` | 纯 Qt Widgets |

### 1.2 技术栈

- Qt 6.7.2（MSVC2019 64-bit），C++20（见 `untitled5.pro:4`）
- FFmpeg **shared** 版（动态库：avcodec/avformat/avutil/swscale/swresample，`untitled5.pro:42-47`），路径 `D:/download/ffmpeg-master-latest-win64-gpl-shared`
- Windows 原生：D3D11 / DXGI / WASAPI / DWM / WinRT（`untitled5.pro:50`）
- UI 全部手写代码（无 .ui 文件），深色 QSS 主题

---

## 二、编译与运行

1. **环境**：Windows + Qt 6.7（MSVC 2019/2022 kit）+ FFmpeg shared 开发包。
2. **FFmpeg 路径**：把 `untitled5.pro:42` 的 `FFMPEG_PATH` 改成你本机 FFmpeg 的实际路径（目录下需有 `include/` 与 `lib/`）。
3. **运行分发**：FFmpeg shared 版是动态库，程序跑起来前需要把 FFmpeg 的 `.dll`（avcodec-*.dll 等）放到 exe 同目录或加入 PATH，否则启动即报找不到 DLL。
4. 用 Qt Creator 打开 `untitled5.pro`，选择 MSVC 套件，构建即可。
5. **注意**：项目只能在 Windows 跑（DXGI/WASAPI/WinRT），Linux/macOS 无法编译。

---

## 三、总体架构与线程模型

### 3.1 三层架构：解耦靠一条「事件总线」

```
┌──────────────────────────────────────────────────────────┐
│第1层：UI 层（用户界面）                                    │
│MainWindow: 无边框窗口、预览画面、图层列表、控制按钮          │
│SettingsDialog: 设置弹窗                                   │
│AddSourceDialog: 添加图层弹窗                              │
└──────────────────────┬───────────────────────────────────┘
                       │ 命令(用户操作)        │ 事件(数据/状态回传)
             ┌─────────▼─────────────────────▼──────────────┐
             │   第2层：EventBus（信号中转站）                │ 
             │  sendCommandXxx() → emit cmd_xxx             │ 
             │  fireXxx() → emit state_xxx                  │         
             └─────────┬─────────────────────┬──────────────┘
                       │                     │
┌──────────────────────▼─────────────────────▼────────────────────────────────────────┐
│ 第3层：Controller 层：SystemController（协调所有后台工作，运行在主线程，main 线程居民）  │
│- 启动/停止录制                                                                       │
│- 切换摄像头                                                                          │
│- 管理音频设备                                                                        │
└───────┬───────────┬───────────┬───────────┬─────────────────────────────────────────┘
        │           │           │           │
  DxGiCapturer  WasapiCapturer  WinRTCamera 池  FFmpegRecorder
  （视频线程）    （音频线程×2）   （同视频线程）   （编码线程）
   视频抓取         音频采集         摄像头       运行在编码线程
 运行在视频线程
```

- **View → Controller**：调 `EventBus::sendCommandXxx()`，内部只是 `emit cmd_xxx`
- **Controller → View**：调 `EventBus::fireXxx()`
- UI 与后台互不引用对方类——**信号的广播语义天然解耦**。想加"快捷键触发录制"，再调一次 `sendCommandToggleRecording()` 即可，零改动。

### 3.2 六线程图

| 线程 | 对象 | 在干什么 | 线程内事件循环 |
|---|---|---|---|
| main / GUI | MainWindow、SystemController、EventBus、QTimer | 画 UI、响应用户、调度 | ✅ `a.exec()` |
| 视频线程 | DxGiCapturer（moveToThread 搬入） | DXGI 抓屏 + D3D11 合成 + 读回 | ⚠️ 无，靠 `processEvents()` 泵 |
| 编码线程 | FFmpegRecorder（QThread 子类） | sws 转格式 → 编码 → 写文件/推流 | ⚠️ 无，靠 `processEvents()` 泵 |
| 音频线程 ×2 | WasapiCapturer（Mic / Sys） | WASAPI 采集 PCM | 无（run() 内 while + msleep） |
| WinRT 回调线程 | WinRTCamera 内部 | 摄像头帧到达回调 → 更新 GPU 纹理 | WinRT 自己管理 |

---

## 四、数据流三大管线

### 4.1 视频管线（重点理解"GPU 合成 → CPU 读回"）

```
DXGI AcquireNextFrame(抓屏)            WinRTCamera(摄像头帧, GPU纹理)
        │                                       │
        ▼                                       ▼
桌面纹理 ──► D3D11 合成纹理（背景 + 各叠加层按 Z 序画上去）
                │
                ▼
      CopyResource → Staging 纹理（GPU→CPU 唯一通路）
                │ Map 读回
                ▼
         QImage（BGRA）──┬── 录制中 → emit frameCaptured → FFmpegRecorder 队列
                        ├── 预览开 → emit frameCapturedForPreview → UI 画背景
                        └── 有截图请求 → emit snapshotCaptured → 存 PNG
```

代码主线：`DxGiCapturer::startCapture()`（`DxGiCapturer.cpp:386-534`）单帧流程：
- `AcquireNextFrame` 抓桌面（`:421`），`CopyResource` 到缓存纹理（`:433`）
- 建立合成纹理 + RenderTargetView（`:459-462`），依次画背景（`:478-481`）、按 `m_renderOrder` 画叠加层（`:486-508`）
- 读回：`CopyResource` 到 staging → `Map` 拷成 QImage（`:513-525`）
- 分发（`:521-523`）；`preciseSleep` 卡 30fps 节奏（`:528-530`）

**流量控制细节**：
1. 预览背压 `m_uiInFlight`（`DxGiCapturer.cpp:80,522` + `mainwindow.cpp:368` 的 `sendUiFrameProcessed`）：UI 显示完一帧才允许发下一帧，防 UI 消息队列爆掉。
2. 录制丢帧策略：视频队列超 150 帧直接丢（`FFmpegRecorder.cpp:68`）——实时系统宁可丢帧，不可积压。

### 4.2 音频管线

```
WASAPI 采集（16bit 整型 或 32bit float, 包长度任意）
   │ 统一转 float32（WasapiCapturer.cpp:170-181）
   ▼
emit audioDataReady(rate, channels) ─(跨线程信号)→ FFmpegRecorder 入队
   │                                        
   ▼
SwrContext 重采样 → FLTP / 44100Hz / 立体声（FFmpegRecorder.cpp:543-569）
   ▼
各自写入 AVAudioFifo（凑够编码器 frame_size）
   ▼
混音循环：两路各取 frame_size 相加 → clamp[-1,1] → 淡入淡出增益（:193-207）
   ▼
AAC 编码 → 写文件/推流
```

### 4.3 叠加层（图片/文字/摄像头）数据流

UI 上每张图/文字/摄像头都是一个 `ResizablePixmapItem`（可拖、可缩放、可吸附）。
**UI 只是编辑层，真正生效在 GPU**：任何改动 → `geometryChanged(id, rect)` → EventBus → 视频线程更新矩形（图片内容走 `data_overlayUpdate` QueuedConnection 上传纹理）。
图层顺序同步三角：QListWidget 顺序 ↔ Scene ZValue ↔ 后台渲染顺序 `m_renderOrder`，由 `MainWindow::updateZOrder()`（`mainwindow.cpp:276-298`）维护（列表第 0 行=最顶层，需倒序换算）。

摄像头是"懒路径"：UI 只显示一个占位虚线框；真画面由视频线程每帧直接从共享 GPU 纹理 `getLatestFrame()` 采出画上去（`DxGiCapturer.cpp:498-506`），零 CPU 拷贝。

---


## 五、Qt 桌面（上位机）开发知识对照

### 5.1 信号槽与元对象系统（Qt 的地基）

| 概念 | 本项目位置 | 说明 |
|---|---|---|
| Q_OBJECT + moc | `EventBus.h:12` | moc 编译期生成信号/槽的字符串元数据表，Qt 才可能运行时反射 |
| 新语法 connect（函数指针） | `SystemController.cpp:34` | 编译期类型检查，首选 |
| lambda connect | `SystemController.cpp:80` | 免写槽函数；注意 receiver 决定执行线程 |
| 重载信号 QOverload | `mainwindow.cpp:94` | `currentIndexChanged(int)` vs `(QString)` 二选一 |
| 发信号方法包一层 | `EventBus.h:25-27` | `sendCommandXxx(){ emit cmd_xxx; }` —— 发送方不关心谁接收，广播语义 |
| QueuedConnection 显式声明 | `SystemController.cpp:91` | 跨线程边界写清楚，代码即文档 |

**上位机启示**：设备数据（串口/网口/采集卡）→ 后台线程 → `readyRead`/自定义信号 → UI 槽更新控件。这套「数据生产者/消费者 + 信号通知」模式与本项目完全同构，只是把"摄像头帧"换成"设备报文"。

### 5.2 事件循环与线程亲和性（最重要）

- **规则一**：槽函数在 receiver 所在线程执行。所以 receiver 是 `m_capturer`（已搬到视频线程）的 lambda，虽然写在 main 线程函数里，实际在视频线程跑（`SystemController.cpp:80-88`）——这就是让大活自动进子线程的手法。
- **规则二**：QObject 出生在哪条线程就住哪条线程；`moveToThread` 可以搬家。摄像头对象必须"出生"在视频线程（`SystemController.cpp:126-140`），因为要用该线程的 D3D 设备。
- **规则三**：QThread 子类重写 `run()`（`FFmpegRecorder.cpp:120`）是另一种姿势——run() 在子线程执行，没有事件循环，所以循环体里 `processEvents()` 手动泵（`FFmpegRecorder.cpp:153`、`DxGiCapturer.cpp:398`）。
- **规则四**：main 线程只能做轻活（本项目中截图、编码、采集全在子线程）。`a.exec()` 每返回一次才处理一个事件，UI 卡死 = 主事件循环被长时间阻塞。
- **规则五**：`BlockingQueuedConnection` 用于"等子线程干完再继续"，只可从主线程用（`SystemController.cpp:108`）。


---

## 六、FFmpeg 音视频知识对照

### 6.1 必须先懂的基础概念

| 概念 | 通俗解释 | 对应代码 |
|---|---|---|
| 像素格式 | 一帧画面内存怎么排布 | RGBA8888（Qt/D3D 侧）→ YUV420P（软编）/ NV12（硬编），`FFmpegRecorder.cpp:478-481` |
| 采样格式 | PCM 音频内存怎么排布 | FLT=交错 float；FLTP=planar 分离声道 float，AAC 只吃 FLTP |
| 编码器/编解码上下文 | 编解码器与它的"工作台" | `AVCodecContext`，`avcodec_alloc_context3` |
| Frame / Packet | 解码前(后)的一帧 / 编码后的一包 | `AVFrame`、`AVPacket` |
| 容器/封装格式 | 把视频流+音频流打包成文件的规则 | MP4、FLV |
| time_base | 时间戳的刻度（有理数，如 1/1000000 秒） | `av_rescale_q` 随时换算 |
| PTS/DTS | 显示时间 / 解码时间 | RTMP 必须单调递增 |
| GOP / B 帧 | 关键帧间隔 / 双向预测帧 | `gop_size = fps*2`、`max_b_frames=0`（`FFmpegRecorder.cpp:469-470`） |

> FFmpeg 里一切时间都是"刻度数"，要配 `time_base`（刻度粗细）才有意义，换算一律 `av_rescale_q`，绝不要手算乘法。本项目视频时间基统一为 1/1,000,000 秒（微秒刻度，`FFmpegRecorder.cpp:467`）。

### 6.2 本项目 FFmpeg 管线总图

```
                   视频：QImage(RGBA)                   音频：float32 PCM
                        │                                    │
             sws_scale（转像素格式）              swr_convert（重采样到 FLTP/44.1k/立体声）
                        │                                    │
              H.264 编码器（send_frame/receive_packet）←frame_size 凑齐→ AVAudioFifo×2 → 混音+淡入淡出
                        │                                    │
                        └──────────────┬─────────────────────┘
                                       ▼
                          一个编码输出 = 每帧 AVPacket
                                       │ av_packet_clone 克隆两份
                       ┌───────────────┴───────────────┐
                       ▼                               ▼
                 MP4 本地文件                      RTMP 推流(强制 FLV)
                 av_interleaved_write_frame          av_interleaved_write_frame
```

---
## 七、直播推流使用说明

"开始录制/推流"是一个开关，同时控制本地录制（MP4）与 RTMP 直播两路输出，是否推流取决于设置里的勾选，不是单独按钮。

### 7.1 推流步骤（以 MediaMTX 为例）

1. **启动 MediaMTX**（默认监听 RTMP `:1935`）。
2. **设置里填推流参数**（SettingsDialog → 保存 → `onUpdateConfig`）：
   - 推流地址：`/`
   - 流名（streamKey）：`stream`
   - **勾选"开启直播"**——`RecorderConfig.h` 中 `enableStreaming` 默认是 `false`，不勾选就只录本地文件
   - 想"只推流不录文件"：把本地录制关掉（`enableFileRecord = false`）
3. 程序会拼出完整地址 `rtmp://127.0.0.1:1935/stream`（`FFmpegRecorder::initStreamOutput`：URL + `/` + streamKey），强制按 FLV 封装推送。
4. **点主界面"开始"按钮** → `onToggleRecording(true)` → `FFmpegRecorder::run()` 先初始化编码器、再连 RTMP。
5. **验证成功**：MediaMTX 日志出现
   `is publishing to path 'stream', 2 tracks (H264, MPEG-4 Audio)`。
   播放端测试地址（流名 `stream` 时）：
   ```
   rtmp://127.0.0.1:1935/stream                    (RTMP)
   http://127.0.0.1:8888/stream/index.m3u8         (HLS)
   rtsp://127.0.0.1:8554/stream                    (RTSP)
   ```

### 7.2 推流配置速查

| 配置 | 默认值 | 说明 |
|---|---|---|
| `enableStreaming` | `false` | 不勾选不推流（最常见漏配点） |
| `enableFileRecord` | `true` | 本地 MP4 是否同时录制 |
| `rtmpUrl` | `""` | 服务器地址（如 `rtmp://127.0.0.1:1935`） |
| `streamKey` | `""` | 流名（B 站/斗鱼等平台则是推流码） |
| `encoderName` | `h264_nvenc` | 无 N 卡时自动降级 libx264（`FFmpegRecorder.cpp` 降级链） |
| `audioSampleRate` | `44100` | 两路输入音频最终统一重采样到该值 |



