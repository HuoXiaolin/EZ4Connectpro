#include "vpnprofiledialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

VpnProfileDialog::VpnProfileDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("新建连接配置");
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 22, 22, 20);
    layout->setSpacing(14);
    auto *title = new QLabel("新建深信服连接", this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);
    auto *form = new QFormLayout;
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);

    nameEdit = new QLineEdit(this);
    nameEdit->setObjectName("vpnProfileNameEdit");
    nameEdit->setMaxLength(60);
    nameEdit->setPlaceholderText("例如 办公 VPN");
    protocolCombo = new QComboBox(this);
    protocolCombo->setObjectName("vpnProtocolCombo");
    const auto &protocols = vpnProtocolNames();
    for (auto it = protocols.cbegin(); it != protocols.cend(); ++it)
    {
        protocolCombo->addItem(it.value(), it.key());
    }
    serverEdit = new QLineEdit(this);
    serverEdit->setObjectName("vpnGatewayEdit");
    serverEdit->setPlaceholderText("域名或 IP 地址");
    portEdit = new QSpinBox(this);
    portEdit->setRange(1, 65535);
    portEdit->setValue(443);
    form->addRow("配置名称", nameEdit);
    form->addRow("协议", protocolCombo);
    form->addRow("网关", serverEdit);
    form->addRow("端口", portEdit);

    help = new QLabel("创建后可在编辑配置中选择认证方式。Windows 版连接后自动建立内网路由。", this);
    help->setWordWrap(true);
    help->setTextFormat(Qt::PlainText);
    layout->addWidget(help);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText("创建配置");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &VpnProfileDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    nameEdit->setFocus();
}

VpnProfile VpnProfileDialog::profile() const
{
    return editedProfile;
}

void VpnProfileDialog::validateAndAccept()
{
    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty())
    {
        nameEdit->setFocus();
        help->setText("请填写配置名称。");
        return;
    }
    const QUrl address = QUrl::fromUserInput(serverEdit->text().trimmed());
    if (!address.isValid() || address.host().isEmpty() || address.hasQuery()
        || address.hasFragment() || !address.userInfo().isEmpty()
        || (!address.path().isEmpty() && address.path() != "/"))
    {
        serverEdit->setFocus();
        help->setText("请填写 VPN 网关的域名或 IP 地址。");
        return;
    }
    editedProfile.name = name;
    editedProfile.protocolId = protocolCombo->currentData().toString();
    editedProfile.server = address.host();
    editedProfile.port = address.port(portEdit->value());
    accept();
}
