#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QLineEdit> 
#include "RecorderConfig.h"

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(RecorderConfig& config, QWidget *parent = nullptr);
    RecorderConfig getConfig() const;

private:
    void setupUi();
    void loadFromConfig();
    void saveToConfig();

private:
    RecorderConfig& m_config;

    // Video
    QComboBox* m_comboEncoder;
    QSpinBox* m_spinWidth;
    QSpinBox* m_spinHeight;
    QSpinBox* m_spinFps;
    QSpinBox* m_spinBitrate;

    // Recording
    QCheckBox* m_chkEnableFile; 

    // Streaming 
    QCheckBox* m_chkEnableStream;
    QLineEdit* m_editRtmpUrl;
    QLineEdit* m_editStreamKey;

    // Audio
    QSpinBox* m_spinFadeIn;
    QSpinBox* m_spinFadeOut;

    // Window Behavior
    QCheckBox* m_chkResizable;
    QCheckBox* m_chkMaximizable;
};
#endif // SETTINGSDIALOG_H
