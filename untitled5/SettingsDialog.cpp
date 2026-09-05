#include "SettingsDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLabel>

SettingsDialog::SettingsDialog(RecorderConfig& config, QWidget *parent)
    : QDialog(parent), m_config(config) {
    setWindowTitle("Global Settings");
    resize(550, 650); // 增加高度以容纳直播设置
    setStyleSheet("QDialog { background-color: #333; color: white; } "
                  "QLabel, QCheckBox { color: white; } "
                  "QLineEdit { background-color: #444; color: white; border: 1px solid #555; padding: 4px; } "
                  "QGroupBox { color: white; border: 1px solid #555; margin-top: 10px; } "
                  "QGroupBox::title { subcontrol-origin: margin; left: 10px; }");
    setupUi();
    loadFromConfig();
}

void SettingsDialog::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // 1. Window Behavior
    QGroupBox* grpWindow = new QGroupBox("Window Behavior");
    QVBoxLayout* winLayout = new QVBoxLayout(grpWindow);
    m_chkResizable = new QCheckBox("Enable Window Resizing");
    m_chkMaximizable = new QCheckBox("Enable Window Maximizing");
    winLayout->addWidget(m_chkResizable);
    winLayout->addWidget(m_chkMaximizable);
    mainLayout->addWidget(grpWindow);

    // 2. Video Settings
    QGroupBox* grpVideo = new QGroupBox("Video Encoding");
    QFormLayout* vidForm = new QFormLayout(grpVideo);
    m_comboEncoder = new QComboBox();
    m_comboEncoder->addItems({"h264_nvenc", "h264_qsv", "libx264"});
    vidForm->addRow("Encoder:", m_comboEncoder);

    QHBoxLayout* resLayout = new QHBoxLayout();
    m_spinWidth = new QSpinBox(); m_spinWidth->setRange(100, 7680);
    m_spinHeight = new QSpinBox(); m_spinHeight->setRange(100, 4320);
    resLayout->addWidget(m_spinWidth);
    resLayout->addWidget(new QLabel("x"));
    resLayout->addWidget(m_spinHeight);
    vidForm->addRow("Resolution:", resLayout);

    m_spinFps = new QSpinBox(); m_spinFps->setRange(1, 144);
    vidForm->addRow("FPS:", m_spinFps);
    m_spinBitrate = new QSpinBox(); m_spinBitrate->setRange(1, 100); m_spinBitrate->setSuffix(" Mbps");
    vidForm->addRow("Bitrate:", m_spinBitrate);
    mainLayout->addWidget(grpVideo);

    // 3. Output Outputs (Recording & Streaming)
    QGroupBox* grpOutput = new QGroupBox("Outputs");
    QVBoxLayout* outLayout = new QVBoxLayout(grpOutput);

    // File Recording
    m_chkEnableFile = new QCheckBox("Save to Local File (MP4)");
    outLayout->addWidget(m_chkEnableFile);

    // Streaming
    m_chkEnableStream = new QCheckBox("Enable Live Streaming (RTMP)");
    outLayout->addWidget(m_chkEnableStream);

    QFormLayout* streamForm = new QFormLayout();
    m_editRtmpUrl = new QLineEdit();
    m_editRtmpUrl->setPlaceholderText("rtmp://server-url/app");
    streamForm->addRow("RTMP URL:", m_editRtmpUrl);

    m_editStreamKey = new QLineEdit();
    m_editStreamKey->setEchoMode(QLineEdit::Password);
    m_editStreamKey->setPlaceholderText("Stream Key");
    streamForm->addRow("Stream Key:", m_editStreamKey);

    // 联动逻辑：只有勾选直播才显示输入框
    connect(m_chkEnableStream, &QCheckBox::toggled, [=](bool checked){
        m_editRtmpUrl->setEnabled(checked);
        m_editStreamKey->setEnabled(checked);
    });
    outLayout->addLayout(streamForm);
    mainLayout->addWidget(grpOutput);

    // 4. Audio Settings
    QGroupBox* grpAudio = new QGroupBox("Audio Processing");
    QFormLayout* audForm = new QFormLayout(grpAudio);
    m_spinFadeIn = new QSpinBox(); m_spinFadeIn->setRange(0, 5000); m_spinFadeIn->setSuffix(" ms");
    audForm->addRow("Fade In:", m_spinFadeIn);
    m_spinFadeOut = new QSpinBox(); m_spinFadeOut->setRange(0, 5000); m_spinFadeOut->setSuffix(" ms");
    audForm->addRow("Fade Out:", m_spinFadeOut);
    mainLayout->addWidget(grpAudio);

    // Buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this](){ saveToConfig(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

void SettingsDialog::loadFromConfig() {
    // Window
    m_chkResizable->setChecked(m_config.windowResizable);
    m_chkMaximizable->setChecked(m_config.windowMaximizable);

    // Video
    m_comboEncoder->setCurrentText(m_config.encoderName);
    m_spinWidth->setValue(m_config.width);
    m_spinHeight->setValue(m_config.height);
    m_spinFps->setValue(m_config.fps);
    m_spinBitrate->setValue(m_config.videoBitrate / 1000000);

    // Output
    m_chkEnableFile->setChecked(m_config.enableFileRecord);
    m_chkEnableStream->setChecked(m_config.enableStreaming);
    m_editRtmpUrl->setText(m_config.rtmpUrl);
    m_editStreamKey->setText(m_config.streamKey);
    m_editRtmpUrl->setEnabled(m_config.enableStreaming);
    m_editStreamKey->setEnabled(m_config.enableStreaming);

    // Audio
    m_spinFadeIn->setValue(m_config.fadeInDurationMs);
    m_spinFadeOut->setValue(m_config.fadeOutDurationMs);
}

void SettingsDialog::saveToConfig() {
    // Window
    m_config.windowResizable = m_chkResizable->isChecked();
    m_config.windowMaximizable = m_chkMaximizable->isChecked();

    // Video
    m_config.encoderName = m_comboEncoder->currentText();
    m_config.width = m_spinWidth->value();
    m_config.height = m_spinHeight->value();
    m_config.fps = m_spinFps->value();
    m_config.videoBitrate = m_spinBitrate->value() * 1000000;

    // Output
    m_config.enableFileRecord = m_chkEnableFile->isChecked();
    m_config.enableStreaming = m_chkEnableStream->isChecked();
    m_config.rtmpUrl = m_editRtmpUrl->text().trimmed();
    m_config.streamKey = m_editStreamKey->text().trimmed();

    // Audio
    m_config.fadeInDurationMs = m_spinFadeIn->value();
    m_config.fadeOutDurationMs = m_spinFadeOut->value();
}

RecorderConfig SettingsDialog::getConfig() const {
    return m_config;
}
