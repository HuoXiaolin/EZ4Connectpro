#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include "infrastructure/settings/profilemanager.h"

namespace
{
bool createsPhysicalFilesForEmptyProfiles()
{
    QTemporaryDir directory;
    ProfileManager manager(directory.path());

    if (!manager.activeProfile().isEmpty()
        || !QFileInfo::exists(manager.profilePath(""))
        || !manager.setActiveProfile(""))
    {
        qCritical() << "default profile file was not created";
        return false;
    }

    const QString profileId = manager.createProfile("empty");
    if (profileId != "empty"
        || !QFileInfo::exists(manager.profilePath(profileId))
        || !manager.listProfiles().contains(profileId)
        || !manager.setActiveProfile(profileId))
    {
        qCritical() << "empty named profile file was not created";
        return false;
    }
    return true;
}

bool keepsStartupOptionsInGlobalState()
{
    QTemporaryDir directory;
    ProfileManager manager(directory.path());
    QSettings legacyState(directory.filePath("state.ini"), QSettings::IniFormat);
    legacyState.setValue("Global/AutoStart", true);
    legacyState.setValue("Global/SilentStart", false);
    legacyState.sync();
    QSettings defaultProfile(manager.profilePath({}), QSettings::IniFormat);
    defaultProfile.setValue("Common/ConnectAfterStart", true);
    manager.migrateStartupOptions(defaultProfile, {});
    auto startup = manager.startupOptions();
    if (!startup.enabled || startup.silent || !startup.profileId || !startup.profileId->isEmpty())
    {
        return false;
    }

    const QString line = manager.createProfile("office");
    if (line.isEmpty())
    {
        return false;
    }
    QSettings office(manager.profilePath(line), QSettings::IniFormat);
    office.setValue("Common/ConnectAfterStart", true);
    manager.migrateStartupOptions(office, line);
    if (manager.startupOptions().profileId != QString{})
    {
        return false;
    }
    startup.profileId = line;
    return manager.saveStartupOptions(startup) && manager.startupOptions().profileId == line
        && manager.removeProfile(line) && !manager.startupOptions().profileId && manager.startupOptions().enabled;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return createsPhysicalFilesForEmptyProfiles() && keepsStartupOptionsInGlobalState() ? 0 : 1;
}
