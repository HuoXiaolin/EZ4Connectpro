#include "browserssosession.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSaveFile>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QUuid>

namespace
{
constexpr int requestLimit = 32768;
const QByteArray pageStart = "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>EZ4Connectpro</title><style>"
    "body{max-width:40rem;margin:12vh auto;padding:0 1.5rem;font:1rem/1.6 system-ui,sans-serif;"
    "color:#182a33;background:#f2f4f3}h1{font-size:1.5rem}</style>";

bool isHttpUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && !url.authority().contains('@')
        && (url.scheme() == "https" || url.scheme() == "http");
}

int effectivePort(const QUrl &url)
{
    return url.port(url.scheme() == "https" ? 443 : 80);
}

QByteArray nonce()
{
    return QUuid::createUuid().toByteArray(QUuid::Id128);
}

void respond(QTcpSocket *client, const QByteArray &status, const QByteArray &type = "text/plain",
             const QByteArray &body = {}, const QByteArray &scriptNonce = {})
{
    QByteArray policy = "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'";
    if (!scriptNonce.isEmpty())
    {
        policy += "; script-src 'nonce-" + scriptNonce + "'; connect-src 'self'";
    }
    client->write("HTTP/1.1 " + status + "\r\nConnection: close\r\nCache-Control: no-store\r\n"
                  "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
                  "Content-Security-Policy: " + policy + "\r\nContent-Type: " + type
                  + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    client->disconnectFromHost();
}
}

BrowserSsoSession::BrowserSsoSession(QObject *parent) : QObject(parent)
{
    server.setProxy(QNetworkProxy::NoProxy);
    server.setMaxPendingConnections(16);
    timeout.setSingleShot(true);
    connect(&server, &QTcpServer::newConnection, this, &BrowserSsoSession::acceptClient);
    connect(&timeout, &QTimer::timeout, this, [this]()
    {
        fail(QStringLiteral("浏览器登录已超时。请确认回传扩展已在当前浏览器配置中启用，再重新连接。"));
    });
}

bool BrowserSsoSession::isCallback(const QByteArray &rawUrl, const QUrl &server)
{
    if (rawUrl.contains('\r') || rawUrl.contains('\n') || rawUrl.contains('\0'))
    {
        return false;
    }
    const QUrl url = QUrl::fromEncoded(rawUrl, QUrl::StrictMode);
    if (!isHttpUrl(url) || url.hasFragment() || url.scheme() != server.scheme()
        || url.host() != server.host() || effectivePort(url) != effectivePort(server))
    {
        return false;
    }
    const QString path = url.path(QUrl::FullyEncoded);
    if (path != "/passport/v1/auth/cas" && path != "/passport/v1/auth/httpsOauth2")
    {
        return false;
    }
    const QUrlQuery query(url);
    const QStringList values = query.allQueryItemValues(path.endsWith("/cas") ? "ticket" : "code", QUrl::FullyDecoded);
    return values.size() == 1 && !values.first().trimmed().isEmpty()
        && !values.first().contains('\r') && !values.first().contains('\n') && !values.first().contains(QChar::Null);
}

QString BrowserSsoSession::exportExtension(const QString &directory)
{
    const QDir parent(directory);
    const QString name = QStringLiteral("EZ4Connectpro-浏览器回传");
    if (!parent.mkpath(name))
    {
        return {};
    }
    const QDir destination(parent.filePath(name));
    const QStringList files{"manifest.json", "launch.js", "authorize.html", "authorize.js", "cleanup.js"};
    for (const QString &name : files)
    {
        QFile source(":/browser_extension/" + name);
        QSaveFile output(destination.filePath(name));
        if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        {
            return {};
        }
        const QByteArray content = source.readAll();
        if (content.isEmpty() || output.write(content) != content.size() || !output.commit())
        {
            return {};
        }
    }
    return destination.absolutePath();
}

QUrl BrowserSsoSession::prepare(const QUrl &loginUrl, const QUrl &serverUrl)
{
    cancel();
    if (!isHttpUrl(loginUrl) || !isHttpUrl(serverUrl))
    {
        emit failed(QStringLiteral("单点登录地址或 VPN 网关地址无效。"));
        return {};
    }
    if (!server.listen(QHostAddress::LocalHost))
    {
        emit failed(QStringLiteral("无法启动本机登录票据接收端。"));
        return {};
    }
    login = loginUrl;
    gateway = serverUrl.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    sessionPath = "/ez4connectpro/" + nonce() + '/';
    authority = "127.0.0.1:" + QByteArray::number(server.serverPort());
    timeout.start(5 * 60 * 1000);
    return entryUrl();
}

QUrl BrowserSsoSession::entryUrl() const
{
    return server.isListening() ? QUrl::fromEncoded("http://" + authority + sessionPath + "begin") : QUrl{};
}

void BrowserSsoSession::acceptClient()
{
    while (auto *client = server.nextPendingConnection())
    {
        if (server.findChildren<QTcpSocket *>(QString(), Qt::FindDirectChildrenOnly).size() > 16)
        {
            client->deleteLater();
            continue;
        }
        client->setReadBufferSize(requestLimit + 1);
        connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        QTimer::singleShot(5000, client, [client]() { client->abort(); });
        const auto readRequest = [this, client, request = QByteArray{}]() mutable
        {
            request += client->readAll();
            if (request.size() > requestLimit)
            {
                respond(client, "413 Content Too Large");
                return;
            }
            const qsizetype headerEnd = request.indexOf("\r\n\r\n");
            if (headerEnd < 0)
            {
                return;
            }
            const auto lines = request.first(headerEnd).split('\n');
            const auto first = lines.value(0).trimmed().split(' ');
            QByteArray host;
            QByteArray origin;
            int contentLength = 0;
            for (qsizetype i = 1; i < lines.size(); ++i)
            {
                const auto header = lines.at(i);
                const qsizetype separator = header.indexOf(':');
                const QByteArray key = header.first(qMax(qsizetype{0}, separator)).toLower();
                const QByteArray value = header.sliced(separator + 1).trimmed();
                if (key == "host")
                {
                    host = value;
                }
                else if (key == "origin")
                {
                    origin = value;
                }
                else if (key == "content-length")
                {
                    bool ok = false;
                    contentLength = value.toInt(&ok);
                    if (!ok || contentLength < 0 || contentLength > requestLimit - headerEnd - 4)
                    {
                        respond(client, "400 Bad Request");
                        return;
                    }
                }
            }
            if (host != authority || first.size() != 3)
            {
                respond(client, "400 Bad Request");
                return;
            }
            if (request.size() < headerEnd + 4 + contentLength)
            {
                return;
            }
            handleRequest(client, first.at(0), first.at(1), request.sliced(headerEnd + 4, contentLength), origin);
        };
        connect(client, &QTcpSocket::readyRead, this, readRequest);
    }
}

void BrowserSsoSession::handleRequest(QTcpSocket *client, const QByteArray &method,
                                     const QByteArray &target, const QByteArray &body, const QByteArray &origin)
{
    if (!server.isListening() || !target.startsWith(sessionPath))
    {
        respond(client, "404 Not Found");
        return;
    }
    const QByteArray action = target.sliced(sessionPath.size());
    if (method == "GET" && action == "begin")
    {
        respond(client, "200 OK", "text/html; charset=utf-8",
                pageStart + "<meta name=\"ez4connectpro\" content=\"sso\"><h1>浏览器单点登录</h1>"
                "<p>回传扩展启用后会在此标签页打开登录页。</p>"
                "<p>首次使用时，请在 EZ4Connectpro 导出回传扩展，并加载到当前浏览器配置后刷新本页。</p>");
    }
    else if (method == "GET" && action == "metadata")
    {
        respond(client, "200 OK", "application/json",
                QJsonDocument(QJsonObject{{"login", login.toString(QUrl::FullyEncoded)},
                                          {"gateway", gateway.toString(QUrl::FullyEncoded)}})
                    .toJson(QJsonDocument::Compact));
    }
    else if (method == "GET" && action == "return")
    {
        const QByteArray scriptNonce = nonce();
        respond(client, "200 OK", "text/html; charset=utf-8",
                pageStart + "<h1>单点登录回传</h1><p id=\"status\">正在回传登录票据…</p><script nonce=\""
                + scriptNonce + "\">" + R"(
const ticket = location.hash.slice(1);
history.replaceState(null, "", location.pathname);
const statusLine = document.getElementById("status");
fetch(location.pathname, {method: "POST", body: ticket, credentials: "omit"})
    .then(response => {
        statusLine.textContent = response.ok ? "票据已提交，可以关闭此标签页。" : "票据无效，请回到工作台重新连接。";
    })
    .catch(() => { statusLine.textContent = "登录会话已结束，请回到工作台重新连接。"; });
</script>)", scriptNonce);
    }
    else if (method == "POST" && action == "return")
    {
        if (origin != "http://" + authority || !isCallback(body, gateway))
        {
            respond(client, "400 Bad Request");
            return;
        }
        respond(client, "204 No Content");
        cancel();
        emit callbackReceived(body);
    }
    else
    {
        respond(client, "404 Not Found");
    }
}

void BrowserSsoSession::cancel()
{
    timeout.stop();
    server.close();
}

void BrowserSsoSession::fail(const QString &message)
{
    cancel();
    emit failed(message);
}
