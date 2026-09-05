#ifndef ADDSOURCEDIALOG_H
#define ADDSOURCEDIALOG_H
#include <QGroupBox>
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QDialogButtonBox>

struct SourceSelection {
    bool addImage = false;
    QString imagePath;

    bool addText = false;
    QString textContent;

    bool addCamera = false;
    QVariant cameraDevice;
};

class AddSourceDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddSourceDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle("Add Sources");
        resize(400, 300);
        setupUI();
    }

    SourceSelection getSelection() const { return m_selection; }

private:
    void setupUI() {
        QVBoxLayout *layout = new QVBoxLayout(this);

        // --- 1. Image Section ---
        QGroupBox *grpImg = new QGroupBox("Image Source");
        QVBoxLayout *lImg = new QVBoxLayout(grpImg);
        chkImage = new QCheckBox("Add Image File");
        btnSelectImg = new QPushButton("Select File...");
        btnSelectImg->setEnabled(false);
        lImg->addWidget(chkImage);
        lImg->addWidget(btnSelectImg);
        layout->addWidget(grpImg);

        connect(chkImage, &QCheckBox::toggled, btnSelectImg, &QPushButton::setEnabled);
        connect(btnSelectImg, &QPushButton::clicked, this, [this](){
            QString path = QFileDialog::getOpenFileName(this, "Select Image", "", "Images (*.png *.jpg *.jpeg)");
            if(!path.isEmpty()) { m_selection.imagePath = path; btnSelectImg->setText(path); }
        });

        // --- 2. Text Section ---
        QGroupBox *grpText = new QGroupBox("Text Source");
        QVBoxLayout *lText = new QVBoxLayout(grpText);
        chkText = new QCheckBox("Add Text Label");
        editText = new QLineEdit("New Text Layer");
        editText->setEnabled(false);
        lText->addWidget(chkText);
        lText->addWidget(editText);
        layout->addWidget(grpText);

        connect(chkText, &QCheckBox::toggled, editText, &QLineEdit::setEnabled);

        // --- 3. Camera Section ---
        QGroupBox *grpCam = new QGroupBox("Camera Source");
        QVBoxLayout *lCam = new QVBoxLayout(grpCam);
        chkCam = new QCheckBox("Add Camera");
        comboCam = new QComboBox();
        comboCam->setEnabled(false);
        const auto cameras = QMediaDevices::videoInputs();
        for(const auto &c : cameras) comboCam->addItem(c.description(), QVariant::fromValue(c));

        lCam->addWidget(chkCam);
        lCam->addWidget(comboCam);
        layout->addWidget(grpCam);

        connect(chkCam, &QCheckBox::toggled, comboCam, &QComboBox::setEnabled);

        // --- Buttons ---
        QDialogButtonBox *bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(bbox);

        connect(bbox, &QDialogButtonBox::accepted, this, [this](){
            if(chkImage->isChecked()) m_selection.addImage = true;
            if(chkText->isChecked()) { m_selection.addText = true; m_selection.textContent = editText->text(); }
            if(chkCam->isChecked()) { m_selection.addCamera = true; m_selection.cameraDevice = comboCam->currentData(); }
            accept();
        });
        connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    QCheckBox *chkImage, *chkText, *chkCam;
    QPushButton *btnSelectImg;
    QLineEdit *editText;
    QComboBox *comboCam;
    SourceSelection m_selection;
};

#endif // ADDSOURCEDIALOG_H