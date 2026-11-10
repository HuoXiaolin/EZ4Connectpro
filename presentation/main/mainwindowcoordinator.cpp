#include "mainwindowcoordinator.h"

#include <QFileInfo>
#include <QSettings>
#include <QTcpServer>

#include <memory>

#include "application/connectionsession.h"
#include "application/profileservice.h"
#include "application/systemproxysession.h"
#include "infrastructure/coreprocess/coreexecutable.h"
#include "infrastructure/coreprocess/zjuconnectprocess.h"
#include "infrastructure/platform/platformsystemproxybackend.h"
#include "infrastructure/platform/platformtunreservation.h"
#include "infrastructure/platform/privileges.h"
#include "infrastructure/settings/profilemanager.h"
#include "infrastructure/update/updatechecker.h"
#include "presentation/coordinators/authdialogcoordinator.h"

namespace
{
QString connectionKey(const QString &path)
{
    const QFileInfo file(path);
    QString key = file.canonicalFilePath();
    if (key.isEmpty())
    {
        key = file.absoluteFilePath();
    }
#if defined(Q_OS_WIN)
    key = key.toCaseFolded();
#endif
    return key;
}
}

MainWindowCoordinator::MainWindowCoordinator(QWidget *parentWidget, const QString &overrideConfigPath, QObject *parent)
    : QObject(parent),
      parentWidget(parentWidget),
      profileService(new ProfileService(std::make_unique<ProfileManager>(), overrideConfigPath, this)),
      systemProxySession(new SystemProxySession(std::make_unique<PlatformSystemProxyBackend>(), this)),
      updateChecker(new UpdateChecker(this))
{
}

ProfileService *MainWindowCoordinator::profiles() const
{
    return profileService;
}

ProfileConnection *MainWindowCoordinator::connectionFor(const QString &settingsPath) const
{
    return connections.value(connectionKey(settingsPath), nullptr);
}

ProfileConnection *MainWindowCoordinator::createConnection(const VpnProfile &profile)
{
    const QString key = connectionKey(profile.settingsPath);
    if (auto *existing = connections.value(key, nullptr))
    {
        return existing;
    }

    auto *runtime = new ProfileConnection(this);
    runtime->profileId = profile.id;
    runtime->settings = new QSettings(profile.settingsPath, QSettings::IniFormat, runtime);
    runtime->session = new ConnectionSession(new ZjuConnectProcess(), runtime);
    runtime->auth = new AuthDialogCoordinator(parentWidget, runtime->settings, runtime);
    connections.insert(key, runtime);

    auto *session = runtime->session;
    auto *auth = runtime->auth;
    connect(session, &ConnectionSession::askSudoPass, auth, &AuthDialogCoordinator::requestSudoPassword);
    connect(session, &ConnectionSession::graphCaptcha, auth, &AuthDialogCoordinator::requestGraphCaptcha);
    connect(session, &ConnectionSession::smsCode, auth, &AuthDialogCoordinator::requestSmsCode);
    connect(session, &ConnectionSession::totpCode, auth, &AuthDialogCoordinator::requestTotpCode);
    connect(session, &ConnectionSession::randCode, auth, [auth]() { auth->requestSmsCode(false); });
    connect(session, &ConnectionSession::radiusCode, auth, &AuthDialogCoordinator::requestRadiusCode);
    connect(session, &ConnectionSession::ssoAuth, auth, &AuthDialogCoordinator::requestSsoLogin);
    connect(auth, &AuthDialogCoordinator::sudoPasswordSubmitted, session, &ConnectionSession::submitSudoPassword);
    connect(auth, &AuthDialogCoordinator::interactiveInputSubmitted, session, &ConnectionSession::submitInput);
    connect(auth, &AuthDialogCoordinator::interactiveInputCancelled, session, &ConnectionSession::cancelInteractiveInput);
    connect(session, &ConnectionSession::stateChanged, runtime, [this, runtime, auth](ConnectionState state)
    {
        if (state != ConnectionState::Running)
        {
            systemProxySession->releaseFor(runtime->settings->fileName());
        }
        if (state == ConnectionState::Stopping || state == ConnectionState::Reconnecting)
        {
            auth->dismissSsoLogin();
        }
    });
    connect(session, &ConnectionSession::finished, auth, [auth](ZJU_ERROR)
    {
        auth->dismissSsoLogin();
    });
    return runtime;
}

bool MainWindowCoordinator::hasActiveConnections() const
{
    for (const auto *runtime : connections)
    {
        if (runtime->session->isActive())
        {
            return true;
        }
    }
    return false;
}

void MainWindowCoordinator::forgetConnection(const QString &settingsPath)
{
    const QString key = connectionKey(settingsPath);
    const auto *runtime = connections.value(key, nullptr);
    if (runtime && !runtime->session->isActive())
    {
        delete connections.take(key);
    }
}

ProfileNetworkPlan MainWindowCoordinator::reserveNetwork(ProfileConnection *requested)
{
    const VpnProfile profile = readVpnProfile(*requested->settings, requested->profileId);
    ProfileNetworkPlan plan{preferredIntranetRouting(*requested->settings), {}, {}};
    const bool usesTun = plan.routing != IntranetRouting::LocalProxy;
    const bool advancedNetwork = advancedNetworkOptionsEnabled(*requested->settings);
    if (usesTun && advancedNetwork
        && requested->settings->value("ZJUConnect/TCPTunnelMode", false).toBool())
    {
        plan.blockedReason = "TUN 模式与 TCP 隧道模式同时开启。请在高级设置关闭其中一项。";
        return plan;
    }
    if (usesTun && CoreExecutable::supportsIndependentTunnels())
    {
        plan.tunAdapterId = PlatformTunReservation::profileAdapterId(profile.settingsPath);
    }
    const bool interceptsDns = usesTun && requested->settings->value("ZJUConnect/DNSHijack", false).toBool();
    bool otherConnectionActive = false;
    for (const auto *runtime : connections)
    {
        if (runtime == requested || !runtime->session->isActive())
        {
            continue;
        }
        otherConnectionActive = true;
        if (runtime->profileId == requested->profileId)
        {
            plan.blockedReason = "这个配置标识已在运行。请先断开原连接。";
            return plan;
        }
        const QString name = readVpnProfile(*runtime->settings, runtime->profileId).name;
        if (usesTun && plan.tunAdapterId.isEmpty() && runtime->routing != IntranetRouting::LocalProxy)
        {
            plan.blockedReason = "「" + name + "」正在使用虚拟网卡。请先断开该连接，或关闭当前配置的 TUN 开关。";
            return plan;
        }
        if (interceptsDns && runtime->interceptsDns)
        {
            plan.blockedReason = "「" + name + "」已启用 DNS 劫持。请在当前配置关闭 DNS 劫持，或先断开该连接。";
            return plan;
        }
        if (profile.socksPort == runtime->socksPort || profile.socksPort == runtime->httpPort
            || profile.httpPort == runtime->socksPort || profile.httpPort == runtime->httpPort)
        {
            plan.blockedReason = "本地端口与「" + name + "」重叠。请在当前配置的高级设置中改用其他端口。";
            return plan;
        }
    }
#if defined(Q_OS_WIN)
    if (usesTun && otherConnectionActive && !Privileges::isElevated())
    {
        plan.blockedReason = "建立内网路由需要管理员权限。请先断开当前会话，再重新连接。";
        return plan;
    }
#else
    Q_UNUSED(otherConnectionActive)
#endif
    if (usesTun)
    {
        plan.blockedReason = PlatformTunReservation::conflictReason(plan.tunAdapterId);
        if (!plan.blockedReason.isEmpty())
        {
            return plan;
        }
    }
    const QHostAddress bind = advancedNetwork && requested->settings->value("ZJUConnect/OutsideAccess", false).toBool()
        ? QHostAddress::AnyIPv6 : QHostAddress::LocalHost;
    QTcpServer socksProbe;
    QTcpServer httpProbe;
    if (profile.socksPort < 1 || profile.socksPort > 65535 || profile.httpPort < 1 || profile.httpPort > 65535
        || profile.socksPort == profile.httpPort
        || !socksProbe.listen(bind, static_cast<quint16>(profile.socksPort))
        || !httpProbe.listen(bind, static_cast<quint16>(profile.httpPort)))
    {
        plan.blockedReason = "请在高级设置中选择两个不同且空闲的 SOCKS5 和 HTTP 端口。";
        return plan;
    }
    requested->routing = plan.routing;
    requested->interceptsDns = interceptsDns;
    requested->socksPort = profile.socksPort;
    requested->httpPort = profile.httpPort;
    return plan;
}

void MainWindowCoordinator::stopConnections()
{
    for (auto *runtime : connections)
    {
        if (runtime->session->isActive())
        {
            runtime->session->stop();
        }
    }
}

SystemProxySession *MainWindowCoordinator::systemProxy() const
{
    return systemProxySession;
}

UpdateChecker *MainWindowCoordinator::updates() const
{
    return updateChecker;
}

void MainWindowCoordinator::prepareForShutdown()
{
    for (auto *runtime : connections)
    {
        runtime->auth->dismissSsoLogin();
    }
    systemProxySession->clearBeforeShutdown();
}
