#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

#include <windows.h>
#include <dbghelp.h>
#include <QString>
#include <QDateTime>
#include "Logger.h"

#pragma comment(lib, "dbghelp.lib")

class CrashHandler {
public:
    static void init() {
        SetUnhandledExceptionFilter(UnhandledExceptionFilter);
    }

private:
    static LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* pExceptionInfo) {
        LOG_ERROR("CRASH DETECTED! Generating MiniDump (Lightweight)...");

        QString fileName = QString("Crash_%1.dmp").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));

        HANDLE hFile = CreateFileW(
            fileName.toStdWString().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
            );

        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mdei;
            mdei.ThreadId = GetCurrentThreadId();
            mdei.ExceptionPointers = pExceptionInfo;
            mdei.ClientPointers = FALSE;

            // 【优化】只生成标准 MiniDump，不包含全内存堆，文件极小
            MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(
                MiniDumpNormal |
                MiniDumpWithHandleData |
                MiniDumpWithIndirectlyReferencedMemory // 只包含栈引用的内存
                );

            BOOL ret = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hFile,
                dumpType,
                &mdei,
                nullptr,
                nullptr
                );

            CloseHandle(hFile);

            if (ret) {
                LOG_ERROR("Minidump saved successfully: " + fileName);
            } else {
                LOG_ERROR("Failed to save Minidump. Error Code: " + QString::number(GetLastError()));
            }
        } else {
            LOG_ERROR("Failed to create dump file.");
        }

        LOG_ERROR("Application Terminating...");
        return EXCEPTION_EXECUTE_HANDLER;
    }
};

#endif // CRASHHANDLER_H