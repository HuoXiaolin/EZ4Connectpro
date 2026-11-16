#ifndef DEVICETRUSTDIALOG_H
#define DEVICETRUSTDIALOG_H
#include <QDialog>
#include "application/vpnprofile.h"
#include "infrastructure/coreprocess/devicetrust.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class DeviceTrustDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DeviceTrustDialog(const VpnProfile &profile, QWidget *parent = nullptr,
                               DeviceTrust::Client *backend = nullptr);
    void reject() override;
private:
    void refresh();
    void display(const DeviceTrust::Snapshot &snapshot);
    void updateActions();
    void untrustSelected();
    void setStatus(const QString &text);
    DeviceTrust::Client *client;
    DeviceTrust::Snapshot snapshot;
    QLabel *summary;
    QLabel *status;
    QTableWidget *table;
    QLineEdit *identifier;
    QPushButton *refreshButton;
    QPushButton *trustButton;
    QPushButton *untrustButton;
    QPushButton *closeButton;
    bool validSnapshot = false;
    bool verifyChange = false;
    DeviceTrust::Client::Action expectedAction = DeviceTrust::Client::Action::List;
    QString expectedId;
    QString completion;
};
#endif
