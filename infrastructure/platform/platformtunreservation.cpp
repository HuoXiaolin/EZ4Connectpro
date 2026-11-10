#include "platformtunreservation.h"

#include <QFileInfo>
#include <QUuid>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <cfgmgr32.h>

namespace
{
QString leaseName(const QString &adapterId)
{
    const QString base = "Global\\EZ4Connectpro-ZjuConnectTun";
    return adapterId.isEmpty() ? base : base + '-' + adapterId;
}

QString occupiedReason(const QString &adapterId)
{
    return adapterId.isEmpty()
        ? QStringLiteral("已有 EZ4Connectpro 会话正在使用 TUN。请先断开该连接。")
        : QStringLiteral("已有连接正在使用此配置的 TUN 网卡。请先断开该连接。");
}

QString adapterConflict(const QString &adapterId)
{
    const QString guid = adapterId.isEmpty() ? QStringLiteral("{4F5CDE94-D2A3-4AA5-A4A3-0FE6CB909E83}") : adapterId;
    std::wstring instanceId = (QStringLiteral("SWD\\WINTUN\\") + guid).toStdWString();
    DEVINST device{};
    if (CM_Locate_DevNodeW(&device, instanceId.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
    {
        return adapterId.isEmpty()
            ? QStringLiteral("检测到已有的 ZJU Connect 虚拟网卡。请先断开占用它的连接，再启动 TUN。")
            : QStringLiteral("此配置的虚拟网卡已经存在。请先断开占用它的连接，再启动 TUN。");
    }
    return {};
}
}
#endif

PlatformTunReservation::~PlatformTunReservation()
{
    release();
}

QString PlatformTunReservation::profileAdapterId(const QString &settingsPath)
{
    const QFileInfo file(settingsPath);
    const QString canonical = file.canonicalFilePath();
    const QString key = canonical.isEmpty() ? file.absoluteFilePath() : canonical;
    const QUuid nameSpace("{19308d2f-0fb1-49b9-8249-18d6a47d7250}");
    return QUuid::createUuidV5(nameSpace, key.toCaseFolded().toUtf8()).toString();
}

QString PlatformTunReservation::conflictReason(const QString &adapterId)
{
#if defined(Q_OS_WIN)
    const auto name = leaseName(adapterId).toStdWString();
    if (HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str()))
    {
        CloseHandle(existing);
        return occupiedReason(adapterId);
    }
    if (GetLastError() == ERROR_ACCESS_DENIED)
    {
        return occupiedReason(adapterId);
    }
    return adapterConflict(adapterId);
#else
    Q_UNUSED(adapterId)
    return {};
#endif
}

QString PlatformTunReservation::acquire(const QString &adapterId)
{
#if defined(Q_OS_WIN)
    const auto name = leaseName(adapterId).toStdWString();
    SetLastError(ERROR_SUCCESS);
    HANDLE candidate = CreateMutexW(nullptr, FALSE, name.c_str());
    const DWORD status = GetLastError();
    if (!candidate)
    {
        return status == ERROR_ACCESS_DENIED ? occupiedReason(adapterId)
            : "无法检查 TUN 占用（Windows 错误 " + QString::number(status) + "）。";
    }
    if (status == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(candidate);
        return occupiedReason(adapterId);
    }
    const QString reason = adapterConflict(adapterId);
    if (!reason.isEmpty())
    {
        CloseHandle(candidate);
        return reason;
    }
    lease = candidate;
#else
    Q_UNUSED(adapterId)
#endif
    return {};
}

void PlatformTunReservation::release()
{
#if defined(Q_OS_WIN)
    if (lease)
    {
        CloseHandle(lease);
        lease = nullptr;
    }
#endif
}
