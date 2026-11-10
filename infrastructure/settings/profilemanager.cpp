#include "profilemanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include "infrastructure/storage/applicationpaths.h"

namespace
{
bool createEmptyFile(const QString &path)
{
    const QFileInfo fileInfo(path);
    if (fileInfo.exists())
    {
        return fileInfo.isFile();
    }

    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
}
}

ProfileManager::ProfileManager(const QString &storageRoot)
{
    configRootPath = storageRoot.isEmpty()
        ? ApplicationPaths::profileRoot()
        : storageRoot;
    profilesPath = QDir(configRootPath).filePath("profiles");
    statePath = QDir(configRootPath).filePath("state.ini");
    defaultProfilePath = QDir(configRootPath).filePath("config.ini");

    ensureStorage();
}

QStringList ProfileManager::listProfiles() const
{
    QDir profilesDir(profilesPath);
    QStringList profiles;
    for (const QFileInfo &entry : profilesDir.entryInfoList(QStringList() << "*.ini", QDir::Files, QDir::Name))
    {
        if (entry.fileName() == "state.ini")
        {
            continue;
        }
        profiles << entry.baseName();
    }
    return profiles;
}

QString ProfileManager::activeProfile() const
{
    QSettings state(statePath, QSettings::IniFormat);
    QString profileId = normalizeProfileId(state.value("Profile/Active").toString());

    if (!QFileInfo::exists(profilePath(profileId)))
    {
        profileId = "";
        if (!QFileInfo::exists(profilePath(profileId)))
        {
            createEmptyFile(profilePath(profileId));
        }
        state.setValue("Profile/Active", profileId);
        state.sync();
    }
    return profileId;
}

bool ProfileManager::setActiveProfile(const QString &profileId) const
{
    const QString normalizedId = normalizeProfileId(profileId);
    if (!QFileInfo::exists(profilePath(normalizedId)))
    {
        return false;
    }

    QSettings state(statePath, QSettings::IniFormat);
    state.setValue("Profile/Active", normalizedId);
    state.sync();
    return state.status() == QSettings::NoError;
}

QString ProfileManager::profilePath(const QString &profileId) const
{
    QString normalizedId = normalizeProfileId(profileId);
    if (normalizedId.isEmpty())
    {
        return defaultProfilePath;
    }
    return QDir(profilesPath).filePath(normalizedId + ".ini");
}

QString ProfileManager::createProfile(const QString &requestedName, const QString &sourcePath) const
{
    const QString normalizedId = ensureUniqueProfileId(normalizeProfileId(requestedName));
    const QString path = profilePath(normalizedId);

    if (!sourcePath.isEmpty() && QFileInfo::exists(sourcePath))
    {
        if (QFile::exists(path))
        {
            QFile::remove(path);
        }
        if (!QFile::copy(sourcePath, path))
        {
            return QString();
        }
    }
    else
    {
        if (!createEmptyFile(path))
        {
            return {};
        }
    }

    return normalizedId;
}

bool ProfileManager::renameProfile(const QString &oldId, const QString &newId) const
{
    const QString sourceId = normalizeProfileId(oldId);
    const QString targetId = normalizeProfileId(newId);
    if (sourceId.isEmpty() || targetId.isEmpty() || sourceId == targetId)
    {
        return false;
    }

    const QString sourcePath = profilePath(sourceId);
    const QString targetPath = profilePath(targetId);
    if (!QFileInfo::exists(sourcePath) || QFileInfo::exists(targetPath))
    {
        return false;
    }

    return QFile::rename(sourcePath, targetPath);
}

bool ProfileManager::removeProfile(const QString &profileId) const
{
    const QString normalizedId = normalizeProfileId(profileId);
    if (normalizedId.isEmpty())
    {
        return false;
    }
    const auto startup = startupOptions();
    if (startup.profileId != normalizedId)
    {
        return QFile::remove(profilePath(normalizedId));
    }
    auto updatedStartup = startup;
    updatedStartup.profileId.reset();
    if (!saveStartupOptions(updatedStartup))
    {
        return false;
    }
    if (QFile::remove(profilePath(normalizedId)))
    {
        return true;
    }
    saveStartupOptions(startup);
    return false;
}

QString ProfileManager::normalizeProfileId(const QString &name) const
{
    QString normalized = name.trimmed();
    normalized.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    normalized.replace(QRegularExpression("_+"), "_");
    if (normalized.startsWith('_'))
    {
        normalized.remove(0, 1);
    }
    if (normalized.endsWith('_'))
    {
        normalized.chop(1);
    }
    return normalized;
}

ProfileManager::StartupOptions ProfileManager::startupOptions() const
{
    QSettings state(statePath, QSettings::IniFormat);
    StartupOptions options;
    options.enabled = state.value("Global/AutoStart", false).toBool();
    options.silent = state.value("Global/SilentStart", true).toBool();
    if (state.value("Global/StartupConnect", false).toBool() && state.contains("Global/StartupProfile"))
    {
        options.profileId = state.value("Global/StartupProfile").toString();
    }
    return options;
}

bool ProfileManager::saveStartupOptions(const StartupOptions &options) const
{
    QSettings state(statePath, QSettings::IniFormat);
    state.setValue("Global/AutoStart", options.enabled);
    state.setValue("Global/SilentStart", options.silent);
    state.setValue("Global/StartupConnect", options.profileId.has_value());
    if (options.profileId)
    {
        state.setValue("Global/StartupProfile", *options.profileId);
    }
    else
    {
        state.remove("Global/StartupProfile");
    }
    state.sync();
    return state.status() == QSettings::NoError;
}

void ProfileManager::migrateStartupOptions(const QSettings &profile, const QString &profileId) const
{
    QSettings state(statePath, QSettings::IniFormat);
    if (state.contains("Global/StartupConnect"))
    {
        return;
    }
    auto options = startupOptions();
    if (!state.contains("Global/AutoStart") && profile.value("Common/ConfigVersion").toInt() == 6)
    {
        options.enabled = profile.value("Common/AutoStart", false).toBool();
    }
    if (options.enabled && profile.value("Common/ConnectAfterStart", false).toBool())
    {
        options.profileId = profileId;
    }
    saveStartupOptions(options);
}

QString ProfileManager::ensureUniqueProfileId(const QString &baseId) const
{
    QString candidate = baseId;
    if (candidate.isEmpty())
    {
        candidate = "profile";
    }

    if (!QFileInfo::exists(profilePath(candidate)))
    {
        return candidate;
    }

    int suffix = 2;
    while (QFileInfo::exists(profilePath(candidate + "_" + QString::number(suffix))))
    {
        suffix++;
    }
    return candidate + "_" + QString::number(suffix);
}

void ProfileManager::ensureStorage() const
{
    QDir configRoot(configRootPath);
    if (!configRoot.exists())
    {
        configRoot.mkpath(".");
    }

    QDir profilesDir(profilesPath);
    if (!profilesDir.exists())
    {
        profilesDir.mkpath(".");
    }
    createEmptyFile(defaultProfilePath);
}
