#ifndef AUTOSTART_H
#define AUTOSTART_H

namespace AutoStart
{
enum class RegistrationStatus
{
    Missing,
    Registered,
    NeedsRepair,
    Unknown
};

RegistrationStatus registrationStatus();
bool setEnabled(bool enabled);
}

#endif // AUTOSTART_H
