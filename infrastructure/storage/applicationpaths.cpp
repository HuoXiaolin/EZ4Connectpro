#include "applicationpaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace
{
QString customProfileRoot;

QString localDataRoot()
{
    return customProfileRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        : QDir(customProfileRoot).filePath("data");
}

QString safeProfileName(const QString &profileId)
{
    QString name = profileId.isEmpty() ? "default" : profileId;
    name.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    return name;
}

void createPrivateFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
}

void ApplicationPaths::setProfileRoot(const QString &path)
{
    customProfileRoot = path.isEmpty() ? QString{} : QFileInfo(path).absoluteFilePath();
}

QString ApplicationPaths::profileRoot()
{
    return customProfileRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        : customProfileRoot;
}

QString ApplicationPaths::clientDataFile(const QString &profileId, bool createIfMissing)
{
    QDir profileDirectory(QDir(localDataRoot()).filePath("profiles/" + profileId));
    QFile clientData(profileDirectory.filePath("client-data.json"));
    if (createIfMissing && !clientData.exists())
    {
        profileDirectory.mkpath(".");
        if (clientData.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            clientData.write("{}");
        }
    }
    return clientData.fileName();
}

bool ApplicationPaths::clearClientData(const QString &profileId)
{
    QFile clientData(clientDataFile(profileId, false));
    return !clientData.exists() || clientData.remove();
}

QString ApplicationPaths::logDirectory()
{
    QDir logDirectory(
        QDir(localDataRoot())
            .filePath("logs")
    );
    if (!logDirectory.exists())
    {
        logDirectory.mkpath(".");
    }
    return logDirectory.absolutePath();
}

QString ApplicationPaths::logFile()
{
    return QDir(logDirectory()).filePath("ez4connectpro.log");
}

QString ApplicationPaths::portableLauncher()
{
#if defined(Q_OS_WIN)
    const QFileInfo launcher(qEnvironmentVariable("EZ4CONNECTPRO_LAUNCHER"));
    if (launcher.isAbsolute() && launcher.isFile() && launcher.suffix().compare("exe", Qt::CaseInsensitive) == 0)
    {
        return launcher.absoluteFilePath();
    }
#endif
    return {};
}

QString ApplicationPaths::launcherExecutable()
{
    const QString launcher = portableLauncher();
    return launcher.isEmpty() ? QCoreApplication::applicationFilePath() : launcher;
}

ApplicationPaths::DebugArtifactPaths ApplicationPaths::createDebugArtifactFiles(
    const QString &profileId,
    bool createPcap,
    bool createTlsLog
)
{
    const QString baseName = QString("debug-%1-%2")
        .arg(
            safeProfileName(profileId),
            QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")
        );
    const QDir directory(logDirectory());

    DebugArtifactPaths paths;
    if (createPcap)
    {
        paths.pcapFile = directory.filePath(baseName + ".pcap");
        createPrivateFile(paths.pcapFile);
    }
    if (createTlsLog)
    {
        paths.tlsLogFile = directory.filePath(baseName + ".keys.log");
        createPrivateFile(paths.tlsLogFile);
    }
    return paths;
}
