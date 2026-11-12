#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
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
    if (!startup.enabled || startup.silent || startup.profileIds != QStringList{QString{}})
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
    if (manager.startupOptions().profileIds != QStringList{QString{}})
    {
        return false;
    }
    startup.profileIds = QStringList{line};
    return manager.saveStartupOptions(startup) && manager.startupOptions().profileIds == QStringList{line}
        && manager.removeProfile(line) && manager.startupOptions().profileIds.isEmpty() && manager.startupOptions().enabled;
}

bool preservesMultipleSelectionsAndOnlyRemovesTheDeletedLine()
{
    QTemporaryDir directory;
    ProfileManager manager(directory.path());
    const QString office = manager.createProfile("office");
    const QString lab = manager.createProfile("lab");
    ProfileManager::StartupOptions startup{true, true, {QString{}, office, lab, office}};
    if (!manager.saveStartupOptions(startup)
        || manager.startupOptions().profileIds != QStringList{QString{}, office, lab}
        || !manager.setActiveProfile(lab)
        || !manager.removeProfile(office)
        || manager.startupOptions().profileIds != QStringList{QString{}, lab}
        || !manager.renameProfile(lab, "site")
        || manager.startupOptions().profileIds != QStringList{QString{}, "site"}) return false;
    startup = manager.startupOptions();
    startup.enabled = false;
    if (!manager.saveStartupOptions(startup) || manager.startupOptions().enabled
        || manager.startupOptions().profileIds != startup.profileIds) return false;
    startup.profileIds.clear();
    return manager.saveStartupOptions(startup) && manager.startupOptions().profileIds.isEmpty();
}

bool migratesSingleSelectionWithoutReadingOtherProfileStartupFlags()
{
    for (const QString id : {QString{}, QString("office")})
    {
        QTemporaryDir directory;
        ProfileManager manager(directory.path());
        QSettings state(directory.filePath("state.ini"), QSettings::IniFormat);
        state.setValue("Global/AutoStart", true);
        state.setValue("Global/SilentStart", false);
        state.setValue("Global/StartupConnect", true);
        state.setValue("Global/StartupProfile", id);
        state.sync();
        QSettings other(manager.profilePath({}), QSettings::IniFormat);
        other.setValue("Common/ConnectAfterStart", true);
        manager.migrateStartupOptions(other, "unrelated");
        const auto startup = manager.startupOptions();
        if (!startup.enabled || startup.silent || startup.profileIds != QStringList{id}) return false;
        state.sync();
        if (!state.contains("Global/StartupProfiles") || state.contains("Global/StartupProfile")) return false;
        if (!manager.saveStartupOptions({true, true, {}})) return false;
        manager.migrateStartupOptions(other, "unrelated");
        if (!manager.startupOptions().profileIds.isEmpty()) return false;
    }
    return true;
}

bool failedDeletionRestoresEverySelection()
{
    QTemporaryDir directory;
    ProfileManager manager(directory.path());
    const QString office = manager.createProfile("office");
    if (!QFile::remove(manager.profilePath(office)) || !QDir().mkpath(manager.profilePath(office))) return false;
    const ProfileManager::StartupOptions startup{true, true, {QString{}, office}};
    return manager.saveStartupOptions(startup) && !manager.removeProfile(office)
        && manager.startupOptions().profileIds == startup.profileIds;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return createsPhysicalFilesForEmptyProfiles() && keepsStartupOptionsInGlobalState()
        && preservesMultipleSelectionsAndOnlyRemovesTheDeletedLine()
        && migratesSingleSelectionWithoutReadingOtherProfileStartupFlags()
        && failedDeletionRestoresEverySelection() ? 0 : 1;
}
