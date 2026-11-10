#include "privileges.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QByteArray>

#include "application/commandlineoptions.h"
#include "infrastructure/storage/applicationpaths.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <shellapi.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

bool Privileges::isElevated()
{
#if defined(Q_OS_WIN)
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup
        ))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
#elif defined(Q_OS_UNIX)
    return geteuid() == 0;
#else
    return false;
#endif
}

bool Privileges::relaunchElevated(const QString &activeConfigPath, bool silent)
{
#if defined(Q_OS_WIN)
    const QString program = ApplicationPaths::launcherExecutable();
    const QStringList currentArguments = QCoreApplication::arguments();
    QStringList arguments;
    const QString profiles = CommandLineOptions::value(currentArguments, "--profiles-dir");
    if (!profiles.isEmpty())
    {
        arguments << "--profiles-dir" << QFileInfo(profiles).absoluteFilePath();
    }
    const QString overridePath = CommandLineOptions::value(currentArguments, "--config-path");
    if (!overridePath.isEmpty() && QFileInfo(overridePath) == QFileInfo(activeConfigPath))
    {
        arguments << "--config-path" << QFileInfo(activeConfigPath).absoluteFilePath();
    }
    if (silent)
    {
        arguments << "--silent";
    }
    arguments << "--connect" << "--wait-for-pid" << QString::number(QCoreApplication::applicationPid());
    QStringList quotedArguments;
    for (const QString &argument : arguments)
    {
        quotedArguments << (argument.contains(' ') ? "\"" + argument + "\"" : argument);
    }
    const QString joinedArguments = quotedArguments.join(' ');
    const HINSTANCE result = ShellExecuteW(
        nullptr,
        L"runas",
        reinterpret_cast<LPCWSTR>(program.utf16()),
        reinterpret_cast<LPCWSTR>(joinedArguments.utf16()),
        nullptr,
        SW_SHOWNORMAL
    );
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
    Q_UNUSED(activeConfigPath)
    Q_UNUSED(silent)
    return false;
#endif
}

void Privileges::waitForPreviousInstance(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    if (argc < 3 || QByteArrayView(argv[argc - 2]) != "--wait-for-pid")
    {
        return;
    }
    bool valid = false;
    const DWORD pid = QByteArrayView(argv[argc - 1]).toUInt(&valid);
    if (valid && pid != GetCurrentProcessId())
    {
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid))
        {
            WaitForSingleObject(process, 10000);
            CloseHandle(process);
        }
    }
#else
    Q_UNUSED(argc)
    Q_UNUSED(argv)
#endif
}
