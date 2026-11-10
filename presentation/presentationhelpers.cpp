#include "presentationhelpers.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyleHints>
#include <QSysInfo>

#include "application/applicationconstants.h"

void PresentationHelpers::retainSizeWhenHidden(QWidget *widget)
{
    QSizePolicy policy = widget->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    widget->setSizePolicy(policy);
}

void PresentationHelpers::showAboutDialog(QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setObjectName("aboutDialog");
    dialog.setWindowTitle("关于");
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->setSpacing(12);
    auto *icon = new QLabel(&dialog);
    icon->setPixmap(QIcon(":/resource/icon.png").pixmap(QSize(36, 36), dialog.devicePixelRatioF()));
    auto *title = new QLabel(QApplication::applicationDisplayName(), &dialog);
    title->setObjectName("aboutTitleLabel");
    header->addWidget(icon);
    header->addWidget(title, 1);
    layout->addLayout(header);

    auto *description = new QLabel(
        "把常用的 aTrust 和 EasyConnect 配置集中在一个工作台，按需同时连接多个内网。", &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addSpacing(4);

    const QColor linkColor = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
        ? QColor("#a7e0f7") : QColor("#145f7d");
    const auto linksLabel = [&dialog, &linkColor](const QString &html)
    {
        auto *label = new QLabel(html, &dialog);
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        label->setOpenExternalLinks(true);
        QPalette palette = label->palette();
        palette.setColor(QPalette::Link, linkColor);
        label->setPalette(palette);
        return label;
    };

    QString architecture = QSysInfo::buildCpuArchitecture();
    if (architecture == "x86_64")
    {
        architecture = "x64";
    }
    auto *details = new QFormLayout;
    details->setHorizontalSpacing(18);
    details->setVerticalSpacing(6);
    details->addRow("版本", new QLabel(QApplication::applicationVersion(), &dialog));
    details->addRow("运行平台", new QLabel(QSysInfo::prettyProductName() + " · " + architecture, &dialog));
    details->addRow("Pro 版作者", new QLabel(QStringLiteral(PROJ_AUTHOR), &dialog));
    details->addRow("项目主页", linksLabel(QStringLiteral("<a href='%1'>github.com/%2</a>")
        .arg(ApplicationConstants::ProjectUrl, ApplicationConstants::RepositoryName)));
    details->addRow("开源许可", linksLabel("<a href='https://www.gnu.org/licenses/gpl-3.0.html'>GPL-3.0</a>"));
    layout->addLayout(details);
    layout->addSpacing(6);

    auto *creditsTitle = new QLabel("上游与致谢", &dialog);
    creditsTitle->setObjectName("aboutCreditsLabel");
    layout->addWidget(creditsTitle);
    layout->addWidget(linksLabel(
        "<a href='https://github.com/chenx-dust/EZ4Connect'>EZ4Connect</a> · Chenx Dust<br>"
        "<a href='https://github.com/Mythologyli/ZJU-Connect-for-Windows'>ZJU-Connect-for-Windows</a>"
        "、<a href='https://github.com/Mythologyli/zju-connect'>zju-connect</a> · Myth<br>"
        "<a href='https://github.com/lyc8503/EasierConnect'>EasierConnect</a> · lyc8503"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    buttons->button(QDialogButtonBox::Close)->setObjectName("aboutCloseButton");
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(470, dialog.sizeHint().height());
    dialog.exec();
}

bool PresentationHelpers::confirmCredentials(
    const QString &username,
    const QString &password
)
{
    if (username.isEmpty() || password.isEmpty())
    {
        return QMessageBox::warning(
            nullptr,
            "警告",
            "账号或密码为空！\n\n是否继续？",
            QMessageBox::Ok,
            QMessageBox::Cancel
        ) == QMessageBox::Ok;
    }

    const auto containsNonAscii = [](const QString &value)
    {
        for (const QChar character : value)
        {
            if (character.unicode() > 127)
            {
                return true;
            }
        }
        return false;
    };
    if (containsNonAscii(username) || containsNonAscii(password))
    {
        return QMessageBox::warning(
            nullptr,
            "警告",
            "账号或密码存在非 ASCII 字符！\n建议检查输入法设置。\n\n是否继续？",
            QMessageBox::Ok,
            QMessageBox::Cancel
        ) == QMessageBox::Ok;
    }
    return true;
}
