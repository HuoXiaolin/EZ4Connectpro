#include "profileservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QUuid>

#include <utility>

#include "application/profilebackend.h"
#include "application/applicationconstants.h"
#include "application/defaultsettings.h"

namespace
{
QString profileFileKey(const QFileInfo &file)
{
    QString path = file.canonicalFilePath();
    if (path.isEmpty())
    {
        const QString parent = file.dir().canonicalPath();
        path = parent.isEmpty() ? file.absoluteFilePath() : QDir(parent).filePath(file.fileName());
    }
#if defined(Q_OS_WIN)
    path = path.toCaseFolded();
#endif
    return path;
}
}

ProfileService::ProfileService(
    std::unique_ptr<ProfileBackend> backend,
    const QString &overrideConfigPath,
    QObject *parent
)
    : QObject(parent),
      profileBackend(std::move(backend)),
      currentSettings(nullptr),
      overrideConfiguration(!overrideConfigPath.isEmpty())
{
    if (!overrideConfiguration)
    {
        currentId = profileBackend->activeProfile();
        currentSettings = new QSettings(
            profileBackend->profilePath(currentId),
            QSettings::IniFormat
        );
        if (!currentVpnProfile().isSupported())
        {
            for (const auto &profile : vpnProfiles())
            {
                if (switchTo(profile.id))
                {
                    startupFallback = true;
                    break;
                }
            }
        }
    }
    else
    {
        currentId = "custom";
        currentSettings = new QSettings(overrideConfigPath, QSettings::IniFormat);
    }
}

ProfileService::~ProfileService()
{
    currentSettings->sync();
    delete currentSettings;
}

QSettings *ProfileService::settings() const
{
    return currentSettings;
}

QString ProfileService::currentProfileId() const
{
    return currentId;
}

bool ProfileService::usesOverrideConfiguration() const
{
    return overrideConfiguration;
}

bool ProfileService::recoveredUnsupportedProfile() const
{
    return startupFallback;
}

VpnProfile ProfileService::currentVpnProfile() const
{
    return readVpnProfile(*currentSettings, currentId);
}

QList<VpnProfile> ProfileService::vpnProfiles() const
{
    QStringList ids = profileBackend->listProfiles();
    ids.prepend(QString{});
    QList<VpnProfile> result;
    bool supportedOverride = false;
    if (overrideConfiguration)
    {
        const auto profile = currentVpnProfile();
        supportedOverride = profile.isSupported();
        if (supportedOverride)
        {
            result.append(profile);
        }
    }
    for (const QString &id : ids)
    {
        const QSettings stored(profileBackend->profilePath(id), QSettings::IniFormat);
        const auto profile = !overrideConfiguration && id == currentId
            ? currentVpnProfile() : readVpnProfile(stored, id);
        if (profile.isSupported() && (!supportedOverride
            || (id != currentId && profile.settingsPath != currentSettings->fileName())))
        {
            result.append(profile);
        }
    }
    return result;
}

bool ProfileService::switchTo(const QString &profileId)
{
    if (!overrideConfiguration && profileId == currentId)
    {
        return currentVpnProfile().isSupported();
    }

    const QString path = profileBackend->profilePath(profileId);
    if (!QFileInfo(path).isFile())
    {
        return false;
    }
    auto nextSettings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    if (!vpnProtocolNames().contains(vpnProtocolId(*nextSettings)))
    {
        return false;
    }

    currentSettings->sync();
    if (!profileBackend->setActiveProfile(profileId))
    {
        return false;
    }

    delete currentSettings;
    currentSettings = nextSettings.release();
    currentId = profileId;
    overrideConfiguration = false;
    return true;
}

QString ProfileService::createAndSwitch(const QString &requestedName)
{
    currentSettings->sync();
    const QString profileId = profileBackend->createProfile(
        requestedName,
        currentSettings->fileName()
    );
    if (profileId.isEmpty() || !switchTo(profileId))
    {
        if (!profileId.isEmpty())
        {
            profileBackend->removeProfile(profileId);
        }
        return {};
    }
    return profileId;
}

bool ProfileService::createAndSwitch(const VpnProfile &profile)
{
    return saveNewProfile(profile);
}

ProfileFileResult ProfileService::importProfile(const QString &filePath)
{
    const QFileInfo file(filePath);
    if (!file.isFile() || !file.isReadable())
    {
        return ProfileFileResult::InvalidFile;
    }
    const QSettings imported(file.absoluteFilePath(), QSettings::IniFormat);
    bool versionValid = false;
    const int version = imported.value("Common/ConfigVersion", 0).toInt(&versionValid);
    const auto profile = readVpnProfile(imported, file.completeBaseName());
    if (imported.status() != QSettings::NoError || !versionValid)
    {
        return ProfileFileResult::InvalidFile;
    }
    if (!profile.isSupported())
    {
        return ProfileFileResult::UnsupportedProtocol;
    }
    if (version > ApplicationConstants::ConfigVersion)
    {
        return ProfileFileResult::NewerVersion;
    }
    if (profile.server.isEmpty() || profile.port < 1 || profile.port > 65535)
    {
        return ProfileFileResult::InvalidFile;
    }
    return saveNewProfile(profile, &imported) ? ProfileFileResult::Success : ProfileFileResult::WriteFailed;
}

ProfileFileResult ProfileService::exportCurrentProfile(const QString &filePath) const
{
    const QFileInfo target(filePath);
    const QFileInfo source(currentSettings->fileName());
    const QFileInfo defaultProfile(profileBackend->profilePath({}));
    const QString targetKey = profileFileKey(target);
    if (targetKey == profileFileKey(source) || targetKey == profileFileKey(defaultProfile)
        || targetKey == profileFileKey(QFileInfo(defaultProfile.dir(), "state.ini")))
    {
        return ProfileFileResult::ProtectedDestination;
    }
    for (const QString &id : profileBackend->listProfiles())
    {
        if (targetKey == profileFileKey(QFileInfo(profileBackend->profilePath(id))))
        {
            return ProfileFileResult::ProtectedDestination;
        }
    }

    currentSettings->sync();
    QFile input(source.absoluteFilePath());
    QSaveFile output(target.absoluteFilePath());
    if (currentSettings->status() != QSettings::NoError
        || !input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
    {
        return ProfileFileResult::WriteFailed;
    }
    const QByteArray data = input.readAll();
    return input.error() == QFileDevice::NoError && output.write(data) == data.size() && output.commit()
        ? ProfileFileResult::Success : ProfileFileResult::WriteFailed;
}

bool ProfileService::saveNewProfile(VpnProfile profile, const QSettings *imported)
{
    if (profile.name.trimmed().isEmpty() || !profile.isSupported())
    {
        return false;
    }
    QSet<int> usedPorts;
    QSet<QString> usedNames;
    for (const auto &existing : vpnProfiles())
    {
        usedPorts.insert(existing.socksPort);
        usedPorts.insert(existing.httpPort);
        usedNames.insert(existing.name.toCaseFolded());
    }
    int socksPort = profile.socksPort;
    int httpPort = profile.httpPort;
    if (!imported || socksPort < 1 || socksPort > 65535 || httpPort < 1 || httpPort > 65535
        || socksPort == httpPort || usedPorts.contains(socksPort) || usedPorts.contains(httpPort))
    {
        socksPort = 11080;
        while (usedPorts.contains(socksPort) || usedPorts.contains(socksPort + 1))
        {
            socksPort += 2;
        }
        if (socksPort >= 65535)
        {
            return false;
        }
        httpPort = socksPort + 1;
    }

    if (imported)
    {
        const QString baseName = profile.name.left(60);
        profile.name = baseName;
        for (int number = 2; usedNames.contains(profile.name.toCaseFolded()); ++number)
        {
            const QString suffix = QStringLiteral(" (%1)").arg(number);
            profile.name = baseName.left(60 - suffix.size()) + suffix;
        }
    }
    const QString id = profileBackend->createProfile(imported
        ? "import-" + QUuid::createUuid().toString(QUuid::WithoutBraces) : profile.name);
    if (id.isEmpty())
    {
        return false;
    }
    bool saved = false;
    {
        QSettings newSettings(profileBackend->profilePath(id), QSettings::IniFormat);
        if (imported)
        {
            for (const QString &key : imported->allKeys())
            {
                newSettings.setValue(key, imported->value(key));
            }
            newSettings.remove("Global");
            newSettings.remove("Common/AutoStart");
            newSettings.remove("Common/ConnectAfterStart");
            newSettings.setValue("Common/ConfigVersion", ApplicationConstants::ConfigVersion);
        }
        else
        {
            DefaultSettings::reset(newSettings);
        }
        writeVpnProfile(newSettings, profile);
        newSettings.setValue("ZJUConnect/SOCKS5Port", socksPort);
        newSettings.setValue("ZJUConnect/HTTPPort", httpPort);
        newSettings.sync();
        saved = newSettings.status() == QSettings::NoError;
    }
    if (!saved || !switchTo(id))
    {
        profileBackend->removeProfile(id);
        return false;
    }
    return true;
}

bool ProfileService::renameDisplayName(const QString &name)
{
    if (overrideConfiguration || name.trimmed().isEmpty())
    {
        return false;
    }
    currentSettings->setValue("Profile/DisplayName", name.trimmed());
    currentSettings->sync();
    return currentSettings->status() == QSettings::NoError;
}

bool ProfileService::renameCurrent(const QString &newProfileId)
{
    if (currentId.isEmpty() || overrideConfiguration)
    {
        return false;
    }

    currentSettings->sync();
    if (!profileBackend->renameProfile(currentId, newProfileId))
    {
        return false;
    }

    const QString previousId = currentId;
    if (!profileBackend->setActiveProfile(newProfileId))
    {
        profileBackend->renameProfile(newProfileId, previousId);
        return false;
    }

    currentId = newProfileId;
    delete currentSettings;
    currentSettings = new QSettings(
        profileBackend->profilePath(currentId),
        QSettings::IniFormat
    );
    return true;
}

bool ProfileService::removeCurrentAndSwitchToDefault()
{
    if (currentId.isEmpty() || overrideConfiguration)
    {
        return false;
    }

    const QString removedProfileId = currentId;
    QString nextId;
    for (const auto &profile : vpnProfiles())
    {
        if (profile.id != removedProfileId)
        {
            nextId = profile.id;
            break;
        }
    }
    const QString nextPath = profileBackend->profilePath(nextId);
    if (!QFileInfo::exists(nextPath))
    {
        return false;
    }

    currentSettings->sync();
    if (!profileBackend->setActiveProfile(nextId))
    {
        return false;
    }

    if (!profileBackend->removeProfile(removedProfileId))
    {
        profileBackend->setActiveProfile(removedProfileId);
        return false;
    }

    delete currentSettings;
    currentSettings = new QSettings(nextPath, QSettings::IniFormat);
    currentId = nextId;
    return true;
}

QString ProfileService::normalizeProfileId(const QString &name) const
{
    return profileBackend->normalizeProfileId(name);
}
