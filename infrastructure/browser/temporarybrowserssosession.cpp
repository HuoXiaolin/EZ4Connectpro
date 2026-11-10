#include "temporarybrowserssosession.h"

#include "browserssosession.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <shlwapi.h>
#elif defined(Q_OS_MACOS)
#include <CoreServices/CoreServices.h>
#endif

namespace
{
enum class Engine { Unknown, Chromium, Gecko };

const QString startPage = QStringLiteral("about:blank#ez4connectpro-login");
const QString protocolError = QStringLiteral("无法建立临时浏览器登录会话。可以重试，或选择系统默认浏览器。");
const QStringList callbackPaths{"/passport/v1/auth/cas", "/passport/v1/auth/httpsOauth2"};

Engine browserEngine(const QString &program)
{
    const QString name = QFileInfo(program).completeBaseName().toLower();
    const QStringList gecko{"firefox", "firefox-esr", "zen", "zen-bin"};
    const QStringList chromium{"chrome", "google chrome", "google-chrome", "chromium", "chromium-browser",
                               "msedge", "microsoft edge", "microsoft-edge", "brave", "brave browser",
                               "brave-browser", "vivaldi"};
    if (gecko.contains(name))
    {
        return Engine::Gecko;
    }
    return chromium.contains(name) ? Engine::Chromium : Engine::Unknown;
}

QString defaultBrowser()
{
#if defined(Q_OS_WIN)
    DWORD length = 0;
    if (AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, L"https", L"open", nullptr, &length) == S_FALSE
        && length > 0)
    {
        std::wstring executable(length, L'\0');
        if (AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, L"https", L"open", executable.data(), &length) == S_OK)
        {
            const QFileInfo path(QString::fromWCharArray(executable.c_str()));
            if (path.fileName().compare("chrome_proxy.exe", Qt::CaseInsensitive) == 0)
            {
                return QDir(path.absolutePath()).filePath("chrome.exe");
            }
            return path.absoluteFilePath();
        }
    }
#elif defined(Q_OS_MACOS)
    const auto url = CFURLCreateWithString(nullptr, CFSTR("https://localhost/"), nullptr);
    const auto application = LSCopyDefaultApplicationURLForURL(url, kLSRolesAll, nullptr);
    CFRelease(url);
    if (application)
    {
        const auto bundle = CFBundleCreate(nullptr, application);
        CFRelease(application);
        if (bundle)
        {
            const auto executable = CFBundleCopyExecutableURL(bundle);
            CFRelease(bundle);
            if (executable)
            {
                const auto path = CFURLCopyFileSystemPath(executable, kCFURLPOSIXPathStyle);
                const QString result = QString::fromCFString(path);
                CFRelease(path);
                CFRelease(executable);
                return result;
            }
        }
    }
#elif defined(Q_OS_LINUX)
    QProcess settings;
    settings.start("xdg-mime", {"query", "default", "x-scheme-handler/https"});
    if (settings.waitForFinished(1500) && settings.exitCode() == 0)
    {
        const QString id = QString::fromUtf8(settings.readAllStandardOutput()).trimmed();
        const QString desktopFile = QStandardPaths::locate(QStandardPaths::GenericDataLocation, "applications/" + id);
        if (!desktopFile.isEmpty())
        {
            const QSettings entry(desktopFile, QSettings::IniFormat);
            const QStringList command = QProcess::splitCommand(entry.value("Desktop Entry/Exec").toString());
            return QStandardPaths::findExecutable(command.value(0));
        }
    }
#endif
    return {};
}

QString supportedBrowser()
{
    const QString system = defaultBrowser();
    if (browserEngine(system) != Engine::Unknown && QFileInfo(system).isExecutable())
    {
        return system;
    }
#if defined(Q_OS_WIN)
    const QStringList files{"Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe",
                            "Mozilla Firefox/firefox.exe", "Zen Browser/zen.exe", "BraveSoftware/Brave-Browser/Application/brave.exe"};
    const QStringList roots{qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                            qEnvironmentVariable("LOCALAPPDATA")};
    for (const QString &file : files)
    {
        for (const QString &root : roots)
        {
            const QFileInfo candidate(QDir(root).filePath(file));
            if (candidate.isAbsolute() && candidate.isExecutable())
            {
                return candidate.absoluteFilePath();
            }
        }
    }
#elif defined(Q_OS_MACOS)
    for (const QString &name : {"Google Chrome", "Microsoft Edge", "Firefox", "Zen", "Brave Browser"})
    {
        const QString executable = name == "Firefox" ? QStringLiteral("firefox") : name == "Zen" ? QStringLiteral("zen") : name;
        const QFileInfo candidate("/Applications/" + name + ".app/Contents/MacOS/" + executable);
        if (candidate.isExecutable())
        {
            return candidate.absoluteFilePath();
        }
    }
#else
    for (const QString &name : {"google-chrome", "chromium", "microsoft-edge", "firefox", "zen", "brave-browser"})
    {
        const QString candidate = QStandardPaths::findExecutable(name);
        if (!candidate.isEmpty())
        {
            return candidate;
        }
    }
#endif
    return {};
}

bool isHttpUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && !url.authority().contains('@')
        && (url.scheme() == "https" || url.scheme() == "http");
}
}

TemporaryBrowserSsoSession::TemporaryBrowserSsoSession(QObject *parent) : QObject(parent)
{
    process.setProcessChannelMode(QProcess::MergedChannels);
    socket.setProxy(QNetworkProxy::NoProxy);
    poll.setInterval(100);
    pageRetry.setSingleShot(true);
    pageRetry.setInterval(150);
    timeout.setSingleShot(true);
    connect(&poll, &QTimer::timeout, this, &TemporaryBrowserSsoSession::findChromiumEndpoint);
    connect(&pageRetry, &QTimer::timeout, this, &TemporaryBrowserSsoSession::findLoginPage);
    connect(&timeout, &QTimer::timeout, this, [this]()
    {
        qWarning() << "临时浏览器登录超时" << socket.state() << !contextId.isEmpty();
        fail(contextId.isEmpty() ? protocolError : QStringLiteral("临时浏览器登录已超时，可以重新打开登录页。"));
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, &TemporaryBrowserSsoSession::readBrowserOutput);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
    {
        if (error == QProcess::FailedToStart)
        {
            fail(QStringLiteral("无法启动临时浏览器，可以改用系统默认浏览器。"));
        }
    });
    connect(&process, &QProcess::finished, this, [this]()
    {
        fail(QStringLiteral("临时浏览器已关闭，可以重新打开登录页。"));
    });
    connect(&socket, &QWebSocket::connected, this, [this]()
    {
        if (gecko)
        {
            send("session.new", {{"capabilities", QJsonObject{}}}, [this](const QJsonObject &) { findLoginPage(); });
        }
        else
        {
            findLoginPage();
        }
    });
    connect(&socket, &QWebSocket::textMessageReceived, this, &TemporaryBrowserSsoSession::handleMessage);
    connect(&socket, &QWebSocket::disconnected, this, [this]() { fail(protocolError); });
    connect(&socket, &QWebSocket::errorOccurred, this, [this]()
    {
        if (active)
        {
            qWarning().noquote() << "临时浏览器本地连接失败" << socket.errorString();
            fail(protocolError);
        }
    });
}

TemporaryBrowserSsoSession::~TemporaryBrowserSsoSession()
{
    cancel();
}

void TemporaryBrowserSsoSession::start(const QUrl &url, const QUrl &server, const QString &browser)
{
    cancel();
    if (process.state() != QProcess::NotRunning || !isHttpUrl(url) || !isHttpUrl(server))
    {
        emit failed(QStringLiteral("无法开启临时登录会话，请检查登录地址和 VPN 网关。"));
        return;
    }
    const QString program = browser.isEmpty() ? supportedBrowser() : browser;
    const Engine engine = browserEngine(program);
    if (engine == Engine::Unknown)
    {
        emit failed(QStringLiteral("未找到支持临时配置的浏览器，例如 Chrome、Edge、Firefox 或 Zen。"));
        return;
    }
    profile = std::make_unique<QTemporaryDir>(QDir::temp().filePath("EZ4Connectpro-login-XXXXXX"));
    if (!profile->isValid())
    {
        emit failed(QStringLiteral("无法创建临时浏览器配置目录。"));
        return;
    }

    loginUrl = url;
    gateway = server.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    const int defaultPort = gateway.scheme() == "https" ? 443 : 80;
    if (gateway.port() == defaultPort)
    {
        gateway.setPort(-1);
    }
    gecko = engine == Engine::Gecko;
    QStringList args;
    if (gecko)
    {
        QFile preferences(profile->filePath("user.js"));
        if (!preferences.open(QIODevice::WriteOnly)
            || preferences.write("user_pref(\"browser.aboutwelcome.enabled\", false);\n"
                                 "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n") < 0)
        {
            profile.reset();
            emit failed(QStringLiteral("无法初始化临时浏览器配置。"));
            return;
        }
        args = {"--no-remote", "--profile", profile->path(), "--remote-debugging-port", "0", startPage};
    }
    else
    {
        args = {"--user-data-dir=" + profile->path(), "--remote-debugging-port=0", "--no-first-run",
                "--no-default-browser-check", "--disable-background-mode", "--new-window", startPage};
        poll.start();
    }
    active = true;
    timeout.start(20000);
    process.start(program, args);
}

void TemporaryBrowserSsoSession::readBrowserOutput()
{
    const QByteArray output = process.readAllStandardOutput();
    if (!active || !gecko || socket.state() != QAbstractSocket::UnconnectedState)
    {
        return;
    }
    browserOutput = (browserOutput + output).right(8192);
    static const QRegularExpression endpoint(R"(WebDriver BiDi listening on ws://(127\.0\.0\.1|localhost|\[::1\]):(\d+)[/\r\n])");
    const auto match = endpoint.match(QString::fromUtf8(browserOutput));
    if (match.hasMatch())
    {
        const QString host = match.captured(1) == "[::1]" ? QStringLiteral("::1") : QStringLiteral("127.0.0.1");
        openEndpoint(host, match.captured(2).toInt(), "/session");
    }
}

void TemporaryBrowserSsoSession::findChromiumEndpoint()
{
    QFile file(profile->filePath("DevToolsActivePort"));
    if (file.open(QIODevice::ReadOnly))
    {
        const auto lines = file.read(256).split('\n');
        const QByteArray path = lines.value(1).trimmed();
        if (path.startsWith("/devtools/browser/") && path.size() > 18)
        {
            openEndpoint("127.0.0.1", lines.value(0).trimmed().toInt(), QString::fromLatin1(path));
        }
    }
}

void TemporaryBrowserSsoSession::openEndpoint(const QString &host, int port, const QString &path)
{
    if (port < 1 || port > 65535)
    {
        return;
    }
    poll.stop();
    QUrl endpoint;
    endpoint.setScheme("ws");
    endpoint.setHost(host);
    endpoint.setPort(port);
    endpoint.setPath(path);
    socket.open(endpoint);
}

void TemporaryBrowserSsoSession::findLoginPage()
{
    if (!active)
    {
        return;
    }
    const QString method = gecko ? QStringLiteral("browsingContext.getTree") : QStringLiteral("Target.getTargets");
    const QJsonObject params = gecko ? QJsonObject{{"maxDepth", 0}} : QJsonObject{};
    send(method, params, [this](const QJsonObject &result)
    {
        const QJsonArray pages = result.value(gecko ? "contexts" : "targetInfos").toArray();
        for (const auto &value : pages)
        {
            const QJsonObject page = value.toObject();
            // Chromium's isolated window may initially show its built-in new tab page.
            const bool matches = gecko ? page.value("url").toString() == startPage
                                       : page.value("type").toString() == "page";
            if (!matches)
            {
                continue;
            }
            if (gecko)
            {
                contextId = page.value("context").toString();
                send("session.subscribe", {{"events", QJsonArray{"network.beforeRequestSent"}}},
                     [this](const QJsonObject &) { enableInterception(); });
            }
            else
            {
                send("Target.attachToTarget", {{"targetId", page.value("targetId")}, {"flatten", true}},
                     [this](const QJsonObject &attached)
                {
                    sessionId = attached.value("sessionId").toString();
                    send("Page.getFrameTree", {}, [this](const QJsonObject &frame)
                    {
                        contextId = frame.value("frameTree").toObject().value("frame").toObject().value("id").toString();
                        enableInterception();
                    });
                });
            }
            return;
        }
        pageRetry.start();
    });
}

void TemporaryBrowserSsoSession::enableInterception()
{
    if (contextId.isEmpty())
    {
        fail(protocolError);
        return;
    }
    QJsonArray patterns;
    for (const QString &path : callbackPaths)
    {
        if (gecko)
        {
            patterns.append(QJsonObject{{"type", "pattern"}, {"protocol", gateway.scheme()},
                                        {"hostname", gateway.host(QUrl::FullyEncoded)}, {"pathname", path}});
        }
        else
        {
            QUrl target = gateway;
            target.setPath(path);
            patterns.append(QJsonObject{{"urlPattern", target.toString(QUrl::FullyEncoded) + '*'}, {"resourceType", "Document"}});
        }
    }
    if (gecko)
    {
        send("network.addIntercept", {{"phases", QJsonArray{"beforeRequestSent"}}, {"urlPatterns", patterns},
                                      {"contexts", QJsonArray{contextId}}}, [this](const QJsonObject &) { navigate(); });
    }
    else
    {
        send("Fetch.enable", {{"patterns", patterns}}, [this](const QJsonObject &) { navigate(); });
    }
}

void TemporaryBrowserSsoSession::navigate()
{
    timeout.start(5 * 60 * 1000);
    const QString url = loginUrl.toString(QUrl::FullyEncoded);
    if (gecko)
    {
        send("browsingContext.navigate", {{"context", contextId}, {"url", url}, {"wait", "none"}}, [](const QJsonObject &) {});
    }
    else
    {
        send("Page.navigate", {{"url", url}}, [this](const QJsonObject &result)
        {
            if (callback.isEmpty() && !result.value("errorText").toString().isEmpty())
            {
                fail(QStringLiteral("临时浏览器无法打开登录页，请检查网络与登录地址。"));
            }
        });
    }
}

void TemporaryBrowserSsoSession::send(const QString &method, const QJsonObject &params, Reply reply)
{
    const int id = ++nextCommand;
    if (reply)
    {
        replies.insert(id, [this, method, reply = std::move(reply)](const QJsonObject &event)
        {
            if (event.contains("error") || event.value("type").toString() == "error")
            {
                qWarning() << "临时浏览器控制命令失败" << method;
                fail(protocolError);
            }
            else
            {
                reply(event.value("result").toObject());
            }
        });
    }
    QJsonObject message{{"id", id}, {"method", method}, {"params", params}};
    if (!sessionId.isEmpty())
    {
        message.insert("sessionId", sessionId);
    }
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void TemporaryBrowserSsoSession::handleMessage(const QString &message)
{
    if (!active)
    {
        return;
    }
    const QJsonObject event = QJsonDocument::fromJson(message.toUtf8()).object();
    if (Reply reply = replies.take(event.value("id").toInt()))
    {
        reply(event);
        return;
    }
    const QString method = event.value("method").toString();
    if ((gecko && method == "network.beforeRequestSent")
        || (!gecko && method == "Fetch.requestPaused" && event.value("sessionId").toString() == sessionId))
    {
        handleRequest(event.value("params").toObject());
    }
}

void TemporaryBrowserSsoSession::handleRequest(const QJsonObject &event)
{
    if (gecko && !event.value("isBlocked").toBool())
    {
        return;
    }
    const QJsonObject request = event.value("request").toObject();
    const QByteArray url = request.value("url").toString().toUtf8();
    const QString context = event.value(gecko ? "context" : "frameId").toString();
    const QString requestId = gecko ? request.value("request").toString() : event.value("requestId").toString();
    QJsonObject identity{{gecko ? "request" : "requestId", requestId}};
    if (!BrowserSsoSession::isCallback(url, gateway))
    {
        send(gecko ? "network.continueRequest" : "Fetch.continueRequest", identity);
        return;
    }
    if (!gecko)
    {
        identity.insert("errorReason", "Aborted");
    }
    const QString abortMethod = gecko ? QStringLiteral("network.failRequest") : QStringLiteral("Fetch.failRequest");
    if (!callback.isEmpty() || context != contextId || request.value("method").toString() != "GET"
        || (gecko && event.value("navigation").toString().isEmpty()))
    {
        send(abortMethod, identity);
        return;
    }

    // Abort the browser request before the one-use ticket is submitted by the VPN core.
    callback = url;
    send(abortMethod, identity, [this](const QJsonObject &)
    {
        const QByteArray result = callback;
        cancel();
        emit callbackReceived(result);
    });
}

void TemporaryBrowserSsoSession::cancel()
{
    active = false;
    timeout.stop();
    poll.stop();
    pageRetry.stop();
    replies.clear();
    sessionId.clear();
    contextId.clear();
    browserOutput.clear();
    callback.clear();

    if (process.state() != QProcess::NotRunning)
    {
        if (socket.state() == QAbstractSocket::ConnectedState)
        {
            send(gecko ? "browser.close" : "Browser.close");
            socket.flush();
        }
        else
        {
            process.terminate();
        }
        if (!process.waitForFinished(800))
        {
            process.kill();
            process.waitForFinished(500);
        }
    }
    socket.abort();
    if (process.state() == QProcess::NotRunning)
    {
        profile.reset();
    }
}

void TemporaryBrowserSsoSession::fail(const QString &message)
{
    if (active)
    {
        cancel();
        emit failed(message);
    }
}
