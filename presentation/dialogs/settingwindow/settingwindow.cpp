#include <QFileInfo>
#include <QFileDialog>
#include <QDesktopServices>
#include <QMessageBox>
#include <QHostAddress>
#include <QStandardPaths>

#include "settingwindow.h"

#include "application/vpnprofile.h"
#include "ui_settingwindow.h"
#include "application/applicationconstants.h"
#include "application/defaultsettings.h"
#include "infrastructure/storage/applicationpaths.h"
#include "presentation/presentationhelpers.h"

SettingWindow::SettingWindow(QWidget *parent, QSettings *inputSettings, const QString &profileId) :
    QDialog(parent),
    ui(new Ui::SettingWindow)
{
    ui->setupUi(this);

    this->settings = inputSettings;
    this->profileId = profileId;

    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);

    ui->ssoBrowserModeCombo->addItem("临时浏览器配置", "temporary");
    ui->ssoBrowserModeCombo->addItem("系统默认浏览器", "system");
    loadSettings();
    for (auto *button : {ui->autoIntranetRoutesCheckBox, ui->tunCheckBox, ui->dnsHijackCheckBox})
    {
        connect(button, &QCheckBox::toggled, this, &SettingWindow::updateRoutingControls);
    }
    const auto showBrowserMode = [this]()
    {
        const bool visible = ui->atrustRadioButton->isChecked()
            && (ui->casRadioButton->isChecked() || ui->oauth2RadioButton->isChecked());
        ui->ssoBrowserModeLabel->setVisible(visible);
        ui->ssoBrowserModeCombo->setVisible(visible);
    };
    for (auto *button : {ui->atrustRadioButton, ui->casRadioButton, ui->oauth2RadioButton})
    {
        connect(button, &QRadioButton::toggled, this, showBrowserMode);
    }
    showBrowserMode();

    connect(ui->openLogDirectoryPushButton, &QPushButton::clicked, this, [this]()
    {
        if (!QDesktopServices::openUrl(
                QUrl::fromLocalFile(ApplicationPaths::logDirectory())
            ))
        {
            QMessageBox::warning(this, "日志目录", "无法打开日志目录。");
        }
    });

    connect(ui->portForwardingPushButton, &QPushButton::clicked,
            [&]()
            {
                extraSettingWindow = new ExtraSettingWindow(this);
                extraSettingWindow->setup(tcpPortForwarding, udpPortForwarding, customDNS, customProxyDomain, extraArguments);

                connect(extraSettingWindow, &ExtraSettingWindow::applied, this,
				[&](const QString& tcpForwarding, const QString& udpForwarding, const QString& customDNS_, const QString& customProxyDomain_, const QString& extraArg)
                    {
                        tcpPortForwarding = tcpForwarding;
                        udpPortForwarding = udpForwarding;
						customDNS = customDNS_;
						customProxyDomain = customProxyDomain_;
						extraArguments = extraArg;
                    });

                extraSettingWindow->show();
            });

    connect(ui->buttonBox->button(QDialogButtonBox::Ok), &QPushButton::clicked, [&]() {
        if (shouldCheckCredential() && !PresentationHelpers::confirmCredentials(
                ui->usernameLineEdit->text(),
                ui->passwordLineEdit->text()
            ))
            return;
        if (isAuthSettingChanged())
            ApplicationPaths::clearClientData(this->profileId);
        applySettings();
        accept();
    });

    connect(ui->buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, [&]() {
        if (shouldCheckCredential() && !PresentationHelpers::confirmCredentials(
                ui->usernameLineEdit->text(),
                ui->passwordLineEdit->text()
            ))
            return;
        if (isAuthSettingChanged())
            ApplicationPaths::clearClientData(this->profileId);
        applySettings();
        loadSettings();
    });

    connect(ui->resetDefaultPushButton, &QPushButton::clicked,
        [&]()
        {
            int status = QMessageBox::warning(this, "警告", "将会重置所有设置，是否继续？", QMessageBox::Ok, QMessageBox::Cancel);
            if (status == QMessageBox::Ok)
            {
                settings->clear();
				DefaultSettings::reset(*settings);
				settings->sync();
                loadSettings();
            }
        });

    connect(ui->passwordVisibleCheckBox, &QCheckBox::checkStateChanged,
        [&](Qt::CheckState state)
        {
            ui->passwordLineEdit->setEchoMode(state == Qt::Checked ? QLineEdit::Normal : QLineEdit::Password);
        });

    connect(ui->totpSecretVisibleCheckBox, &QCheckBox::checkStateChanged,
        [&](Qt::CheckState state)
        {
            ui->totpSecretLineEdit->setEchoMode(state == Qt::Checked ? QLineEdit::Normal : QLineEdit::Password);
        });

    connect(ui->certPasswordVisibleCheckBox, &QCheckBox::checkStateChanged,
        [&](Qt::CheckState state)
        {
            ui->certPasswordLineEdit->setEchoMode(state == Qt::Checked ? QLineEdit::Normal : QLineEdit::Password);
        });

    connect(ui->certFileBrowseButton, &QPushButton::clicked,
        [&]()
        {
            QString filename = QFileDialog::getOpenFileName(this, "选择证书文件",
                QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
                "P12 Certificate(*.p12 *.pfx);;All Files(*.*)");
            if (!filename.isEmpty())
            {
                ui->certFileLineEdit->setText(filename);
            }
        });

    connect(ui->authSelectPushButton, &QPushButton::clicked, this, [&]() {
        authInfoWindow = new AuthInfoWindow(this);
        connect(authInfoWindow, &AuthInfoWindow::finishAuthInfo, this,
                [&](const QString &authType, const QString &loginDomain, const QString &loginUrl) {
                    if (authType == "auth/cas")
                    {
                        ui->casRadioButton->setChecked(true);
                        ui->loginUrlLineEdit->setText(loginUrl);
                    }
                    else if (authType == "auth/httpsOauth2")
                    {
                        ui->oauth2RadioButton->setChecked(true);
                        ui->loginUrlLineEdit->setText(loginUrl);
                    }
                    else if (authType == "auth/smsCheckCode")
                    {
                        ui->smsCheckCodeRadioButton->setChecked(true);
                    }
                    else
                    {
                        ui->pswRadioButton->setChecked(true);
                    }
                    ui->loginDomainLineEdit->setText(loginDomain);
        });
        authInfoWindow->fetchAuthInfo(ui->serverAddressLineEdit->text(), ui->serverPortSpinBox->value());
        authInfoWindow->show();
    });
}

SettingWindow::~SettingWindow()
{
    delete ui;
}

bool SettingWindow::shouldCheckCredential()
{
    if (ui->atrustRadioButton->isChecked())
        return ui->pswRadioButton->isChecked();
    else
        return !ui->certFileLineEdit->text().isEmpty();
}

void SettingWindow::loadSettings()
{
    ui->configVersionLabel->setText(
        "当前配置文件版本：" + QString::number(settings->value("Common/ConfigVersion").toInt()) +
        "\n程序配置文件版本：" +
        QString::number(ApplicationConstants::ConfigVersion)
    );
    ui->usernameLineEdit->setText(settings->value("Credential/Username").toString());
    ui->passwordLineEdit->setText(
        QByteArray::fromBase64(settings->value("Credential/Password").toString().toUtf8())
    );
    ui->totpSecretLineEdit->setText(settings->value("Credential/TOTPSecret").toString());
    ui->certFileLineEdit->setText(settings->value("Credential/CertFile").toString());
    ui->certPasswordLineEdit->setText(
        QByteArray::fromBase64(settings->value("Credential/CertPassword").toString().toUtf8())
    );
    ui->credentialsAsArgumentsCheckBox->setChecked(
        settings->value("ZJUConnect/CredentialsAsArguments", false).toBool()
    );

    ui->checkUpdateAfterStartCheckBox->setChecked(settings->value("Common/CheckUpdateAfterStart").toBool());
    ui->autoIntranetRoutesCheckBox->setChecked(automaticIntranetRoutesEnabled(*settings));
    ui->autoIntranetRoutesCheckBox->setToolTip("认证后启动虚拟网卡并应用网关下发的内网路由。");
    ui->reconnectTimeSpinBox->setValue(settings->value("Common/ReconnectTime").toInt());
    ui->autoReconnectCheckBox->setChecked(settings->value("Common/AutoReconnect").toBool());
	ui->systemProxyBypassLineEdit->setText(settings->value("Common/SystemProxyBypass").toString());
    const bool advancedNetwork = advancedNetworkOptionsEnabled(*settings);
    ui->serverAddressLineEdit->setText(settings->value("ZJUConnect/ServerAddress").toString());
    ui->serverPortSpinBox->setValue(settings->value("ZJUConnect/ServerPort").toInt());
    ui->dnsLineEdit->setText(settings->value("ZJUConnect/DNS").toString());
    ui->dnsAutoCheckBox->setChecked(settings->value("ZJUConnect/DNSAuto").toBool());
    ui->secondaryDnsLineEdit->setText(settings->value("ZJUConnect/SecondaryDNS").toString());
    ui->localDnsServerLineEdit->setText(
        advancedNetwork ? settings->value("ZJUConnect/LocalDNSServer").toString() : QString{}
    );
    ui->dnsServerBindLineEdit->setText(
        advancedNetwork ? settings->value("ZJUConnect/DNSServerBind").toString() : QString{}
    );
    ui->dnsTTLSpinBox->setValue(settings->value("ZJUConnect/DNSTTL").toInt());
    const auto profile = readVpnProfile(*settings, profileId);
    ui->socks5PortSpinBox->setValue(profile.socksPort);
    ui->httpPortSpinBox->setValue(profile.httpPort);
    ui->shadowsocksUrlLineEdit->setText(advancedNetwork ? settings->value("ZJUConnect/ShadowsocksURL").toString() : QString{});
    ui->dialDirectProxyLineEdit->setText(advancedNetwork ? settings->value("ZJUConnect/DialDirectProxy").toString() : QString{});
    ui->updateBestNodesIntervalSpinBox->setValue(
        settings->value("ZJUConnect/UpdateBestNodesInterval", 300).toInt());

    if (profile.protocolId == "atrust")
        ui->atrustRadioButton->setChecked(true);
    else
        ui->easyconnectRadioButton->setChecked(true);
    ui->loginDomainLineEdit->setText(settings->value("ZJUConnect/LoginDomain").toString());
    auto authType = settings->value("ZJUConnect/AuthType").toString();
    if (authType == "smsCheckCode")
        ui->smsCheckCodeRadioButton->setChecked(true);
    else if (authType == "cas")
        ui->casRadioButton->setChecked(true);
    else if (authType == "httpsOauth2")
        ui->oauth2RadioButton->setChecked(true);
    else
        ui->pswRadioButton->setChecked(true);
    ui->loginUrlLineEdit->setText(settings->value("ZJUConnect/LoginURL").toString());
    ui->ssoBrowserModeCombo->setCurrentIndex(settings->value(ApplicationConstants::SsoBrowserModeKey).toString() == "system" ? 1 : 0);
    ui->countryCodeLineEdit->setText(settings->value("ZJUConnect/PhoneCountryCode").toString());
    ui->phoneNumberLineEdit->setText(settings->value("ZJUConnect/PhoneNumber").toString());

    ui->multiLineCheckBox->setChecked(settings->value("ZJUConnect/MultiLine").toBool());
    ui->keepAliveCheckBox->setChecked(settings->value("ZJUConnect/KeepAlive").toBool());
    ui->keepAliveUrlLineEdit->setText(settings->value("ZJUConnect/KeepAliveURL", "").toString());
    ui->bindInterfaceLineEdit->setText(settings->value("ZJUConnect/BindInterface", "").toString());
    ui->outsideAccessCheckBox->setChecked(advancedNetwork && settings->value("ZJUConnect/OutsideAccess").toBool());

    ui->skipDomainResourceCheckBox->setChecked(settings->value("ZJUConnect/SkipDomainResource").toBool());
    ui->disableServerConfigCheckBox->setChecked(settings->value("ZJUConnect/DisableServerConfig").toBool());
    ui->proxyAllCheckBox->setChecked(settings->value("ZJUConnect/ProxyAll").toBool());
    
    ui->zjuDefaultCheckBox->setChecked(settings->value("ZJUConnect/ZJUDefault").toBool());
    ui->disableDNSCheckBox->setChecked(settings->value("ZJUConnect/DisableZJUDNS").toBool());
    ui->detailedDebugCheckBox->setChecked(settings->value("ZJUConnect/Debug").toBool());
    ui->debugPcapCheckBox->setChecked(
        settings->value("ZJUConnect/DebugPCAP", false).toBool()
    );
    ui->debugTlsLogCheckBox->setChecked(
        settings->value("ZJUConnect/DebugTLSLog", false).toBool()
    );

    ui->tunCheckBox->setChecked(tunEnabled(*settings));
    ui->tunCheckBox->setToolTip("开启后使用虚拟网卡。关闭后只启动本机代理。");
    ui->routeCheckBox->setChecked(settings->value("ZJUConnect/AddRoute").toBool());
    ui->dnsHijackCheckBox->setChecked(settings->value("ZJUConnect/DNSHijack").toBool());
    ui->fakeIPCheckBox->setChecked(settings->value("ZJUConnect/FakeIP").toBool());
    ui->tcpTunnelModeCheckBox->setChecked(advancedNetwork && settings->value("ZJUConnect/TCPTunnelMode").toBool());
    ui->autoDetectInterfaceCheckBox->setChecked(settings->value("ZJUConnect/AutoDetectInterface", false).toBool());

    tcpPortForwarding = advancedNetwork ? settings->value("ZJUConnect/TCPPortForwarding").toString() : QString{};
    udpPortForwarding = advancedNetwork ? settings->value("ZJUConnect/UDPPortForwarding").toString() : QString{};
	customDNS = settings->value("ZJUConnect/CustomDNS").toString();
	customProxyDomain = settings->value("ZJUConnect/CustomProxyDomain").toString();
    extraArguments = advancedNetwork ? settings->value("ZJUConnect/ExtraArguments").toString() : QString{};

    updateRoutingControls();

	ui->dnsLineEdit->setDisabled(ui->dnsAutoCheckBox->isChecked());
}

void SettingWindow::updateRoutingControls()
{
    const bool hasTun = ui->tunCheckBox->isChecked();
    ui->autoIntranetRoutesCheckBox->setEnabled(hasTun);
    ui->routeCheckBox->setEnabled(hasTun && !ui->autoIntranetRoutesCheckBox->isChecked());
    ui->dnsHijackCheckBox->setEnabled(hasTun);
    ui->fakeIPCheckBox->setEnabled(hasTun && ui->dnsHijackCheckBox->isChecked());
}

void SettingWindow::applySettings()
{
    settings->setValue("Credential/Username", ui->usernameLineEdit->text());
    settings->setValue("Credential/Password", QString(ui->passwordLineEdit->text().toUtf8().toBase64()));
    settings->setValue("Credential/TOTPSecret", ui->totpSecretLineEdit->text());
    settings->setValue("Credential/CertFile", ui->certFileLineEdit->text());
    settings->setValue("Credential/CertPassword", QString(ui->certPasswordLineEdit->text().toUtf8().toBase64()));
    settings->setValue(
        "ZJUConnect/CredentialsAsArguments",
        ui->credentialsAsArgumentsCheckBox->isChecked()
    );

    settings->setValue("Common/CheckUpdateAfterStart", ui->checkUpdateAfterStartCheckBox->isChecked());
    settings->setValue("Common/AutoIntranetRoutes", ui->autoIntranetRoutesCheckBox->isChecked());
    settings->remove("Common/AutoBrowserProxy");
    settings->remove("Common/AutoSetProxy");
    settings->remove("Common/ProxyMode");
    settings->setValue("Common/ReconnectTime", ui->reconnectTimeSpinBox->value());
    settings->setValue("Common/AutoReconnect", ui->autoReconnectCheckBox->isChecked());
    settings->setValue("Common/SystemProxyBypass", ui->systemProxyBypassLineEdit->text());


    settings->setValue("ZJUConnect/ServerAddress", ui->serverAddressLineEdit->text());
    settings->setValue("ZJUConnect/ServerPort", ui->serverPortSpinBox->value());
    settings->setValue("ZJUConnect/DNS", ui->dnsLineEdit->text());
    settings->setValue("ZJUConnect/DNSAuto", ui->dnsAutoCheckBox->isChecked());
    settings->setValue("ZJUConnect/SecondaryDNS", ui->secondaryDnsLineEdit->text());
    settings->setValue(
        "ZJUConnect/LocalDNSServer",
        ui->localDnsServerLineEdit->text().trimmed()
    );
    settings->setValue(
        "ZJUConnect/DNSServerBind",
        ui->dnsServerBindLineEdit->text().trimmed()
    );
    settings->setValue("ZJUConnect/DNSTTL", ui->dnsTTLSpinBox->value());
    settings->setValue("ZJUConnect/SOCKS5Port", ui->socks5PortSpinBox->value());
    settings->setValue("ZJUConnect/HTTPPort", ui->httpPortSpinBox->value());
    settings->setValue("ZJUConnect/ShadowsocksURL", ui->shadowsocksUrlLineEdit->text());
    settings->setValue("ZJUConnect/DialDirectProxy", ui->dialDirectProxyLineEdit->text());
    settings->setValue("ZJUConnect/UpdateBestNodesInterval", ui->updateBestNodesIntervalSpinBox->value());

    settings->setValue("ZJUConnect/Protocol", ui->atrustRadioButton->isChecked() ? "atrust" : "easyconnect");
    settings->setValue(
        "ZJUConnect/EasyConnectAuthType",
        ui->certFileLineEdit->text().isEmpty() ? "password" : "certificate"
    );
    settings->setValue("ZJUConnect/LoginDomain", ui->loginDomainLineEdit->text());
    QString authType;
    if (ui->smsCheckCodeRadioButton->isChecked())
        authType = "smsCheckCode";
    else if (ui->casRadioButton->isChecked())
        authType = "cas";
    else if (ui->oauth2RadioButton->isChecked())
        authType = "httpsOauth2";
    else
        authType = "psw";
    settings->setValue("ZJUConnect/AuthType", authType);
    settings->setValue("ZJUConnect/LoginURL", ui->loginUrlLineEdit->text());
    settings->setValue(ApplicationConstants::SsoBrowserModeKey, ui->ssoBrowserModeCombo->currentData().toString());
    settings->setValue("ZJUConnect/PhoneCountryCode", ui->countryCodeLineEdit->text());
    settings->setValue("ZJUConnect/PhoneNumber", ui->phoneNumberLineEdit->text());

    settings->setValue("ZJUConnect/MultiLine", ui->multiLineCheckBox->isChecked());
    settings->setValue("ZJUConnect/KeepAlive", ui->keepAliveCheckBox->isChecked());
    settings->setValue("ZJUConnect/KeepAliveURL", ui->keepAliveUrlLineEdit->text().trimmed());
    settings->setValue("ZJUConnect/BindInterface", ui->bindInterfaceLineEdit->text().trimmed());
    settings->setValue("ZJUConnect/OutsideAccess", ui->outsideAccessCheckBox->isChecked());

    settings->setValue("ZJUConnect/SkipDomainResource", ui->skipDomainResourceCheckBox->isChecked());
    settings->setValue("ZJUConnect/DisableServerConfig", ui->disableServerConfigCheckBox->isChecked());
    settings->setValue("ZJUConnect/ProxyAll", ui->proxyAllCheckBox->isChecked());

    settings->setValue("ZJUConnect/DisableZJUDNS", ui->disableDNSCheckBox->isChecked());
    settings->setValue("ZJUConnect/ZJUDefault", ui->zjuDefaultCheckBox->isChecked());
    settings->setValue("ZJUConnect/Debug", ui->detailedDebugCheckBox->isChecked());
    settings->setValue("ZJUConnect/DebugPCAP", ui->debugPcapCheckBox->isChecked());
    settings->setValue("ZJUConnect/DebugTLSLog", ui->debugTlsLogCheckBox->isChecked());

    settings->setValue("ZJUConnect/TunEnabled", ui->tunCheckBox->isChecked());
    settings->remove("ZJUConnect/EnableTun");
    settings->remove("ZJUConnect/TUNMode");
    settings->setValue("ZJUConnect/AddRoute", ui->routeCheckBox->isChecked());
    settings->setValue("ZJUConnect/DNSHijack", ui->dnsHijackCheckBox->isChecked());
    settings->setValue("ZJUConnect/FakeIP", ui->fakeIPCheckBox->isChecked());
    settings->setValue("ZJUConnect/TCPTunnelMode", ui->tcpTunnelModeCheckBox->isChecked());
    settings->setValue("ZJUConnect/AutoDetectInterface", ui->autoDetectInterfaceCheckBox->isChecked());


    settings->setValue("ZJUConnect/TCPPortForwarding", tcpPortForwarding);
    settings->setValue("ZJUConnect/UDPPortForwarding", udpPortForwarding);
    settings->setValue("ZJUConnect/CustomDNS", customDNS);
    settings->setValue("ZJUConnect/CustomProxyDomain", customProxyDomain);
    settings->setValue("ZJUConnect/ExtraArguments", extraArguments);

    settings->setValue(
        "Common/ConfigVersion",
        ApplicationConstants::ConfigVersion
    );

    settings->sync();
}

bool SettingWindow::isAuthSettingChanged()
{
    if (ui->atrustRadioButton->isChecked() == false &&
        settings->value("ZJUConnect/Protocol").toString() != "atrust")
        return false;
    if (ui->atrustRadioButton->isChecked() == true &&
        settings->value("ZJUConnect/Protocol").toString() != "atrust")
        return true;
    QString currentAuthType;
    if (ui->casRadioButton->isChecked())
        currentAuthType = "cas";
    else if (ui->oauth2RadioButton->isChecked())
        currentAuthType = "httpsOauth2";
    else if (ui->smsCheckCodeRadioButton->isChecked())
        currentAuthType = "smsCheckCode";
    else
        currentAuthType = "psw";

    return currentAuthType != settings->value("ZJUConnect/AuthType").toString() ||
           ui->loginDomainLineEdit->text() != settings->value("ZJUConnect/LoginDomain").toString() ||
           ((currentAuthType == "cas" || currentAuthType == "httpsOauth2") &&
            ui->loginUrlLineEdit->text() != settings->value("ZJUConnect/LoginURL").toString()) ||
           ui->serverAddressLineEdit->text() != settings->value("ZJUConnect/ServerAddress").toString() ||
           ui->serverPortSpinBox->value() != settings->value("ZJUConnect/ServerPort").toInt();
}
