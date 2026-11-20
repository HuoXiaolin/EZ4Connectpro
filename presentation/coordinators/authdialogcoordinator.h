#ifndef AUTHDIALOGCOORDINATOR_H
#define AUTHDIALOGCOORDINATOR_H

#include <QObject>
#include <QPointer>

class GraphCaptchaWindow;
class LoginWindow;
class QDialog;
class QSettings;
class SudoWindow;
class QWidget;
class TemporaryBrowserSsoSession;
class BrowserSsoSession;

class AuthDialogCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit AuthDialogCoordinator(
        QWidget *parentWidget,
        QSettings *settings,
        QObject *parent = nullptr,
        const QString &profileId = {}
    );

    void requestLogin(const QString &username, const QString &password);
    void requestPhoneNumber(
        const QString &countryCode,
        const QString &phoneNumber
    );
    void requestSudoPassword();
    void requestGraphCaptcha(const QString &graphFile);
    void requestSmsCode(bool showSkipSecondaryAuthOption);
    void requestTotpCode();
    void requestRadiusCode(bool showSkipSecondaryAuthOption);
    void requestSsoLogin(const QString &loginUrl);
    void dismissSsoLogin();
    void setShowLoginBrowserImmediately(bool enabled) { showLoginBrowserImmediately = enabled; }
    void openResourcesInLoginBrowser();
    void openResourcePage();

signals:
    void loginSubmitted(const QString &username, const QString &password, bool saveDetails);
    void phoneNumberSubmitted(
        const QString &countryCode,
        const QString &phoneNumber,
        bool saveDetails
    );
    void sudoPasswordSubmitted(const QString &password, bool remember);
    void interactiveInputSubmitted(const QByteArray &input);
    void interactiveInputCancelled();

private:
    QString dialogTitle(const QString &title) const;
    QWidget *parentWidget;
    QSettings *settings;
    QString profileId;
    QPointer<LoginWindow> loginWindow;
    QPointer<QDialog> phoneNumberDialog;
    QPointer<SudoWindow> sudoWindow;
    QPointer<GraphCaptchaWindow> graphCaptchaWindow;
    QPointer<QDialog> ssoLoginDialog;
    enum class LoginBrowser { None, Temporary, System };
    LoginBrowser loginBrowser = LoginBrowser::None;
    bool showLoginBrowserImmediately = false;
    TemporaryBrowserSsoSession *resourceBrowser;
    BrowserSsoSession *resourceBridge;
};

#endif // AUTHDIALOGCOORDINATOR_H
