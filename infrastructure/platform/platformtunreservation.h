#ifndef PLATFORMTUNRESERVATION_H
#define PLATFORMTUNRESERVATION_H

#include <QString>

class PlatformTunReservation
{
public:
    PlatformTunReservation() = default;
    ~PlatformTunReservation();
    PlatformTunReservation(const PlatformTunReservation &) = delete;
    PlatformTunReservation &operator=(const PlatformTunReservation &) = delete;

    static QString profileAdapterId(const QString &settingsPath);
    static QString conflictReason(const QString &adapterId);
    QString acquire(const QString &adapterId);
    void release();

private:
#if defined(Q_OS_WIN)
    void *lease = nullptr;
#endif
};

#endif // PLATFORMTUNRESERVATION_H
