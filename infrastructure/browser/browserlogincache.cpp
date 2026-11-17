#include "browserlogincache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace
{
constexpr qint64 maxBytes = 2 * 1024 * 1024;
const QByteArray signature("EZ4SSO1\0", 8);

QByteArray binding(const QString &file, QUrl gateway)
{
    gateway = gateway.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    if (gateway.port() == (gateway.scheme() == "https" ? 443 : 80)) gateway.setPort(-1);
    return QCryptographicHash::hash(QFileInfo(file).absoluteFilePath().toUtf8() + '\n' + gateway.toEncoded(), QCryptographicHash::Sha256);
}

QByteArray protect(QByteArray data, QByteArray entropy, bool encrypt)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{DWORD(data.size()), reinterpret_cast<BYTE *>(data.data())};
    DATA_BLOB salt{DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data())};
    DATA_BLOB output{};
    const BOOL ok = encrypt
        ? CryptProtectData(&input, L"EZ4Connectpro browser login", &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) return {};
    const QByteArray result(reinterpret_cast<char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    Q_UNUSED(data)
    Q_UNUSED(entropy)
    Q_UNUSED(encrypt)
    // Never fall back to plaintext on platforms without a credential store.
    return {};
#endif
}
}

QJsonArray BrowserLoginCache::cookieParameters(const QJsonArray &cookies)
{
    QJsonArray result;
    if (cookies.size() > 512) return result;
    const double now = QDateTime::currentSecsSinceEpoch();
    for (const auto &value : cookies)
    {
        const auto source = value.toObject();
        const QString name = source.value("name").toString();
        const QString domain = source.value("domain").toString();
        const QString path = source.value("path").toString();
        const double expires = source.value("expires").toDouble(-1);
        // Partitioned cookies cannot safely be restored without their partition.
        if (name.isEmpty() || !source.value("value").isString() || domain.isEmpty()
            || !path.startsWith('/') || source.contains("partitionKey") || source.value("partitionKeyOpaque").toBool()
            || (expires > 0 && expires <= now)) continue;
        QUrl origin;
        origin.setScheme(source.value("secure").toBool() ? "https" : "http");
        origin.setHost(domain.startsWith('.') ? domain.mid(1) : domain);
        origin.setPath(path);
        if (!origin.isValid() || origin.host().isEmpty() || origin.authority().contains('@')) continue;
        QJsonObject cookie{{"name", name}, {"value", source.value("value")}, {"path", path},
                           {"secure", source.value("secure").toBool()}, {"httpOnly", source.value("httpOnly").toBool()}};
        if (domain.startsWith('.')) cookie.insert("domain", domain);
        else cookie.insert("url", origin.toString(QUrl::FullyEncoded)); // Preserve host-only scope.
        const QString sameSite = source.value("sameSite").toString();
        if (sameSite == "Strict" || sameSite == "Lax" || sameSite == "None") cookie.insert("sameSite", sameSite);
        if (expires > 0) cookie.insert("expires", expires); // Session cookies stay session cookies.
        result.append(cookie);
    }
    return result;
}

QJsonArray BrowserLoginCache::load(const QString &path, const QUrl &gateway)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isAbsolute() || info.isSymLink() || info.size() > maxBytes) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes || !bytes.startsWith(signature)) return {};
    const auto data = QJsonDocument::fromJson(protect(bytes.mid(signature.size()), binding(path, gateway), false)).object();
    return data.value("version").toInt() == 1 ? cookieParameters(data.value("cookies").toArray()) : QJsonArray{};
}

bool BrowserLoginCache::save(const QString &path, const QUrl &gateway, const QJsonArray &cookies)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isAbsolute() || info.isSymLink() || cookies.size() > 512) return false;
    const QByteArray plain = QJsonDocument(QJsonObject{{"version", 1}, {"cookies", cookies}}).toJson(QJsonDocument::Compact);
    if (plain.size() > maxBytes - 4096) return false;
    const QByteArray encrypted = protect(plain, binding(path, gateway), true);
    if (encrypted.isEmpty() || !QDir().mkpath(info.absolutePath())) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(signature + encrypted) == signature.size() + encrypted.size() && file.commit();
}

QJsonArray BrowserLoginCache::gatewaySessionCookies(const QString &path, const QUrl &gateway)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || info.isSymLink() || info.size() > maxBytes
        || !gateway.isValid() || gateway.host().isEmpty()
        || (gateway.scheme() != "https" && gateway.scheme() != "http")) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes) return {};
    const auto cookies = QJsonDocument::fromJson(bytes).object().value("cookies").toArray();
    if (cookies.size() > 512) return {};
    const auto port = [](const QUrl &url) { return url.port(url.scheme() == "https" ? 443 : 80); };
    static const QRegularExpression namePattern("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$");
    QJsonArray result;
    bool hasSession = false;
    for (const auto &value : cookies)
    {
        const auto cookie = value.toObject();
        const QUrl source(cookie.value("scheme").toString() + "://" + cookie.value("host").toString());
        const QString name = cookie.value("name").toString();
        const QString content = cookie.value("value").toString();
        if (source.scheme() != gateway.scheme() || source.host() != gateway.host()
            || port(source) != port(gateway) || !source.userInfo().isEmpty() || !source.path().isEmpty()
            || source.hasQuery() || source.hasFragment()
            || !namePattern.match(name).hasMatch() || !cookie.value("value").isString()
            || content.size() > 16384 || content.contains('\r') || content.contains('\n') || content.contains(QChar::Null)) continue;
        QUrl origin = gateway.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
        origin.setPath("/");
        result.append(QJsonObject{{"name", name}, {"value", content}, {"url", origin.toString()},
                                  {"path", "/"}, {"secure", gateway.scheme() == "https"}, {"httpOnly", true}, {"sameSite", "Lax"}});
        hasSession |= name == "sid" && !content.isEmpty();
    }
    return hasSession ? result : QJsonArray{};
}
