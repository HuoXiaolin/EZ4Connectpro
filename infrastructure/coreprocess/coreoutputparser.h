#ifndef COREOUTPUTPARSER_H
#define COREOUTPUTPARSER_H

#include <QByteArray>
#include <QString>

enum class CoreOutputEvent
{
    None,
    AskSudoPassword,
    GraphCaptcha,
    SmsCodeWithSkipOption,
    SmsCode,
    TotpCode,
    RandCode,
    RadiusCodeWithSkipOption,
    SsoCallback,
    ClientStarted,
    HttpProxyStarting,
    HttpProxyFailed,
    CaptchaFailed,
    AccessDenied,
    ListenFailed,
    TunSetupFailed,
    TunDeviceConflict,
    InvalidCredentials,
    BruteForceBlocked,
    LoginFailed,
    InteractiveError,
    AuthNotAvailable,
    AuthExpired,
    ClientFailed,
    CorePanic,
};

class CoreOutputParser
{
public:
    static CoreOutputEvent parse(const QString &output);

    static bool hasInteractivePrompt(const QByteArray &output);

    static QString graphCaptchaFile(const QString &output);
    static QString tunSetupError(const QString &output);
    static QString ssoLoginUrl(const QString &output);
};

#endif // COREOUTPUTPARSER_H
