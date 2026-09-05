QT += core gui widgets multimedia multimediawidgets concurrent

CONFIG += c++20

# Windows specific settings
win32 {
    QMAKE_CXXFLAGS += /permissive- /Zc:twoPhase-
}

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    DxGiCapturer.cpp \
    FFmpegRecorder.cpp \
    FramelessWidget.cpp \
    ResizablePixmapItem.cpp \
    SettingsDialog.cpp \
    SystemController.cpp \
    WasapiCapturer.cpp \
    WgcCapturer.cpp \
    WinRTCamera.cpp

HEADERS += \
    AddSourceDialog.h \
    CrashHandler.h \
    DxGiCapturer.h \
    EventBus.h \
    FFmpegRecorder.h \
    FramelessWidget.h \
    Logger.h \
    RecorderConfig.h \
    ResizablePixmapItem.h \
    ResizableTextItem.h \
    SettingsDialog.h \
    SystemController.h \
    WasapiCapturer.h \
    WgcCapturer.h \
    WinRTCamera.h \
    mainwindow.h

# FFmpeg configuration - USE FORWARD SLASHES
FFMPEG_PATH = D:/download/ffmpeg-master-latest-win64-gpl-shared

INCLUDEPATH += $$FFMPEG_PATH/include
LIBS += -L$$FFMPEG_PATH/lib

LIBS += -lavcodec -lavformat -lavutil -lswscale -lswresample

# Windows libraries
LIBS += -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lwindowsapp -luser32 -lmmdevapi -lole32 -lshell32

# Default rules
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
