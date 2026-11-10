#ifndef VPNPROFILEDIALOG_H
#define VPNPROFILEDIALOG_H

#include <QDialog>

#include "application/vpnprofile.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class VpnProfileDialog : public QDialog
{
public:
    explicit VpnProfileDialog(QWidget *parent);
    VpnProfile profile() const;

private:
    void validateAndAccept();

    VpnProfile editedProfile;
    QLineEdit *nameEdit;
    QComboBox *protocolCombo;
    QLineEdit *serverEdit;
    QSpinBox *portEdit;
    QLabel *help;
};

#endif // VPNPROFILEDIALOG_H
