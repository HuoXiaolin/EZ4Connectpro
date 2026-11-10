#include "authdialogcoordinator.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>

#include "application/applicationconstants.h"
#include "infrastructure/browser/browserssosession.h"
#include "infrastructure/browser/temporarybrowserssosession.h"
#include "presentation/dialogs/graphcaptchawindow/graphcaptchawindow.h"
#include "presentation/dialogs/loginwindow/loginwindow.h"
#include "presentation/dialogs/sudowindow/sudowindow.h"

AuthDialogCoordinator::AuthDialogCoordinator(
    QWidget *parentWidget,
    QSettings *settings,
    QObject *parent
)
    : QObject(parent),
      parentWidget(parentWidget),
      settings(settings)
{
}

QString AuthDialogCoordinator::dialogTitle(const QString &title) const
{
    if (!settings)
    {
        return title;
    }
    const QString id = QFileInfo(settings->fileName()).completeBaseName();
    const QString name = settings->value("Profile/DisplayName", id == "config" ? QStringLiteral("默认配置") : id).toString();
    return name.isEmpty() ? title : title + " · " + name;
}

void AuthDialogCoordinator::requestLogin(
    const QString &username,
    const QString &password
)
{
    if (loginWindow != nullptr)
    {
        loginWindow->raise();
        loginWindow->activateWindow();
        return;
    }

    loginWindow = new LoginWindow(parentWidget);
    loginWindow->setWindowTitle(dialogTitle("账户登录"));
    loginWindow->setAttribute(Qt::WA_DeleteOnClose);
    loginWindow->setDetail(username, password);
    connect(loginWindow, &LoginWindow::login, this,
            &AuthDialogCoordinator::loginSubmitted);
    loginWindow->show();
}

void AuthDialogCoordinator::requestPhoneNumber(
    const QString &countryCode,
    const QString &phoneNumber
)
{
    if (phoneNumberDialog != nullptr)
    {
        phoneNumberDialog->raise();
        phoneNumberDialog->activateWindow();
        return;
    }

    auto *dialog = new QDialog(parentWidget);
    phoneNumberDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowTitle(dialogTitle("短信验证"));

    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel("请输入接收验证码的手机号码：", dialog));

    auto *phoneLayout = new QHBoxLayout;
    phoneLayout->addWidget(new QLabel("+", dialog));
    auto *countryCodeEdit = new QLineEdit(countryCode, dialog);
    countryCodeEdit->setObjectName("countryCodeLineEdit");
    countryCodeEdit->setMaximumWidth(60);
    countryCodeEdit->setPlaceholderText("国家码");
    phoneLayout->addWidget(countryCodeEdit);
    phoneLayout->addWidget(new QLabel("-", dialog));
    auto *phoneNumberEdit = new QLineEdit(phoneNumber, dialog);
    phoneNumberEdit->setObjectName("phoneNumberLineEdit");
    phoneNumberEdit->setPlaceholderText("手机号码");
    phoneLayout->addWidget(phoneNumberEdit);
    layout->addLayout(phoneLayout);

    auto *saveCheckBox = new QCheckBox("记住手机号码（可在设置中删除）", dialog);
    layout->addWidget(saveCheckBox);

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        dialog
    );
    connect(
        buttonBox,
        &QDialogButtonBox::accepted,
        dialog,
        [this, dialog, countryCodeEdit, phoneNumberEdit, saveCheckBox]()
        {
            const QString submittedCountryCode = countryCodeEdit->text().trimmed();
            const QString submittedPhoneNumber = phoneNumberEdit->text().trimmed();
            if (submittedCountryCode.isEmpty() || submittedPhoneNumber.isEmpty())
            {
                QMessageBox::warning(
                    dialog,
                    "警告",
                    "国家码或手机号码不能为空。"
                );
                return;
            }

            emit phoneNumberSubmitted(
                submittedCountryCode,
                submittedPhoneNumber,
                saveCheckBox->isChecked()
            );
            dialog->accept();
        }
    );
    connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (phoneNumber.isEmpty())
    {
        phoneNumberEdit->setFocus();
    }
    else
    {
        countryCodeEdit->setFocus();
    }
    dialog->show();
}

void AuthDialogCoordinator::requestSudoPassword()
{
    if (sudoWindow != nullptr)
    {
        sudoWindow->raise();
        sudoWindow->activateWindow();
        return;
    }

    sudoWindow = new SudoWindow(parentWidget);
    sudoWindow->setWindowTitle(dialogTitle("系统权限"));
    sudoWindow->setAttribute(Qt::WA_DeleteOnClose);
    connect(sudoWindow, &SudoWindow::sudo, this,
            &AuthDialogCoordinator::sudoPasswordSubmitted);
    connect(sudoWindow, &QDialog::rejected, this,
            [this]()
            {
                emit sudoPasswordSubmitted({}, false);
            });
    sudoWindow->show();
}

void AuthDialogCoordinator::requestGraphCaptcha(const QString &graphFile)
{
    qInfo().noquote() << "需要图形验证码";
    const bool textInputMode = settings == nullptr
        || settings->value("ZJUConnect/Protocol", "easyconnect").toString() == "easyconnect";
    if (graphCaptchaWindow != nullptr)
    {
        graphCaptchaWindow->setGraph(graphFile, textInputMode);
        graphCaptchaWindow->raise();
        graphCaptchaWindow->activateWindow();
        return;
    }

    graphCaptchaWindow = new GraphCaptchaWindow(parentWidget);
    graphCaptchaWindow->setWindowTitle(dialogTitle("图形验证码"));
    graphCaptchaWindow->setAttribute(Qt::WA_DeleteOnClose);
    graphCaptchaWindow->setGraph(graphFile, textInputMode);
    connect(graphCaptchaWindow, &GraphCaptchaWindow::finishCaptcha, this,
            [this](const QByteArray &captcha)
            {
                qInfo().noquote() << "图形验证码已提交";
                emit interactiveInputSubmitted(captcha + "\n");
            });
    connect(graphCaptchaWindow, &GraphCaptchaWindow::cancelled, this,
            [this]()
            {
                qInfo().noquote() << "图形验证码输入已取消";
                emit interactiveInputCancelled();
            });
    graphCaptchaWindow->show();
}

void AuthDialogCoordinator::requestSmsCode(bool showSkipSecondaryAuthOption)
{
    qInfo().noquote() << "需要短信验证码";

    QDialog dialog(parentWidget);
    dialog.setWindowTitle(dialogTitle("短信验证码"));

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("请输入短信验证码：", &dialog));

    auto *codeEdit = new QLineEdit(&dialog);
    layout->addWidget(codeEdit);

    auto *skipCheckBox = new QCheckBox("跳过以后的短信验证", &dialog);
    skipCheckBox->setVisible(showSkipSecondaryAuthOption);
    layout->addWidget(skipCheckBox);

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog
    );
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (!accepted)
    {
        qInfo().noquote() << "短信验证码输入已取消";
        emit interactiveInputCancelled();
        return;
    }

    QByteArray input;
    input = codeEdit->text().toLocal8Bit();
    if (showSkipSecondaryAuthOption && skipCheckBox->isChecked())
    {
        input.prepend('$');
    }

    qInfo().noquote() << "短信验证码已提交";
    emit interactiveInputSubmitted(input + "\n");
}

void AuthDialogCoordinator::requestRadiusCode(bool showSkipSecondaryAuthOption)
{
    qInfo().noquote() << "需要 RADIUS 动态口令（短信验证码）";

    QDialog dialog(parentWidget);
    dialog.setWindowTitle(dialogTitle("RADIUS 动态口令"));

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("请输入收到的短信验证码（RADIUS token）：", &dialog));

    auto *codeEdit = new QLineEdit(&dialog);
    layout->addWidget(codeEdit);

    auto *skipCheckBox = new QCheckBox("跳过以后的二次认证", &dialog);
    skipCheckBox->setVisible(showSkipSecondaryAuthOption);
    layout->addWidget(skipCheckBox);

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog
    );
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (!accepted)
    {
        qInfo().noquote() << "RADIUS 动态口令输入已取消";
        emit interactiveInputCancelled();
        return;
    }

    QByteArray input;
    input = codeEdit->text().toLocal8Bit();
    if (showSkipSecondaryAuthOption && skipCheckBox->isChecked())
    {
        input.prepend('$');
    }

    qInfo().noquote() << "RADIUS 动态口令已提交";
    emit interactiveInputSubmitted(input + "\n");
}

void AuthDialogCoordinator::requestTotpCode()
{
    qInfo().noquote() << "需要 TOTP 验证码";
    bool accepted = false;
    const QString totp = QInputDialog::getText(
        parentWidget,
        dialogTitle("TOTP 验证码"),
        "请输入 TOTP 验证码：",
        QLineEdit::Normal,
        "",
        &accepted
    );
    if (!accepted)
    {
        qInfo().noquote() << "TOTP 验证码输入已取消";
        emit interactiveInputCancelled();
        return;
    }

    qInfo().noquote() << "TOTP 验证码已提交";
    emit interactiveInputSubmitted(totp.toLocal8Bit() + "\n");
}

void AuthDialogCoordinator::requestSsoLogin(const QString &coreLoginUrl)
{
    if (settings == nullptr)
    {
        emit interactiveInputCancelled();
        return;
    }
    if (ssoLoginDialog != nullptr)
    {
        ssoLoginDialog->raise();
        ssoLoginDialog->activateWindow();
        return;
    }

    const QString serverHost =
        settings->value("ZJUConnect/ServerAddress", "trust.hitsz.edu.cn").toString();
    const int serverPort = settings->value("ZJUConnect/ServerPort", 443).toInt();
    QUrl serverUrl;
    serverUrl.setScheme("https");
    serverUrl.setHost(serverHost);
    if (serverPort != 443)
    {
        serverUrl.setPort(serverPort);
    }

    QString ssoUrl = settings->value("ZJUConnect/LoginURL").toString().trimmed();
    if (ssoUrl.isEmpty())
    {
        ssoUrl = coreLoginUrl;
    }
    if (ssoUrl.isEmpty())
    {
        QUrl defaultSsoUrl = serverUrl;
        defaultSsoUrl.setPath("/passport/v1/public/casLogin");
        QUrlQuery query;
        query.addQueryItem(
            "sfDomain",
            settings->value("ZJUConnect/LoginDomain").toString()
        );
        defaultSsoUrl.setQuery(query);
        ssoUrl = defaultSsoUrl.toString();
    }
    if (ssoUrl.startsWith('/'))
    {
        ssoUrl = serverUrl.resolved(QUrl(ssoUrl)).toString();
    }

    auto *dialog = new QDialog(parentWidget);
    ssoLoginDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(dialogTitle("浏览器单点登录"));
    dialog->setMinimumWidth(450);
    auto *layout = new QVBoxLayout(dialog);
    auto *mode = new QComboBox(dialog);
    mode->setObjectName("ssoBrowserMode");
    mode->addItem("临时浏览器配置", "temporary");
    mode->addItem("系统默认浏览器", "system");
    mode->setCurrentIndex(settings->value(ApplicationConstants::SsoBrowserModeKey).toString() == "system" ? 1 : 0);
    auto *form = new QFormLayout;
    form->addRow("登录浏览器", mode);
    layout->addLayout(form);
    auto *status = new QLabel(dialog);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto *openAgain = buttons->addButton("重新打开", QDialogButtonBox::ActionRole);
    auto *copyLink = buttons->addButton("复制登录链接", QDialogButtonBox::ActionRole);
    auto *exportExtension = buttons->addButton("导出回传扩展", QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto *browser = new BrowserSsoSession(dialog);
    auto *temporary = new TemporaryBrowserSsoSession(dialog);
    connect(dialog, &QDialog::finished, browser, &BrowserSsoSession::cancel);
    connect(dialog, &QDialog::finished, temporary, &TemporaryBrowserSsoSession::cancel);
    connect(dialog, &QDialog::finished, this, [this](int result)
    {
        ssoLoginDialog.clear();
        if (result == QDialog::Rejected)
        {
            emit interactiveInputCancelled();
        }
    });

    const auto complete = [this, dialog](const QByteArray &url)
    {
        dialog->accept();
        emit interactiveInputSubmitted(url + '\n');
    };
    connect(browser, &BrowserSsoSession::callbackReceived, dialog, complete);
    connect(temporary, &TemporaryBrowserSsoSession::callbackReceived, dialog, complete);
    connect(browser, &BrowserSsoSession::failed, status, &QLabel::setText);
    connect(temporary, &TemporaryBrowserSsoSession::failed, status, &QLabel::setText);

    const QUrl loginUrl = QUrl::fromUserInput(ssoUrl);
    const auto openLogin = [browser, temporary, mode, status, copyLink, exportExtension, loginUrl, serverUrl]()
    {
        const bool system = mode->currentData().toString() == "system";
        copyLink->setVisible(system);
        exportExtension->setVisible(system);
        if (!system)
        {
            browser->cancel();
            status->setText("请在临时浏览器窗口中完成登录。票据会自动回传，无需加载扩展。");
            temporary->start(loginUrl, serverUrl);
            return;
        }
        temporary->cancel();
        status->setText("请在系统默认浏览器的现有配置中完成登录。首次使用时加载回传扩展。");
        const QUrl entry = browser->prepare(loginUrl, serverUrl);
        if (!entry.isEmpty() && !QDesktopServices::openUrl(entry))
        {
            status->setText("无法打开系统默认浏览器。可以复制链接后手动打开。");
        }
    };
    connect(mode, &QComboBox::currentIndexChanged, dialog, openLogin);
    connect(openAgain, &QPushButton::clicked, dialog, openLogin);
    connect(copyLink, &QPushButton::clicked, dialog, [browser, status]()
    {
        const QUrl entry = browser->entryUrl();
        if (entry.isEmpty())
        {
            status->setText("登录链接已失效，请点击「重新打开」。");
            return;
        }
        QApplication::clipboard()->setText(entry.toString(QUrl::FullyEncoded));
        status->setText("登录链接已复制。请粘贴到已启用回传扩展的浏览器配置。");
    });
    connect(exportExtension, &QPushButton::clicked, dialog, [dialog, status]()
    {
        const QString parent = QFileDialog::getExistingDirectory(dialog, "选择回传扩展的保存位置");
        if (parent.isEmpty())
        {
            return;
        }
        const QString path = BrowserSsoSession::exportExtension(parent);
        if (path.isEmpty())
        {
            status->setText("无法导出回传扩展，请选择可写入的位置。");
            return;
        }
        QApplication::clipboard()->setText(path);
        status->setText("扩展目录已复制。请在常用浏览器的扩展页加载，再刷新登录标签页。");
    });
    qInfo().noquote() << "已请求浏览器单点登录";
    dialog->show();
    openLogin();
}

void AuthDialogCoordinator::dismissSsoLogin()
{
    if (ssoLoginDialog != nullptr)
    {
        ssoLoginDialog->accept();
    }
}
