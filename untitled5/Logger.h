#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QMutex>
#include <QRecursiveMutex> // 引入递归锁头文件
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QThread>
#include <iostream>

// 日志级别
enum class LogLevel {
    INFO,
    WARNING,
    CRITICAL,
    DEBUG_TRACE
};

class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }

    void init(const QString& filename) {
        // 【关键】使用递归锁，允许 init 内部调用 log 不会死锁
        QMutexLocker locker(&m_mutex);
        m_file.setFileName(filename);
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            m_stream.setDevice(&m_file);
            // 之前这里调用 log() 会导致死锁，因为 log() 也要抢锁
            // 现在换成 QRecursiveMutex 就没问题了
            log(LogLevel::INFO, "=== Session Started ===");
        } else {
            // 如果文件打不开，至少输出到控制台
            std::cerr << "Failed to open log file: " << filename.toStdString() << std::endl;
        }
    }

    void log(LogLevel level, const QString& message, const char* /*file*/ = "", int line = 0, const char* func = "") {
        QString levelStr;
        switch(level) {
        case LogLevel::INFO: levelStr = "[INFO]"; break;
        case LogLevel::WARNING: levelStr = "[WARN]"; break;
        case LogLevel::CRITICAL: levelStr = "[CRIT]"; break;
        case LogLevel::DEBUG_TRACE: levelStr = "[TRCE]"; break;
        }

        // 获取当前毫秒级时间
        QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
        // 获取当前线程 ID
        Qt::HANDLE threadId = QThread::currentThreadId();

        // 格式化日志: [时间] [线程ID] [级别] [函数:行号] 内容
        QString logLine = QString("%1 [%2] %3 [%4:%5] %6")
                              .arg(timeStr)
                              .arg((uint64_t)threadId)
                              .arg(levelStr)
                              .arg(func)
                              .arg(line)
                              .arg(message);

        // 加锁写入
        QMutexLocker locker(&m_mutex);

        // 1. 写入文件
        if (m_file.isOpen()) {
            m_stream << logLine << "\n";
            m_stream.flush(); // 立即刷新，防止崩溃时日志丢失
        }

        // 2. 写入控制台 (方便调试)
#ifdef QT_DEBUG
        std::cout << logLine.toStdString() << std::endl;
#endif
    }

private:
    Logger() {}
    ~Logger() {
        if (m_file.isOpen()) m_file.close();
    }
    Q_DISABLE_COPY(Logger)

    QFile m_file;
    QTextStream m_stream;

    // 【核心修复】将 QMutex 改为 QRecursiveMutex
    // 允许同一线程多次加锁，解决 init() -> log() 的死锁问题
    QRecursiveMutex m_mutex;
};

// 宏定义方便调用
#define LOG_INFO(msg) Logger::instance().log(LogLevel::INFO, msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_WARN(msg) Logger::instance().log(LogLevel::WARNING, msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_ERROR(msg) Logger::instance().log(LogLevel::CRITICAL, msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_TRACE(msg) Logger::instance().log(LogLevel::DEBUG_TRACE, msg, __FILE__, __LINE__, __FUNCTION__)

#endif // LOGGER_H
