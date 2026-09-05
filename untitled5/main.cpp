#include "MainWindow.h"
#include "SystemController.h"
#include "Logger.h"
#include "CrashHandler.h"
#include <QApplication>
#include <QMessageBox>
#include <QList> 

int main(int argc, char *argv[])
{
    // 1. 初始化崩溃捕获
    CrashHandler::init();

    // 2. 初始化日志
    Logger::instance().init("app_log.txt");
    LOG_INFO("Application Launching...");

    // 注册 QList<int> 元类型，确保跨线程 invokeMethod 能识别此参数
    // 必须在 QApplication 之前或尽量早的地方调用
    qRegisterMetaType<QList<int>>("QList<int>");

    // 适配高分屏
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication a(argc, argv);

    try {
        // 启动后台逻辑控制器
        LOG_INFO("Initializing SystemController...");
        SystemController controller;
        controller.initialize();

        // 启动无边框 UI
        LOG_INFO("Showing MainWindow...");
        MainWindow w;
        w.show();

        return a.exec();

    } catch (const std::exception& e) {
        LOG_ERROR(QString("Unhandled C++ Exception: %1").arg(e.what()));
        QMessageBox::critical(nullptr, "Fatal Error", "An unexpected error occurred. Check log file.");
        return -1;
    } catch (...) {
        LOG_ERROR("Unknown Exception caught in main loop.");
        QMessageBox::critical(nullptr, "Fatal Error", "Unknown crash occurred.");
        return -1;
    }
}
