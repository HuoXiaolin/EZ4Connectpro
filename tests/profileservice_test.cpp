#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>

#include "application/profilebackend.h"
#include "application/profileservice.h"

namespace
{
class FakeProfileBackend : public ProfileBackend
{
public:
    explicit FakeProfileBackend(const QString &rootPath)
        : rootPath(rootPath)
    {
        QSettings defaultSettings(profilePath(""), QSettings::IniFormat);
        defaultSettings.setValue("ZJUConnect/Protocol", "easyconnect");
        defaultSettings.sync();
    }

    QStringList listProfiles() const override
    {
        QStringList result;
        for (const QFileInfo &file : QDir(rootPath).entryInfoList(
                 {"*.ini"},
                 QDir::Files,
                 QDir::Name
             ))
        {
            if (file.baseName() != "default")
            {
                result << file.baseName();
            }
        }
        return result;
    }

    QString activeProfile() const override
    {
        return activeId;
    }

    bool setActiveProfile(const QString &profileId) const override
    {
        if (!allowActiveProfileChange)
        {
            return false;
        }
        activeId = profileId;
        return true;
    }

    QString profilePath(const QString &profileId) const override
    {
        return QDir(rootPath).filePath(
            (profileId.isEmpty() ? QStringLiteral("default") : profileId) + ".ini"
        );
    }

    QString createProfile(
        const QString &requestedName,
        const QString &sourcePath
    ) const override
    {
        const QString id = normalizeProfileId(requestedName);
        if (!id.isEmpty() && sourcePath.isEmpty())
        {
            QFile file(profilePath(id));
            return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ? id : QString{};
        }
        return !id.isEmpty() && QFile::copy(sourcePath, profilePath(id)) ? id : QString();
    }

    bool renameProfile(
        const QString &oldId,
        const QString &newId
    ) const override
    {
        return QFile::rename(profilePath(oldId), profilePath(newId));
    }

    bool removeProfile(const QString &profileId) const override
    {
        return allowProfileRemoval && QFile::remove(profilePath(profileId));
    }

    QString normalizeProfileId(const QString &name) const override
    {
        return name.trimmed();
    }

    mutable QString activeId;
    bool allowActiveProfileChange = true;
    bool allowProfileRemoval = true;

private:
    QString rootPath;
};

bool managesCurrentProfileAsOneUnit()
{
    QTemporaryDir directory;
    auto backend = std::make_unique<FakeProfileBackend>(directory.path());
    FakeProfileBackend *fake = backend.get();
    ProfileService service(std::move(backend));
    service.settings()->setValue("Credential/Username", "alice");
    service.settings()->sync();

    fake->allowActiveProfileChange = false;
    if (!service.createAndSwitch("blocked").isEmpty()
        || service.currentProfileId() != ""
        || QFileInfo::exists(fake->profilePath("blocked")))
    {
        qCritical() << "failed activation left a created profile behind";
        return false;
    }
    fake->allowActiveProfileChange = true;

    if (!service.switchTo("missing"))
    {
        // Expected: missing profiles are rejected.
    }
    else
    {
        qCritical() << "missing profile was accepted";
        return false;
    }

    if (service.createAndSwitch("work") != "work"
        || service.currentProfileId() != "work"
        || service.settings()->value("Credential/Username").toString() != "alice")
    {
        qCritical() << "profile creation failed";
        return false;
    }

    fake->allowActiveProfileChange = false;
    QSettings *workSettings = service.settings();
    if (service.renameCurrent("blocked")
        || service.currentProfileId() != "work"
        || service.settings() != workSettings
        || !QFileInfo::exists(fake->profilePath("work"))
        || QFileInfo::exists(fake->profilePath("blocked")))
    {
        qCritical() << "failed activation did not roll back profile rename";
        return false;
    }
    fake->allowActiveProfileChange = true;

    if (!service.renameCurrent("renamed")
        || service.currentProfileId() != "renamed")
    {
        qCritical() << "profile rename failed";
        return false;
    }

    QSettings *renamedSettings = service.settings();
    fake->allowActiveProfileChange = false;
    if (service.removeCurrentAndSwitchToDefault()
        || service.currentProfileId() != "renamed"
        || service.settings() != renamedSettings
        || !QFileInfo::exists(fake->profilePath("renamed"))
        || fake->activeId != "renamed")
    {
        qCritical() << "failed default activation changed the current profile";
        return false;
    }
    fake->allowActiveProfileChange = true;

    fake->allowProfileRemoval = false;
    if (service.removeCurrentAndSwitchToDefault()
        || service.currentProfileId() != "renamed"
        || service.settings() != renamedSettings
        || !QFileInfo::exists(fake->profilePath("renamed"))
        || fake->activeId != "renamed")
    {
        qCritical() << "failed removal changed the current profile";
        return false;
    }

    fake->allowProfileRemoval = true;
    if (!service.removeCurrentAndSwitchToDefault()
        || !service.currentProfileId().isEmpty())
    {
        qCritical() << "profile removal failed";
        return false;
    }
    return true;
}

bool acceptsCustomAsARegularProfileId()
{
    QTemporaryDir directory;
    auto backend = std::make_unique<FakeProfileBackend>(directory.path());
    FakeProfileBackend *fake = backend.get();
    ProfileService service(std::move(backend));
    service.settings()->setValue("Credential/Username", "custom-user");
    service.settings()->sync();

    if (service.createAndSwitch("custom") != "custom"
        || service.usesOverrideConfiguration()
        || service.currentProfileId() != "custom")
    {
        qCritical() << "custom profile creation failed";
        return false;
    }
    if (!service.renameCurrent("renamed")
        || service.currentProfileId() != "renamed")
    {
        qCritical() << "custom profile rename failed";
        return false;
    }
    if (!service.removeCurrentAndSwitchToDefault())
    {
        qCritical() << "renamed custom profile removal failed";
        return false;
    }
    return fake->activeId.isEmpty();
}

bool switchesAwayFromAnOverrideNamedCustom()
{
    QTemporaryDir directory;
    const QString overridePath = directory.filePath("override.ini");
    const QString profilesPath = directory.filePath("profiles");
    QDir().mkpath(profilesPath);
    {
        QSettings overrideSettings(overridePath, QSettings::IniFormat);
        overrideSettings.setValue("Credential/Username", "override-user");
        overrideSettings.sync();
    }

    auto backend = std::make_unique<FakeProfileBackend>(profilesPath);
    FakeProfileBackend *fake = backend.get();
    ProfileService service(std::move(backend), overridePath);

    if (!service.usesOverrideConfiguration()
        || service.createAndSwitch("custom") != "custom"
        || service.usesOverrideConfiguration()
        || service.currentProfileId() != "custom"
        || service.settings()->fileName() != fake->profilePath("custom")
        || fake->activeId != "custom")
    {
        qCritical() << "override configuration was confused with profile custom";
        return false;
    }
    return true;
}

bool createsIndependentSangforProfilesAndPorts()
{
    QTemporaryDir directory;
    auto backend = std::make_unique<FakeProfileBackend>(directory.path());
    ProfileService service(std::move(backend));
    service.settings()->setValue("Credential/Username", "existing-account");
    service.settings()->sync();

    VpnProfile profile;
    profile.name = "办公连接";
    profile.protocolId = "atrust";
    profile.server = "vpn1.example";
    if (!service.createAndSwitch(profile))
    {
        return false;
    }
    const auto first = service.currentVpnProfile();
    if (first.name != profile.name || first.socksPort != 11082 || first.httpPort != 11083
        || !service.settings()->value("Credential/Username").toString().isEmpty()
        || !service.renameDisplayName("新办公名称") || service.currentProfileId() != first.id)
    {
        return false;
    }
    profile.name = "第二连接";
    profile.protocolId = "easyconnect";
    if (!service.createAndSwitch(profile) || service.currentVpnProfile().socksPort != 11084)
    {
        return false;
    }
    profile.name = "旧协议";
    profile.protocolId = "openvpn-gui";
    return !service.createAndSwitch(profile) && service.vpnProfiles().size() == 3;
}

bool ignoresUnsupportedProfilesWithoutRewritingThem()
{
    QTemporaryDir directory;
    auto backend = std::make_unique<FakeProfileBackend>(directory.path());
    const QString legacyPath = backend->profilePath("legacy");
    {
        QSettings legacy(legacyPath, QSettings::IniFormat);
        legacy.setValue("Profile/Adapter", "openvpn-gui");
        legacy.setValue("ZJUConnect/Protocol", "atrust");
        legacy.setValue("Common/ConnectAfterStart", true);
    }
    backend->activeId = "legacy";
    const auto legacyContents = [&legacyPath]()
    {
        QFile file(legacyPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const QByteArray original = legacyContents();
    {
        ProfileService service(std::move(backend));
        if (!service.recoveredUnsupportedProfile() || !service.currentProfileId().isEmpty()
            || service.vpnProfiles().size() != 1 || service.switchTo("legacy"))
        {
            return false;
        }
    }
    {
        ProfileService service(std::make_unique<FakeProfileBackend>(directory.path()), legacyPath);
        if (!service.usesOverrideConfiguration() || service.currentVpnProfile().isSupported()
            || service.recoveredUnsupportedProfile() || service.vpnProfiles().size() != 1
            || !service.switchTo("") || !service.currentVpnProfile().isSupported())
        {
            return false;
        }
    }
    if (original.isEmpty() || original != legacyContents())
    {
        qCritical() << "legacy profile changed during startup or switching";
        return false;
    }
    return true;
}

bool importsIndependentProfilesAndProtectsExportDestinations()
{
    QTemporaryDir storage;
    QTemporaryDir files;
    ProfileService service(std::make_unique<FakeProfileBackend>(storage.path()));
    service.settings()->setValue("Profile/DisplayName", "Office");
    const QString original = service.settings()->fileName();
    const QString importPath = files.filePath("office.ini");
    QSettings imported(importPath, QSettings::IniFormat);
    imported.setValue("Profile/DisplayName", "Office");
    imported.setValue("ZJUConnect/ServerAddress", "vpn.office.example");
    imported.setValue("ZJUConnect/Protocol", "openvpn-gui");
    imported.sync();
    if (service.importProfile(importPath) != ProfileFileResult::UnsupportedProtocol
        || service.settings()->fileName() != original || service.vpnProfiles().size() != 1)
    {
        qCritical() << "Invalid profile affected the workspace";
        return false;
    }

    imported.setValue("ZJUConnect/Protocol", "atrust");
    imported.setValue("Credential/Password", "Zml4dHVyZQ==");
    imported.setValue("Common/AutoStart", true);
    imported.setValue("Common/ConnectAfterStart", true);
    imported.sync();
    const auto importedResult = service.importProfile(importPath);
    if (importedResult != ProfileFileResult::Success)
    {
        qCritical() << "Could not import a valid profile" << static_cast<int>(importedResult);
        return false;
    }
    const auto profile = service.currentVpnProfile();
    if (profile.name != "Office (2)" || profile.server != "vpn.office.example" || profile.port != 443
        || profile.socksPort != 11082 || profile.httpPort != 11083 || service.vpnProfiles().size() != 2
        || service.settings()->contains("Common/AutoStart") || service.settings()->contains("Common/ConnectAfterStart")
        || service.exportCurrentProfile(original) != ProfileFileResult::ProtectedDestination
        || service.exportCurrentProfile(profile.settingsPath) != ProfileFileResult::ProtectedDestination)
    {
        qCritical() << "Imported profile differs from expected" << profile.name << profile.port
            << profile.socksPort << profile.httpPort << service.vpnProfiles().size();
        return false;
    }

    const QString backupPath = files.filePath("backup.ini");
    const auto exportedResult = service.exportCurrentProfile(backupPath);
    if (exportedResult != ProfileFileResult::Success)
    {
        qCritical() << "Could not export to a new file" << static_cast<int>(exportedResult);
        return false;
    }
    const QSettings exported(backupPath, QSettings::IniFormat);
    return exported.value("Credential/Password").toString() == "Zml4dHVyZQ=="
        && exported.value("ZJUConnect/ServerAddress").toString() == profile.server;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return managesCurrentProfileAsOneUnit()
        && acceptsCustomAsARegularProfileId()
        && createsIndependentSangforProfilesAndPorts()
        && importsIndependentProfilesAndProtectsExportDestinations()
        && ignoresUnsupportedProfilesWithoutRewritingThem()
        && switchesAwayFromAnOverrideNamedCustom() ? 0 : 1;
}
