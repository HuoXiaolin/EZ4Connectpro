#include "connectionuicontroller.h"

#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QSettings>
#include <QWidget>

#include <utility>

#include "application/applicationlogger.h"
#include "application/connectionsession.h"
#include "application/systemproxysession.h"
#include "application/vpnprofile.h"
#include "infrastructure/coreprocess/coreexecutable.h"
#include "infrastructure/platform/privileges.h"
#include "infrastructure/settings/settingsprofileloader.h"
#include "presentation/coordinators/authdialogcoordinator.h"

ConnectionUiController::ConnectionUiController(
    QWidget *parentWidget,
    ConnectionSession *connectionSession,
    SystemProxySession *systemProxySession,
    AuthDialogCoordinator *authenticationDialogs,
    ApplicationLogger *applicationLogger,
    SettingsProvider settingsProvider,
    ProfileIdProvider profileIdProvider,
    StartPermission startPermission,
    NotificationHandler notificationHandler,
    QObject *parent
)
    : QObject(parent),
      parentWidget(parentWidget),
      connectionSession(connectionSession),
      systemProxySession(systemProxySession),
      authenticationDialogs(authenticationDialogs),
      settingsProvider(std::move(settingsProvider)),
      profileIdProvider(std::move(profileIdProvider)),
      startPermission(std::move(startPermission)),
      notificationHandler(std::move(notificationHandler))
{
    connect(
        connectionSession,
        &ConnectionSession::outputRead,
        applicationLogger,
        [this, applicationLogger](const QString &output)
        {
            const QString prefix = "[" + readVpnProfile(*settings(), this->profileIdProvider()).name.simplified() + "] ";
            applicationLogger->appendCoreOutput(prefix + QString(output).replace('\n', '\n' + prefix));
        }
    );
    connect(
        connectionSession,
        &ConnectionSession::savedSudoPasswordRejected,
        this,
        []() { qWarning().noquote() << "sudo 密码可能有误，不使用记住的密码"; }
    );
    connect(
        authenticationDialogs,
        &AuthDialogCoordinator::loginSubmitted,
        this,
        [this](const QString &username, const QString &password, bool saveDetails)
        {
            if (saveDetails)
            {
                settings()->setValue("Credential/Username", username);
                settings()->setValue(
                    "Credential/Password",
                    QString(password.toUtf8().toBase64())
                );
                settings()->sync();
            }
            startConnection(username, password);
        }
    );
    connect(
        authenticationDialogs,
        &AuthDialogCoordinator::phoneNumberSubmitted,
        this,
        [this](
            const QString &countryCode,
            const QString &phoneNumber,
            bool saveDetails
        )
        {
            if (saveDetails)
            {
                settings()->setValue("ZJUConnect/PhoneCountryCode", countryCode);
                settings()->setValue("ZJUConnect/PhoneNumber", phoneNumber);
                settings()->sync();
            }

            const QString username =
                settings()->value("Credential/Username", "").toString();
            const QString password = QByteArray::fromBase64(
                settings()->value("Credential/Password", "").toString().toUtf8()
            );
            startConnection(
                username,
                password,
                countryCode + "-" + phoneNumber
            );
        }
    );
    connect(
        connectionSession,
        &ConnectionSession::reconnectScheduled,
        this,
        [](int) { qInfo().noquote() << "正在尝试重新连接..."; }
    );
    connect(
        connectionSession,
        &ConnectionSession::finished,
        this,
        [this](ZJU_ERROR error)
        {
            qInfo().noquote() << "VPN 断开！";
            if (error != ZJU_ERROR::NONE)
            {
                this->notificationHandler(
                    "VPN",
                    "VPN 意外断开！",
                    QSystemTrayIcon::MessageIcon::Warning
                );
            }
            if (this->connectionSession->state() != ConnectionState::Interrupted)
            {
                showConnectionError(error);
            }
        }
    );
    connect(connectionSession, &ConnectionSession::httpProxyReadyChanged, this, [this](bool ready)
    {
        if (!ready)
        {
            this->systemProxySession->releaseFor(settings()->fileName());
        }
    });
}

QSettings *ConnectionUiController::settings() const
{
    return settingsProvider();
}

void ConnectionUiController::handleConnectClicked()
{
    if (connectionSession->isActive())
    {
        connectionSession->stop();
        return;
    }
    if (settings()->contains("ZJUConnect/ServerAddress") &&
        settings()->value("ZJUConnect/ServerAddress").toString().isEmpty())
    {
        QMessageBox::critical(parentWidget, "错误", "服务器地址不能为空");
        return;
    }

    const QString username = settings()->value("Credential/Username", "").toString();
    const QString password = QByteArray::fromBase64(
        settings()->value("Credential/Password", "").toString().toUtf8()
    );
    const QString protocol = vpnProtocolId(*settings());
    const QString authType =
        settings()->value("ZJUConnect/AuthType", "psw").toString();
    const QString easyconnectAuthType = settings()->value(
        "ZJUConnect/EasyConnectAuthType",
        settings()->value("Credential/CertFile", "").toString().isEmpty()
            ? "password"
            : "certificate"
    ).toString();

    if (protocol == "easyconnect"
        && easyconnectAuthType == "certificate"
        && settings()->value("Credential/CertFile", "").toString().isEmpty())
    {
        QMessageBox::information(
            parentWidget,
            "需要配置证书",
            "当前配置选择了证书认证。\n"
            "请通过“文件 → 配置引导”选择证书文件。"
        );
        return;
    }

    if (!checkStartPermission(startPermission()))
    {
        return;
    }

    const bool passwordLogin =
        (protocol == "atrust" && authType == "psw") ||
        (protocol == "easyconnect" && easyconnectAuthType != "certificate");
    if (protocol == "atrust" && authType == "smsCheckCode")
    {
        QString countryCode = settings()
            ->value("ZJUConnect/PhoneCountryCode", "86")
            .toString()
            .trimmed();
        const QString phoneNumber = settings()
            ->value("ZJUConnect/PhoneNumber", "")
            .toString()
            .trimmed();
        if (countryCode.isEmpty() || phoneNumber.isEmpty())
        {
            if (countryCode.isEmpty())
            {
                countryCode = "86";
            }
            authenticationDialogs->requestPhoneNumber(countryCode, phoneNumber);
            return;
        }
    }
    if (passwordLogin && (username.isEmpty() || password.isEmpty()))
    {
        authenticationDialogs->requestLogin(username, password);
        return;
    }
    startConnection(username, password);
}

bool ConnectionUiController::checkStartPermission(const ProfileNetworkPlan &plan)
{
    if (!plan.blockedReason.isEmpty())
    {
        QMessageBox::information(parentWidget, "无法启动配置", plan.blockedReason);
        return false;
    }
#if defined(Q_OS_WIN)
    if (plan.routing != IntranetRouting::LocalProxy && !Privileges::isElevated())
    {
        if (Privileges::relaunchElevated(settings()->fileName(), !parentWidget->isVisible()))
        {
            QApplication::quit();
        }
        else
        {
            QMessageBox::information(parentWidget, "内网路由需要管理员权限",
                "请在 Windows 授权窗口中允许此应用建立内网路由，然后继续连接。");
        }
        return false;
    }
#endif
    return true;
}

void ConnectionUiController::handleProxyClicked()
{
    if (!connectionSession->isHttpProxyReady() || systemProxySession->isBusy())
    {
        return;
    }
    const QString owner = settings()->fileName();
    if (systemProxySession->isEnabled())
    {
        systemProxySession->releaseFor(owner);
        return;
    }

    const VpnProfile profile = readVpnProfile(*settings(), profileIdProvider());
    const SystemProxyConfig proxyConfig{
        profile.httpPort,
        profile.socksPort,
        settings()->value("Common/SystemProxyBypass").toString()
    };
    if (!systemProxySession->enable(proxyConfig, owner))
    {
        return;
    }
    connect(
        systemProxySession,
        &SystemProxySession::operationFinished,
        this,
        [this, profile](bool enabled)
        {
            if (!connectionSession->isHttpProxyReady())
            {
                return;
            }

            const QString prefix = "[" + profile.name.simplified() + "] ";
            if (enabled)
            {
                qInfo().noquote() << prefix + "浏览器系统代理已启用，HTTP 端口 " + QString::number(profile.httpPort);
                return;
            }

            qInfo().noquote() << prefix + "系统代理未启用。请检查是否已有代理、PAC 或系统策略。";
            QMessageBox::information(parentWidget, "系统代理不可用",
                "请检查是否已有代理、PAC 或系统策略。此连接可通过界面显示的本机代理端口使用。");
        },
        Qt::SingleShotConnection
    );
}

void ConnectionUiController::startConnection(
    const QString &username,
    const QString &password,
    const QString &phone
)
{
    if (connectionSession->isActive())
    {
        return;
    }
    const ProfileNetworkPlan plan = startPermission();
    if (!checkStartPermission(plan))
    {
        return;
    }
    ConnectionProfile profile = SettingsProfileLoader::load(
        *settings(),
        profileIdProvider(),
        username,
        password,
        plan.routing
    );
    profile.tunnel.adapterId = plan.tunAdapterId;
    if (!phone.isNull())
    {
        profile.endpoint.phone = phone;
    }
    profile.program = CoreExecutable::path();
    const ReconnectPolicy reconnectPolicy{
        settings()->value("Common/AutoReconnect", false).toBool(),
        settings()->value("Common/ReconnectTime", 1).toInt() * 1000
    };
    const QString network = profile.tunnel.tunMode ? QStringLiteral("内网路由") : QStringLiteral("本机代理");
    qInfo().noquote() << "启动连接「" + readVpnProfile(*settings(), profileIdProvider()).name.simplified() + "」 · " + network;
    connectionSession->start(profile, reconnectPolicy);
}

void ConnectionUiController::showConnectionError(ZJU_ERROR error)
{
    QString message;
    switch (error)
    {
    case ZJU_ERROR::INVALID_DETAIL:
        message = "登录失败！\n请检查设置中的网络账号和密码是否设置正确。";
        break;
    case ZJU_ERROR::BRUTE_FORCE:
        message = "登录失败！\n登录尝试过于频繁，IP 被风控，请稍后重试或换用 EasyConnect。";
        break;
    case ZJU_ERROR::OTHER_LOGIN_FAILED:
        message = "登录失败！\n未知原因，可将日志反馈给开发者以便调查。";
        break;
    case ZJU_ERROR::ACCESS_DENIED:
        message = "权限不足！\n请关闭程序，点击右键以管理员身份运行。";
        break;
    case ZJU_ERROR::LISTEN_FAILED:
        message = "监听失败！\n请关闭占用端口的程序（如残留的 zju-connect.exe），或者监听其它端口。";
        break;
    case ZJU_ERROR::TUN_SETUP_FAILED:
    case ZJU_ERROR::TUN_CONFLICT:
        message = error == ZJU_ERROR::TUN_CONFLICT ? "内网通道因网络配置冲突未启动。" : "内网通道启动失败。";
        if (const QString reason = connectionSession->errorDetail(); !reason.isEmpty())
        {
            message += "\n\n" + reason.left(500);
        }
        message += "\n\n完整的网卡错误可在活动日志中查看。";
        break;
    case ZJU_ERROR::CLIENT_FAILED:
        message = "连接失败！\n可能是响应超时，请检查本地网络配置是否正常，服务器设置是否正确。";
        break;
    case ZJU_ERROR::CAPTCHA_FAILED:
        message = "登录失败！\n验证码问题，可能是已验证码过期或者有误。";
        break;
    case ZJU_ERROR::PROGRAM_NOT_FOUND:
        message = "程序未找到！\n请检查核心是否在正确路径下，检查是否解压在当前目录下。";
        break;
    case ZJU_ERROR::INTERACTIVE_ERROR:
        message = "登录失败！\n请检查您的输入是否正确，检查是否完成 SSO 登录。";
        break;
    case ZJU_ERROR::AUTH_NOT_AVAILABLE:
        message = "认证方式/登录域不可用！\n请通过“获取认证方式”按钮配置认证方式。";
        break;
    case ZJU_ERROR::AUTH_EXPIRED:
        message = "认证已过期！\n请重新登录。";
        break;
    case ZJU_ERROR::OTHER:
        message = "其它错误！\n未知原因，可将日志反馈给开发者以便调查。";
        break;
    case ZJU_ERROR::NONE:
        return;
    }
    QMessageBox dialog(QMessageBox::Critical, "错误", message, QMessageBox::Ok, parentWidget);
    dialog.setTextFormat(Qt::PlainText);
    dialog.exec();
}
