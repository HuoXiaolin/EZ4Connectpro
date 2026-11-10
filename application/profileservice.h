#ifndef PROFILESERVICE_H
#define PROFILESERVICE_H

#include <QObject>
#include <QStringList>

#include <memory>

#include "application/vpnprofile.h"

class ProfileBackend;
class QSettings;

enum class ProfileFileResult
{
    Success,
    InvalidFile,
    UnsupportedProtocol,
    NewerVersion,
    ProtectedDestination,
    WriteFailed
};

class ProfileService : public QObject
{
    Q_OBJECT

public:
    explicit ProfileService(
        std::unique_ptr<ProfileBackend> backend,
        const QString &overrideConfigPath = {},
        QObject *parent = nullptr
    );
    ~ProfileService() override;

    QSettings *settings() const;
    QString currentProfileId() const;
    bool usesOverrideConfiguration() const;
    bool recoveredUnsupportedProfile() const;
    VpnProfile currentVpnProfile() const;
    QList<VpnProfile> vpnProfiles() const;

    bool switchTo(const QString &profileId);
    QString createAndSwitch(const QString &requestedName);
    bool createAndSwitch(const VpnProfile &profile);
    ProfileFileResult importProfile(const QString &filePath);
    ProfileFileResult exportCurrentProfile(const QString &filePath) const;
    bool renameDisplayName(const QString &name);
    bool renameCurrent(const QString &newProfileId);
    bool removeCurrentAndSwitchToDefault();

    QString normalizeProfileId(const QString &name) const;

private:
    bool saveNewProfile(VpnProfile profile, const QSettings *imported = nullptr);
    std::unique_ptr<ProfileBackend> profileBackend;
    QSettings *currentSettings;
    QString currentId;
    bool overrideConfiguration;
    bool startupFallback = false;
};

#endif // PROFILESERVICE_H
