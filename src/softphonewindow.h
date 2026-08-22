#pragma once

#include "trunkmonkey/Profile.h"

#include <QWidget>

class SipController;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

class SoftphoneWindow final : public QWidget {
    Q_OBJECT
public:
    explicit SoftphoneWindow(SipController *controller, QWidget *parent = nullptr);

public slots:
    void showAndRaise();

signals:
    void profileSaveRequested(const QString &accountId,
                              const trunkmonkey::SipProfile &profile,
                              bool savePassword);

private slots:
    void refreshAccounts();
    void refreshState();
    void refreshCalls();
    void refreshSipLog();
    void refreshLadder();
    void refreshActivity();
    void saveProfile(bool reregister);
    void dial();
    void answerSelected();
    void rejectSelected();
    void hangupSelected();
    void holdSelected();
    void resumeSelected();
    void muteSelected();
    void sendDtmf();
    void incomingCall(const QString &accountId, int id, const QString &remoteUri);
    void accountSelectionChanged();

private:
    int selectedCallId() const;
    int comboCallId(QComboBox *combo) const;
    QString selectedAccountId() const;
    void populateCallCombos();
    void loadProfileFields();
    void showError(const QString &title, const QString &message);

    SipController *m_controller = nullptr;
    QTabWidget *m_tabs = nullptr;

    QComboBox *m_account = nullptr;
    QComboBox *m_profileAccount = nullptr;
    QLabel *m_registration = nullptr;
    QLabel *m_audio = nullptr;
    QLabel *m_phoneStatus = nullptr;
    QLineEdit *m_destination = nullptr;
    QLineEdit *m_runtimeDialPrefix = nullptr;
    QLineEdit *m_callerId = nullptr;
    QCheckBox *m_autoAudio = nullptr;
    QPushButton *m_startStop = nullptr;

    QTableWidget *m_calls = nullptr;
    QLineEdit *m_dtmf = nullptr;

    QComboBox *m_logCall = nullptr;
    QPlainTextEdit *m_sipLog = nullptr;
    QComboBox *m_ladderCall = nullptr;
    QPlainTextEdit *m_ladder = nullptr;

    QLineEdit *m_profileName = nullptr;
    QLineEdit *m_domain = nullptr;
    QLineEdit *m_registrar = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_authUsername = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_displayName = nullptr;
    QLineEdit *m_outboundProxy = nullptr;
    QLineEdit *m_callerIdDomain = nullptr;
    QLineEdit *m_dialPrefix = nullptr;
    QLineEdit *m_stunServer = nullptr;
    QComboBox *m_transport = nullptr;
    QComboBox *m_identityMode = nullptr;
    QSpinBox *m_localPort = nullptr;
    QSpinBox *m_regExpires = nullptr;
    QCheckBox *m_useIce = nullptr;
    QCheckBox *m_enableSrtp = nullptr;
    QCheckBox *m_savePassword = nullptr;

    QPlainTextEdit *m_activity = nullptr;
};
