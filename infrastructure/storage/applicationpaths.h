#ifndef APPLICATIONPATHS_H
#define APPLICATIONPATHS_H

#include <QString>

namespace ApplicationPaths
{
struct DebugArtifactPaths
{
    QString pcapFile;
    QString tlsLogFile;
};

QString clientDataFile(const QString &profileId, bool createIfMissing = true);
void setProfileRoot(const QString &path);
QString profileRoot();
bool clearClientData(const QString &profileId);
QString logDirectory();
QString logFile();
QString portableLauncher();
QString launcherExecutable();
DebugArtifactPaths createDebugArtifactFiles(
    const QString &profileId,
    bool createPcap,
    bool createTlsLog
);
}

#endif // APPLICATIONPATHS_H
