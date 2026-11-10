#include "coreoutputparser.h"

namespace
{
constexpr auto tunSetupMarker = "Tun stack setup error";
}

CoreOutputEvent CoreOutputParser::parse(const QString &output)
{
    if (output.contains("SUDO_ASK_PASS"))
    {
        return CoreOutputEvent::AskSudoPassword;
    }
    if (output.contains("Graph check code saved to "))
    {
        return CoreOutputEvent::GraphCaptcha;
    }
    if (output.contains("Please enter the SMS verification code: "))
    {
        return CoreOutputEvent::SmsCodeWithSkipOption;
    }
    if (output.contains("Please enter your SMS code:"))
    {
        return CoreOutputEvent::SmsCode;
    }
    if (output.contains("Please enter your TOTP code:"))
    {
        return CoreOutputEvent::TotpCode;
    }
    if (output.contains("Please enter rand code:"))
    {
        return CoreOutputEvent::RandCode;
    }
    // aTrust RADIUS Access-Challenge：核心在密码认证后输出此提示并等待用户输入短信验证码。
    if (output.contains("Please enter the RADIUS token:"))
    {
        return CoreOutputEvent::RadiusCodeWithSkipOption;
    }
    if (output.contains("Please enter the callback url:", Qt::CaseInsensitive))
    {
        return CoreOutputEvent::SsoCallback;
    }
    if (output.contains("VPN client started"))
    {
        return CoreOutputEvent::ClientStarted;
    }
    if (output.contains("HTTP server listening on "))
    {
        return CoreOutputEvent::HttpProxyStarting;
    }
    if (output.contains("HTTP listen failed:"))
    {
        return CoreOutputEvent::HttpProxyFailed;
    }
    if (output.contains("graph check code still required after second login attempt") ||
        output.contains("The characters are incorrect or has expired!"))
    {
        return CoreOutputEvent::CaptchaFailed;
    }
    if (output.contains("Failed to initiate stub device creation", Qt::CaseInsensitive)
        && output.contains("0x800700B7", Qt::CaseInsensitive))
    {
        return CoreOutputEvent::TunDeviceConflict;
    }
    if (output.contains(tunSetupMarker, Qt::CaseInsensitive))
    {
        if (output.contains("TUN conflict:", Qt::CaseInsensitive)
            || output.contains("Cannot create a file when that file already exists", Qt::CaseInsensitive)
            || output.contains("0x800700B7", Qt::CaseInsensitive))
        {
            return CoreOutputEvent::TunDeviceConflict;
        }
        return CoreOutputEvent::TunSetupFailed;
    }
    if (output.contains("Access is denied."))
    {
        return CoreOutputEvent::AccessDenied;
    }
    if (output.contains("listen failed"))
    {
        return CoreOutputEvent::ListenFailed;
    }
    if (output.contains("Invalid username or password!") || output.contains("ticket is empty"))
    {
        return CoreOutputEvent::InvalidCredentials;
    }
    if (output.contains("You are trying brute-force login on this IP address."))
    {
        return CoreOutputEvent::BruteForceBlocked;
    }
    if (output.contains("Login failed") || output.contains("too many login failures"))
    {
        return CoreOutputEvent::LoginFailed;
    }
    if (output.contains("unexpected newline"))
    {
        return CoreOutputEvent::InteractiveError;
    }
    if (output.contains("auth type/login domain combination not found"))
    {
        return CoreOutputEvent::AuthNotAvailable;
    }
    if (output.contains("invalid SID") || output.contains("l3-tunnel tunnel auth failed:"))
    {
        return CoreOutputEvent::AuthExpired;
    }
    if (output.contains("client setup error"))
    {
        return CoreOutputEvent::ClientFailed;
    }
    if (output.contains("panic"))
    {
        return CoreOutputEvent::CorePanic;
    }
    return CoreOutputEvent::None;
}

QString CoreOutputParser::graphCaptchaFile(const QString &output)
{
    const QString prefix = "Graph check code saved to ";
    const qsizetype markerIndex = output.indexOf(prefix);
    if (markerIndex < 0)
    {
        return {};
    }

    return output.mid(markerIndex + prefix.size()).trimmed();
}

QString CoreOutputParser::ssoLoginUrl(const QString &output)
{
    const QString prefix = QStringLiteral("Visit ");
    const QString suffix = QStringLiteral(" to login, and catch the callback url");
    const qsizetype start = output.indexOf(prefix);
    const qsizetype end = output.indexOf(suffix, start < 0 ? 0 : start + prefix.size(), Qt::CaseInsensitive);
    return start < 0 || end < 0 ? QString{} : output.mid(start + prefix.size(), end - start - prefix.size()).trimmed();
}

QString CoreOutputParser::tunSetupError(const QString &output)
{
    const qsizetype start = output.indexOf(tunSetupMarker, 0, Qt::CaseInsensitive);
    if (start < 0)
    {
        return {};
    }
    const qsizetype detail = output.indexOf(':', start);
    return detail < 0 ? QString{} : output.mid(detail + 1).trimmed();
}

bool CoreOutputParser::hasInteractivePrompt(const QByteArray &output)
{
    return output.contains("SUDO_ASK_PASS")
           || output.contains("Please enter the SMS verification code: ")
           || output.contains("Please enter your SMS code:")
           || output.contains("Please enter your TOTP code:")
           || output.contains("Please enter rand code:")
           || output.contains("Please enter the RADIUS token:")
           || output.toLower().contains("please enter the callback url:");
}
