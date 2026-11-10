#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

struct VersionInfo
{
    QString uiVersion;
    QString uiLatest;
};

class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    const VersionInfo &versionInfo() const;
    void markDisabled();
    void check();

    static QString normalizeVersionTag(const QString &tag);
    static std::optional<QString> parseReleaseVersion(const QByteArray &payload);
    static bool isNewerVersion(const QString &currentVersion, const QString &latestVersion);

signals:
    void versionInfoChanged(const VersionInfo &versionInfo);
    void checkFailed(const QString &reason);
    void checkSucceeded(const QString &version);

private:
    void handleReply(QNetworkReply *reply);

    VersionInfo versions;
    QNetworkAccessManager *networkManager;
    bool checking = false;
};

#endif // UPDATECHECKER_H
