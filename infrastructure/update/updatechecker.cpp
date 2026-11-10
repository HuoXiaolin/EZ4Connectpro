#include "infrastructure/update/updatechecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringView>
#include <QVersionNumber>

#include "application/applicationconstants.h"

namespace
{
enum class VersionSuffixKind
{
    Prerelease,
    Stable,
    Hotfix
};

QString normalizedSuffix(const QString &version, qsizetype suffixIndex)
{
    QString suffix = version.mid(suffixIndex);
    while (!suffix.isEmpty() && QStringLiteral("-+_.").contains(suffix.front()))
    {
        suffix.removeFirst();
    }
    return suffix;
}

VersionSuffixKind suffixKind(const QString &suffix)
{
    if (suffix.isEmpty())
    {
        return VersionSuffixKind::Stable;
    }
    if (suffix.startsWith(QStringLiteral("hotfix"), Qt::CaseInsensitive))
    {
        return VersionSuffixKind::Hotfix;
    }
    return VersionSuffixKind::Prerelease;
}

int compareSuffixes(const QString &currentSuffix, const QString &latestSuffix)
{
    qsizetype currentIndex = 0;
    qsizetype latestIndex = 0;
    while (currentIndex < currentSuffix.size() &&
           latestIndex < latestSuffix.size())
    {
        const bool currentIsDigit = currentSuffix.at(currentIndex).isDigit();
        const bool latestIsDigit = latestSuffix.at(latestIndex).isDigit();
        if (currentIsDigit && latestIsDigit)
        {
            qsizetype currentEnd = currentIndex;
            qsizetype latestEnd = latestIndex;
            while (currentEnd < currentSuffix.size() &&
                   currentSuffix.at(currentEnd).isDigit())
            {
                ++currentEnd;
            }
            while (latestEnd < latestSuffix.size() &&
                   latestSuffix.at(latestEnd).isDigit())
            {
                ++latestEnd;
            }

            qsizetype currentNumberStart = currentIndex;
            qsizetype latestNumberStart = latestIndex;
            while (currentNumberStart < currentEnd - 1 &&
                   currentSuffix.at(currentNumberStart) == '0')
            {
                ++currentNumberStart;
            }
            while (latestNumberStart < latestEnd - 1 &&
                   latestSuffix.at(latestNumberStart) == '0')
            {
                ++latestNumberStart;
            }

            const qsizetype currentLength = currentEnd - currentNumberStart;
            const qsizetype latestLength = latestEnd - latestNumberStart;
            if (currentLength != latestLength)
            {
                return currentLength < latestLength ? -1 : 1;
            }

            const QStringView currentNumber(currentSuffix);
            const QStringView latestNumber(latestSuffix);
            const int numberComparison =
                currentNumber.mid(currentNumberStart, currentLength).compare(
                    latestNumber.mid(latestNumberStart, latestLength)
                );
            if (numberComparison != 0)
            {
                return numberComparison;
            }

            currentIndex = currentEnd;
            latestIndex = latestEnd;
            continue;
        }

        const QChar currentCharacter = currentSuffix.at(currentIndex).toLower();
        const QChar latestCharacter = latestSuffix.at(latestIndex).toLower();
        if (currentCharacter != latestCharacter)
        {
            return currentCharacter < latestCharacter ? -1 : 1;
        }
        ++currentIndex;
        ++latestIndex;
    }

    if (currentIndex == currentSuffix.size() &&
        latestIndex == latestSuffix.size())
    {
        return 0;
    }
    return currentIndex == currentSuffix.size() ? -1 : 1;
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent),
      versions{QCoreApplication::applicationVersion(), QStringLiteral("尚未检查")},
      networkManager(new QNetworkAccessManager(this))
{
    connect(networkManager, &QNetworkAccessManager::finished, this, &UpdateChecker::handleReply);
}

const VersionInfo &UpdateChecker::versionInfo() const
{
    return versions;
}

void UpdateChecker::markDisabled()
{
    versions.uiLatest = QStringLiteral("已禁用");
    emit versionInfoChanged(versions);
}

void UpdateChecker::check()
{
    if (checking)
    {
        return;
    }
    checking = true;
    versions.uiLatest = QStringLiteral("正在检查");
    emit versionInfoChanged(versions);

    QNetworkRequest request(QUrl("https://api.github.com/repos/"
        + ApplicationConstants::RepositoryName + "/releases/latest"));
    request.setTransferTimeout(10000);
    request.setRawHeader("Accept", "application/vnd.github+json");
    networkManager->get(request);
}

QString UpdateChecker::normalizeVersionTag(const QString &tag)
{
    return tag.startsWith('v') ? tag.mid(1) : tag;
}

std::optional<QString> UpdateChecker::parseReleaseVersion(
    const QByteArray &payload
)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }

    const QJsonValue tagValue = document.object().value("tag_name");
    if (!tagValue.isString())
    {
        return std::nullopt;
    }

    const QString version =
        normalizeVersionTag(tagValue.toString().trimmed());
    return version.isEmpty()
        ? std::nullopt
        : std::optional<QString>(version);
}

bool UpdateChecker::isNewerVersion(const QString &currentVersion, const QString &latestVersion)
{
    qsizetype currentSuffix = 0;
    qsizetype latestSuffix = 0;
    const QVersionNumber current = QVersionNumber::fromString(currentVersion, &currentSuffix);
    const QVersionNumber latest = QVersionNumber::fromString(latestVersion, &latestSuffix);

    if (latest != current)
    {
        return latest > current;
    }

    const QString currentSuffixText =
        normalizedSuffix(currentVersion, currentSuffix);
    const QString latestSuffixText =
        normalizedSuffix(latestVersion, latestSuffix);
    const VersionSuffixKind currentKind = suffixKind(currentSuffixText);
    const VersionSuffixKind latestKind = suffixKind(latestSuffixText);
    if (latestKind != currentKind)
    {
        return latestKind > currentKind;
    }
    return compareSuffixes(currentSuffixText, latestSuffixText) < 0;
}

void UpdateChecker::handleReply(QNetworkReply *reply)
{
    checking = false;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError)
    {
        const bool notFound = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404;
        const QString reason = notFound ? QStringLiteral("暂时无法读取项目的公开正式版本。")
            : QStringLiteral("无法读取项目发布信息，请稍后重试。");
        qWarning().noquote() << "检查更新失败" << reply->errorString();
        versions.uiLatest = QStringLiteral("检查失败");
        emit versionInfoChanged(versions);
        emit checkFailed(reason);
        return;
    }

    const std::optional<QString> latestVersion =
        parseReleaseVersion(reply->readAll());
    if (!latestVersion.has_value())
    {
        const QString reason =
            QStringLiteral("响应中缺少有效的版本标签");
        qWarning().noquote() << "检查更新失败" << reason;
        versions.uiLatest = QStringLiteral("检查失败");
        emit versionInfoChanged(versions);
        emit checkFailed(reason);
        return;
    }

    qInfo().noquote() << "最新发布版本" << *latestVersion;
    versions.uiLatest = *latestVersion;
    emit versionInfoChanged(versions);
    emit checkSucceeded(*latestVersion);
}
