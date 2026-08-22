#include "mainwindow.h"
#include "platforminfo.h"

#include "chatwindow.h"
#include "appbranding.h"
#include "appicon.h"
#include "ircbackend.h"
#include "oscarbackend.h"
#include "telnetbackend.h"
#include "bbsdirectory.h"
#include "transferwindow.h"
#include "sipcontroller.h"
#include "sipbackend.h"
#include "softphonewindow.h"
#include "modernstyle.h"
#include "notificationmanager.h"
#include "filetransport.h"
#include "useractivity.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QCryptographicHash>
#include <QIcon>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QFrame>
#include <QFont>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QTextBrowser>
#include <QTabWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace {

class ConnectionDialog final : public QDialog {
public:
    explicit ConnectionDialog(const ConnectionSettings &defaults,
                              bool editing = false,
                              QWidget *parent = nullptr)
        : QDialog(parent),
          m_editing(editing),
          m_original(defaults)
    {
        setWindowTitle(m_editing
                           ? QStringLiteral("Edit Connection — %1").arg(appDisplayName())
                           : QStringLiteral("Add Connection — %1").arg(appDisplayName()));
        setModal(true);
        setMinimumWidth(400);

        auto *outer = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        outer->addLayout(form);

        m_protocol = new QComboBox(this);
        m_protocol->addItem(QStringLiteral("Select protocol…"),
                            static_cast<int>(ConnectionSettings::Protocol::Unknown));
        m_protocol->addItem(QStringLiteral("AIM / OSCAR"),
                            static_cast<int>(ConnectionSettings::Protocol::Oscar));
        m_protocol->addItem(QStringLiteral("IRC"),
                            static_cast<int>(ConnectionSettings::Protocol::Irc));
        m_protocol->addItem(QStringLiteral("Telnet / MUD / BBS"),
                            static_cast<int>(ConnectionSettings::Protocol::Telnet));
        m_protocol->addItem(QStringLiteral("SIP / VoIP"),
                            static_cast<int>(ConnectionSettings::Protocol::Sip));

        const int wanted = m_protocol->findData(static_cast<int>(defaults.protocol));
        if (wanted >= 0 && (m_editing || defaults.protocol != ConnectionSettings::Protocol::Unknown)) {
            m_protocol->setCurrentIndex(wanted);
        } else {
            m_protocol->setCurrentIndex(0);
        }
        form->addRow(QStringLiteral("Protocol:"), m_protocol);
        m_protocol->setEnabled(!m_editing);

        m_serverLabel = new QLabel(QStringLiteral("Server:"), this);
        m_server = new QLineEdit(defaults.server, this);
        form->addRow(m_serverLabel, m_server);

        m_portLabel = new QLabel(QStringLiteral("Port:"), this);
        m_port = new QSpinBox(this);
        m_port->setRange(1, 65535);
        m_port->setValue(defaults.port ? defaults.port : 5190);
        form->addRow(m_portLabel, m_port);

        m_userLabel = new QLabel(QStringLiteral("Screen name:"), this);
        m_user = new QLineEdit(defaults.username, this);
        form->addRow(m_userLabel, m_user);

        m_passwordLabel = new QLabel(QStringLiteral("Password:"), this);
        m_password = new QLineEdit(this);
        m_password->setEchoMode(QLineEdit::Password);
        m_password->setText(defaults.password);
        form->addRow(m_passwordLabel, m_password);

        m_savePassword = new QCheckBox(QStringLiteral("Save password on this computer"), this);
        m_savePassword->setChecked(defaults.savePassword);
        m_savePassword->setToolTip(
            QStringLiteral("Stores this password in the local WaffleHouse-Client settings file. "
                           "The saved value is not encrypted at rest."));
        m_savePassword->setEnabled(!m_password->text().isEmpty());
        connect(m_password, &QLineEdit::textChanged, m_savePassword,
                [this](const QString &text) {
                    m_savePassword->setEnabled(!text.isEmpty());
                    if (text.isEmpty()) m_savePassword->setChecked(false);
                });
        form->addRow(QString(), m_savePassword);

        m_secretNote = new QLabel(
            QStringLiteral(
                "Passwords are session-only unless Save password is selected. "
                "Saved passwords are stored in the local application settings and are not "
                "encrypted at rest. Leave the password blank to be prompted when you connect."),
            this);
        m_secretNote->setWordWrap(true);
        form->addRow(QString(), m_secretNote);

        m_realNameLabel = new QLabel(QStringLiteral("Real name:"), this);
        m_realName = new QLineEdit(defaults.realName, this);
        form->addRow(m_realNameLabel, m_realName);

        m_tls = new QCheckBox(QStringLiteral("Use TLS"), this);
        m_tls->setChecked(defaults.tls);
        form->addRow(QString(), m_tls);

        m_redirectHostLabel = new QLabel(QStringLiteral("Redirect host:"), this);
        m_redirectHost = new QLineEdit(defaults.redirectHost, this);
        m_redirectHost->setPlaceholderText(QStringLiteral("optional"));
        form->addRow(m_redirectHostLabel, m_redirectHost);

        m_redirectPortLabel = new QLabel(QStringLiteral("Redirect port:"), this);
        m_redirectPort = new QSpinBox(this);
        m_redirectPort->setRange(0, 65535);
        m_redirectPort->setSpecialValueText(QStringLiteral("server default"));
        m_redirectPort->setValue(defaults.redirectPort);
        form->addRow(m_redirectPortLabel, m_redirectPort);

        m_telnetTerminalLabel = new QLabel(QStringLiteral("Terminal type:"), this);
        m_telnetTerminal = new QLineEdit(
            defaults.telnetTerminalType.isEmpty()
                ? QStringLiteral("ANSI")
                : defaults.telnetTerminalType,
            this);
        form->addRow(m_telnetTerminalLabel, m_telnetTerminal);

        m_sipProfileNameLabel = new QLabel(QStringLiteral("Account label:"), this);
        m_sipProfileName = new QLineEdit(defaults.sipProfileName, this);
        m_sipProfileName->setPlaceholderText(QStringLiteral("Office SIP / PBX / trunk"));
        form->addRow(m_sipProfileNameLabel, m_sipProfileName);

        m_sipDomainLabel = new QLabel(QStringLiteral("SIP domain:"), this);
        m_sipDomain = new QLineEdit(defaults.sipDomain.isEmpty() ? defaults.server : defaults.sipDomain, this);
        form->addRow(m_sipDomainLabel, m_sipDomain);

        m_sipRegistrarLabel = new QLabel(QStringLiteral("Registrar:"), this);
        m_sipRegistrar = new QLineEdit(defaults.sipRegistrar, this);
        m_sipRegistrar->setPlaceholderText(QStringLiteral("blank = sip:<domain>"));
        form->addRow(m_sipRegistrarLabel, m_sipRegistrar);

        m_sipAuthLabel = new QLabel(QStringLiteral("Auth username:"), this);
        m_sipAuth = new QLineEdit(defaults.sipAuthUsername, this);
        m_sipAuth->setPlaceholderText(QStringLiteral("blank = SIP username"));
        form->addRow(m_sipAuthLabel, m_sipAuth);

        m_sipDisplayNameLabel = new QLabel(QStringLiteral("Display name:"), this);
        m_sipDisplayName = new QLineEdit(defaults.sipDisplayName, this);
        form->addRow(m_sipDisplayNameLabel, m_sipDisplayName);

        m_sipOutboundProxyLabel = new QLabel(QStringLiteral("Outbound proxy:"), this);
        m_sipOutboundProxy = new QLineEdit(defaults.sipOutboundProxy, this);
        m_sipOutboundProxy->setPlaceholderText(QStringLiteral("optional"));
        form->addRow(m_sipOutboundProxyLabel, m_sipOutboundProxy);

        m_sipCallerIdDomainLabel = new QLabel(QStringLiteral("Caller-ID domain:"), this);
        m_sipCallerIdDomain = new QLineEdit(defaults.sipCallerIdDomain, this);
        m_sipCallerIdDomain->setPlaceholderText(QStringLiteral("optional"));
        form->addRow(m_sipCallerIdDomainLabel, m_sipCallerIdDomain);

        m_sipDialPrefixLabel = new QLabel(QStringLiteral("Dial prefix:"), this);
        m_sipDialPrefix = new QLineEdit(defaults.sipDialPrefix, this);
        m_sipDialPrefix->setPlaceholderText(QStringLiteral("optional"));
        form->addRow(m_sipDialPrefixLabel, m_sipDialPrefix);

        m_sipStunLabel = new QLabel(QStringLiteral("STUN server:"), this);
        m_sipStun = new QLineEdit(defaults.sipStunServer, this);
        m_sipStun->setPlaceholderText(QStringLiteral("optional"));
        form->addRow(m_sipStunLabel, m_sipStun);

        m_sipTransportLabel = new QLabel(QStringLiteral("Transport:"), this);
        m_sipTransport = new QComboBox(this);
        m_sipTransport->addItems({QStringLiteral("udp"), QStringLiteral("tcp"), QStringLiteral("tls")});
        m_sipTransport->setCurrentText(defaults.sipTransport.isEmpty() ? QStringLiteral("udp") : defaults.sipTransport.toCaseFolded());
        form->addRow(m_sipTransportLabel, m_sipTransport);

        m_sipIdentityLabel = new QLabel(QStringLiteral("Caller-ID identity:"), this);
        m_sipIdentity = new QComboBox(this);
        m_sipIdentity->addItems({QStringLiteral("from"), QStringLiteral("pai"), QStringLiteral("rpid"), QStringLiteral("from+pai")});
        m_sipIdentity->setCurrentText(defaults.sipIdentityMode.isEmpty() ? QStringLiteral("from") : defaults.sipIdentityMode.toCaseFolded());
        form->addRow(m_sipIdentityLabel, m_sipIdentity);

        m_sipLocalPortLabel = new QLabel(QStringLiteral("Local SIP port:"), this);
        m_sipLocalPort = new QSpinBox(this);
        m_sipLocalPort->setRange(1, 65535);
        m_sipLocalPort->setValue(defaults.sipLocalPort ? defaults.sipLocalPort : 5060);
        form->addRow(m_sipLocalPortLabel, m_sipLocalPort);

        m_sipExpiresLabel = new QLabel(QStringLiteral("Registration expiry:"), this);
        m_sipExpires = new QSpinBox(this);
        m_sipExpires->setRange(30, 86400);
        m_sipExpires->setSuffix(QStringLiteral(" sec"));
        m_sipExpires->setValue(defaults.sipRegistrationExpires ? static_cast<int>(defaults.sipRegistrationExpires) : 300);
        form->addRow(m_sipExpiresLabel, m_sipExpires);

        m_sipIce = new QCheckBox(QStringLiteral("Enable ICE"), this);
        m_sipIce->setChecked(defaults.sipUseIce);
        form->addRow(QString(), m_sipIce);
        m_sipSrtp = new QCheckBox(QStringLiteral("Enable SRTP"), this);
        m_sipSrtp->setChecked(defaults.sipEnableSrtp);
        form->addRow(QString(), m_sipSrtp);

        m_debug = new QCheckBox(QStringLiteral("Protocol debug logging"), this);
        m_debug->setChecked(defaults.debug);
        form->addRow(QString(), m_debug);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
        buttons->button(QDialogButtonBox::Ok)->setText(
            m_editing ? QStringLiteral("Save") : QStringLiteral("Add & Connect"));
        outer->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            const ConnectionSettings candidate = settings();
            if (candidate.protocol == ConnectionSettings::Protocol::Unknown) {
                QMessageBox::warning(this,
                                     QStringLiteral("Select a protocol"),
                                     QStringLiteral("Choose AIM/OSCAR, IRC, Telnet, or SIP/VoIP."));
                return;
            }
            if (candidate.protocol == ConnectionSettings::Protocol::Telnet) {
                if (candidate.server.trimmed().isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("Missing information"), QStringLiteral("A Telnet server is required."));
                    return;
                }
            } else if (candidate.protocol == ConnectionSettings::Protocol::Sip) {
                if (candidate.sipDomain.trimmed().isEmpty() || candidate.username.trimmed().isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("Missing information"), QStringLiteral("SIP domain and SIP username/extension are required."));
                    return;
                }
                try {
                    trunkmonkey::ProfileStore::validate(sipProfileFromConnectionSettings(candidate));
                } catch (const std::exception &e) {
                    QMessageBox::warning(this, QStringLiteral("Invalid SIP Account"), QString::fromLocal8Bit(e.what()));
                    return;
                }
            } else if (candidate.server.trimmed().isEmpty() || candidate.username.trimmed().isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Missing information"), QStringLiteral("Server and account/nickname are required."));
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_protocol, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this] { updateFields(); });
        connect(m_tls, &QCheckBox::toggled, this, [this](bool enabled) {
            if (currentProtocol() == ConnectionSettings::Protocol::Irc) {
                if (enabled && m_port->value() == 6667) {
                    m_port->setValue(6697);
                } else if (!enabled && m_port->value() == 6697) {
                    m_port->setValue(6667);
                }
            }
        });

        updateFields();
    }

    ConnectionSettings settings() const
    {
        ConnectionSettings value;
        value.protocol = currentProtocol();
        value.password = m_password->text();
        value.savePassword = m_savePassword->isChecked() && !value.password.isEmpty();
        value.debug = m_debug->isChecked();

        value.server = m_server->text().trimmed();
        value.port = static_cast<quint16>(m_port->value());
        value.username = m_user->text().trimmed();
        value.realName = m_realName->text().trimmed();
        value.tls = m_tls->isChecked();
        // Buddy/contact lists belong to the saved connection and are managed
        // by their dedicated Accounts window; editing transport/login fields
        // must not silently erase them.
        value.ircBuddies = m_original.ircBuddies;
        value.sipContacts = m_original.sipContacts;
        value.redirectHost = m_redirectHost->text().trimmed();
        value.redirectPort = static_cast<quint16>(m_redirectPort->value());
        value.telnetTerminalType = m_telnetTerminal->text().trimmed();
        value.sipProfileName = m_sipProfileName->text().trimmed();
        value.sipDomain = m_sipDomain->text().trimmed();
        value.sipRegistrar = m_sipRegistrar->text().trimmed();
        value.sipAuthUsername = m_sipAuth->text().trimmed();
        value.sipDisplayName = m_sipDisplayName->text().trimmed();
        value.sipOutboundProxy = m_sipOutboundProxy->text().trimmed();
        value.sipCallerIdDomain = m_sipCallerIdDomain->text().trimmed();
        value.sipDialPrefix = m_sipDialPrefix->text().trimmed();
        value.sipStunServer = m_sipStun->text().trimmed();
        value.sipTransport = m_sipTransport->currentText();
        value.sipIdentityMode = m_sipIdentity->currentText();
        value.sipLocalPort = static_cast<quint16>(m_sipLocalPort->value());
        value.sipRegistrationExpires = static_cast<quint32>(m_sipExpires->value());
        value.sipUseIce = m_sipIce->isChecked();
        value.sipEnableSrtp = m_sipSrtp->isChecked();
        if (value.protocol == ConnectionSettings::Protocol::Sip) {
            value.server = value.sipDomain;
            value.port = value.sipLocalPort;
        }
        return value;
    }

private:
    ConnectionSettings::Protocol currentProtocol() const
    {
        return static_cast<ConnectionSettings::Protocol>(m_protocol->currentData().toInt());
    }

    void updateFields()
    {
        const auto protocol = currentProtocol();
        const bool unknown = protocol == ConnectionSettings::Protocol::Unknown;
        const bool irc = protocol == ConnectionSettings::Protocol::Irc;
        const bool oscar = protocol == ConnectionSettings::Protocol::Oscar;
        const bool telnet = protocol == ConnectionSettings::Protocol::Telnet;
        const bool sip = protocol == ConnectionSettings::Protocol::Sip;

        m_serverLabel->setVisible(!unknown && !sip);
        m_server->setVisible(!unknown && !sip);
        m_portLabel->setVisible(!unknown && !sip);
        m_port->setVisible(!unknown && !sip);
        m_userLabel->setVisible(!unknown);
        m_user->setVisible(!unknown);

        m_passwordLabel->setVisible(!telnet && !unknown);
        m_password->setVisible(!telnet && !unknown);
        m_savePassword->setVisible(!telnet && !unknown);
        m_secretNote->setVisible(!telnet && !unknown);

        m_realNameLabel->setVisible(irc);
        m_realName->setVisible(irc);
        m_tls->setVisible(irc);

        m_redirectHostLabel->setVisible(oscar);
        m_redirectHost->setVisible(oscar);
        m_redirectPortLabel->setVisible(oscar);
        m_redirectPort->setVisible(oscar);

        m_telnetTerminalLabel->setVisible(telnet);
        m_telnetTerminal->setVisible(telnet);

        const QList<QWidget *> sipWidgets = {
            m_sipProfileNameLabel, m_sipProfileName, m_sipDomainLabel, m_sipDomain,
            m_sipRegistrarLabel, m_sipRegistrar, m_sipAuthLabel, m_sipAuth,
            m_sipDisplayNameLabel, m_sipDisplayName, m_sipOutboundProxyLabel, m_sipOutboundProxy,
            m_sipCallerIdDomainLabel, m_sipCallerIdDomain, m_sipDialPrefixLabel, m_sipDialPrefix,
            m_sipStunLabel, m_sipStun, m_sipTransportLabel, m_sipTransport,
            m_sipIdentityLabel, m_sipIdentity, m_sipLocalPortLabel, m_sipLocalPort,
            m_sipExpiresLabel, m_sipExpires, m_sipIce, m_sipSrtp};
        for (QWidget *widget : sipWidgets) widget->setVisible(sip);

        if (irc) {
            m_userLabel->setText(QStringLiteral("Nickname:"));
            m_passwordLabel->setText(QStringLiteral("Server password:"));
            m_password->setPlaceholderText(QStringLiteral("optional"));
            if (m_port->value() == 5190 || m_port->value() == 23) {
                m_port->setValue(m_tls->isChecked() ? 6697 : 6667);
            }
        } else if (telnet) {
            m_userLabel->setText(QStringLiteral("Session label:"));
            m_user->setPlaceholderText(QStringLiteral("optional"));
            if (m_port->value() == 5190 || m_port->value() == 6667
                || m_port->value() == 6697) {
                m_port->setValue(23);
            }
        } else if (oscar) {
            m_userLabel->setText(QStringLiteral("Screen name:"));
            m_passwordLabel->setText(QStringLiteral("Password:"));
            m_password->setPlaceholderText(QString());
            m_user->setPlaceholderText(QString());
            if (m_port->value() == 6667 || m_port->value() == 6697 || m_port->value() == 23) m_port->setValue(5190);
        } else if (sip) {
            m_userLabel->setText(QStringLiteral("SIP username / extension:"));
            m_passwordLabel->setText(QStringLiteral("SIP password:"));
            m_user->setPlaceholderText(QStringLiteral("1001 / user / auth identity"));
            m_password->setPlaceholderText(QString());
            if (m_sipProfileName->text().trimmed().isEmpty() && !m_user->text().trimmed().isEmpty())
                m_sipProfileName->setPlaceholderText(QStringLiteral("SIP — %1").arg(m_user->text().trimmed()));
        }
    }

    bool m_editing = false;
    ConnectionSettings m_original;
    QComboBox *m_protocol = nullptr;
    QLabel *m_serverLabel = nullptr;
    QLineEdit *m_server = nullptr;
    QLabel *m_portLabel = nullptr;
    QSpinBox *m_port = nullptr;
    QLabel *m_userLabel = nullptr;
    QLineEdit *m_user = nullptr;
    QLabel *m_passwordLabel = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_savePassword = nullptr;
    QLabel *m_secretNote = nullptr;
    QLabel *m_realNameLabel = nullptr;
    QLineEdit *m_realName = nullptr;
    QCheckBox *m_tls = nullptr;
    QLabel *m_redirectHostLabel = nullptr;
    QLineEdit *m_redirectHost = nullptr;
    QLabel *m_redirectPortLabel = nullptr;
    QSpinBox *m_redirectPort = nullptr;
    QLabel *m_telnetTerminalLabel = nullptr;
    QLineEdit *m_telnetTerminal = nullptr;
    QLabel *m_sipProfileNameLabel = nullptr; QLineEdit *m_sipProfileName = nullptr;
    QLabel *m_sipDomainLabel = nullptr; QLineEdit *m_sipDomain = nullptr;
    QLabel *m_sipRegistrarLabel = nullptr; QLineEdit *m_sipRegistrar = nullptr;
    QLabel *m_sipAuthLabel = nullptr; QLineEdit *m_sipAuth = nullptr;
    QLabel *m_sipDisplayNameLabel = nullptr; QLineEdit *m_sipDisplayName = nullptr;
    QLabel *m_sipOutboundProxyLabel = nullptr; QLineEdit *m_sipOutboundProxy = nullptr;
    QLabel *m_sipCallerIdDomainLabel = nullptr; QLineEdit *m_sipCallerIdDomain = nullptr;
    QLabel *m_sipDialPrefixLabel = nullptr; QLineEdit *m_sipDialPrefix = nullptr;
    QLabel *m_sipStunLabel = nullptr; QLineEdit *m_sipStun = nullptr;
    QLabel *m_sipTransportLabel = nullptr; QComboBox *m_sipTransport = nullptr;
    QLabel *m_sipIdentityLabel = nullptr; QComboBox *m_sipIdentity = nullptr;
    QLabel *m_sipLocalPortLabel = nullptr; QSpinBox *m_sipLocalPort = nullptr;
    QLabel *m_sipExpiresLabel = nullptr; QSpinBox *m_sipExpires = nullptr;
    QCheckBox *m_sipIce = nullptr; QCheckBox *m_sipSrtp = nullptr;
    QCheckBox *m_debug = nullptr;
};

QString statusWord(bool connected)
{
    return connected ? QStringLiteral("Online") : QStringLiteral("Offline");
}

} // namespace

MainWindow::MainWindow(const ConnectionSettings &defaults, QWidget *parent)
    : QMainWindow(parent),
      m_defaults(defaults)
{
    setWindowTitle(QStringLiteral("%1 %2").arg(appEditionDisplayName(), appVersionString()));
    // Compact by default: keep the full communications workspace usable without
    // monopolizing the desktop. The window remains freely resizable.
    resize(860, 560);
    setMinimumSize(720, 480);

    m_sipController = new SipController(this);
    m_softphoneWindow = new SoftphoneWindow(m_sipController, nullptr);
    connect(m_softphoneWindow, &SoftphoneWindow::profileSaveRequested, this,
            [this](const QString &accountId, const trunkmonkey::SipProfile &profile, bool savePassword) {
                BackendState *state = stateById(accountId);
                if (!state || !state->backend
                    || state->backend->settings().protocol != ConnectionSettings::Protocol::Sip) {
                    return;
                }
                ConnectionSettings updated = state->backend->settings();
                applySipProfileToConnectionSettings(profile, updated);
                updated.savePassword = savePassword && !updated.password.isEmpty();
                state->backend->setConnectionSettings(updated);
                const ConnectionSettings applied = state->backend->settings();
                state->secretRequired = true;
                state->hasSessionSecret = !applied.password.isEmpty();
                updateConnectionItem(state);
                saveConnections();
                refreshBuddyList();
                appendActivity(state->backend, QStringLiteral("SIP account updated from Softphone Profile."));
            });

    buildUi();
    buildMenus();
    buildConnectionsWindow();
    m_transferWindow = new TransferWindow(nullptr);
    connect(m_transferWindow, &TransferWindow::cancelRequested,
            this, &MainWindow::cancelFileTransfer);
    connect(m_transferWindow, &TransferWindow::resumeRequested,
            this, &MainWindow::resumeFileTransfer);
    connect(m_transferWindow, &TransferWindow::clearRequested,
            this, &MainWindow::clearFileTransfer);
    connect(&m_directTransfers, &CpxDirectTransferManager::progress,
            this, &MainWindow::handleDirectProgress);
    connect(&m_directTransfers, &CpxDirectTransferManager::incomingFinished,
            this, &MainWindow::handleDirectIncomingFinished);
    connect(&m_directTransfers, &CpxDirectTransferManager::outgoingFinished,
            this, &MainWindow::handleDirectOutgoingFinished);
    connect(&m_directTransfers, &CpxDirectTransferManager::failed,
            this, &MainWindow::handleDirectFailure);
    buildTrayIcon();
    loadUiSettings();
    loadOptions();

    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();
    qApp->installEventFilter(this);
    m_presenceTimer = new QTimer(this);
    m_presenceTimer->setInterval(1000);
    connect(m_presenceTimer, &QTimer::timeout, this, &MainWindow::updateAutoPresence);
    m_presenceTimer->start();

    m_secureReady = m_secure.initialize(&m_secureError);
    if (m_secureReady) {
        QString roomError;
        if (!m_secureRooms.initialize(&roomError)) {
            m_secureReady = false;
            m_secureError = roomError;
        }
    }
    m_fileTransferTimer = new QTimer(this);
    m_fileTransferTimer->setInterval(100);
    connect(m_fileTransferTimer, &QTimer::timeout, this, &MainWindow::pumpFileTransfers);
    m_fileTransferTimer->start();
    loadConnections();
    m_sipController->initialize();
    connect(m_sipController, &SipController::accountsChanged, this, &MainWindow::refreshSoftphoneControls);
    connect(m_sipController, &SipController::accountStateChanged, this, [this](const QString &) { refreshBuddyList(); refreshSoftphoneControls(); });
    connect(m_sipController, &SipController::callsChanged, this, [this] { refreshBuddyList(); refreshSoftphoneControls(); });
    connect(m_sipController, &SipController::incomingCall, this, [this](const QString &accountId, int, const QString &) {
        m_sipController->setSelectedAccountId(accountId);
        refreshBuddyList(); refreshSoftphoneControls();
    });
    refreshSoftphoneControls();
    applyTheme();

    setWindowOpacity(m_buddyOpacity);
    if (m_connectionsWindow) {
        m_connectionsWindow->setWindowOpacity(m_connectionsOpacity);
    }

    updateActions();
    refreshBuddyList();

    if (m_connectionList && m_connectionList->count() == 0) {
        statusBar()->showMessage(
            QStringLiteral("No saved connections. Use Connection → Add to create one."));
    } else if (m_connectionList) {
        statusBar()->showMessage(
            QStringLiteral("%1 saved connection(s) restored.")
                .arg(m_connectionList->count()));
    }

    if (!m_secureReady && !m_secureError.isEmpty()) {
        if (m_activity) {
            m_activity->appendPlainText(
                QStringLiteral("[security] Encrypted communications unavailable: %1").arg(m_secureError));
        }
    }
}

MainWindow::~MainWindow()
{
    m_quitting = true;
    if (qApp) qApp->removeEventFilter(this);

    const auto windows = m_windows.values();
    for (ChatWindow *window : windows) {
        if (window) {
            window->close();
        }
    }

    const auto states = m_states.values();
    for (BackendState *state : states) {
        if (state && state->backend) {
            QObject::disconnect(state->backend, nullptr, this, nullptr);
            state->backend->stop();
        }
    }

    for (BackendState *state : states) {
        delete state;
    }
    m_states.clear();

    delete m_transferWindow;
    m_transferWindow = nullptr;

    delete m_softphoneWindow;
    m_softphoneWindow = nullptr;

    delete m_connectionsWindow;
    m_connectionsWindow = nullptr;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
        case QEvent::TabletPress:
            markUserActivity();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::markUserActivity()
{
    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();
    for (BackendState *state : m_states) {
        if (!state || state->autoPresenceState.isEmpty() || !state->connected || !state->backend) continue;
        if (auto *oscar = qobject_cast<OscarBackend *>(state->backend)) {
            oscar->setBack();
            state->autoPresenceState.clear();
        }
    }
}

void MainWindow::updateAutoPresence()
{
    if (!m_options.autoPresenceEnabled || m_lastUserActivityMs <= 0) return;
    const qint64 inactiveSeconds = UserActivity::idleMilliseconds(m_lastUserActivityMs) / 1000;
    const qint64 idleThreshold = static_cast<qint64>(m_options.autoIdleMinutes) * 60;
    const qint64 awayThreshold = static_cast<qint64>(m_options.autoAwayMinutes) * 60;

    for (BackendState *state : m_states) {
        if (!state || !state->connected || !state->backend
            || state->backend->settings().protocol != ConnectionSettings::Protocol::Oscar) continue;
        auto *oscar = qobject_cast<OscarBackend *>(state->backend);
        if (!oscar) continue;

        // Respect manual Away/AFK/Idle. Automation only manages ONLINE or a
        // presence state that it set itself.
        const bool managed = !state->autoPresenceState.isEmpty();
        const bool manuallyChanged = !managed
            && (state->presenceState.compare(QStringLiteral("ONLINE"), Qt::CaseInsensitive) != 0
                || state->idleSeconds > 0);
        if (manuallyChanged) continue;
        if (managed && inactiveSeconds < idleThreshold) {
            oscar->setBack();
            state->autoPresenceState.clear();
            continue;
        }

        if (inactiveSeconds >= awayThreshold) {
            if (state->autoPresenceState != QStringLiteral("AWAY")) {
                oscar->setAwayMessage(QStringLiteral("Auto-away — inactive for %1 minutes")
                                          .arg(m_options.autoAwayMinutes));
                oscar->setIdleSeconds(static_cast<quint32>(std::min<qint64>(inactiveSeconds, 0xffffffffLL)));
                state->autoPresenceState = QStringLiteral("AWAY");
            }
        } else if (inactiveSeconds >= idleThreshold) {
            if (state->autoPresenceState.isEmpty()) {
                oscar->setIdleSeconds(static_cast<quint32>(std::min<qint64>(inactiveSeconds, 0xffffffffLL)));
                state->autoPresenceState = QStringLiteral("IDLE");
            }
        }
    }
}

void MainWindow::requestClientVersion(BackendState *state, const QString &target)
{
    if (!state || !state->connected || !state->backend || target.trimmed().isEmpty()) return;
    const QString clean = target.trimmed();
    const QString key = state->profileId + QChar(0x1f) + clean.toCaseFolded();
    m_pendingVersionQueries.insert(key);
    const auto protocol = state->backend->settings().protocol;
    QTimer::singleShot(3500, this, [this, key, clean, protocol] {
        if (!m_pendingVersionQueries.remove(key)) return;
        const QString report = protocol == ConnectionSettings::Protocol::Oscar
            ? QStringLiteral("[version] %1: no 3.2-Termux reply; peer may be an older WaffleHouse/CPX client or another AIM client (exact version unavailable)").arg(clean)
            : QStringLiteral("[version] %1: no CTCP VERSION reply received").arg(clean);
        statusBar()->showMessage(report, 7000);
    });
    if (auto *irc = qobject_cast<IrcBackend *>(state->backend)) {
        irc->requestClientVersion(clean);
        statusBar()->showMessage(QStringLiteral("Version query sent to %1 via IRC CTCP.").arg(clean), 4000);
        return;
    }
    if (auto *oscar = qobject_cast<OscarBackend *>(state->backend)) {
        oscar->requestClientVersion(clean);
        statusBar()->showMessage(QStringLiteral("WaffleHouse version query sent to %1 via AIM.").arg(clean), 4000);
        return;
    }
    m_pendingVersionQueries.remove(key);
    statusBar()->showMessage(QStringLiteral("/version is available for AIM/OSCAR and IRC peers."), 4000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveConnections();
    saveUiSettings();

    if (!m_quitting && m_trayIcon && m_trayIcon->isVisible()) {
        hide();
        event->ignore();
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_trayIcon->showMessage(
                QStringLiteral("%1 is still running").arg(appDisplayName()),
                QStringLiteral("Use the tray icon to reopen the Buddy List or quit %1.").arg(appDisplayName()),
                QSystemTrayIcon::Information,
                3500);
        }
        return;
    }

    if (!m_quitting) {
        m_quitting = true;
        QApplication::quit();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("ModernRoot"));

    auto *shell = new QHBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    // WaffleHouse 3.0: persistent application rail.  It exposes the most common
    // workflows without taking any capability away from the traditional menus.
    auto *sidebar = new QFrame(central);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(184);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(14, 16, 14, 14);
    side->setSpacing(8);

    auto *brand = new QLabel(QStringLiteral("WAFFLEHOUSE"), sidebar);
    brand->setObjectName(QStringLiteral("BrandTitle"));
    auto *edition = new QLabel(QStringLiteral("CLIENT %1").arg(appVersionString().toUpper()), sidebar);
    edition->setObjectName(QStringLiteral("BrandVersion"));
    side->addWidget(brand);
    side->addWidget(edition);
    side->addSpacing(12);

    auto makeNav = [sidebar](const QString &text, bool checked = false) {
        auto *button = new QPushButton(text, sidebar);
        button->setProperty("nav", true);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setChecked(checked);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    auto *navDashboard = makeNav(QStringLiteral("  Dashboard"), true);
    auto *navMessages = makeNav(QStringLiteral("  Communications"));
    auto *navSoftphone = makeNav(QStringLiteral("  Softphone"));
    auto *navConnections = makeNav(QStringLiteral("  Connections"));
    auto *navTransfers = makeNav(QStringLiteral("  File Transfers"));
    side->addWidget(navDashboard);
    side->addWidget(navMessages);
    side->addWidget(navSoftphone);
    side->addWidget(navConnections);
    side->addWidget(navTransfers);
    side->addStretch(1);

    // Primary creation action lives in the application rail so the top bar stays
    // focused on page context.  Keep it directly above Settings for a predictable
    // lower-sidebar action cluster.
    auto *addConnection = new QPushButton(QStringLiteral("+ New Connection"), sidebar);
    addConnection->setProperty("role", "primary");
    addConnection->setCursor(Qt::PointingHandCursor);
    side->addWidget(addConnection);

    // Settings intentionally uses the normal application button treatment rather
    // than the transparent/checkable navigation style.
    auto *navSettings = new QPushButton(QStringLiteral("Settings"), sidebar);
    navSettings->setCursor(Qt::PointingHandCursor);
    side->addWidget(navSettings);

    shell->addWidget(sidebar);

    auto *content = new QWidget(central);
    auto *outer = new QVBoxLayout(content);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    auto *topBar = new QFrame(content);
    topBar->setObjectName(QStringLiteral("TopBar"));
    auto *top = new QHBoxLayout(topBar);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(12);

    auto *titleBlock = new QVBoxLayout;
    titleBlock->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("Communications Hub"), topBar);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto *subtitle = new QLabel(
        QStringLiteral("AIM / OSCAR  •  IRC  •  Telnet / BBS  •  SIP / VoIP"), topBar);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    titleBlock->addWidget(title);
    titleBlock->addWidget(subtitle);
    top->addLayout(titleBlock, 1);

    outer->addWidget(topBar);

    auto *workspace = new QHBoxLayout;
    workspace->setSpacing(12);

    // Primary account/buddy card.
    auto *buddyCard = new QFrame(content);
    buddyCard->setObjectName(QStringLiteral("Card"));
    auto *buddyLayout = new QVBoxLayout(buddyCard);
    buddyLayout->setContentsMargins(12, 12, 12, 12);
    buddyLayout->setSpacing(10);

    auto *buddyHeader = new QHBoxLayout;
    auto *buddyTitle = new QLabel(QStringLiteral("Accounts & Buddies"), buddyCard);
    buddyTitle->setObjectName(QStringLiteral("CardTitle"));
    auto *buddyHint = new QLabel(QStringLiteral("Double-click a buddy to start communicating"), buddyCard);
    buddyHint->setObjectName(QStringLiteral("Muted"));
    buddyHeader->addWidget(buddyTitle);
    buddyHeader->addStretch(1);
    buddyHeader->addWidget(buddyHint);
    buddyLayout->addLayout(buddyHeader);

    m_buddyTree = new QTreeWidget(buddyCard);
    m_buddyTree->setHeaderLabels({QStringLiteral("Buddy / Account"), QStringLiteral("Status")});
    m_buddyTree->setRootIsDecorated(true);
    m_buddyTree->setAlternatingRowColors(false);
    m_buddyTree->setUniformRowHeights(true);
    m_buddyTree->setAnimated(true);
    m_buddyTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_buddyTree->setIndentation(18);
    buddyLayout->addWidget(m_buddyTree, 1);
    workspace->addWidget(buddyCard, 3);

    // The main Communications Hub intentionally stays focused on accounts and buddies.
    // SIP accounts remain visible in this tree, but the embedded quick-dial/phone card
    // is intentionally removed. The full Softphone stays available from the left rail,
    // Tools > Open Softphone, and the tray menu.

    outer->addLayout(workspace, 1);
    shell->addWidget(content, 1);

    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("%1 %2 ready").arg(appEditionDisplayName(), appVersionString()));

    connect(addConnection, &QPushButton::clicked, this,
            [this] { openConnectionDialog(m_defaults, nullptr); });
    connect(navDashboard, &QPushButton::clicked, this, [this] {
        showBuddyWindow();
        if (m_buddyTree) m_buddyTree->setFocus();
    });
    connect(navMessages, &QPushButton::clicked, this, &MainWindow::newIm);
    connect(navSoftphone, &QPushButton::clicked, this, [this] {
        if (m_softphoneWindow) m_softphoneWindow->showAndRaise();
    });
    connect(navConnections, &QPushButton::clicked, this, &MainWindow::showConnectionsWindow);
    connect(navTransfers, &QPushButton::clicked, this, &MainWindow::showTransferWindow);
    connect(navSettings, &QPushButton::clicked, this, &MainWindow::showOptionsDialog);

    connect(m_buddyTree, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *current) {
                if (BackendState *state = stateFromBuddyItem(current)) {
                    if (m_connectionList && state->connectionItem
                        && m_connectionList->currentItem() != state->connectionItem) {
                        m_connectionList->setCurrentItem(state->connectionItem);
                    }
                }
                updateActions();
            });

    connect(m_buddyTree, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) {
                QTreeWidgetItem *item = m_buddyTree->itemAt(pos);
                if (!item || item->parent()) return; // account rows only
                BackendState *state = stateFromBuddyItem(item);
                if (!state) return;
                m_buddyTree->setCurrentItem(item);
                showAccountContextMenu(state, m_buddyTree->viewport()->mapToGlobal(pos));
            });

    connect(m_buddyTree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) {
                BackendState *state = stateFromBuddyItem(item);
                if (!state || !state->backend) return;
                selectState(state);
                if (state->backend->settings().protocol == ConnectionSettings::Protocol::Sip) {
                    m_sipController->setSelectedAccountId(state->backend->id());
                    if (item && item->parent()) {
                        const QString contact = item->data(0, Qt::UserRole + 1).toString().trimmed();
                        const QVariant callData = item->data(0, Qt::UserRole + 2);
                        if (!contact.isEmpty()) {
                            if (m_buddySipAccount) {
                                const int idx = m_buddySipAccount->findData(state->backend->id());
                                if (idx >= 0) m_buddySipAccount->setCurrentIndex(idx);
                            }
                            if (m_buddyDial) {
                                m_buddyDial->setText(contact);
                                m_buddyDial->setFocus();
                            }
                            return;
                        }
                        if (callData.isValid()) {
                            m_softphoneWindow->showAndRaise();
                            return;
                        }
                    }
                    return;
                }
                if (item && item->parent() && state->connected) {
                    const QString buddy = item->data(0, Qt::UserRole + 1).toString();
                    if (!buddy.isEmpty()) ensureConversationWindow(state->backend, QStringLiteral("im"), buddy, true);
                }
            });
}

void MainWindow::buildMenus()
{
    // Keep the menu bar owned by the WaffleHouse window instead of allowing a
    // desktop/global-menu integration to export it elsewhere. This also makes
    // menu behavior consistent between X11, Wayland, Linux, and FreeBSD.
    QMenuBar *bar = menuBar();
    bar->setNativeMenuBar(false);
    bar->setVisible(true);

    QMenu *connectionMenu = bar->addMenu(QStringLiteral("&Connection"));
    m_addConnectionAction = connectionMenu->addAction(QStringLiteral("&Add…"));
    m_importBbsAction = connectionMenu->addAction(QStringLiteral("Import &BBS List…"));
    m_editConnectionAction = connectionMenu->addAction(QStringLiteral("&Edit Selected…"));
    m_deleteConnectionAction = connectionMenu->addAction(QStringLiteral("&Delete Selected"));
    connectionMenu->addSeparator();
    m_connectAction = connectionMenu->addAction(QStringLiteral("&Connect Selected"));
    m_disconnectAction = connectionMenu->addAction(QStringLiteral("&Disconnect Selected"));
    connectionMenu->addSeparator();
    QAction *quitAction = connectionMenu->addAction(QStringLiteral("&Quit"));

    // Accounts is rebuilt from the saved WaffleHouse connection model. Each
    // account gets its own submenu so buddy/contact and conversation actions
    // cannot accidentally target whichever account happened to be selected in
    // another window.
    m_accountsMenu = bar->addMenu(QStringLiteral("&Accounts"));
    connect(m_accountsMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildAccountsMenu);

    QMenu *viewMenu = bar->addMenu(QStringLiteral("&View"));

    // Themes are available directly from View -> Theme as well as from the full
    // Options dialog. The direct menu makes theme switching quick and obvious.
    QMenu *themeMenu = viewMenu->addMenu(QStringLiteral("&Theme"));
    auto *themeGroup = new QActionGroup(themeMenu);
    themeGroup->setExclusive(true);

    struct ThemeEntry {
        const char *label;
        const char *id;
    };
    static constexpr ThemeEntry themeEntries[] = {
        {"System", "system"}, {"Hacker", "hacker"}, {"Matrix", "matrix"},
        {"Phosphor", "phosphor"}, {"Midnight", "midnight"}, {"Amber", "amber"},
        {"Ice", "ice"}, {"Classic Light", "classic-light"}, {"Solarized", "solarized"},
        {"Solarized Dark", "solarized-dark"}, {"Dracula", "dracula"}, {"Nord", "nord"},
        {"Cyberpunk", "cyberpunk"}, {"Blood Moon", "blood-moon"}, {"Ocean", "ocean"},
        {"Retro Blue", "retro-blue"}, {"Monochrome", "monochrome"},
        {"Blue Box", "blue-box"}, {"Red Box", "red-box"}, {"Beige Box", "beige-box"},
        {"2600", "2600"}, {"WarGames", "wargames"}, {"CRT Green", "crt-green"},
        {"VT220", "vt220"}, {"Cobalt", "cobalt"}, {"Vaporwave", "vaporwave"},
        {"Stealth", "stealth"}, {"Synthwave", "synthwave"}, {"C64", "c64"},
        {"DOS", "dos"}, {"Waffle Iron", "waffle-iron"}, {"Ghostline", "ghostline"},
        {"Hot Dog Stand", "hot-dog-stand"}, {"Neon Miami", "neon-miami"},
    };

    for (const ThemeEntry &entry : themeEntries) {
        QAction *action = themeMenu->addAction(QString::fromLatin1(entry.label));
        action->setCheckable(true);
        action->setData(QString::fromLatin1(entry.id));
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, action] {
            m_options.theme = action->data().toString();
            saveOptions();
            applyTheme();
            statusBar()->showMessage(
                QStringLiteral("Theme changed to %1.").arg(action->text()), 2500);
        });
    }

    // loadOptions() runs after the menu is constructed, so refresh the check mark
    // immediately before the submenu opens instead of assuming the startup value.
    connect(themeMenu, &QMenu::aboutToShow, this, [this, themeMenu] {
        for (QAction *action : themeMenu->actions()) {
            action->setChecked(action->data().toString() == m_options.theme);
        }
    });

    viewMenu->addSeparator();
    m_transferWindowAction = viewMenu->addAction(QStringLiteral("&File Transfers…"));
    viewMenu->addSeparator();
    m_buddyTransparencyAction =
        viewMenu->addAction(QStringLiteral("Buddy List &Transparency…"));
    m_connectionsTransparencyAction =
        viewMenu->addAction(QStringLiteral("Connections Window T&ransparency…"));

    QMenu *toolsMenu = bar->addMenu(QStringLiteral("&Tools"));
    m_phoneAction = toolsMenu->addAction(QStringLiteral("Open &Softphone…"));
    m_showConnectionsAction = toolsMenu->addAction(QStringLiteral("Show &Connections Window"));
    toolsMenu->addSeparator();
    m_changePasswordAction = toolsMenu->addAction(QStringLiteral("Change AIM &Password…"));
    m_fingerprintAction = toolsMenu->addAction(QStringLiteral("Secure Identity &Fingerprint…"));
    toolsMenu->addSeparator();
    m_rawAction = toolsMenu->addAction(QStringLiteral("Send &Raw Protocol Command…"));
    toolsMenu->addSeparator();
    m_optionsAction = toolsMenu->addAction(QStringLiteral("&Options…"));
    m_optionsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    m_optionsAction->setStatusTip(
        QStringLiteral("Open application options, themes, and security settings"));

    QMenu *helpMenu = bar->addMenu(QStringLiteral("&Help"));
    QAction *helpAction = helpMenu->addAction(QStringLiteral("%1 &Help…").arg(appDisplayName()));
    QAction *aboutAction = helpMenu->addAction(QStringLiteral("&About %1").arg(appDisplayName()));

    connect(m_addConnectionAction, &QAction::triggered,
            this, [this] { openConnectionDialog(m_defaults, nullptr); });
    connect(m_importBbsAction, &QAction::triggered,
            this, &MainWindow::importBbsList);
    connect(m_editConnectionAction, &QAction::triggered,
            this, &MainWindow::editSelected);
    connect(m_deleteConnectionAction, &QAction::triggered,
            this, &MainWindow::deleteSelected);
    connect(m_connectAction, &QAction::triggered,
            this, &MainWindow::connectSelected);
    connect(m_disconnectAction, &QAction::triggered,
            this, &MainWindow::disconnectSelected);
    connect(m_showConnectionsAction, &QAction::triggered,
            this, &MainWindow::showConnectionsWindow);
    connect(m_phoneAction, &QAction::triggered,
            m_softphoneWindow, &SoftphoneWindow::showAndRaise);
    connect(quitAction, &QAction::triggered,
            this, &MainWindow::quitApplication);

    connect(m_changePasswordAction, &QAction::triggered,
            this, &MainWindow::changePassword);
    connect(m_fingerprintAction, &QAction::triggered,
            this, &MainWindow::showSelectedFingerprint);
    connect(m_transferWindowAction, &QAction::triggered,
            this, &MainWindow::showTransferWindow);
    connect(m_buddyTransparencyAction, &QAction::triggered,
            this, &MainWindow::setBuddyTransparency);
    connect(m_connectionsTransparencyAction, &QAction::triggered,
            this, &MainWindow::setConnectionsTransparency);
    connect(m_rawAction, &QAction::triggered,
            this, &MainWindow::rawProtocolCommand);
    connect(m_optionsAction, &QAction::triggered,
            this, &MainWindow::showOptionsDialog);
    connect(helpAction, &QAction::triggered,
            this, &MainWindow::showHelpDialog);

    connect(aboutAction, &QAction::triggered, this, [this] {
        const QString aboutHtml = QStringLiteral(
                "<pre>%1</pre>"
                "<b>%2 — Version %3</b><br><br>"
                "A native C++/Qt communications client for AIM/OSCAR, IRC, Telnet/MUD/BBS, and SIP/VoIP calls.<br><br>"
                "%2 supports optional CPX3-compatible encrypted private messages with fingerprint verification.")
                .arg(appAsciiLogo().toHtmlEscaped(), appDisplayName(), appVersionString());
        QMessageBox::about(this, QStringLiteral("About %1").arg(appDisplayName()), aboutHtml);
    });
}

QString MainWindow::accountMenuLabel(BackendState *state) const
{
    if (!state || !state->backend) return QStringLiteral("Unknown account");
    const ConnectionSettings &cfg = state->backend->settings();
    QString name;
    if (cfg.protocol == ConnectionSettings::Protocol::Sip) {
        name = cfg.sipProfileName.trimmed();
        if (name.isEmpty() && !cfg.username.trimmed().isEmpty()) {
            const QString domain = cfg.sipDomain.trimmed().isEmpty() ? cfg.server.trimmed() : cfg.sipDomain.trimmed();
            name = domain.isEmpty() ? cfg.username.trimmed()
                                    : QStringLiteral("%1@%2").arg(cfg.username.trimmed(), domain);
        }
    } else {
        name = state->identity.trimmed();
        if (name.isEmpty()) name = cfg.username.trimmed();
        if (name.isEmpty()) name = cfg.server.trimmed();
    }
    if (name.isEmpty()) name = state->backend->protocolName();
    return QStringLiteral("%1 / %2").arg(name, state->backend->protocolName());
}


void MainWindow::showAccountContextMenu(BackendState *state, const QPoint &globalPos)
{
    if (!state || !state->backend) return;
    selectState(state);

    const QString backendId = state->backend->id();
    const auto protocol = state->backend->settings().protocol;
    QMenu menu(this);

    QAction *toggle = menu.addAction(
        state->connected || state->connecting
            ? QStringLiteral("Disconnect")
            : (protocol == ConnectionSettings::Protocol::Sip
                   ? QStringLiteral("Connect / Register")
                   : QStringLiteral("Connect")));
    connect(toggle, &QAction::triggered, this, [this, backendId] {
        if (BackendState *target = stateById(backendId)) {
            selectState(target);
            if (target->connected || target->connecting) disconnectSelected();
            else connectSelected();
        }
    });

    if (protocol == ConnectionSettings::Protocol::Oscar
        || protocol == ConnectionSettings::Protocol::Irc) {
        menu.addSeparator();

        QAction *startIm = menu.addAction(QStringLiteral("Start IM…"));
        startIm->setEnabled(state->connected);
        connect(startIm, &QAction::triggered, this, [this, backendId] {
            if (BackendState *target = stateById(backendId)) {
                selectState(target);
                openMessagingDialog(target, QString(), false);
            }
        });

        QAction *joinChat = menu.addAction(
            protocol == ConnectionSettings::Protocol::Irc
                ? QStringLiteral("Join IRC Channel…")
                : QStringLiteral("Join AIM Chat…"));
        joinChat->setEnabled(state->connected);
        connect(joinChat, &QAction::triggered, this, [this, backendId] {
            if (BackendState *target = stateById(backendId)) {
                selectState(target);
                openMessagingDialog(target, QString(), true);
            }
        });

        QAction *buddies = menu.addAction(QStringLiteral("Add / Remove Buddies…"));
        connect(buddies, &QAction::triggered, this, [this, backendId] {
            if (BackendState *target = stateById(backendId)) {
                selectState(target);
                openBuddyManager(target);
            }
        });
    }

    menu.addSeparator();
    QAction *edit = menu.addAction(QStringLiteral("Edit Connection…"));
    edit->setEnabled(!state->connected && !state->connecting);
    connect(edit, &QAction::triggered, this, [this, backendId] {
        if (BackendState *target = stateById(backendId)) {
            selectState(target);
            openConnectionDialog(target->backend->settings(), target);
        }
    });

    menu.exec(globalPos);
}

void MainWindow::rebuildAccountsMenu()
{
    if (!m_accountsMenu) return;
    m_accountsMenu->clear();

    QList<BackendState *> states = m_states.values();
    std::sort(states.begin(), states.end(), [this](BackendState *a, BackendState *b) {
        return accountMenuLabel(a).compare(accountMenuLabel(b), Qt::CaseInsensitive) < 0;
    });

    if (states.isEmpty()) {
        QAction *empty = m_accountsMenu->addAction(QStringLiteral("No saved accounts"));
        empty->setEnabled(false);
        return;
    }

    for (BackendState *state : states) {
        if (!state || !state->backend) continue;
        const QString backendId = state->backend->id();
        const auto protocol = state->backend->settings().protocol;
        QMenu *account = m_accountsMenu->addMenu(accountMenuLabel(state));

        QAction *select = account->addAction(QStringLiteral("Select Account"));
        connect(select, &QAction::triggered, this, [this, backendId] {
            if (BackendState *target = stateById(backendId)) selectState(target);
        });

        QAction *edit = account->addAction(QStringLiteral("Edit Connection…"));
        edit->setEnabled(!state->connected && !state->connecting);
        connect(edit, &QAction::triggered, this, [this, backendId] {
            if (BackendState *target = stateById(backendId)) {
                selectState(target);
                openConnectionDialog(target->backend->settings(), target);
            }
        });

        QAction *toggle = account->addAction(
            state->connected || state->connecting
                ? QStringLiteral("Disconnect")
                : QStringLiteral("Connect"));
        connect(toggle, &QAction::triggered, this, [this, backendId] {
            if (BackendState *target = stateById(backendId)) {
                selectState(target);
                if (target->connected || target->connecting) disconnectSelected();
                else connectSelected();
            }
        });

        if (protocol == ConnectionSettings::Protocol::Oscar
            || protocol == ConnectionSettings::Protocol::Irc) {
            account->addSeparator();
            QAction *conversation = account->addAction(QStringLiteral("IM / Chatroom…"));
            conversation->setEnabled(state->connected);
            connect(conversation, &QAction::triggered, this, [this, backendId] {
                if (BackendState *target = stateById(backendId)) {
                    selectState(target);
                    openMessagingDialog(target);
                }
            });

            QAction *buddies = account->addAction(QStringLiteral("Add / Remove Buddies…"));
            connect(buddies, &QAction::triggered, this, [this, backendId] {
                if (BackendState *target = stateById(backendId)) {
                    selectState(target);
                    openBuddyManager(target);
                }
            });

            if (protocol == ConnectionSettings::Protocol::Oscar) {
                QAction *presence = account->addAction(QStringLiteral("Set AIM Status / AFK…"));
                presence->setEnabled(state->connected);
                connect(presence, &QAction::triggered, this, [this, backendId] {
                    if (BackendState *target = stateById(backendId)) {
                        selectState(target);
                        setAimPresence(target);
                    }
                });
            }

            if (protocol == ConnectionSettings::Protocol::Irc) {
                QAction *nick = account->addAction(QStringLiteral("Change IRC Nickname…"));
                nick->setEnabled(state->connected);
                connect(nick, &QAction::triggered, this, [this, backendId] {
                    if (BackendState *target = stateById(backendId)) {
                        selectState(target);
                        changeIrcNick();
                    }
                });
            }
        } else if (protocol == ConnectionSettings::Protocol::Sip) {
            account->addSeparator();
            QAction *contacts = account->addAction(QStringLiteral("Add / Remove Buddies / Contacts…"));
            connect(contacts, &QAction::triggered, this, [this, backendId] {
                if (BackendState *target = stateById(backendId)) {
                    selectState(target);
                    openBuddyManager(target);
                }
            });
        }
    }
}

void MainWindow::openMessagingDialog(BackendState *state,
                                     const QString &presetTarget,
                                     bool startRoomTab)
{
    if (!state || !state->backend) return;
    const auto protocol = state->backend->settings().protocol;
    if (protocol != ConnectionSettings::Protocol::Oscar
        && protocol != ConnectionSettings::Protocol::Irc) {
        QMessageBox::information(this, QStringLiteral("IM / Chatroom"),
                                 QStringLiteral("This connection type does not provide WaffleHouse IM/chat rooms."));
        return;
    }
    if (!state->connected) {
        QMessageBox::information(this, QStringLiteral("IM / Chatroom"),
                                 QStringLiteral("Connect %1 first.").arg(accountMenuLabel(state)));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 — IM / Chatroom").arg(accountMenuLabel(state)));
    dialog.setMinimumWidth(360);
    auto *outer = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);

    auto *imTab = new QWidget(tabs);
    auto *imForm = new QFormLayout(imTab);
    auto *target = new QComboBox(imTab);
    target->setEditable(true);
    target->setInsertPolicy(QComboBox::NoInsert);
    QStringList buddies = state->buddies.values();
    buddies.sort(Qt::CaseInsensitive);
    target->addItems(buddies);
    if (!presetTarget.trimmed().isEmpty()) target->setCurrentText(presetTarget.trimmed());
    else target->setCurrentIndex(-1);
    target->setPlaceholderText(QStringLiteral("screen name or nickname"));
    imForm->addRow(QStringLiteral("Buddy / user:"), target);
    auto *imHint = new QLabel(QStringLiteral("Open a private message using this account."), imTab);
    imHint->setWordWrap(true);
    imForm->addRow(QString(), imHint);
    tabs->addTab(imTab, QStringLiteral("Instant Message"));

    auto *roomTab = new QWidget(tabs);
    auto *roomForm = new QFormLayout(roomTab);
    auto *room = new QComboBox(roomTab);
    room->setEditable(true);
    room->setInsertPolicy(QComboBox::NoInsert);
    QStringList rooms = state->discoveredRooms.values();
    rooms.sort(Qt::CaseInsensitive);
    room->addItems(rooms);
    room->setCurrentIndex(-1);
    room->setPlaceholderText(protocol == ConnectionSettings::Protocol::Irc
                                 ? QStringLiteral("#channel")
                                 : QStringLiteral("room name"));
    roomForm->addRow(protocol == ConnectionSettings::Protocol::Irc
                         ? QStringLiteral("Channel:")
                         : QStringLiteral("Room:"), room);
    QCheckBox *privateRoom = nullptr;
    if (protocol == ConnectionSettings::Protocol::Oscar) {
        privateRoom = new QCheckBox(QStringLiteral("Private AIM exchange (create if needed)"), roomTab);
        roomForm->addRow(QString(), privateRoom);
    }
    auto *roomHint = new QLabel(
        protocol == ConnectionSettings::Protocol::Irc
            ? QStringLiteral("Join an IRC channel and open its conversation window.")
            : QStringLiteral("Join or create an AIM chat room and open its conversation window."),
        roomTab);
    roomHint->setWordWrap(true);
    roomForm->addRow(QString(), roomHint);
    tabs->addTab(roomTab, QStringLiteral("Chat Room"));

    tabs->setCurrentIndex(startRoomTab ? 1 : 0);
    outer->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    if (auto *ok = buttons->button(QDialogButtonBox::Ok)) ok->setText(QStringLiteral("Open"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    if (tabs->currentIndex() == 0) {
        const QString buddy = target->currentText().trimmed();
        if (!buddy.isEmpty()) ensureConversationWindow(state->backend, QStringLiteral("im"), buddy, true);
        return;
    }

    QString roomName = room->currentText().trimmed();
    if (roomName.isEmpty()) return;
    if (protocol == ConnectionSettings::Protocol::Irc
        && !QStringLiteral("#&+!").contains(roomName.front())) {
        roomName.prepend(QLatin1Char('#'));
    }
    m_closedRoomKeys.remove(conversationKey(state->backend, QStringLiteral("chat"), roomName));
    state->backend->joinRoom(roomName, privateRoom && privateRoom->isChecked());
    ensureConversationWindow(state->backend, QStringLiteral("chat"), roomName, true);
}

void MainWindow::openBuddyManager(BackendState *state)
{
    if (!state || !state->backend) return;
    const auto protocol = state->backend->settings().protocol;
    if (protocol != ConnectionSettings::Protocol::Oscar
        && protocol != ConnectionSettings::Protocol::Irc
        && protocol != ConnectionSettings::Protocol::Sip) {
        QMessageBox::information(this, QStringLiteral("Buddies / Contacts"),
                                 QStringLiteral("This connection type does not maintain a buddy/contact list."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 — Buddies / Contacts").arg(accountMenuLabel(state)));
    dialog.resize(410, 350);
    auto *outer = new QVBoxLayout(&dialog);

    auto *description = new QLabel(&dialog);
    if (protocol == ConnectionSettings::Protocol::Sip) {
        description->setText(QStringLiteral(
            "SIP contacts are local WaffleHouse dial targets for this SIP account. "
            "They are not uploaded to the PBX."));
    } else if (!state->connected) {
        description->setText(QStringLiteral(
            "This account is offline. Connect it before changing its buddy list."));
    } else if (protocol == ConnectionSettings::Protocol::Irc) {
        description->setText(QStringLiteral(
            "IRC buddies are local nickname watches for this saved IRC account."));
    } else {
        description->setText(QStringLiteral(
            "AIM buddy changes are sent through the selected AIM/OSCAR account."));
    }
    description->setWordWrap(true);
    outer->addWidget(description);

    auto *list = new QListWidget(&dialog);
    outer->addWidget(list, 1);

    auto refill = [list](const QStringList &provided) {
        QStringList names = provided;
        names.removeDuplicates();
        names.sort(Qt::CaseInsensitive);
        const QString selected = list->currentItem() ? list->currentItem()->text() : QString();
        list->clear();
        list->addItems(names);
        if (!selected.isEmpty()) {
            const auto matches = list->findItems(selected, Qt::MatchFixedString);
            if (!matches.isEmpty()) list->setCurrentItem(matches.first());
        }
    };
    refill(state->buddies.values());

    auto *addRow = new QHBoxLayout;
    auto *entry = new QLineEdit(&dialog);
    entry->setPlaceholderText(protocol == ConnectionSettings::Protocol::Sip
                                  ? QStringLiteral("extension, number, user@domain, or SIP URI")
                                  : protocol == ConnectionSettings::Protocol::Irc
                                        ? QStringLiteral("nickname to watch")
                                        : QStringLiteral("AIM screen name"));
    auto *add = new QPushButton(QStringLiteral("Add"), &dialog);
    auto *remove = new QPushButton(QStringLiteral("Remove Selected"), &dialog);
    addRow->addWidget(entry, 1);
    addRow->addWidget(add);
    outer->addLayout(addRow);
    outer->addWidget(remove);

    const bool mutableNow = protocol == ConnectionSettings::Protocol::Sip || state->connected;
    entry->setEnabled(mutableNow);
    add->setEnabled(mutableNow);
    remove->setEnabled(mutableNow && list->currentItem());
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [remove, mutableNow](QListWidgetItem *current) {
                remove->setEnabled(mutableNow && current);
            });

    connect(state->backend, &ChatBackend::buddyListChanged, &dialog,
            [refill](const QStringList &names) mutable { refill(names); });

    auto addBuddyNow = [this, state, entry] {
        const QString name = entry->text().trimmed();
        if (name.isEmpty()) return;
        state->backend->addBuddy(name);
        saveConnections();
        entry->clear();
    };
    connect(add, &QPushButton::clicked, &dialog, addBuddyNow);
    connect(entry, &QLineEdit::returnPressed, &dialog, addBuddyNow);
    connect(remove, &QPushButton::clicked, &dialog, [this, state, list] {
        QListWidgetItem *item = list->currentItem();
        if (!item) return;
        state->backend->removeBuddy(item->text());
        saveConnections();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::showTransferWindow()
{
    if (m_transferWindow) m_transferWindow->showAndRaise();
}

void MainWindow::buildConnectionsWindow()
{
    m_connectionsWindow = new QMainWindow(nullptr);
    m_connectionsWindow->setWindowTitle(QStringLiteral("%1 %2 — Connections").arg(appDisplayName(), appVersionString()));
    m_connectionsWindow->resize(620, 430);
    m_connectionsWindow->setMinimumSize(500, 340);
    m_connectionsWindow->setAttribute(Qt::WA_QuitOnClose, false);

    auto *central = new QWidget(m_connectionsWindow);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(16, 14, 16, 14);
    outer->setSpacing(10);

    auto *header = new QHBoxLayout;
    auto *headings = new QVBoxLayout;
    headings->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("Connections"), central);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto *subtitle = new QLabel(QStringLiteral("Saved profiles and live connection activity"), central);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    headings->addWidget(title);
    headings->addWidget(subtitle);
    header->addLayout(headings, 1);
    m_addConnectionButton = new QPushButton(QStringLiteral("+ Add Connection"), central);
    m_addConnectionButton->setProperty("role", "primary");
    header->addWidget(m_addConnectionButton);
    outer->addLayout(header);

    auto *listCard = new QFrame(central);
    listCard->setObjectName(QStringLiteral("ConnectionCard"));
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(10, 10, 10, 10);
    listLayout->setSpacing(10);
    auto *listTitle = new QLabel(QStringLiteral("Saved Accounts"), listCard);
    listTitle->setObjectName(QStringLiteral("CardTitle"));
    listLayout->addWidget(listTitle);
    m_connectionList = new QListWidget(listCard);
    m_connectionList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_connectionList->setMinimumHeight(120);
    listLayout->addWidget(m_connectionList, 1);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    m_editConnectionButton = new QPushButton(QStringLiteral("Edit"), listCard);
    m_deleteConnectionButton = new QPushButton(QStringLiteral("Delete"), listCard);
    m_deleteConnectionButton->setProperty("role", "danger");
    m_connectButton = new QPushButton(QStringLiteral("Connect"), listCard);
    m_connectButton->setProperty("role", "primary");
    m_disconnectButton = new QPushButton(QStringLiteral("Disconnect"), listCard);
    buttonRow->addWidget(m_editConnectionButton);
    buttonRow->addWidget(m_deleteConnectionButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_connectButton);
    buttonRow->addWidget(m_disconnectButton);
    listLayout->addLayout(buttonRow);
    outer->addWidget(listCard, 1);

    auto *activityCard = new QFrame(central);
    activityCard->setObjectName(QStringLiteral("Card"));
    auto *activityLayout = new QVBoxLayout(activityCard);
    activityLayout->setContentsMargins(14, 14, 14, 14);
    auto *activityLabel = new QLabel(QStringLiteral("Activity"), activityCard);
    activityLabel->setObjectName(QStringLiteral("CardTitle"));
    activityLayout->addWidget(activityLabel);
    m_activity = new QPlainTextEdit(activityCard);
    m_activity->setReadOnly(true);
    m_activity->setMaximumBlockCount(2500);
    m_activity->setMinimumHeight(125);
    activityLayout->addWidget(m_activity, 1);
    outer->addWidget(activityCard, 1);

    m_connectionsWindow->setCentralWidget(central);

    connect(m_addConnectionButton, &QPushButton::clicked,
            this, [this] { openConnectionDialog(m_defaults, nullptr); });
    connect(m_editConnectionButton, &QPushButton::clicked, this, &MainWindow::editSelected);
    connect(m_deleteConnectionButton, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::connectSelected);
    connect(m_disconnectButton, &QPushButton::clicked, this, &MainWindow::disconnectSelected);

    connect(m_connectionList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem *current) {
                BackendState *state = current ? stateById(current->data(Qt::UserRole).toString()) : nullptr;
                if (state) selectState(state);
                updateActions();
            });

    connect(m_connectionList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) {
                QListWidgetItem *item = m_connectionList->itemAt(pos);
                if (!item) return;
                m_connectionList->setCurrentItem(item);
                BackendState *state = stateById(item->data(Qt::UserRole).toString());
                if (!state) return;
                showAccountContextMenu(state, m_connectionList->viewport()->mapToGlobal(pos));
            });
}

void MainWindow::buildTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        statusBar()->showMessage(
            QStringLiteral("System tray is unavailable; closing Buddy List will quit %1.").arg(appDisplayName()),
            6000);
        return;
    }

    m_trayIcon = new QSystemTrayIcon(this);
    // WaffleHouse-Client 3.1 ships its own multi-resolution icon so the tray
    // appearance is consistent across Linux and FreeBSD desktop themes.
    QIcon icon = appIcon();
    if (icon.isNull()) {
        icon = windowIcon();
    }
    if (icon.isNull()) {
        icon = QIcon::fromTheme(QStringLiteral("internet-chat"));
    }
    if (icon.isNull() && QApplication::style()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    if (icon.isNull()) {
        delete m_trayIcon;
        m_trayIcon = nullptr;
        statusBar()->showMessage(
            QStringLiteral("No usable tray icon is available; closing Buddy List will quit %1.")
                .arg(appDisplayName()),
            6000);
        return;
    }
    m_trayIcon->setIcon(icon);
    m_trayIcon->setToolTip(appDisplayName());

    m_trayMenu = new QMenu(this);
    m_trayShowBuddyAction = m_trayMenu->addAction(QStringLiteral("Show Buddy List"));
    m_trayShowConnectionsAction = m_trayMenu->addAction(QStringLiteral("Show Connections"));
    m_trayShowPhoneAction = m_trayMenu->addAction(QStringLiteral("Show Softphone"));
    m_trayMenu->addSeparator();
    m_trayConnectAction = m_trayMenu->addAction(QStringLiteral("Connect Selected"));
    m_trayDisconnectAction = m_trayMenu->addAction(QStringLiteral("Disconnect Selected"));
    m_trayMenu->addSeparator();
    QAction *quitAction = m_trayMenu->addAction(QStringLiteral("Quit %1").arg(appDisplayName()));

    connect(m_trayShowBuddyAction, &QAction::triggered,
            this, &MainWindow::showBuddyWindow);
    connect(m_trayShowConnectionsAction, &QAction::triggered,
            this, &MainWindow::showConnectionsWindow);
    connect(m_trayShowPhoneAction, &QAction::triggered,
            m_softphoneWindow, &SoftphoneWindow::showAndRaise);
    connect(m_trayConnectAction, &QAction::triggered,
            this, &MainWindow::connectSelected);
    connect(m_trayDisconnectAction, &QAction::triggered,
            this, &MainWindow::disconnectSelected);
    connect(quitAction, &QAction::triggered,
            this, &MainWindow::quitApplication);

    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    showBuddyWindow();
                }
            });

    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->show();
}

void MainWindow::openConnectionDialog(const ConnectionSettings &defaults,
                                      BackendState *editingState)
{
    if (editingState && (editingState->connected || editingState->connecting)) {
        QMessageBox::information(
            this,
            QStringLiteral("Edit Connection"),
            QStringLiteral("Disconnect this connection before editing it."));
        return;
    }

    if (editingState && editingState->backend
        && editingState->backend->settings().protocol == ConnectionSettings::Protocol::Sip) {
        for (const auto &call : m_sipController->calls()) {
            if (!call.disconnected
                && QString::fromStdString(call.accountId) == editingState->backend->id()) {
                QMessageBox::information(
                    this,
                    QStringLiteral("SIP Account In Use"),
                    QStringLiteral("Hang up active calls on this SIP account before editing it."));
                return;
            }
        }
    }

    ConnectionDialog dialog(defaults, editingState != nullptr, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    ConnectionSettings settings = dialog.settings();

    if (editingState && editingState->backend) {
        const ConnectionSettings oldSettings = editingState->backend->settings();
        if (settings.password.isEmpty()) {
            settings.password = oldSettings.password;
            // An unchecked Save password box explicitly disables persistence even when
            // the existing session password remains usable until the application exits.
            if (!settings.savePassword) {
                settings.savePassword = false;
            }
        }

        editingState->backend->setConnectionSettings(settings);
        editingState->identity.clear();
        editingState->endpoint.clear();
        editingState->buddies.clear();
        editingState->onlineBuddies.clear();
        if (settings.protocol == ConnectionSettings::Protocol::Irc) {
            for (const QString &buddy : settings.ircBuddies)
                if (!buddy.trimmed().isEmpty()) editingState->buddies.insert(buddy.trimmed());
        } else if (settings.protocol == ConnectionSettings::Protocol::Sip) {
            for (const QString &contact : settings.sipContacts)
                if (!contact.trimmed().isEmpty()) editingState->buddies.insert(contact.trimmed());
        }
        editingState->targetNames.clear();
        editingState->discoveredRooms.clear();

        if (settings.protocol == ConnectionSettings::Protocol::Irc) {
            editingState->secretRequired =
                editingState->secretRequired || !settings.password.isEmpty();
        } else if (settings.protocol == ConnectionSettings::Protocol::Telnet) {
            editingState->secretRequired = false;
        } else {
            editingState->secretRequired = true;
        }
        editingState->hasSessionSecret = !settings.password.isEmpty();

        updateConnectionItem(editingState);
        refreshBuddyList();
        updateActions();
        saveConnections();
        appendActivity(editingState->backend,
                       QStringLiteral("Saved connection updated."));
        return;
    }

    ChatBackend *backend = createBackend(settings);
    if (!backend) {
        return;
    }

    const bool secretRequired =
        settings.protocol == ConnectionSettings::Protocol::Oscar
        || settings.protocol == ConnectionSettings::Protocol::Sip
        || (settings.protocol == ConnectionSettings::Protocol::Irc
            && !settings.password.isEmpty());

    attachBackend(backend,
                  true,
                  secretRequired,
                  !settings.password.isEmpty(),
                  false);
}


void MainWindow::importBbsList()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import BBS List"), QString(),
        QStringLiteral("BBS Lists (*.csv *.tsv *.json *.txt);;All Files (*)"));
    if (path.isEmpty()) return;

    QString error;
    const auto entries = BbsDirectory::loadFile(path, &error);
    if (entries.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("BBS Import"),
                             error.isEmpty() ? QStringLiteral("No BBS entries were found in the selected file.") : error);
        return;
    }

    int added = 0, skipped = 0;
    for (const auto &bbs : entries) {
        bool duplicate = false;
        for (BackendState *state : m_states) {
            if (!state || !state->backend) continue;
            const auto &cfg = state->backend->settings();
            if (cfg.protocol == ConnectionSettings::Protocol::Telnet
                && cfg.server.compare(bbs.host, Qt::CaseInsensitive) == 0
                && cfg.port == bbs.port) { duplicate = true; break; }
        }
        if (duplicate) { ++skipped; continue; }
        ConnectionSettings cfg;
        cfg.protocol = ConnectionSettings::Protocol::Telnet;
        cfg.server = bbs.host;
        cfg.port = bbs.port;
        cfg.username = bbs.name;
        cfg.telnetTerminalType = bbs.terminalType.isEmpty() ? QStringLiteral("ANSI") : bbs.terminalType;
        if (ChatBackend *backend = createBackend(cfg)) {
            attachBackend(backend, true, false, false, false);
            ++added;
        }
    }
    QMessageBox::information(this, QStringLiteral("BBS Import"),
        QStringLiteral("Imported %1 BBS entries. %2 duplicates skipped.\n\nImported BBSes are saved offline; use Connect when you want to open one.")
            .arg(added).arg(skipped));
}

void MainWindow::loadUiSettings()
{
    QSettings settings;
    m_buddyOpacity = std::clamp(
        settings.value(QStringLiteral("ui/buddyOpacity"), 1.0).toDouble(),
        0.30,
        1.0);
    m_connectionsOpacity = std::clamp(
        settings.value(QStringLiteral("ui/connectionsOpacity"), 1.0).toDouble(),
        0.30,
        1.0);
}

void MainWindow::saveUiSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/buddyOpacity"), m_buddyOpacity);
    settings.setValue(QStringLiteral("ui/connectionsOpacity"), m_connectionsOpacity);
}

void MainWindow::loadOptions()
{
    QSettings settings;
    m_options.theme = settings.value(QStringLiteral("ui/theme"), QStringLiteral("system"))
                          .toString().toCaseFolded();
    const QSet<QString> validThemes{
        QStringLiteral("system"), QStringLiteral("hacker"), QStringLiteral("matrix"),
        QStringLiteral("phosphor"), QStringLiteral("midnight"), QStringLiteral("amber"),
        QStringLiteral("ice"), QStringLiteral("classic-light"), QStringLiteral("solarized"),
        QStringLiteral("solarized-dark"), QStringLiteral("dracula"), QStringLiteral("nord"),
        QStringLiteral("cyberpunk"), QStringLiteral("blood-moon"), QStringLiteral("ocean"),
        QStringLiteral("retro-blue"), QStringLiteral("monochrome"), QStringLiteral("blue-box"),
        QStringLiteral("red-box"), QStringLiteral("beige-box"), QStringLiteral("2600"),
        QStringLiteral("wargames"), QStringLiteral("crt-green"), QStringLiteral("vt220"),
        QStringLiteral("cobalt"), QStringLiteral("vaporwave"), QStringLiteral("stealth"),
        QStringLiteral("synthwave"), QStringLiteral("c64"), QStringLiteral("dos"),
        QStringLiteral("waffle-iron"), QStringLiteral("ghostline"),
        QStringLiteral("hot-dog-stand"), QStringLiteral("neon-miami")
    };
    if (!validThemes.contains(m_options.theme)) {
        m_options.theme = QStringLiteral("system");
    }
    m_options.showTimestamps = settings.value(QStringLiteral("ui/showTimestamps"), true).toBool();
    m_options.showSidePanes = settings.value(QStringLiteral("ui/showSidePanes"), true).toBool();
    m_options.encryptedDmEnabled = settings.value(QStringLiteral("security/encryptedDmEnabled"), true).toBool();
    m_options.autoReplySecure = settings.value(QStringLiteral("security/autoReplySecure"), true).toBool();
    m_options.showSecureFingerprints = settings.value(QStringLiteral("security/showSecureFingerprints"), true).toBool();
    m_options.autoPresenceEnabled = settings.value(QStringLiteral("presence/autoEnabled"), true).toBool();
    m_options.autoIdleMinutes = std::clamp(settings.value(QStringLiteral("presence/idleMinutes"), 5).toInt(), 1, 1440);
    m_options.autoAwayMinutes = std::clamp(settings.value(QStringLiteral("presence/awayMinutes"), 15).toInt(), m_options.autoIdleMinutes + 1, 2880);
}

void MainWindow::saveOptions() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/theme"), m_options.theme);
    settings.setValue(QStringLiteral("ui/showTimestamps"), m_options.showTimestamps);
    settings.setValue(QStringLiteral("ui/showSidePanes"), m_options.showSidePanes);
    settings.setValue(QStringLiteral("security/encryptedDmEnabled"), m_options.encryptedDmEnabled);
    settings.setValue(QStringLiteral("security/autoReplySecure"), m_options.autoReplySecure);
    settings.setValue(QStringLiteral("security/showSecureFingerprints"), m_options.showSecureFingerprints);
    settings.setValue(QStringLiteral("presence/autoEnabled"), m_options.autoPresenceEnabled);
    settings.setValue(QStringLiteral("presence/idleMinutes"), m_options.autoIdleMinutes);
    settings.setValue(QStringLiteral("presence/awayMinutes"), m_options.autoAwayMinutes);
    settings.sync();
}

void MainWindow::applyTheme()
{
    // 3.0 keeps every existing theme key but routes them through one modern
    // design system so the main window, dialogs, chat windows, softphone,
    // transfers, menus, and secondary windows all share the same visual language.
    qApp->setStyleSheet(ModernStyle::styleSheet(m_options.theme));
    applyConversationOptions();
}

void MainWindow::applyConversationOptions()
{
    for (ChatWindow *window : m_windows) {
        if (!window) continue;
        window->setShowTimestamps(m_options.showTimestamps);
        window->setShowSidePane(m_options.showSidePanes);
        updateConversationSecurity(window);
    }
}

void MainWindow::showOptionsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Options — %1").arg(appDisplayName()));
    dialog.setMinimumWidth(360);
    auto *outer = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    QComboBox theme(&dialog);
    theme.addItem(QStringLiteral("System"), QStringLiteral("system"));
    theme.addItem(QStringLiteral("Hacker"), QStringLiteral("hacker"));
    theme.addItem(QStringLiteral("Matrix"), QStringLiteral("matrix"));
    theme.addItem(QStringLiteral("Phosphor"), QStringLiteral("phosphor"));
    theme.addItem(QStringLiteral("Midnight"), QStringLiteral("midnight"));
    theme.addItem(QStringLiteral("Amber"), QStringLiteral("amber"));
    theme.addItem(QStringLiteral("Ice"), QStringLiteral("ice"));
    theme.addItem(QStringLiteral("Classic Light"), QStringLiteral("classic-light"));
    theme.addItem(QStringLiteral("Solarized"), QStringLiteral("solarized"));
    theme.addItem(QStringLiteral("Solarized Dark"), QStringLiteral("solarized-dark"));
    theme.addItem(QStringLiteral("Dracula"), QStringLiteral("dracula"));
    theme.addItem(QStringLiteral("Nord"), QStringLiteral("nord"));
    theme.addItem(QStringLiteral("Cyberpunk"), QStringLiteral("cyberpunk"));
    theme.addItem(QStringLiteral("Blood Moon"), QStringLiteral("blood-moon"));
    theme.addItem(QStringLiteral("Ocean"), QStringLiteral("ocean"));
    theme.addItem(QStringLiteral("Retro Blue"), QStringLiteral("retro-blue"));
    theme.addItem(QStringLiteral("Monochrome"), QStringLiteral("monochrome"));
    theme.addItem(QStringLiteral("Blue Box"), QStringLiteral("blue-box"));
    theme.addItem(QStringLiteral("Red Box"), QStringLiteral("red-box"));
    theme.addItem(QStringLiteral("Beige Box"), QStringLiteral("beige-box"));
    theme.addItem(QStringLiteral("2600"), QStringLiteral("2600"));
    theme.addItem(QStringLiteral("WarGames"), QStringLiteral("wargames"));
    theme.addItem(QStringLiteral("CRT Green"), QStringLiteral("crt-green"));
    theme.addItem(QStringLiteral("VT220"), QStringLiteral("vt220"));
    theme.addItem(QStringLiteral("Cobalt"), QStringLiteral("cobalt"));
    theme.addItem(QStringLiteral("Vaporwave"), QStringLiteral("vaporwave"));
    theme.addItem(QStringLiteral("Stealth"), QStringLiteral("stealth"));
    theme.addItem(QStringLiteral("Synthwave"), QStringLiteral("synthwave"));
    theme.addItem(QStringLiteral("C64"), QStringLiteral("c64"));
    theme.addItem(QStringLiteral("DOS"), QStringLiteral("dos"));
    theme.addItem(QStringLiteral("Waffle Iron"), QStringLiteral("waffle-iron"));
    theme.addItem(QStringLiteral("Ghostline"), QStringLiteral("ghostline"));
    theme.addItem(QStringLiteral("Hot Dog Stand"), QStringLiteral("hot-dog-stand"));
    theme.addItem(QStringLiteral("Neon Miami"), QStringLiteral("neon-miami"));
    const int themeIndex = theme.findData(m_options.theme);
    if (themeIndex >= 0) theme.setCurrentIndex(themeIndex);
    form->addRow(QStringLiteral("Theme:"), &theme);

    QCheckBox timestamps(QStringLiteral("Show timestamps in conversations"), &dialog);
    timestamps.setChecked(m_options.showTimestamps);
    form->addRow(QString(), &timestamps);

    QCheckBox sidePanes(QStringLiteral("Show room member side panes"), &dialog);
    sidePanes.setChecked(m_options.showSidePanes);
    form->addRow(QString(), &sidePanes);

    QCheckBox encrypted(QStringLiteral("Enable CPX3 encrypted communications (DMs + rooms)"), &dialog);
    encrypted.setChecked(m_options.encryptedDmEnabled);
    form->addRow(QString(), &encrypted);

    QCheckBox autoReply(QStringLiteral("Automatically reply to secure handshakes"), &dialog);
    autoReply.setChecked(m_options.autoReplySecure);
    form->addRow(QString(), &autoReply);

    QCheckBox fingerprints(QStringLiteral("Show secure fingerprint notices"), &dialog);
    fingerprints.setChecked(m_options.showSecureFingerprints);
    form->addRow(QString(), &fingerprints);

    QCheckBox autoPresence(QStringLiteral("Automatically set AIM/OSCAR Idle and Away when inactive"), &dialog);
    autoPresence.setChecked(m_options.autoPresenceEnabled);
    form->addRow(QString(), &autoPresence);
    QSpinBox autoIdle(&dialog);
    autoIdle.setRange(1, 1440);
    autoIdle.setSuffix(QStringLiteral(" minutes"));
    autoIdle.setValue(m_options.autoIdleMinutes);
    form->addRow(QStringLiteral("Auto-idle after:"), &autoIdle);
    QSpinBox autoAway(&dialog);
    autoAway.setRange(m_options.autoIdleMinutes + 1, 2880);
    autoAway.setSuffix(QStringLiteral(" minutes"));
    autoAway.setValue(std::max(m_options.autoAwayMinutes, m_options.autoIdleMinutes + 1));
    form->addRow(QStringLiteral("Auto-away after:"), &autoAway);
    connect(&autoIdle, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&autoAway](int minutes) {
        autoAway.setMinimum(minutes + 1);
    });

    QLabel identity(&dialog);
    if (BackendState *state = selectedState(); m_secureReady && state) {
        identity.setText(QStringLiteral("Selected profile fingerprint:\n%1")
                             .arg(m_secure.localFingerprint(state->profileId)));
    } else if (!m_secureReady) {
        identity.setText(QStringLiteral("Encrypted communications unavailable: %1").arg(m_secureError));
    } else {
        identity.setText(QStringLiteral("Select a connection to view its secure fingerprint."));
    }
    identity.setTextInteractionFlags(Qt::TextSelectableByMouse);
    identity.setWordWrap(true);
    outer->addLayout(form);
    outer->addWidget(&identity);

    auto *notifyBox = new QGroupBox(QStringLiteral("Notification Sounds"), &dialog);
    auto *notifyLayout = new QGridLayout(notifyBox);
    auto *notifyMaster = new QCheckBox(QStringLiteral("Enable notification sounds"), notifyBox);
    notifyMaster->setChecked(NotificationManager::globalEnabled());
    notifyLayout->addWidget(notifyMaster, 0, 0, 1, 5);
    auto *notifyHint = new QLabel(QStringLiteral("Built-in sounds are included. Choose Custom to use your own audio file (WAV is recommended). GUI and CLI share these settings."), notifyBox);
    notifyHint->setWordWrap(true);
    notifyHint->setObjectName(QStringLiteral("Muted"));
    notifyLayout->addWidget(notifyHint, 1, 0, 1, 5);

    struct NotificationRow {
        NotificationManager::Event event;
        QCheckBox *enabled = nullptr;
        QComboBox *source = nullptr;
        QLineEdit *path = nullptr;
        QPushButton *browse = nullptr;
        QPushButton *test = nullptr;
    };
    QList<NotificationRow> notificationRows;
    int notifyRow = 2;
    for (const auto event : NotificationManager::configurableEvents()) {
        const auto cfg = NotificationManager::setting(event);
        NotificationRow row;
        row.event = event;
        row.enabled = new QCheckBox(NotificationManager::displayName(event), notifyBox);
        row.enabled->setChecked(cfg.enabled);
        row.source = new QComboBox(notifyBox);
        row.source->addItem(QStringLiteral("Built-in"), QStringLiteral("builtin"));
        row.source->addItem(QStringLiteral("Custom file"), QStringLiteral("custom"));
        row.source->addItem(QStringLiteral("None"), QStringLiteral("none"));
        row.path = new QLineEdit(notifyBox);
        row.path->setPlaceholderText(QStringLiteral("/path/to/notification.wav"));
        row.browse = new QPushButton(QStringLiteral("Browse…"), notifyBox);
        row.test = new QPushButton(QStringLiteral("Test"), notifyBox);

        if (NotificationManager::isCustomSpec(cfg.soundSpec)) {
            row.source->setCurrentIndex(row.source->findData(QStringLiteral("custom")));
            row.path->setText(NotificationManager::customPath(cfg.soundSpec));
        } else if (cfg.soundSpec.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
                   || cfg.soundSpec.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0) {
            row.source->setCurrentIndex(row.source->findData(QStringLiteral("none")));
        } else {
            row.source->setCurrentIndex(row.source->findData(QStringLiteral("builtin")));
        }

        auto updateCustom = [source=row.source, path=row.path, browse=row.browse] {
            const bool custom = source->currentData().toString() == QStringLiteral("custom");
            path->setEnabled(custom);
            browse->setEnabled(custom);
        };
        updateCustom();
        connect(row.source, &QComboBox::currentIndexChanged, &dialog, [updateCustom](int) { updateCustom(); });
        connect(row.browse, &QPushButton::clicked, &dialog, [path=row.path, &dialog] {
            const QString selected = QFileDialog::getOpenFileName(
                &dialog, QStringLiteral("Choose Notification Sound"), path->text(),
                QStringLiteral("Audio files (*.wav *.ogg *.oga *.flac *.mp3);;All files (*)"));
            if (!selected.isEmpty()) path->setText(selected);
        });
        connect(row.test, &QPushButton::clicked, &dialog, [event, source=row.source, path=row.path] {
            const QString mode = source->currentData().toString();
            QString spec = QStringLiteral("none");
            if (mode == QStringLiteral("builtin")) spec = NotificationManager::builtinSpec(event);
            else if (mode == QStringLiteral("custom")) spec = NotificationManager::customSpec(path->text());
            if (!NotificationManager::playSpec(spec, false) && mode != QStringLiteral("none")) QApplication::beep();
        });

        notifyLayout->addWidget(row.enabled, notifyRow, 0);
        notifyLayout->addWidget(row.source, notifyRow, 1);
        notifyLayout->addWidget(row.path, notifyRow, 2);
        notifyLayout->addWidget(row.browse, notifyRow, 3);
        notifyLayout->addWidget(row.test, notifyRow, 4);
        notificationRows.push_back(row);
        ++notifyRow;
    }
    notifyLayout->setColumnStretch(2, 1);
    outer->addWidget(notifyBox);

    QDialogButtonBox buttons(QDialogButtonBox::Cancel | QDialogButtonBox::Save, &dialog);
    outer->addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    m_options.theme = theme.currentData().toString();
    m_options.showTimestamps = timestamps.isChecked();
    m_options.showSidePanes = sidePanes.isChecked();
    m_options.encryptedDmEnabled = encrypted.isChecked();
    m_options.autoReplySecure = autoReply.isChecked();
    m_options.showSecureFingerprints = fingerprints.isChecked();
    m_options.autoPresenceEnabled = autoPresence.isChecked();
    m_options.autoIdleMinutes = autoIdle.value();
    m_options.autoAwayMinutes = std::max(autoAway.value(), autoIdle.value() + 1);
    if (!m_options.autoPresenceEnabled) markUserActivity();
    NotificationManager::setGlobalEnabled(notifyMaster->isChecked());
    for (const NotificationRow &row : notificationRows) {
        NotificationManager::Setting cfg;
        cfg.enabled = row.enabled->isChecked();
        const QString mode = row.source->currentData().toString();
        if (mode == QStringLiteral("custom")) cfg.soundSpec = NotificationManager::customSpec(row.path->text());
        else if (mode == QStringLiteral("none")) cfg.soundSpec = QStringLiteral("none");
        else cfg.soundSpec = NotificationManager::builtinSpec(row.event);
        NotificationManager::setSetting(row.event, cfg);
    }
    saveOptions();
    applyTheme();
}

void MainWindow::showHelpDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 Help").arg(appDisplayName()));
    dialog.resize(580, 460);
    auto *outer = new QVBoxLayout(&dialog);
    auto *help = new QPlainTextEdit(&dialog);
    help->setReadOnly(true);
    help->setPlainText(QStringLiteral(
        "%1\nVersion %3\n\n"
        "%2 HELP\n\n"
        "CONNECTIONS\n"
        "  Connection > Add... creates AIM/OSCAR, IRC, Telnet/BBS, or SIP/VoIP profiles.\n"
        "  New profiles start with no protocol selected. Secrets are not saved unless you opt in.\n"
        "  Accounts contains one submenu per saved connection for connection-specific actions.\n"
        "  AIM/IRC account menus provide IM / Chatroom and Add / Remove Buddies windows.\n"
        "  Tools contains Show Connections Window, Open Softphone, AIM password, fingerprint, and Options.\n"
        "  WaffleHouse media adds the Media menu for local audio/video, SHOUTcast/Icecast, HTTP/HLS streams, and playlists.\n\n"
        "SIP / VOIP SOFTPHONE\n"
        "  SIP accounts are normal saved WaffleHouse connections and remain visible in the Communications Hub.\n"
        "  The old right-side quick-dial card is removed; the left-rail Softphone button opens the full phone workspace.\n"
        "  SIP contacts remain available from the account management menus and the Softphone profile/workspace.\n"
        "  Tools > Open Softphone also opens the full phone workspace.\n"
        "  Softphone > Profile edits the same saved SIP account as Connection > Edit.\n"
        "  Softphone left rail: Phone, Active Calls, SIP Log, SIP Ladder, Profile, and Activity.\n"
        "  The Phone page includes Prefix, Destination, Caller ID, a live status strip, and a telephone dial pad.\n\n"
        "TELNET / MUD / BBS\n"
        "  Add a Telnet profile, choose host/port and terminal type, then connect.\n"
        "  A terminal session window opens automatically. Closing it disconnects that Telnet profile.\n"
        "  Telnet is plaintext; credentials and traffic are not encrypted by the Telnet protocol.\n\n"
        "SECURE PRIVATE MESSAGES\n"
        "  Secure DMs interoperate with WaffleHouse-Client, WaffleHouse 1.9.1 family clients, GhostPulse, CrossPoint, and legacy CPX3-compatible clients.\n"
        "  Encryption applies to AIM/OSCAR and IRC private messages; 3.1 also supports CPX secure AIM/IRC rooms.\n"
        "  Telnet traffic, routing metadata, and server-visible endpoints remain outside CPX encryption.\n\n"
        "  1. Open an IM with another compatible WaffleHouse/CPX3-compatible user.\n"
        "  2. Choose Security > Start Secure Session.\n"
        "  3. Choose Security > Secure Session Status to view both fingerprints.\n"
        "  4. Compare the peer fingerprint through a separate trusted channel (voice, phone, in person, etc.).\n"
        "  5. Choose Security > Trust Peer Fingerprint after it matches.\n"
        "  6. Type normally. Messages are encrypted automatically while the secure session is active.\n\n"
        "SECURE AIM / IRC ROOMS\n"
        "  Open an AIM chat room or IRC channel and choose Security > Start Secure Room (or type /secure).\n"
        "  WaffleHouse creates an XChaCha20-Poly1305 shared room key and delivers it only through established CPX encrypted PM sessions.\n"
        "  Public room traffic contains CPXROOM ciphertext. WaffleHouse peers with the key display [secure-room] plaintext locally; ordinary traffic is marked [plaintext].\n"
        "  The key owner rotates the room key when membership changes and redistributes it to current secure peers.\n\n"
        "  An unverified secure session is encrypted but not identity-verified.\n"
        "  If a trusted peer later presents a different key, the client rejects that secure session.\n"
        "  Security > Forget Trusted Fingerprint removes saved trust.\n"
        "  Security > Close Secure Session returns that conversation to plaintext.\n\n"
        "FILE TRANSFER\n"
        "  Choose Send File from an AIM/IRC private-message window, then select Secure or Unsecured.\n"
        "  Secure uses CPX encryption/authentication and requires a verified secure session; the dialog explains setup if one is not active.\n"
        "  Unsecured proceeds over ordinary AIM/IRC PM transport without CPX encryption/authentication.\n"
        "  Both modes retain chunking/resume and SHA-256 verification; incoming transfers require explicit acceptance and a destination path.\n\n"
        "SLASH ALIASES INSIDE CONVERSATION WINDOWS\n"
        "  Tab          complete/cycle matching slash commands\n"
        "  Shift+Tab    cycle matching commands backward\n"
        "  /options     open Options\n"
        "  /help        open this Help window\n"
        "  /fingerprint show this connection profile's local secure fingerprint\n"
        "  /secure      start a secure IM, or start/rotate secure-room mode in an AIM/IRC room\n"
        "  /securestatus show IM trust state or the active room-key status\n"
        "  /trust       trust the active peer fingerprint\n"
        "  /untrust     forget the trusted peer fingerprint\n"
        "  /secureoff   close the secure session\n\n"
        "IRC SLASH COMMANDS (IRC CONVERSATIONS)\n"
        "  /nick NEWNICK          change nickname\n"
        "  /op NICK... /deop...   grant/remove channel operator\n"
        "  /voice NICK... /devoice grant/remove channel voice\n"
        "  /kick NICK [reason]     kick from the active channel\n"
        "  /ban NICK|MASK /unban   set/remove channel ban\n"
        "  /topic, /mode, /me, /notice, /invite, /who, /whois, /whowas, /ison, /list, /motd\n"
        "  /raw or /quote COMMAND  send an IRC protocol line directly\n"
        "  Unknown /text is sent as normal chat text (and remains CPX-encrypted when the conversation is secured).\n\n"
        "RUNTIME ENVIRONMENT\n"
        "  %4\n"
        "  Graphical-terminal sessions are distinguished from desktop launches and console-only TTYs.\n\n"
        "THEMES\n"
        "  Use the left-rail Settings button, Tools > Options, or Ctrl+, for full settings.\n"
        "  View > Theme provides the complete WaffleHouse + S.I.P.H.E.R. theme collection.\n")
        .arg(appAsciiLogo(), appDisplayName(), appVersionString(), RuntimeEnvironment::detect().summary()));
    outer->addWidget(help, 1);
    QDialogButtonBox close(QDialogButtonBox::Close, &dialog);
    outer->addWidget(&close);
    connect(&close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(&close, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::loadConnections()
{
    if (!m_connectionList) return;

    QSettings settings;
    const int count = settings.beginReadArray(QStringLiteral("connections"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        ConnectionSettings value;
        const int protocolValue = settings.value(
            QStringLiteral("protocol"),
            static_cast<int>(ConnectionSettings::Protocol::Unknown)).toInt();
        if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Oscar))
            value.protocol = ConnectionSettings::Protocol::Oscar;
        else if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Irc))
            value.protocol = ConnectionSettings::Protocol::Irc;
        else if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Telnet))
            value.protocol = ConnectionSettings::Protocol::Telnet;
        else if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Sip))
            value.protocol = ConnectionSettings::Protocol::Sip;
        else
            continue;

        value.server = settings.value(QStringLiteral("server")).toString();
        const int defaultPort = value.protocol == ConnectionSettings::Protocol::Irc ? 6667
            : value.protocol == ConnectionSettings::Protocol::Telnet ? 23
            : value.protocol == ConnectionSettings::Protocol::Sip ? 5060 : 5190;
        value.port = static_cast<quint16>(settings.value(QStringLiteral("port"), defaultPort).toUInt());
        value.username = settings.value(QStringLiteral("username")).toString();
        value.redirectHost = settings.value(QStringLiteral("redirectHost")).toString();
        value.redirectPort = static_cast<quint16>(settings.value(QStringLiteral("redirectPort"), 0).toUInt());
        value.realName = settings.value(QStringLiteral("realName"), appDefaultRealName()).toString();
        value.tls = settings.value(QStringLiteral("tls"), false).toBool();
        value.ircBuddies = settings.value(QStringLiteral("ircBuddies")).toStringList();
        value.sipContacts = settings.value(QStringLiteral("sipContacts")).toStringList();
        value.telnetTerminalType = settings.value(QStringLiteral("telnetTerminalType"), QStringLiteral("ANSI")).toString();
        value.sipProfileName = settings.value(QStringLiteral("sipProfileName")).toString();
        value.sipDomain = settings.value(QStringLiteral("sipDomain"), value.protocol == ConnectionSettings::Protocol::Sip ? value.server : QString()).toString();
        value.sipRegistrar = settings.value(QStringLiteral("sipRegistrar")).toString();
        value.sipAuthUsername = settings.value(QStringLiteral("sipAuthUsername")).toString();
        value.sipDisplayName = settings.value(QStringLiteral("sipDisplayName")).toString();
        value.sipOutboundProxy = settings.value(QStringLiteral("sipOutboundProxy")).toString();
        value.sipCallerIdDomain = settings.value(QStringLiteral("sipCallerIdDomain")).toString();
        value.sipDialPrefix = settings.value(QStringLiteral("sipDialPrefix")).toString();
        value.sipStunServer = settings.value(QStringLiteral("sipStunServer")).toString();
        value.sipTransport = settings.value(QStringLiteral("sipTransport"), QStringLiteral("udp")).toString();
        value.sipIdentityMode = settings.value(QStringLiteral("sipIdentityMode"), QStringLiteral("from")).toString();
        value.sipLocalPort = static_cast<quint16>(settings.value(QStringLiteral("sipLocalPort"), value.protocol == ConnectionSettings::Protocol::Sip ? value.port : 5060).toUInt());
        value.sipRegistrationExpires = settings.value(QStringLiteral("sipRegistrationExpires"), 300).toUInt();
        value.sipUseIce = settings.value(QStringLiteral("sipUseIce"), false).toBool();
        value.sipEnableSrtp = settings.value(QStringLiteral("sipEnableSrtp"), false).toBool();
        value.debug = settings.value(QStringLiteral("debug"), false).toBool();
        value.savePassword = settings.value(QStringLiteral("savePassword"), false).toBool();
        value.password = value.savePassword
            ? settings.value(QStringLiteral("password")).toString()
            : QString();

        const bool defaultSecretRequired = value.protocol == ConnectionSettings::Protocol::Oscar
            || value.protocol == ConnectionSettings::Protocol::Sip;
        const bool secretRequired = settings.value(QStringLiteral("secretRequired"), defaultSecretRequired).toBool();

        QString profileId = settings.value(QStringLiteral("id")).toString().trimmed();
        if (profileId.isEmpty()) {
            const QString seed = QStringLiteral("%1|%2|%3|%4|%5")
                .arg(static_cast<int>(value.protocol))
                .arg(value.server.toCaseFolded())
                .arg(value.port)
                .arg(value.username.toCaseFolded())
                .arg(i);
            profileId = QStringLiteral("migrated-%1").arg(QString::fromLatin1(
                QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256)
                    .left(16).toHex()));
        }

        ChatBackend *backend = createBackend(value);
        if (backend) {
            attachBackend(backend, false, secretRequired, !value.password.isEmpty(), false, profileId);
        }
    }
    settings.endArray();

    if (m_connectionList->count() > 0) {
        m_connectionList->setCurrentRow(0);
        saveConnections();
    }
}

void MainWindow::saveConnections() const
{
    if (!m_connectionList) return;
    QSettings settings;
    settings.remove(QStringLiteral("connections"));
    settings.beginWriteArray(QStringLiteral("connections"));

    int outputIndex = 0;
    for (int row = 0; row < m_connectionList->count(); ++row) {
        QListWidgetItem *item = m_connectionList->item(row);
        if (!item) continue;
        BackendState *state = stateById(item->data(Qt::UserRole).toString());
        if (!state || !state->backend) continue;

        const ConnectionSettings &value = state->backend->settings();
        settings.setArrayIndex(outputIndex++);
        settings.setValue(QStringLiteral("id"), state->profileId);
        settings.setValue(QStringLiteral("protocol"), static_cast<int>(value.protocol));
        settings.setValue(QStringLiteral("server"), value.server);
        settings.setValue(QStringLiteral("port"), value.port);
        settings.setValue(QStringLiteral("username"), value.username);
        settings.setValue(QStringLiteral("redirectHost"), value.redirectHost);
        settings.setValue(QStringLiteral("redirectPort"), value.redirectPort);
        settings.setValue(QStringLiteral("realName"), value.realName);
        settings.setValue(QStringLiteral("tls"), value.tls);
        settings.setValue(QStringLiteral("ircBuddies"), value.ircBuddies);
        settings.setValue(QStringLiteral("sipContacts"), value.sipContacts);
        settings.setValue(QStringLiteral("telnetTerminalType"), value.telnetTerminalType);
        settings.setValue(QStringLiteral("sipProfileName"), value.sipProfileName);
        settings.setValue(QStringLiteral("sipDomain"), value.sipDomain);
        settings.setValue(QStringLiteral("sipRegistrar"), value.sipRegistrar);
        settings.setValue(QStringLiteral("sipAuthUsername"), value.sipAuthUsername);
        settings.setValue(QStringLiteral("sipDisplayName"), value.sipDisplayName);
        settings.setValue(QStringLiteral("sipOutboundProxy"), value.sipOutboundProxy);
        settings.setValue(QStringLiteral("sipCallerIdDomain"), value.sipCallerIdDomain);
        settings.setValue(QStringLiteral("sipDialPrefix"), value.sipDialPrefix);
        settings.setValue(QStringLiteral("sipStunServer"), value.sipStunServer);
        settings.setValue(QStringLiteral("sipTransport"), value.sipTransport);
        settings.setValue(QStringLiteral("sipIdentityMode"), value.sipIdentityMode);
        settings.setValue(QStringLiteral("sipLocalPort"), value.sipLocalPort);
        settings.setValue(QStringLiteral("sipRegistrationExpires"), value.sipRegistrationExpires);
        settings.setValue(QStringLiteral("sipUseIce"), value.sipUseIce);
        settings.setValue(QStringLiteral("sipEnableSrtp"), value.sipEnableSrtp);
        settings.setValue(QStringLiteral("debug"), value.debug);
        settings.setValue(QStringLiteral("secretRequired"), state->secretRequired);
        settings.setValue(QStringLiteral("savePassword"), value.savePassword);
        if (value.savePassword && !value.password.isEmpty()) {
            settings.setValue(QStringLiteral("password"), value.password);
        }
    }
    settings.endArray();
    settings.sync();
}

bool MainWindow::ensureConnectionSecret(BackendState *state)
{
    if (!state || !state->backend) {
        return false;
    }
    if (!state->secretRequired || state->hasSessionSecret) {
        return true;
    }

    const auto protocol = state->backend->settings().protocol;
    QString title;
    QString label;
    bool allowEmpty = false;

    switch (protocol) {
    case ConnectionSettings::Protocol::Oscar:
        title = QStringLiteral("AIM / OSCAR Password");
        label = QStringLiteral("Password:");
        break;
    case ConnectionSettings::Protocol::Irc:
        title = QStringLiteral("IRC Server Password");
        label = QStringLiteral("Server password (leave blank if none):");
        allowEmpty = true;
        break;
    case ConnectionSettings::Protocol::Telnet:
        return true;
    case ConnectionSettings::Protocol::Sip:
        title = QStringLiteral("SIP Account Password");
        label = QStringLiteral("SIP password:");
        break;
    case ConnectionSettings::Protocol::Unknown:
        return false;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setModal(true);

    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    auto *promptLabel = new QLabel(label, &dialog);
    auto *secretEdit = new QLineEdit(&dialog);
    secretEdit->setEchoMode(QLineEdit::Password);
    secretEdit->setMinimumWidth(280);
    auto *savePassword = new QCheckBox(QStringLiteral("Save password on this computer"), &dialog);
    savePassword->setChecked(false);
    savePassword->setToolTip(
        QStringLiteral("Stores this password in the local WaffleHouse-Client settings file. "
                       "The saved value is not encrypted at rest."));

    auto *note = new QLabel(
        QStringLiteral("Saved passwords are stored in your local application settings and "
                       "are not encrypted at rest."),
        &dialog);
    note->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(secretEdit, &QLineEdit::textChanged, savePassword,
                     [savePassword](const QString &text) {
                         savePassword->setEnabled(!text.isEmpty());
                         if (text.isEmpty()) savePassword->setChecked(false);
                     });
    savePassword->setEnabled(false);

    outer->addWidget(promptLabel);
    outer->addWidget(secretEdit);
    outer->addWidget(savePassword);
    outer->addWidget(note);
    outer->addWidget(buttons);
    secretEdit->setFocus();

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    const QString secret = secretEdit->text();
    if (!allowEmpty && secret.isEmpty()) {
        QMessageBox::warning(
            this, title, QStringLiteral("A password is required to connect."));
        return false;
    }

    ConnectionSettings updated = state->backend->settings();
    updated.password = secret;
    updated.savePassword = savePassword->isChecked() && !secret.isEmpty();
    state->backend->setConnectionSettings(updated);
    state->hasSessionSecret = true;
    saveConnections();
    return true;
}

ChatBackend *MainWindow::createBackend(const ConnectionSettings &settings)
{
    switch (settings.protocol) {
    case ConnectionSettings::Protocol::Oscar:
        return new OscarBackend(settings, this);
    case ConnectionSettings::Protocol::Irc:
        return new IrcBackend(settings, this);
    case ConnectionSettings::Protocol::Telnet:
        return new TelnetBackend(settings, this);
    case ConnectionSettings::Protocol::Sip:
        return new SipBackend(settings, m_sipController, this);
    case ConnectionSettings::Protocol::Unknown:
        break;
    }
    return nullptr;
}

void MainWindow::wireBackend(ChatBackend *backend)
{
    if (!backend) {
        return;
    }

    connect(backend, &ChatBackend::connected, this,
            [this, backend](const QString &identity, const QString &endpoint) {
                handleConnected(backend, identity, endpoint);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::disconnected, this,
            [this, backend](const QString &reason) {
                handleDisconnected(backend, reason);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::eventReceived, this,
            [this, backend](const QString &kind, const QString &target, const QString &text) {
                handleEvent(backend, kind, target, text);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::membersChanged, this,
            [this, backend](const QString &room, const QString &action,
                            const QStringList &names) {
                handleMembers(backend, room, action, names);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::targetNamed, this,
            [this, backend](const QString &kind, const QString &target,
                            const QString &name) {
                handleTargetNamed(backend, kind, target, name);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::roomDiscovered, this,
            [this, backend](const QString &roomId, const QString &name) {
                handleRoomDiscovered(backend, roomId, name);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::buddyListChanged, this,
            [this, backend](const QStringList &names) {
                handleBuddyList(backend, names);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::buddyPresenceChanged, this,
            [this, backend](const QString &name, bool online) {
                handleBuddyPresence(backend, name, online);
            }, Qt::QueuedConnection);
    connect(backend, &ChatBackend::backendError, this,
            [this, backend](const QString &context, const QString &message) {
                handleBackendError(backend, context, message);
            }, Qt::QueuedConnection);
    if (auto *oscar = qobject_cast<OscarBackend *>(backend)) {
        connect(oscar, &OscarBackend::presenceChanged, this,
                [this, backend](const QString &presence, const QString &message, quint32 idleSeconds) {
                    if (BackendState *state = stateFor(backend)) {
                        state->presenceState = presence;
                        state->presenceMessage = message;
                        state->idleSeconds = idleSeconds;
                        updateConnectionItem(state);
                        refreshBuddyList();
                        QString text = presence;
                        if (idleSeconds > 0) text += QStringLiteral(" + IDLE %1s").arg(idleSeconds);
                        if (!message.isEmpty()) text += QStringLiteral(" — %1").arg(message);
                        statusBar()->showMessage(QStringLiteral("AIM presence: %1").arg(text), 4000);
                    }
                }, Qt::QueuedConnection);
    }
}

void MainWindow::attachBackend(ChatBackend *backend,
                               bool persist,
                               bool secretRequired,
                               bool hasSessionSecret,
                               bool autoConnect,
                               const QString &profileId)
{
    if (!backend || !m_connectionList) {
        return;
    }

    auto *state = new BackendState;
    state->backend = backend;
    state->profileId = profileId.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : profileId.trimmed();
    state->secretRequired = secretRequired;
    state->hasSessionSecret = hasSessionSecret;
    state->connectionItem = new QListWidgetItem(m_connectionList);
    state->connectionItem->setData(Qt::UserRole, backend->id());
    m_states.insert(backend->id(), state);
    if (backend->settings().protocol == ConnectionSettings::Protocol::Irc) {
        for (const QString &buddy : backend->settings().ircBuddies) {
            if (!buddy.trimmed().isEmpty()) state->buddies.insert(buddy.trimmed());
        }
    }

    // Wire and attach the backend to WaffleHouse state before a SIP account is
    // inserted into the live PJSUA2 endpoint. This prevents controller signals
    // from re-entering GUI refresh code while a new SipBackend is still under
    // construction (the 2.5 Add-SIP crash).
    wireBackend(backend);
    bool backendReady = true;
    if (auto *sip = qobject_cast<SipBackend *>(backend)) {
        for (const QString &contact : backend->settings().sipContacts) {
            if (!contact.trimmed().isEmpty()) state->buddies.insert(contact.trimmed());
        }
        QString sipError;
        backendReady = sip->initializeAccount(&sipError);
        if (!backendReady) {
            appendActivity(backend, QStringLiteral("SIP account initialization failed: %1").arg(sipError));
            QMessageBox::warning(this, QStringLiteral("SIP Account"),
                                 QStringLiteral("The SIP account was saved, but the softphone endpoint could not add it:\n\n%1").arg(sipError));
        }
    }
    updateConnectionItem(state);
    selectState(state);

    appendActivity(
        backend,
        persist
            ? QStringLiteral("Connection added.")
            : QStringLiteral("Saved connection restored."));

    refreshBuddyList();
    updateActions();

    if (persist) {
        saveConnections();
    }

    if (autoConnect && backendReady) {
        connectState(state);
    }
}

MainWindow::BackendState *MainWindow::stateFor(ChatBackend *backend) const
{
    return backend ? m_states.value(backend->id(), nullptr) : nullptr;
}

MainWindow::BackendState *MainWindow::stateById(const QString &id) const
{
    return m_states.value(id, nullptr);
}

MainWindow::BackendState *MainWindow::stateFromBuddyItem(QTreeWidgetItem *item) const
{
    if (!item) {
        return nullptr;
    }
    return stateById(item->data(0, Qt::UserRole).toString());
}

MainWindow::BackendState *MainWindow::selectedState() const
{
    if (m_buddyTree) {
        if (BackendState *state = stateFromBuddyItem(m_buddyTree->currentItem())) {
            return state;
        }
    }
    QListWidgetItem *item = m_connectionList ? m_connectionList->currentItem() : nullptr;
    return item ? stateById(item->data(Qt::UserRole).toString()) : nullptr;
}

void MainWindow::selectState(BackendState *state)
{
    if (!state || !state->backend) {
        return;
    }

    if (m_connectionList && state->connectionItem
        && m_connectionList->currentItem() != state->connectionItem) {
        m_connectionList->setCurrentItem(state->connectionItem);
    }

    if (m_buddyTree) {
        for (int i = 0; i < m_buddyTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *root = m_buddyTree->topLevelItem(i);
            if (root && root->data(0, Qt::UserRole).toString() == state->backend->id()) {
                if (!m_buddyTree->currentItem()
                    || stateFromBuddyItem(m_buddyTree->currentItem()) != state) {
                    m_buddyTree->setCurrentItem(root);
                }
                break;
            }
        }
    }
}

void MainWindow::updateConnectionItem(BackendState *state)
{
    if (!state || !state->backend || !state->connectionItem) {
        return;
    }

    const QString identity = state->identity.isEmpty()
        ? state->backend->settings().username
        : state->identity;
    QString stateWord = state->connecting
        ? QStringLiteral("Connecting")
        : statusWord(state->connected);
    if (state->connected
        && state->backend->settings().protocol == ConnectionSettings::Protocol::Oscar) {
        stateWord = state->presenceState.isEmpty() ? QStringLiteral("ONLINE") : state->presenceState;
        if (state->idleSeconds > 0) stateWord += QStringLiteral(" + Idle");
    }

    QString text = QStringLiteral("%1 — %2").arg(stateWord, state->backend->protocolName());
    if (!identity.isEmpty()) {
        text += QStringLiteral(" — %1").arg(identity);
    }
    state->connectionItem->setText(text);
    state->connectionItem->setToolTip(state->endpoint);
}

void MainWindow::updateActions()
{
    BackendState *state = selectedState();
    const bool exists = state && state->backend;
    const bool online = exists && state->connected;
    const bool connecting = exists && state->connecting;
    const bool editable = exists && !online && !connecting;
    const bool oscar = online
        && state->backend->settings().protocol == ConnectionSettings::Protocol::Oscar;
    const bool isTelnet = exists
        && state->backend->settings().protocol == ConnectionSettings::Protocol::Telnet;
    const bool isSip = exists
        && state->backend->settings().protocol == ConnectionSettings::Protocol::Sip;

    if (m_editConnectionAction) m_editConnectionAction->setEnabled(editable);
    if (m_deleteConnectionAction) m_deleteConnectionAction->setEnabled(exists);
    if (m_connectAction) m_connectAction->setEnabled(exists && !online && !connecting);
    if (m_disconnectAction) m_disconnectAction->setEnabled(exists && (online || connecting));

    if (m_editConnectionButton) m_editConnectionButton->setEnabled(editable);
    if (m_deleteConnectionButton) m_deleteConnectionButton->setEnabled(exists);
    if (m_connectButton) m_connectButton->setEnabled(exists && !online && !connecting);
    if (m_disconnectButton) m_disconnectButton->setEnabled(exists && (online || connecting));

    if (m_rawAction) m_rawAction->setEnabled(online && !isSip);
    if (m_changePasswordAction) m_changePasswordAction->setEnabled(oscar);
    if (m_fingerprintAction) m_fingerprintAction->setEnabled(exists && m_secureReady && !isTelnet && !isSip);


    if (m_trayConnectAction) {
        m_trayConnectAction->setEnabled(exists && !online && !connecting);
    }
    if (m_trayDisconnectAction) {
        m_trayDisconnectAction->setEnabled(exists && (online || connecting));
    }

    rebuildTrayMenu();
}

void MainWindow::refreshBuddyList()
{
    if (!m_buddyTree) {
        return;
    }

    QString selectedBackendId;
    QString selectedBuddy;
    if (QTreeWidgetItem *current = m_buddyTree->currentItem()) {
        selectedBackendId = current->data(0, Qt::UserRole).toString();
        selectedBuddy = current->data(0, Qt::UserRole + 1).toString();
    }

    m_buddyTree->clear();

    QList<BackendState *> states = m_states.values();
    std::sort(states.begin(), states.end(), [](BackendState *a, BackendState *b) {
        if (!a || !a->backend) return false;
        if (!b || !b->backend) return true;
        const int protocolCompare = a->backend->protocolName().compare(
            b->backend->protocolName(), Qt::CaseInsensitive);
        if (protocolCompare != 0) {
            return protocolCompare < 0;
        }
        const QString aName = a->identity.isEmpty() ? a->backend->settings().username : a->identity;
        const QString bName = b->identity.isEmpty() ? b->backend->settings().username : b->identity;
        return aName.compare(bName, Qt::CaseInsensitive) < 0;
    });

    QTreeWidgetItem *restoreItem = nullptr;

    // Every saved connection remains a top-level Communications Hub account,
    // including SIP/VoIP. Only the old embedded quick-dial panel was removed.
    for (BackendState *state : states) {
        if (!state || !state->backend) {
            continue;
        }

        QString accountName = state->identity.isEmpty()
            ? state->backend->settings().username
            : state->identity;
        if (accountName.isEmpty()) {
            accountName = state->backend->settings().protocol == ConnectionSettings::Protocol::Telnet
                ? state->backend->settings().server
                : state->backend->protocolName();
        }

        auto *root = new QTreeWidgetItem(m_buddyTree);
        root->setText(0, accountName);
        QString accountStatus = state->connecting ? QStringLiteral("Connecting")
                                                        : statusWord(state->connected);
        if (state->connected
            && state->backend->settings().protocol == ConnectionSettings::Protocol::Oscar) {
            accountStatus = state->presenceState.isEmpty() ? QStringLiteral("ONLINE") : state->presenceState;
            if (state->idleSeconds > 0) {
                const quint32 minutes = state->idleSeconds / 60;
                accountStatus += minutes > 0 ? QStringLiteral(" · Idle %1m").arg(minutes)
                                             : QStringLiteral(" · Idle %1s").arg(state->idleSeconds);
            }
        }
        root->setText(1, QStringLiteral("%1 — %2").arg(state->backend->protocolName(), accountStatus));
        if (!state->presenceMessage.isEmpty()) root->setToolTip(1, state->presenceMessage);
        root->setData(0, Qt::UserRole, state->backend->id());
        QFont rootFont = root->font(0);
        rootFont.setBold(true);
        root->setFont(0, rootFont);

        if (state->backend->settings().protocol == ConnectionSettings::Protocol::Oscar
            || state->backend->settings().protocol == ConnectionSettings::Protocol::Irc) {
            QStringList buddies = state->buddies.values();
            std::sort(buddies.begin(), buddies.end(), [state](const QString &a, const QString &b) {
                const bool aOnline = state->onlineBuddies.contains(a.toCaseFolded());
                const bool bOnline = state->onlineBuddies.contains(b.toCaseFolded());
                if (aOnline != bOnline) {
                    return aOnline;
                }
                return a.compare(b, Qt::CaseInsensitive) < 0;
            });

            for (const QString &buddy : buddies) {
                auto *item = new QTreeWidgetItem(root);
                item->setText(0, buddy);
                const bool online = state->onlineBuddies.contains(buddy.toCaseFolded());
                item->setText(1, online ? QStringLiteral("Online") : QStringLiteral("Offline"));
                item->setData(0, Qt::UserRole, state->backend->id());
                item->setData(0, Qt::UserRole + 1, buddy);
                if (online) {
                    QFont font = item->font(0);
                    font.setBold(true);
                    item->setFont(0, font);
                }

                if (state->backend->id() == selectedBackendId
                    && buddy == selectedBuddy) {
                    restoreItem = item;
                }
            }
        }

        root->setExpanded(true);
        if (!restoreItem && state->backend->id() == selectedBackendId
            && selectedBuddy.isEmpty()) {
            restoreItem = root;
        }
    }

    m_buddyTree->resizeColumnToContents(0);
    if (restoreItem) {
        m_buddyTree->setCurrentItem(restoreItem);
    } else if (m_buddyTree->topLevelItemCount() > 0 && !m_buddyTree->currentItem()) {
        m_buddyTree->setCurrentItem(m_buddyTree->topLevelItem(0));
    }
    refreshSoftphoneControls();
}

void MainWindow::refreshSoftphoneControls()
{
    if (!m_buddySipAccount) return;
    const QString previous = m_buddySipAccount->currentData().toString().isEmpty()
        ? m_sipController->selectedAccountId() : m_buddySipAccount->currentData().toString();
    const QSignalBlocker blocker(m_buddySipAccount);
    m_buddySipAccount->clear();
    QList<BackendState *> states = m_states.values();
    std::sort(states.begin(), states.end(), [](BackendState *a, BackendState *b) {
        if (!a || !a->backend) {
            return false;
        }
        if (!b || !b->backend) {
            return true;
        }
        return a->backend->settings().sipProfileName.compare(b->backend->settings().sipProfileName, Qt::CaseInsensitive) < 0;
    });
    for (BackendState *state : states) {
        if (!state || !state->backend || state->backend->settings().protocol != ConnectionSettings::Protocol::Sip) continue;
        const auto &cfg = state->backend->settings();
        const QString label = cfg.sipProfileName.trimmed().isEmpty()
            ? QStringLiteral("%1@%2").arg(cfg.username, cfg.sipDomain.isEmpty() ? cfg.server : cfg.sipDomain)
            : cfg.sipProfileName;
        const QString reg = m_sipController->registrationText(state->backend->id());
        m_buddySipAccount->addItem(QStringLiteral("%1 — %2").arg(label, reg), state->backend->id());
    }
    int idx = m_buddySipAccount->findData(previous);
    if (idx < 0 && m_buddySipAccount->count() > 0) idx = 0;
    if (idx >= 0) {
        m_buddySipAccount->setCurrentIndex(idx);
        m_sipController->setSelectedAccountId(m_buddySipAccount->currentData().toString());
    }
    const bool haveSip = m_buddySipAccount->count() > 0;
    const QString selectedSipId=haveSip?m_buddySipAccount->currentData().toString():QString();
    if(m_buddyDialPrefix){
        const QSignalBlocker prefixBlocker(m_buddyDialPrefix);
        m_buddyDialPrefix->setEnabled(haveSip);
        m_buddyDialPrefix->setText(haveSip?m_sipController->dialPrefix(selectedSipId):QString());
    }
    BackendState *selectedSip = haveSip ? stateById(selectedSipId) : nullptr;
    const bool sipOnline = selectedSip && selectedSip->connected;
    const bool sipConnecting = selectedSip && selectedSip->connecting;
    if (m_buddyDial) m_buddyDial->setEnabled(haveSip);
    if (m_buddyDialButton) m_buddyDialButton->setEnabled(haveSip);
    if (m_buddySipConnectButton) m_buddySipConnectButton->setEnabled(selectedSip && !sipOnline && !sipConnecting);
    if (m_buddySipDisconnectButton) m_buddySipDisconnectButton->setEnabled(selectedSip && (sipOnline || sipConnecting));
}

void MainWindow::appendActivity(ChatBackend *backend, const QString &text)
{
    if (!m_activity) {
        return;
    }
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString label = backend ? backend->protocolName() : appDisplayName();
    m_activity->appendPlainText(
        QStringLiteral("[%1] [%2] %3").arg(timestamp, label, text));
}

void MainWindow::rebuildTrayMenu()
{
    if (!m_trayIcon) {
        return;
    }

    int onlineCount = 0;
    int totalCount = 0;
    for (BackendState *state : m_states) {
        if (!state || !state->backend) {
            continue;
        }
        ++totalCount;
        if (state->connected) {
            ++onlineCount;
        }
    }

    m_trayIcon->setToolTip(
        QStringLiteral("%1 — %2/%3 connection(s) online")
            .arg(appDisplayName())
            .arg(onlineCount)
            .arg(totalCount));
}

QString MainWindow::conversationKey(ChatBackend *backend,
                                    const QString &kind,
                                    const QString &target) const
{
    return QStringLiteral("%1|%2|%3")
        .arg(backend ? backend->id() : QStringLiteral("none"),
             kind,
             target.toCaseFolded());
}

QString MainWindow::targetDisplayName(BackendState *state,
                                      const QString &kind,
                                      const QString &target) const
{
    if (!state) {
        return target;
    }
    return state->targetNames.value(QStringLiteral("%1|%2").arg(kind, target), target);
}

QString MainWindow::conversationOpacityKey(ChatBackend *backend,
                                           const QString &kind,
                                           const QString &target) const
{
    if (!backend) {
        return QStringLiteral("ui/conversations/default/opacity");
    }
    const ConnectionSettings &s = backend->settings();
    const QString identity = QStringLiteral("%1|%2|%3|%4|%5|%6")
        .arg(static_cast<int>(s.protocol))
        .arg(s.server)
        .arg(s.port)
        .arg(s.username)
        .arg(kind)
        .arg(target.toCaseFolded());
    const QByteArray hash = QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("ui/conversations/%1/opacity")
        .arg(QString::fromLatin1(hash));
}

ChatWindow *MainWindow::ensureConversationWindow(ChatBackend *backend,
                                                 const QString &kind,
                                                 const QString &target,
                                                 bool showWindow)
{
    if (!backend || target.isEmpty()) {
        return nullptr;
    }

    BackendState *state = stateFor(backend);
    if (!state || !state->connected) {
        return nullptr;
    }

    const QString key = conversationKey(backend, kind, target);
    if (ChatWindow *existing = m_windows.value(key, nullptr)) {
        if (showWindow) {
            existing->show();
            existing->raise();
            existing->activateWindow();
        }
        return existing;
    }

    auto *window = new ChatWindow(
        backend,
        kind,
        target,
        targetDisplayName(state, kind, target),
        conversationOpacityKey(backend, kind, target));
    window->setBackendOnline(true);
    window->setShowTimestamps(m_options.showTimestamps);
    window->setShowSidePane(m_options.showSidePanes);
    m_windows.insert(key, window);

    connect(window, &ChatWindow::conversationClosing,
            this, &MainWindow::handleConversationClosing);
    connect(window, &ChatWindow::messageSubmitted,
            this, &MainWindow::handleConversationMessage);
    connect(window, &ChatWindow::terminalBytesSubmitted, this,
            [this](ChatWindow *w, const QByteArray &bytes) {
                if (!w) return;
                BackendState *state = stateById(w->backendId());
                if (state && state->backend && state->connected
                    && state->backend->settings().protocol == ConnectionSettings::Protocol::Telnet) {
                    state->backend->sendTerminalInput(bytes);
                }
            });
    connect(window, &ChatWindow::secureRequested,
            this, &MainWindow::startSecureSession);
    connect(window, &ChatWindow::secureStatusRequested,
            this, &MainWindow::showSecureStatus);
    connect(window, &ChatWindow::trustRequested,
            this, &MainWindow::trustSecurePeer);
    connect(window, &ChatWindow::untrustRequested,
            this, &MainWindow::untrustSecurePeer);
    connect(window, &ChatWindow::secureOffRequested,
            this, &MainWindow::closeSecureSession);
    connect(window, &ChatWindow::fileSendRequested,
            this, &MainWindow::sendFile);
    connect(window, &QObject::destroyed, this, [this, key] {
        m_windows.remove(key);
    });
    updateConversationSecurity(window);

    if (showWindow) {
        window->show();
    }
    return window;
}

void MainWindow::closeBackendWindows(ChatBackend *backend)
{
    if (!backend) {
        return;
    }
    const QString backendId = backend->id();
    const auto windows = m_windows.values();
    for (ChatWindow *window : windows) {
        if (window && window->backendId() == backendId) {
            window->setBackendOnline(false);
            window->close();
        }
    }
}

void MainWindow::connectState(BackendState *state)
{
    if (!state || !state->backend || state->connected || state->connecting) {
        return;
    }
    if (!ensureConnectionSecret(state)) {
        return;
    }

    state->connecting = true;
    updateConnectionItem(state);
    refreshBuddyList();
    updateActions();
    appendActivity(state->backend, QStringLiteral("Connecting…"));
    state->backend->start();
}

void MainWindow::connectSelected()
{
    connectState(selectedState());
}

void MainWindow::disconnectSelected()
{
    BackendState *state = selectedState();
    if (!state || !state->backend || (!state->connected && !state->connecting)) {
        return;
    }

    appendActivity(state->backend, QStringLiteral("Disconnecting…"));
    state->connected = false;
    state->connecting = false;
    state->onlineBuddies.clear();
    m_secure.closeConnection(state->profileId);
    closeBackendWindows(state->backend);
    updateConnectionItem(state);
    refreshBuddyList();
    updateActions();
    state->backend->stop();
}

void MainWindow::deleteSelected()
{
    BackendState *state = selectedState();
    if (!state || !state->backend) {
        return;
    }
    if (state->backend->settings().protocol == ConnectionSettings::Protocol::Sip) {
        for (const auto &call : m_sipController->calls()) {
            if (!call.disconnected && QString::fromStdString(call.accountId) == state->backend->id()) {
                QMessageBox::information(this, QStringLiteral("SIP Account In Use"),
                                         QStringLiteral("Hang up active calls on this SIP account before deleting it."));
                return;
            }
        }
    }

    const QString identity = state->identity.isEmpty()
        ? state->backend->settings().username
        : state->identity;
    const QString description = identity.isEmpty()
        ? state->backend->protocolName()
        : QStringLiteral("%1 — %2").arg(state->backend->protocolName(), identity);

    if (QMessageBox::question(
            this,
            QStringLiteral("Delete Connection"),
            QStringLiteral("Delete the saved connection for %1?")
                .arg(description)) != QMessageBox::Yes) {
        return;
    }

    ChatBackend *backend = state->backend;
    const QString backendId = backend->id();
    state->connected = false;
    state->connecting = false;
    m_secure.closeConnection(state->profileId);
    closeBackendWindows(backend);
    QObject::disconnect(backend, nullptr, this, nullptr);
    backend->stop();

    if (m_connectionList && state->connectionItem) {
        const int row = m_connectionList->row(state->connectionItem);
        if (row >= 0) {
            delete m_connectionList->takeItem(row);
        }
    }

    m_states.remove(backendId);
    state->connectionItem = nullptr;
    delete state;
    backend->deleteLater();

    refreshBuddyList();
    updateActions();
    saveConnections();
    statusBar()->showMessage(QStringLiteral("Connection deleted"), 3000);
}

void MainWindow::editSelected()
{
    BackendState *state = selectedState();
    if (!state || !state->backend) {
        return;
    }
    openConnectionDialog(state->backend->settings(), state);
}

void MainWindow::newIm()
{
    BackendState *state = selectedState();
    if (!state || !state->backend) return;

    QString preset;
    if (QTreeWidgetItem *item = m_buddyTree ? m_buddyTree->currentItem() : nullptr) {
        if (item->parent()) preset = item->data(0, Qt::UserRole + 1).toString().trimmed();
    }
    openMessagingDialog(state, preset, false);
}

void MainWindow::joinRoom()
{
    BackendState *state = selectedState();
    if (!state || !state->backend) return;
    openMessagingDialog(state, QString(), true);
}

void MainWindow::addBuddy()
{
    BackendState *state = selectedState();
    if (!state || !state->connected || !state->backend) return;
    const auto protocol = state->backend->settings().protocol;
    if (protocol != ConnectionSettings::Protocol::Oscar
        && protocol != ConnectionSettings::Protocol::Irc) return;

    const bool ircBuddy = protocol == ConnectionSettings::Protocol::Irc;
    bool ok = false;
    const QString buddy = QInputDialog::getText(
        this,
        ircBuddy ? QStringLiteral("Add IRC Buddy / Watch") : QStringLiteral("Add AIM Buddy"),
        ircBuddy ? QStringLiteral("Nickname to watch:") : QStringLiteral("Screen name:"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (ok && !buddy.isEmpty()) {
        state->backend->addBuddy(buddy);
    }
}

void MainWindow::removeBuddy()
{
    QTreeWidgetItem *item = m_buddyTree ? m_buddyTree->currentItem() : nullptr;
    if (!item || !item->parent()) {
        return;
    }

    BackendState *state = stateFromBuddyItem(item);
    const QString buddy = item->data(0, Qt::UserRole + 1).toString();
    if (!state || !state->connected || !state->backend || buddy.isEmpty()) {
        return;
    }

    const bool ircBuddy = state->backend->settings().protocol == ConnectionSettings::Protocol::Irc;
    if (QMessageBox::question(
            this,
            QStringLiteral("Remove Buddy"),
            ircBuddy
                ? QStringLiteral("Remove %1 from this IRC profile's local buddy/watch list?").arg(buddy)
                : QStringLiteral("Remove %1 from the AIM buddy list?").arg(buddy))
        == QMessageBox::Yes) {
        state->backend->removeBuddy(buddy);
    }
}

void MainWindow::setAimPresence(BackendState *state)
{
    if (!state || !state->backend || !state->connected
        || state->backend->settings().protocol != ConnectionSettings::Protocol::Oscar) {
        QMessageBox::information(this, QStringLiteral("AIM Status"),
                                 QStringLiteral("Select and connect an AIM/OSCAR account first."));
        return;
    }
    auto *oscar = qobject_cast<OscarBackend *>(state->backend);
    if (!oscar) return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("AIM Status / AFK — %1").arg(accountMenuLabel(state)));
    auto *form = new QFormLayout(&dialog);
    auto *mode = new QComboBox(&dialog);
    mode->addItems({QStringLiteral("Online"), QStringLiteral("Away"),
                    QStringLiteral("AFK"), QStringLiteral("Idle")});
    const QString current = state->presenceState.toCaseFolded();
    if (current == QStringLiteral("away")) mode->setCurrentText(QStringLiteral("Away"));
    else if (current == QStringLiteral("afk")) mode->setCurrentText(QStringLiteral("AFK"));
    else if (state->idleSeconds > 0) mode->setCurrentText(QStringLiteral("Idle"));
    auto *message = new QLineEdit(state->presenceMessage, &dialog);
    message->setPlaceholderText(QStringLiteral("Away/AFK message (optional)"));
    auto *idle = new QSpinBox(&dialog);
    idle->setRange(0, 2147483647);
    idle->setSuffix(QStringLiteral(" seconds"));
    idle->setValue(static_cast<int>(std::min<quint32>(state->idleSeconds, 2147483647U)));
    form->addRow(QStringLiteral("Status:"), mode);
    form->addRow(QStringLiteral("Message:"), message);
    form->addRow(QStringLiteral("Idle time:"), idle);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    const QString selected = mode->currentText();
    state->autoPresenceState.clear();
    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();
    if (selected == QStringLiteral("Online")) {
        oscar->setBack();
    } else if (selected == QStringLiteral("Away")) {
        oscar->setAwayMessage(message->text());
        if (idle->value() > 0) oscar->setIdleSeconds(static_cast<quint32>(idle->value()));
    } else if (selected == QStringLiteral("AFK")) {
        oscar->setAfkMessage(message->text());
        if (idle->value() > 0) oscar->setIdleSeconds(static_cast<quint32>(idle->value()));
    } else {
        oscar->setIdleSeconds(static_cast<quint32>(std::max(1, idle->value())));
    }
}

void MainWindow::changePassword()
{
    BackendState *state = selectedState();
    if (!state || !state->connected || !state->backend
        || state->backend->settings().protocol != ConnectionSettings::Protocol::Oscar) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Change AIM Password"));
    auto *outer = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    QLineEdit current(&dialog);
    QLineEdit next(&dialog);
    QLineEdit confirm(&dialog);
    current.setEchoMode(QLineEdit::Password);
    next.setEchoMode(QLineEdit::Password);
    confirm.setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Current password:"), &current);
    form->addRow(QStringLiteral("New password:"), &next);
    form->addRow(QStringLiteral("Confirm new password:"), &confirm);
    outer->addLayout(form);
    QDialogButtonBox buttons(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    outer->addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        if (next.text().isEmpty() || next.text() != confirm.text()) {
            QMessageBox::warning(
                this,
                QStringLiteral("Password change"),
                QStringLiteral("The new passwords are empty or do not match."));
            return;
        }
        state->backend->changePassword(current.text(), next.text());
    }
}

void MainWindow::changeIrcNick()
{
    BackendState *state = selectedState();
    if (!state || !state->connected || !state->backend
        || state->backend->settings().protocol != ConnectionSettings::Protocol::Irc) {
        return;
    }

    bool ok = false;
    const QString nick = QInputDialog::getText(
        this,
        QStringLiteral("Change IRC Nickname"),
        QStringLiteral("New nickname:"),
        QLineEdit::Normal,
        state->identity,
        &ok).trimmed();
    if (ok && !nick.isEmpty()) {
        state->backend->changeNickname(nick);
    }
}

void MainWindow::rawProtocolCommand()
{
    BackendState *state = selectedState();
    if (!state || !state->connected || !state->backend) {
        return;
    }

    const auto protocol = state->backend->settings().protocol;
    if (protocol == ConnectionSettings::Protocol::Irc) {
        bool ok = false;
        const QString line = QInputDialog::getText(
            this,
            QStringLiteral("Raw IRC Command"),
            QStringLiteral("IRC line:"),
            QLineEdit::Normal,
            QString(),
            &ok).trimmed();
        if (ok && !line.isEmpty()) {
            state->backend->sendRaw(line);
        }
        return;
    }

    if (protocol == ConnectionSettings::Protocol::Telnet) {
        bool ok = false;
        const QString line = QInputDialog::getText(
            this,
            QStringLiteral("Raw Telnet Line"),
            QStringLiteral("Line to send:"),
            QLineEdit::Normal,
            QString(),
            &ok);
        if (ok) {
            state->backend->sendRaw(line);
        }
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Raw OSCAR SNAC"));
    auto *outer = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    QLineEdit family(QStringLiteral("0x01"), &dialog);
    QLineEdit subtype(QStringLiteral("0x16"), &dialog);
    QLineEdit body(&dialog);
    body.setPlaceholderText(QStringLiteral("hex body, optional"));
    form->addRow(QStringLiteral("Family:"), &family);
    form->addRow(QStringLiteral("Subtype:"), &subtype);
    form->addRow(QStringLiteral("Hex body:"), &body);
    outer->addLayout(form);
    QDialogButtonBox buttons(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    outer->addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        state->backend->sendRaw(family.text(), subtype.text(), body.text());
    }
}

void MainWindow::setBuddyTransparency()
{
    bool ok = false;
    const int current = static_cast<int>(m_buddyOpacity * 100.0 + 0.5);
    const int percent = QInputDialog::getInt(
        this,
        QStringLiteral("Buddy List Transparency"),
        QStringLiteral("Window opacity (%):"),
        current,
        30,
        100,
        5,
        &ok);
    if (!ok) {
        return;
    }
    m_buddyOpacity = static_cast<double>(percent) / 100.0;
    setWindowOpacity(m_buddyOpacity);
    saveUiSettings();
}

void MainWindow::setConnectionsTransparency()
{
    if (!m_connectionsWindow) {
        return;
    }
    bool ok = false;
    const int current = static_cast<int>(m_connectionsOpacity * 100.0 + 0.5);
    const int percent = QInputDialog::getInt(
        this,
        QStringLiteral("Connections Window Transparency"),
        QStringLiteral("Window opacity (%):"),
        current,
        30,
        100,
        5,
        &ok);
    if (!ok) {
        return;
    }
    m_connectionsOpacity = static_cast<double>(percent) / 100.0;
    m_connectionsWindow->setWindowOpacity(m_connectionsOpacity);
    saveUiSettings();
}

void MainWindow::showConnectionsWindow()
{
    if (!m_connectionsWindow) {
        return;
    }
    m_connectionsWindow->show();
    m_connectionsWindow->raise();
    m_connectionsWindow->activateWindow();
}

void MainWindow::showBuddyWindow()
{
    show();
    raise();
    activateWindow();
}

void MainWindow::quitApplication()
{
    if (m_quitting) {
        return;
    }
    m_quitting = true;
    saveConnections();
    saveUiSettings();

    const auto windows = m_windows.values();
    for (ChatWindow *window : windows) {
        if (window) {
            window->close();
        }
    }

    if (m_connectionsWindow) {
        m_connectionsWindow->close();
    }

    for (BackendState *state : m_states) {
        if (state && state->backend) {
            state->connected = false;
            state->connecting = false;
            QObject::disconnect(state->backend, nullptr, this, nullptr);
            state->backend->stop();
        }
    }

    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    close();
    QApplication::quit();
}

QString MainWindow::imPayload(const QString &text) const
{
    if (text.startsWith(QLatin1Char('<'))) {
        const int end = text.indexOf(QStringLiteral("> "));
        if (end >= 0) return text.mid(end + 2);
    }
    return text;
}

QString MainWindow::imSpeakerPrefix(const QString &text) const
{
    if (text.startsWith(QLatin1Char('<'))) {
        const int end = text.indexOf(QStringLiteral("> "));
        if (end >= 0) return text.left(end + 2);
    }
    return {};
}

QString MainWindow::secureTrustKey(BackendState *state, const QString &target) const
{
    if (!state || state->profileId.isEmpty()) return {};
    const QString source = state->profileId + QChar(0x1f) + target.toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("security/trustedPeers/%1").arg(QString::fromLatin1(digest));
}

QString MainWindow::trustedFingerprint(BackendState *state, const QString &target) const
{
    const QString key = secureTrustKey(state, target);
    if (key.isEmpty()) return {};
    QSettings settings;
    return settings.value(key).toString();
}

void MainWindow::setTrustedFingerprint(BackendState *state,
                                       const QString &target,
                                       const QString &fingerprint)
{
    const QString key = secureTrustKey(state, target);
    if (key.isEmpty() || fingerprint.isEmpty()) return;
    QSettings settings;
    settings.setValue(key, fingerprint);
    settings.sync();
}

void MainWindow::clearTrustedFingerprint(BackendState *state, const QString &target)
{
    const QString key = secureTrustKey(state, target);
    if (key.isEmpty()) return;
    QSettings settings;
    settings.remove(key);
    settings.sync();
}

void MainWindow::startSecureSession(ChatWindow *window)
{
    if (window && window->kind() == QStringLiteral("chat")) {
        startSecureRoom(window);
        return;
    }
    if (!window || window->kind() != QStringLiteral("im")) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !state->connected || !state->backend) return;
    if (!m_options.encryptedDmEnabled || !m_secureReady) {
        QMessageBox::warning(this, QStringLiteral("Encrypted DMs"),
                             m_secureReady
                                 ? QStringLiteral("Encrypted communications are disabled in Tools > Options.")
                                 : QStringLiteral("Encrypted communications are unavailable: %1").arg(m_secureError));
        return;
    }
    if (state->backend->settings().protocol == ConnectionSettings::Protocol::Telnet) return;

    QString notice;
    const QString frame = m_secure.beginHandshake(state->profileId, window->target(), &notice);
    if (frame.isEmpty()) {
        window->appendMessage(QStringLiteral("[error] [secure] Could not start secure handshake."));
        return;
    }
    m_outgoingSecureFrames.insert(state->profileId + QChar(0x1f)
        + window->target().toCaseFolded() + QChar(0x1f) + frame);
    state->backend->sendPrivateMessage(window->target(), frame);
    window->appendMessage(QStringLiteral("[secure] %1").arg(notice));
    updateConversationSecurity(window);
}

void MainWindow::showSecureStatus(ChatWindow *window)
{
    if (window && window->kind() == QStringLiteral("chat")) {
        showSecureRoomStatus(window);
        return;
    }
    if (!window || window->kind() != QStringLiteral("im")) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !m_secureReady) return;

    const QString peer = m_secure.peerFingerprint(state->profileId, window->target());
    const QString local = m_secure.localFingerprint(state->profileId);
    const QString trusted = trustedFingerprint(state, window->target());
    QString trustState = QStringLiteral("unverified");
    if (!peer.isEmpty() && trusted == peer) trustState = QStringLiteral("trusted / verified");
    else if (!trusted.isEmpty() && trusted != peer) trustState = QStringLiteral("TRUST MISMATCH");

    const QString body = peer.isEmpty()
        ? QStringLiteral("No secure session is active with %1.\n\nLocal fingerprint:\n%2")
              .arg(window->displayName(), local)
        : QStringLiteral("Secure session with %1\n\nPeer fingerprint:\n%2\n\nLocal fingerprint:\n%3\n\nTrust: %4\n\nCompare the peer fingerprint using a separate trusted channel before trusting it.")
              .arg(window->displayName(), peer, local, trustState);
    QMessageBox::information(this, QStringLiteral("Secure Session Status"), body);
}

void MainWindow::trustSecurePeer(ChatWindow *window)
{
    if (!window || window->kind() != QStringLiteral("im")) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !m_secureReady) return;
    const QString peer = m_secure.peerFingerprint(state->profileId, window->target());
    if (peer.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Trust Peer"),
                                 QStringLiteral("Start a secure session first so the client can receive the peer fingerprint."));
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        QStringLiteral("Trust Peer Fingerprint"),
        QStringLiteral("Peer: %1\n\nFingerprint:\n%2\n\nHave you compared this fingerprint through a separate trusted channel and confirmed it matches?")
            .arg(window->displayName(), peer),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    setTrustedFingerprint(state, window->target(), peer);
    window->appendMessage(QStringLiteral("[secure] Peer fingerprint trusted: %1").arg(peer));
    updateConversationSecurity(window);
}

void MainWindow::untrustSecurePeer(ChatWindow *window)
{
    if (!window || window->kind() != QStringLiteral("im")) return;
    BackendState *state = stateById(window->backendId());
    if (!state) return;
    clearTrustedFingerprint(state, window->target());
    window->appendMessage(QStringLiteral("[secure] Saved trusted fingerprint cleared."));
    updateConversationSecurity(window);
}

void MainWindow::closeSecureSession(ChatWindow *window)
{
    if (window && window->kind() == QStringLiteral("chat")) {
        closeSecureRoom(window);
        return;
    }
    if (!window || window->kind() != QStringLiteral("im")) return;
    BackendState *state = stateById(window->backendId());
    if (!state) return;
    m_secure.closeSession(state->profileId, window->target());
    window->appendMessage(QStringLiteral("[secure] Secure session closed; messages are plaintext until a new secure session is started."));
    updateConversationSecurity(window);
}

void MainWindow::startSecureRoom(ChatWindow *window)
{
    if (!window || window->kind() != QStringLiteral("chat")) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !state->connected || !state->backend) return;
    const auto protocol = state->backend->settings().protocol;
    if (protocol != ConnectionSettings::Protocol::Oscar
        && protocol != ConnectionSettings::Protocol::Irc) {
        window->appendMessage(QStringLiteral("[error] [secure-room] Secure rooms are available only for AIM/OSCAR and IRC chats."));
        return;
    }
    if (!m_options.encryptedDmEnabled || !m_secureReady) {
        QMessageBox::warning(this, QStringLiteral("Secure Room"),
                             m_secureReady
                                 ? QStringLiteral("Secure communications are disabled in Tools > Options.")
                                 : QStringLiteral("Secure communications are unavailable: %1").arg(m_secureError));
        return;
    }

    QString error;
    if (!m_secureRooms.createOrRotate(state->profileId, window->target(), &error)) {
        window->appendMessage(QStringLiteral("[error] [secure-room] %1").arg(error));
        return;
    }

    window->appendMessage(QStringLiteral(
        "[secure-room] New shared room key %1 created. Key material is distributed only through encrypted CPX private sessions; the public room will carry ciphertext.")
        .arg(m_secureRooms.keyId(state->profileId, window->target())));
    distributeSecureRoomKeyToMembers(state, window);
    updateConversationSecurity(window);
}

void MainWindow::showSecureRoomStatus(ChatWindow *window)
{
    if (!window || window->kind() != QStringLiteral("chat")) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !m_secureReady) return;

    const bool active = m_secureRooms.hasRoom(state->profileId, window->target());
    const QString id = m_secureRooms.keyId(state->profileId, window->target());
    const QString role = m_secureRooms.locallyOwned(state->profileId, window->target())
        ? QStringLiteral("key owner / distributor")
        : QStringLiteral("participant");
    const QString body = active
        ? QStringLiteral(
            "Secure room: %1\n\nKey ID: %2\nRole: %3\n\n"
            "Room messages are encrypted with XChaCha20-Poly1305 before being sent to IRC/AIM. "
            "The room key itself is delivered separately through CPX encrypted private sessions. "
            "Any plaintext message received while secure-room mode is active is marked [plaintext].")
              .arg(window->displayName(), id, role)
        : QStringLiteral("No secure room key is active for %1.\n\nUse /secure or Security > Start Secure Room.")
              .arg(window->displayName());
    QMessageBox::information(this, QStringLiteral("Secure Room Status"), body);
}

void MainWindow::closeSecureRoom(ChatWindow *window)
{
    if (!window || window->kind() != QStringLiteral("chat")) return;
    BackendState *state = stateById(window->backendId());
    if (!state) return;
    m_secureRooms.closeRoom(state->profileId, window->target());
    window->appendMessage(QStringLiteral(
        "[secure-room] Secure room closed locally. New messages from this client will be plaintext until /secure is started again."));
    updateConversationSecurity(window);
}

void MainWindow::distributeSecureRoomKey(BackendState *state,
                                         ChatWindow *window,
                                         const QString &peer)
{
    if (!state || !state->backend || !window || peer.trimmed().isEmpty()) return;
    const QString cleanPeer = peer.trimmed();
    const QString own = state->identity.isEmpty()
        ? state->backend->settings().username
        : state->identity;
    if (!own.isEmpty() && cleanPeer.compare(own, Qt::CaseInsensitive) == 0) return;

    const QString pendingKey = state->profileId + QChar(0x1f) + cleanPeer.toCaseFolded();
    const QString room = window->target();

    if (!m_secure.hasSession(state->profileId, cleanPeer)) {
        m_pendingSecureRoomKeys[pendingKey].insert(room);
        window->appendMessage(QStringLiteral(
            "[secure-room] %1 is not included yet: establish a secure PM with that user first. "
            "Once the CPX session is active, WaffleHouse will send this room key automatically.")
            .arg(cleanPeer));
        return;
    }

    const QStringList caps = m_secure.peerCapabilities(state->profileId, cleanPeer);
    if (!m_secure.peerSupports(state->profileId, cleanPeer, QStringLiteral("secure-room-v1"))) {
        if (caps.isEmpty()) {
            m_pendingSecureRoomKeys[pendingKey].insert(room);
            window->appendMessage(QStringLiteral(
                "[secure-room] Waiting for %1 to advertise secure-room capability.").arg(cleanPeer));
        } else {
            m_pendingSecureRoomKeys[pendingKey].remove(room);
            window->appendMessage(QStringLiteral(
                "[secure-room] %1 does not advertise secure-room-v1 and will not receive this room key.").arg(cleanPeer));
        }
        return;
    }

    QString error;
    const QString offer = m_secureRooms.keyOffer(state->profileId, room, &error);
    if (offer.isEmpty()) {
        window->appendMessage(QStringLiteral("[error] [secure-room] %1").arg(error));
        return;
    }
    const QString encrypted = m_secure.encrypt(state->profileId, cleanPeer, offer, &error);
    if (encrypted.isEmpty()) {
        window->appendMessage(QStringLiteral(
            "[error] [secure-room] Could not encrypt room key for %1: %2").arg(cleanPeer, error));
        return;
    }

    m_outgoingSecureFrames.insert(state->profileId + QChar(0x1f)
        + cleanPeer.toCaseFolded() + QChar(0x1f) + encrypted);
    state->backend->sendPrivateMessage(cleanPeer, encrypted);
    m_pendingSecureRoomKeys[pendingKey].remove(room);
    if (m_pendingSecureRoomKeys[pendingKey].isEmpty()) m_pendingSecureRoomKeys.remove(pendingKey);
    window->appendMessage(QStringLiteral(
        "[secure-room] Room key %1 sent privately to %2 over CPX encryption.")
        .arg(m_secureRooms.keyId(state->profileId, room), cleanPeer));
}

void MainWindow::distributeSecureRoomKeyToMembers(BackendState *state, ChatWindow *window)
{
    if (!state || !window) return;
    QStringList members = window->members();
    members.sort(Qt::CaseInsensitive);
    int peers = 0;
    for (const QString &member : members) {
        const QString own = state->identity.isEmpty()
            ? state->backend->settings().username
            : state->identity;
        if (!own.isEmpty() && member.compare(own, Qt::CaseInsensitive) == 0) continue;
        ++peers;
        distributeSecureRoomKey(state, window, member);
    }
    if (peers == 0) {
        window->appendMessage(QStringLiteral(
            "[secure-room] No other room members are currently known; the key will be distributed when members are discovered."));
    }
}

void MainWindow::flushPendingSecureRoomKeys(BackendState *state, const QString &peer)
{
    if (!state || !state->backend || peer.trimmed().isEmpty()) return;
    const QString key = state->profileId + QChar(0x1f) + peer.trimmed().toCaseFolded();
    const QSet<QString> rooms = m_pendingSecureRoomKeys.value(key);
    if (rooms.isEmpty()) return;
    for (const QString &room : rooms) {
        ChatWindow *window = m_windows.value(
            conversationKey(state->backend, QStringLiteral("chat"), room), nullptr);
        if (window && m_secureRooms.hasRoom(state->profileId, room)) {
            distributeSecureRoomKey(state, window, peer);
        }
    }
}

bool MainWindow::handleSecureRoomKeyOffer(BackendState *state,
                                          const QString &peer,
                                          const QString &plaintext)
{
    if (!state || !SecureRoomManager::looksLikeKeyOffer(plaintext)) return false;
    QString room, id, error;
    if (!m_secureRooms.installKeyOffer(state->profileId, plaintext, &room, &id, &error)) {
        appendActivity(state->backend,
            QStringLiteral("[error] [secure-room] Key offer from %1 rejected: %2").arg(peer, error));
        return true;
    }

    ChatWindow *roomWindow = m_windows.value(
        conversationKey(state->backend, QStringLiteral("chat"), room), nullptr);
    const QString notice = QStringLiteral(
        "[secure-room] Installed shared room key %1 received privately from %2. Public room ciphertext can now be decrypted.")
        .arg(id, peer);
    if (roomWindow) {
        roomWindow->appendMessage(notice);
        updateConversationSecurity(roomWindow);
    } else {
        appendActivity(state->backend, QStringLiteral("%1 (%2)").arg(notice, room));
    }
    return true;
}

void MainWindow::showSelectedFingerprint()
{
    BackendState *state = selectedState();
    if (!state || !m_secureReady) {
        QMessageBox::warning(this, QStringLiteral("Secure Identity"),
                             m_secureReady ? QStringLiteral("Select a connection first.")
                                           : QStringLiteral("Encrypted communications are unavailable: %1").arg(m_secureError));
        return;
    }
    QMessageBox::information(
        this,
        QStringLiteral("Secure Identity Fingerprint"),
        QStringLiteral("This saved connection profile has its own stable secure identity.\n\n%1\n\nFingerprint:\n%2")
            .arg(state->backend ? state->backend->protocolName() : QStringLiteral("Connection"),
                 m_secure.localFingerprint(state->profileId)));
}

void MainWindow::sendFile(ChatWindow *window)
{
    if (!window || window->kind() != QStringLiteral("im")) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !state->connected || !state->backend) {
        QMessageBox::information(this, QStringLiteral("Send File"),
                                 QStringLiteral("Connect the AIM or IRC account before sending a file."));
        return;
    }
    if (state->backend->settings().protocol != ConnectionSettings::Protocol::Oscar
        && state->backend->settings().protocol != ConnectionSettings::Protocol::Irc) {
        QMessageBox::information(this, QStringLiteral("Send File"),
                                 QStringLiteral("WaffleHouse file transfer is available in AIM and IRC private messages."));
        return;
    }

    QDialog modeDialog(this);
    modeDialog.setWindowTitle(QStringLiteral("Send File — %1").arg(window->displayName()));
    modeDialog.setMinimumWidth(430);
    auto *outer = new QVBoxLayout(&modeDialog);
    auto *title = new QLabel(QStringLiteral("Choose transfer security"), &modeDialog);
    title->setObjectName(QStringLiteral("CardTitle"));
    outer->addWidget(title);
    auto *secure = new QRadioButton(QStringLiteral("Secure — CPX encrypted and authenticated"), &modeDialog);
    auto *unsecured = new QRadioButton(QStringLiteral("Unsecured — ordinary AIM/IRC private-message transport"), &modeDialog);
    secure->setChecked(true);
    outer->addWidget(secure);
    outer->addWidget(unsecured);
    auto *help = new QLabel(&modeDialog);
    help->setWordWrap(true);
    help->setObjectName(QStringLiteral("Muted"));
    outer->addWidget(help);
    auto updateHelp = [=] {
        help->setText(secure->isChecked()
            ? QStringLiteral("Secure transfer requires an established CPX secure DM with this peer. Open the PM, start the secure session, compare fingerprints, then send. WaffleHouse encrypts/authenticates the transfer and prefers the encrypted direct path when both peers support it.")
            : QStringLiteral("Unsecured transfer proceeds over ordinary AIM/IRC PM traffic without CPX encryption or authentication. File chunks remain resumable and the completed file is still verified with SHA-256."));
    };
    connect(secure, &QRadioButton::toggled, &modeDialog, updateHelp);
    updateHelp();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &modeDialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Continue"));
    connect(buttons, &QDialogButtonBox::accepted, &modeDialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &modeDialog, &QDialog::reject);
    outer->addWidget(buttons);
    if (modeDialog.exec() != QDialog::Accepted) return;

    const bool secureTransfer = secure->isChecked();
    if (secureTransfer) {
        if (!m_secureReady || !m_secure.hasSession(state->profileId, window->target())) {
            QMessageBox::information(
                this, QStringLiteral("Secure File Transfer — Setup Required"),
                QStringLiteral("To send securely:\n\n"
                               "1. Open the private message with %1.\n"
                               "2. Start a secure CPX session (Secure / Start Secure Session).\n"
                               "3. Compare the displayed fingerprints with the other user and trust the peer.\n"
                               "4. Choose Send File again and select Secure.\n\n"
                               "Nothing will be sent until the secure session is established.")
                    .arg(window->displayName()));
            return;
        }
        if (!m_secure.peerSupports(state->profileId, window->target(), QStringLiteral("file-transfer"))) {
            QMessageBox::information(this, QStringLiteral("Secure File Transfer"),
                                     QStringLiteral("This peer has not advertised CPX file-transfer support. The peer may be running an older client."));
            return;
        }
    }

    const QString path = QFileDialog::getOpenFileName(
        this, secureTransfer ? QStringLiteral("Send Secure File") : QStringLiteral("Send Unsecured File"));
    if (path.isEmpty()) return;

    QString transferId;
    QString offer;
    QString error;
    const bool reliableTransfer = secureTransfer
        ? m_secure.peerSupports(state->profileId, window->target(), QStringLiteral("file-ack"))
        : true; // 3.0r1 unsecured peers use the ACK/resume framing by default.
    const bool directPreferred = secureTransfer && reliableTransfer && m_secure.peerSupports(
        state->profileId, window->target(), QStringLiteral("file-direct-v1"));
    if (!m_fileTransfers.createOffer(window->target(), path, transferId, offer, &error,
                                     reliableTransfer)) {
        QMessageBox::warning(this, QStringLiteral("File Transfer"), error);
        return;
    }
    m_fileTransferProfiles.insert(transferId, state->profileId);
    m_fileTransferSecure.insert(transferId, secureTransfer);
    m_fileTransferProgressShown.insert(transferId, -10);

    const QString peer = window->displayName();
    refreshTransferWindow(transferId, QStringLiteral("Upload"), peer, QStringLiteral("Offering"));
    logTransfer(QStringLiteral("Offering %1 to %2 [%3] — %4")
                    .arg(QFileInfo(path).fileName(), peer, transferId,
                         secureTransfer
                             ? (directPreferred ? QStringLiteral("secure CPX; encrypted direct transport preferred")
                                                : QStringLiteral("secure CPX relay"))
                             : QStringLiteral("UNSECURED AIM/IRC relay; SHA-256 verification enabled")));

    if (!sendSecureControlPayload(state, window->target(), offer)) {
        logTransfer(QStringLiteral("Failed to send file offer for %1 [%2]")
                        .arg(QFileInfo(path).fileName(), transferId));
        m_fileTransfers.cancel(transferId, QStringLiteral("transport failed"));
        return;
    }
}

bool MainWindow::sendSecureControlPayload(BackendState *state,
                                          const QString &target,
                                          const QString &plaintext)
{
    if (!state || !state->backend || !state->connected) return false;

    const QString transferId = WaffleFileTransport::transferId(plaintext);
    const bool secureTransfer = transferId.isEmpty()
        ? true : m_fileTransferSecure.value(transferId, true);

    if (!secureTransfer) {
        const QString frame = WaffleFileTransport::wrapUnsecured(plaintext);
        if (state->backend->settings().protocol == ConnectionSettings::Protocol::Irc
            && frame.toUtf8().size() > 400) {
            logTransfer(QStringLiteral("ERROR: unsecured IRC transfer frame exceeded the safe message size."));
            return false;
        }
        m_outgoingUnsecuredFileFrames.insert(state->profileId + QChar(0x1f)
            + target.toCaseFolded() + QChar(0x1f) + frame);
        state->backend->sendPrivateMessage(target, frame);
        return true;
    }

    if (!m_secureReady || !m_secure.hasSession(state->profileId, target)) return false;
    QString error;
    const QString frame = m_secure.encrypt(state->profileId, target, plaintext, &error);
    if (frame.isEmpty()) {
        logTransfer(QStringLiteral("ERROR: %1").arg(error));
        return false;
    }
    if (state->backend->settings().protocol == ConnectionSettings::Protocol::Irc
        && frame.toUtf8().size() > 400) {
        const QString message = QStringLiteral("Internal IRC transfer frame exceeded the safe message size.");
        logTransfer(QStringLiteral("ERROR: %1").arg(message));
        return false;
    }
    m_outgoingSecureFrames.insert(state->profileId + QChar(0x1f)
        + target.toCaseFolded() + QChar(0x1f) + frame);
    state->backend->sendPrivateMessage(target, frame);
    return true;
}

void MainWindow::logTransfer(const QString &message, bool showWindow)
{
    if (!m_transferWindow || message.trimmed().isEmpty()) return;
    m_transferWindow->appendLog(message);
    if (showWindow) m_transferWindow->showAndRaise();
}

void MainWindow::refreshTransferWindow(const QString &transferId,
                                       const QString &direction,
                                       const QString &peer,
                                       const QString &status)
{
    if (!m_transferWindow || transferId.isEmpty()) return;
    const auto info = m_fileTransfers.transfer(transferId);
    if (info.id.isEmpty()) return;
    m_transferWindow->updateTransfer(
        transferId,
        direction,
        peer,
        info.fileName,
        info.transferred,
        info.total,
        status.isEmpty() ? info.status : status,
        info.resumable);
}

void MainWindow::appendTransferProgress(const CpxFileTransferManager::Event &event,
                                        const QString &direction,
                                        const QString &peer)
{
    if (event.id.isEmpty()) return;
    const auto info = m_fileTransfers.transfer(event.id);
    const qint64 transferred = event.transferred > 0 ? event.transferred : info.transferred;
    const qint64 total = event.total > 0 ? event.total : info.total;
    const int percent = total > 0 ? static_cast<int>((transferred * 100) / total) : event.percent;
    const int bucket = percent >= 100 ? 100 : (qMax(0, percent) / 5) * 5;
    const int previous = m_fileTransferProgressShown.value(event.id, -5);

    refreshTransferWindow(event.id, direction, peer,
                          info.status.isEmpty() ? QStringLiteral("Transferring") : info.status);
    if (bucket <= previous && bucket < 100) return;
    m_fileTransferProgressShown[event.id] = bucket;
    logTransfer(QStringLiteral("%1 %2: %3% (%4 / %5 bytes) [%6]")
                    .arg(direction, info.fileName)
                    .arg(qMax(0, percent))
                    .arg(transferred)
                    .arg(total)
                    .arg(event.id), false);
}

bool MainWindow::handleFileTransferPayload(BackendState *state,
                                           const QString &target,
                                           const QString &plaintext,
                                           ChatWindow *window,
                                           bool secureTransport)
{
    Q_UNUSED(window);
    if (!CpxFileTransferManager::looksLikeMessage(plaintext)) return false;
    if (!state) return true;

    const QString peer = targetDisplayName(state, QStringLiteral("im"), target);
    const auto event = m_fileTransfers.processIncoming(target, plaintext);
    if (!event.id.isEmpty()) {
        m_fileTransferProfiles.insert(event.id, state->profileId);
        m_fileTransferSecure.insert(event.id, secureTransport);
    }
    if (!event.replyPayload.isEmpty()) {
        sendSecureControlPayload(state, target, event.replyPayload);
    }

    using Kind = CpxFileTransferManager::EventKind;
    switch (event.kind) {
    case Kind::OfferReceived: {
        refreshTransferWindow(event.id, QStringLiteral("Download"), peer, QStringLiteral("Waiting for acceptance"));
        logTransfer(QStringLiteral("%1 wants to send %2 (%3 bytes) [%4]")
                        .arg(peer, event.fileName).arg(event.total).arg(event.id));
        const auto answer = QMessageBox::question(
            m_transferWindow ? static_cast<QWidget *>(m_transferWindow) : static_cast<QWidget *>(this),
            secureTransport ? QStringLiteral("Secure File Transfer") : QStringLiteral("Unsecured File Transfer"),
            (secureTransport
                ? QStringLiteral("%1 wants to send:\n\n%2\n%3 bytes\n\nAccept this encrypted CPX transfer?")
                : QStringLiteral("%1 wants to send:\n\n%2\n%3 bytes\n\nThis transfer is NOT encrypted or authenticated by CPX. SHA-256 integrity verification remains enabled. Accept?"))
                .arg(peer, event.fileName).arg(event.total),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes) {
            const QString reply = m_fileTransfers.declineIncoming(event.id, QStringLiteral("declined by user"));
            sendSecureControlPayload(state, target, reply);
            refreshTransferWindow(event.id, QStringLiteral("Download"), peer, QStringLiteral("Declined"));
            logTransfer(QStringLiteral("Declined incoming transfer %1 [%2]").arg(event.fileName, event.id));
            return true;
        }
        QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (downloadDir.isEmpty()) downloadDir = QDir::homePath();
        const QString suggested = QDir(downloadDir).filePath(event.fileName);
        const QString destination = QFileDialog::getSaveFileName(
            m_transferWindow ? static_cast<QWidget *>(m_transferWindow) : static_cast<QWidget *>(this),
            secureTransport ? QStringLiteral("Save Secure File") : QStringLiteral("Save Unsecured File"), suggested);
        if (destination.isEmpty()) {
            const QString reply = m_fileTransfers.declineIncoming(event.id, QStringLiteral("save cancelled"));
            sendSecureControlPayload(state, target, reply);
            refreshTransferWindow(event.id, QStringLiteral("Download"), peer, QStringLiteral("Cancelled"));
            logTransfer(QStringLiteral("Save cancelled for %1 [%2]").arg(event.fileName, event.id));
            return true;
        }
        QString error;
        QString reply = m_fileTransfers.acceptIncoming(event.id, destination, &error);
        if (reply.isEmpty()) {
            logTransfer(QStringLiteral("ERROR accepting %1: %2 [%3]").arg(event.fileName, error, event.id));
            const QString cancel = m_fileTransfers.declineIncoming(event.id, error);
            sendSecureControlPayload(state, target, cancel);
            refreshTransferWindow(event.id, QStringLiteral("Download"), peer, QStringLiteral("Error"));
            return true;
        }

        // Prefer the dedicated encrypted TCP data path whenever both peers
        // advertise it. AIM/IRC remains the authenticated control channel;
        // if the direct socket cannot be established we automatically resume
        // at the receiver's current byte offset through the reliable relay.
        const QString relayReply = reply;
        bool directReady = false;
        if (secureTransport
            && m_secure.peerSupports(state->profileId, target, QStringLiteral("file-direct-v1"))
            && m_secure.peerSupports(state->profileId, target, QStringLiteral("file-ack"))) {
            QString keyError;
            const QByteArray transferKey = m_secure.fileTransferKey(
                state->profileId, target, event.id, &keyError);
            const auto acceptedInfo = m_fileTransfers.transfer(event.id);
            CpxDirectTransferManager::ListenResult listener;
            if (!transferKey.isEmpty()
                && m_directTransfers.prepareIncoming(
                    event.id,
                    acceptedInfo.path + QStringLiteral(".cpxpart"),
                    acceptedInfo.total,
                    acceptedInfo.transferred,
                    transferKey,
                    listener,
                    &error)) {
                const QString directReply = m_fileTransfers.acceptIncoming(
                    event.id, destination, &error, listener.port, listener.hosts);
                directReady = !directReply.isEmpty();
                if (directReady) {
                    reply = directReply;
                } else {
                    reply = relayReply;
                    m_directTransfers.cancel(event.id);
                }
            } else if (!keyError.isEmpty()) {
                error = keyError;
            }
            if (!directReady && !error.isEmpty()) {
                logTransfer(QStringLiteral("Direct transfer unavailable for %1; using secure relay fallback: %2 [%3]")
                                .arg(event.fileName, error, event.id), false);
                error.clear();
            }
        }

        m_fileTransferProgressShown.insert(event.id, -5);
        if (!sendSecureControlPayload(state, target, reply) && directReady) {
            m_directTransfers.cancel(event.id);
            directReady = false;
            reply = m_fileTransfers.fallbackIncomingToRelay(event.id);
            if (!reply.isEmpty()) sendSecureControlPayload(state, target, reply);
            logTransfer(QStringLiteral("Direct endpoint advertisement could not be relayed; using secure relay fallback [%1]")
                            .arg(event.id), false);
        }
        const auto info = m_fileTransfers.transfer(event.id);
        refreshTransferWindow(event.id, QStringLiteral("Download"), peer,
                              directReady ? QStringLiteral("Waiting for direct connection")
                                          : (info.transferred > 0 ? QStringLiteral("Resuming relay")
                                                                : QStringLiteral("Receiving by relay")));
        if (directReady) {
            logTransfer(QStringLiteral("Accepted %1 → %2 [%3] — encrypted direct transport prepared")
                            .arg(event.fileName, destination, event.id));
        } else {
            const QString relayMode = secureTransport ? QStringLiteral("secure relay")
                                                        : QStringLiteral("UNSECURED relay");
            logTransfer(info.transferred > 0
                ? QStringLiteral("Resuming %1 at byte %2 → %3 [%4] by %5")
                      .arg(event.fileName).arg(info.transferred).arg(destination, event.id, relayMode)
                : QStringLiteral("Receiving %1 → %2 [%3] by %4")
                      .arg(event.fileName, destination, event.id, relayMode));
        }
        return true;
    }
    case Kind::Accepted:
        if (event.direct && m_fileTransferSecure.value(event.id, true)) {
            refreshTransferWindow(event.id, QStringLiteral("Upload"), peer,
                                  QStringLiteral("Connecting direct"));
            logTransfer(QStringLiteral("%1 accepted %2; establishing encrypted direct data connection [%3]")
                            .arg(peer, event.fileName, event.id));
            startDirectOutgoing(event, state);
        } else {
            refreshTransferWindow(event.id, QStringLiteral("Upload"), peer, QStringLiteral("Sending by relay"));
            logTransfer(QStringLiteral("%1 accepted %2; %3 upload started [%4]")
                            .arg(peer, event.fileName,
                                 m_fileTransferSecure.value(event.id, true) ? QStringLiteral("secure relay")
                                                                            : QStringLiteral("UNSECURED relay"),
                                 event.id));
            appendTransferProgress(event, QStringLiteral("Upload"), peer);
        }
        return true;
    case Kind::Fallback:
        m_directTransfers.cancel(event.id);
        refreshTransferWindow(event.id, QStringLiteral("Download"), peer,
                              QStringLiteral("Relay fallback"));
        logTransfer(QStringLiteral("Direct transport fallback requested%1; resuming through secure relay at byte %2 [%3]")
                        .arg(event.reason.isEmpty() ? QString() : QStringLiteral(": %1").arg(event.reason))
                        .arg(event.transferred).arg(event.id));
        return true;
    case Kind::Declined:
        refreshTransferWindow(event.id, QStringLiteral("Upload"), peer, QStringLiteral("Declined"));
        logTransfer(QStringLiteral("Transfer declined%1 [%2]")
                        .arg(event.reason.isEmpty() ? QString() : QStringLiteral(": %1").arg(event.reason), event.id));
        return true;
    case Kind::ResumeRequested:
        logTransfer(QStringLiteral("%1 requested resume of %2 [%3]")
                        .arg(peer, event.fileName, event.id));
        resumeIncomingFileTransfer(event.id, state, peer);
        return true;
    case Kind::Progress:
        appendTransferProgress(event, event.outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"), peer);
        return true;
    case Kind::Completed:
        appendTransferProgress(event, event.outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"), peer);
        refreshTransferWindow(event.id,
                              event.outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"),
                              peer, QStringLiteral("Complete"));
        if (event.outgoing) {
            logTransfer(QStringLiteral("Receiver confirmed SHA-256 verification; upload complete: %1 [%2]")
                            .arg(event.fileName, event.id));
        } else {
            logTransfer(QStringLiteral("Download complete and SHA-256 verified: %1 [%2]")
                            .arg(event.path, event.id));
        }
        m_fileTransferProgressShown.remove(event.id);
        QTimer::singleShot(1800, this, [this, id = event.id]() {
            clearFileTransfer(id);
        });
        return true;
    case Kind::Cancelled: {
        m_directTransfers.cancel(event.id);
        const auto info = m_fileTransfers.transfer(event.id);
        refreshTransferWindow(event.id,
                              info.outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"),
                              peer, QStringLiteral("Cancelled"));
        logTransfer(QStringLiteral("Transfer cancelled%1 [%2]")
                        .arg(event.reason.isEmpty() ? QString() : QStringLiteral(": %1").arg(event.reason), event.id));
        m_fileTransferProgressShown.remove(event.id);
        return true;
    }
    case Kind::Error: {
        const auto info = m_fileTransfers.transfer(event.id);
        if (!event.id.isEmpty()) {
            m_directTransfers.cancel(event.id);
            const QString cancel = m_fileTransfers.cancel(event.id, event.reason);
            sendSecureControlPayload(state, target, cancel);
        }
        refreshTransferWindow(event.id,
                              info.outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"),
                              peer, QStringLiteral("Error"));
        logTransfer(QStringLiteral("ERROR: %1%2")
                        .arg(event.reason,
                             event.id.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(event.id)));
        return true;
    }
    case Kind::None:
        return true;
    }
    return true;
}

void MainWindow::cancelFileTransfer(const QString &transferId)
{
    const auto info = m_fileTransfers.transfer(transferId);
    if (info.id.isEmpty() || info.complete) return;

    m_directTransfers.cancel(transferId);
    const QString payload = m_fileTransfers.cancel(
        transferId, QStringLiteral("cancelled by user"));
    BackendState *state = stateById(m_fileTransferProfiles.value(transferId));
    if (state && state->connected && state->backend && !info.target.isEmpty()) {
        sendSecureControlPayload(state, info.target, payload);
    }
    const QString peer = state
        ? targetDisplayName(state, QStringLiteral("im"), info.target)
        : info.target;
    refreshTransferWindow(transferId,
                          info.outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"),
                          peer, QStringLiteral("Cancelled"));
    logTransfer(QStringLiteral("Cancelled %1 transfer of %2 [%3]; chat connection remains open")
                    .arg(info.outgoing ? QStringLiteral("upload") : QStringLiteral("download"),
                         info.fileName, transferId));
    m_fileTransferProgressShown.remove(transferId);
}

bool MainWindow::resumeIncomingFileTransfer(const QString &transferId,
                                            BackendState *state,
                                            const QString &peer)
{
    const auto info = m_fileTransfers.transfer(transferId);
    if (info.id.isEmpty() || info.outgoing || !state || !state->connected || !state->backend) return false;
    if (!m_fileTransfers.canResume(transferId)) {
        QMessageBox::information(this, QStringLiteral("Resume File Transfer"),
                                 QStringLiteral("This transfer no longer has resumable partial data."));
        refreshTransferWindow(transferId, QStringLiteral("Download"), peer, QStringLiteral("Cancelled"));
        return false;
    }
    const bool secureTransfer = m_fileTransferSecure.value(transferId, true);
    if (secureTransfer && !m_secure.hasSession(state->profileId, info.target)) {
        QMessageBox::information(this, QStringLiteral("Resume File Transfer"),
                                 QStringLiteral("Re-establish the secure CPX session with this peer before resuming the transfer."));
        return false;
    }

    m_directTransfers.cancel(transferId);
    QString error;
    QString payload;
    bool directReady = false;
    if (secureTransfer
        && m_secure.peerSupports(state->profileId, info.target, QStringLiteral("file-direct-v1"))
        && m_secure.peerSupports(state->profileId, info.target, QStringLiteral("file-ack"))) {
        QString keyError;
        const QByteArray transferKey = m_secure.fileTransferKey(
            state->profileId, info.target, transferId, &keyError);
        const QString partPath = info.path + QStringLiteral(".cpxpart");
        qint64 resumeOffset = 0;
        const QFileInfo partInfo(partPath);
        if (partInfo.exists() && partInfo.isFile() && partInfo.size() <= info.total) {
            resumeOffset = partInfo.size();
        }
        CpxDirectTransferManager::ListenResult listener;
        if (!transferKey.isEmpty()
            && m_directTransfers.prepareIncoming(
                transferId, partPath, info.total, resumeOffset,
                transferKey, listener, &error)) {
            payload = m_fileTransfers.resumeIncoming(
                transferId, &error, listener.port, listener.hosts);
            directReady = !payload.isEmpty();
            if (!directReady) m_directTransfers.cancel(transferId);
        } else if (!keyError.isEmpty()) {
            error = keyError;
        }
    }
    if (!directReady) {
        error.clear();
        payload = m_fileTransfers.resumeIncoming(transferId, &error);
    }
    if (payload.isEmpty()) {
        logTransfer(QStringLiteral("ERROR resuming %1: %2 [%3]")
                        .arg(info.fileName, error, transferId));
        return false;
    }
    if (!sendSecureControlPayload(state, info.target, payload)) {
        if (directReady) m_directTransfers.cancel(transferId);
        logTransfer(QStringLiteral("ERROR: could not send resume acceptance for %1 [%2]")
                        .arg(info.fileName, transferId));
        return false;
    }

    const auto resumed = m_fileTransfers.transfer(transferId);
    m_fileTransferProgressShown.insert(transferId, -5);
    refreshTransferWindow(transferId, QStringLiteral("Download"), peer,
                          directReady ? QStringLiteral("Waiting for direct connection")
                                      : (resumed.transferred > 0 ? QStringLiteral("Resuming relay")
                                                                 : QStringLiteral("Receiving by relay")));
    logTransfer(QStringLiteral("Resuming download %1 at byte %2 [%3]%4")
                    .arg(resumed.fileName)
                    .arg(resumed.transferred)
                    .arg(transferId)
                    .arg(directReady ? QStringLiteral(" — encrypted direct transport prepared")
                                     : QStringLiteral(" — secure relay")));
    return true;
}

void MainWindow::resumeFileTransfer(const QString &transferId)
{
    const auto info = m_fileTransfers.transfer(transferId);
    if (info.id.isEmpty() || !m_fileTransfers.canResume(transferId)) {
        QMessageBox::information(this, QStringLiteral("Resume File Transfer"),
                                 QStringLiteral("This transfer is no longer resumable."));
        return;
    }
    BackendState *state = stateById(m_fileTransferProfiles.value(transferId));
    const bool secureTransfer = m_fileTransferSecure.value(transferId, true);
    if (!state || !state->connected || !state->backend
        || (secureTransfer && !m_secure.hasSession(state->profileId, info.target))) {
        QMessageBox::information(this, QStringLiteral("Resume File Transfer"),
                                 secureTransfer
                                     ? QStringLiteral("Re-establish the connection and secure CPX session with this peer before resuming.")
                                     : QStringLiteral("Re-establish the AIM/IRC connection with this peer before resuming."));
        return;
    }
    const QString peer = targetDisplayName(state, QStringLiteral("im"), info.target);
    if (!info.outgoing) {
        resumeIncomingFileTransfer(transferId, state, peer);
        return;
    }

    QString error;
    const QString payload = m_fileTransfers.requestResume(transferId, &error);
    if (payload.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Resume File Transfer"), error);
        return;
    }
    if (!sendSecureControlPayload(state, info.target, payload)) {
        m_fileTransfers.cancel(transferId, QStringLiteral("resume request could not be sent"));
        refreshTransferWindow(transferId, QStringLiteral("Upload"), peer, QStringLiteral("Cancelled"));
        return;
    }
    refreshTransferWindow(transferId, QStringLiteral("Upload"), peer, QStringLiteral("Resume requested"));
    logTransfer(QStringLiteral("Requested resume of upload %1 [%2]").arg(info.fileName, transferId));
}

void MainWindow::clearFileTransfer(const QString &transferId)
{
    const auto info = m_fileTransfers.transfer(transferId);
    if (info.id.isEmpty()) {
        if (m_transferWindow) m_transferWindow->removeTransfer(transferId);
        return;
    }
    if (!info.complete) return;

    m_directTransfers.cancel(transferId);
    QString error;
    if (!m_fileTransfers.clearTransfer(transferId, &error)) {
        if (!error.isEmpty()) logTransfer(QStringLiteral("ERROR clearing transfer: %1 [%2]").arg(error, transferId));
        return;
    }
    m_fileTransferProfiles.remove(transferId);
    m_fileTransferSecure.remove(transferId);
    m_fileTransferProgressShown.remove(transferId);
    if (m_transferWindow) m_transferWindow->removeTransfer(transferId);
}

void MainWindow::startDirectOutgoing(const CpxFileTransferManager::Event &event,
                                     BackendState *state)
{
    if (!state || event.id.isEmpty()) return;
    const auto info = m_fileTransfers.transfer(event.id);
    QString error;
    const QByteArray key = m_secure.fileTransferKey(
        state->profileId, info.target, event.id, &error);
    if (key.isEmpty()
        || !m_directTransfers.startOutgoing(event.id, info.path, info.total,
                                            info.transferred, event.directHosts,
                                            event.directPort, key, &error)) {
        handleDirectFailure(event.id,
                            error.isEmpty()
                                ? QStringLiteral("direct transfer could not be started")
                                : error,
                            true);
        return;
    }
    const QString peer = targetDisplayName(state, QStringLiteral("im"), info.target);
    refreshTransferWindow(event.id, QStringLiteral("Upload"), peer,
                          QStringLiteral("Direct encrypted transfer"));
    logTransfer(QStringLiteral("Direct encrypted upload connected/preparing for %1 [%2]")
                    .arg(info.fileName, event.id), false);
}

void MainWindow::handleDirectProgress(const QString &transferId,
                                      qint64 transferred,
                                      qint64 total,
                                      bool outgoing)
{
    m_fileTransfers.updateDirectProgress(transferId, transferred, outgoing);
    const auto info = m_fileTransfers.transfer(transferId);
    BackendState *state = stateById(m_fileTransferProfiles.value(transferId));
    const QString peer = state
        ? targetDisplayName(state, QStringLiteral("im"), info.target)
        : info.target;
    CpxFileTransferManager::Event event;
    event.id = transferId;
    event.fileName = info.fileName;
    event.transferred = transferred;
    event.total = total;
    event.outgoing = outgoing;
    appendTransferProgress(event,
                           outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"),
                           peer);
    refreshTransferWindow(transferId,
                          outgoing ? QStringLiteral("Upload") : QStringLiteral("Download"),
                          peer,
                          outgoing ? QStringLiteral("Sending direct")
                                   : QStringLiteral("Receiving direct"));
}

void MainWindow::handleDirectIncomingFinished(const QString &transferId)
{
    const auto before = m_fileTransfers.transfer(transferId);
    BackendState *state = stateById(m_fileTransferProfiles.value(transferId));
    if (before.id.isEmpty() || !state) return;

    QString error;
    if (!m_fileTransfers.finalizeIncomingDirect(transferId, &error)) {
        const QString cancel = m_fileTransfers.cancel(transferId, error);
        refreshTransferWindow(transferId, QStringLiteral("Download"),
                              targetDisplayName(state, QStringLiteral("im"), before.target),
                              QStringLiteral("Error"));
        logTransfer(QStringLiteral("ERROR verifying direct download %1: %2 [%3]")
                        .arg(before.fileName, error, transferId));
        sendSecureControlPayload(state, before.target, cancel);
        return;
    }

    sendSecureControlPayload(state, before.target,
                             m_fileTransfers.completionPayload(transferId));
    const auto info = m_fileTransfers.transfer(transferId);
    const QString peer = targetDisplayName(state, QStringLiteral("im"), info.target);
    refreshTransferWindow(transferId, QStringLiteral("Download"), peer,
                          QStringLiteral("Complete"));
    logTransfer(QStringLiteral("Direct download complete and SHA-256 verified: %1 [%2]")
                    .arg(info.path, transferId));
    m_fileTransferProgressShown.remove(transferId);
    QTimer::singleShot(1800, this, [this, transferId]() {
        clearFileTransfer(transferId);
    });
}

void MainWindow::handleDirectOutgoingFinished(const QString &transferId)
{
    m_fileTransfers.markOutgoingDirectSent(transferId);
    const auto info = m_fileTransfers.transfer(transferId);
    BackendState *state = stateById(m_fileTransferProfiles.value(transferId));
    const QString peer = state
        ? targetDisplayName(state, QStringLiteral("im"), info.target)
        : info.target;
    refreshTransferWindow(transferId, QStringLiteral("Upload"), peer,
                          QStringLiteral("Verifying"));
    logTransfer(QStringLiteral("Direct upload transmitted: %1; waiting for receiver SHA-256 confirmation [%2]")
                    .arg(info.fileName, transferId));
}

void MainWindow::handleDirectFailure(const QString &transferId,
                                     const QString &reason,
                                     bool outgoing)
{
    const auto info = m_fileTransfers.transfer(transferId);
    BackendState *state = stateById(m_fileTransferProfiles.value(transferId));
    if (info.id.isEmpty() || !state || !state->connected || !state->backend) return;

    m_directTransfers.cancel(transferId);
    const QString peer = targetDisplayName(state, QStringLiteral("im"), info.target);
    QString payload;
    if (outgoing) {
        payload = m_fileTransfers.requestOutgoingRelayFallback(transferId, reason);
        refreshTransferWindow(transferId, QStringLiteral("Upload"), peer,
                              QStringLiteral("Switching to relay"));
        logTransfer(QStringLiteral("Direct upload unavailable (%1); requesting secure relay resume [%2]")
                        .arg(reason, transferId));
    } else {
        payload = m_fileTransfers.fallbackIncomingToRelay(transferId);
        refreshTransferWindow(transferId, QStringLiteral("Download"), peer,
                              QStringLiteral("Switching to relay"));
        logTransfer(QStringLiteral("Direct download interrupted (%1); resuming through secure relay at byte %2 [%3]")
                        .arg(reason).arg(info.transferred).arg(transferId));
    }
    if (!payload.isEmpty()) sendSecureControlPayload(state, info.target, payload);
}

void MainWindow::pumpFileTransfers()
{
    const QStringList ids = m_fileTransfers.activeOutgoingIds();
    for (const QString &id : ids) {
        const QString profileId = m_fileTransferProfiles.value(id);
        BackendState *state = nullptr;
        for (BackendState *candidate : m_states) {
            if (candidate && candidate->profileId == profileId) { state = candidate; break; }
        }
        const auto before = m_fileTransfers.transfer(id);
        const bool secureTransfer = m_fileTransferSecure.value(id, true);
        if (!state || !state->connected || !state->backend
            || (secureTransfer && !m_secure.hasSession(profileId, before.target))) continue;

        const QString peer = targetDisplayName(state, QStringLiteral("im"), before.target);
        const bool irc = state->backend->settings().protocol == ConnectionSettings::Protocol::Irc;
        const int rawChunk = irc ? (secureTransfer ? 120 : 96) : 768;
        const int minimumSendIntervalMs = irc ? 1000 : 500;
        bool finished = false;
        QString error;
        const QString payload = m_fileTransfers.nextOutgoingPayload(
            id, rawChunk, &finished, &error, minimumSendIntervalMs);
        if (payload.isEmpty()) {
            if (!error.isEmpty()) {
                refreshTransferWindow(id, QStringLiteral("Upload"), peer, QStringLiteral("Error"));
                logTransfer(QStringLiteral("ERROR: %1 [%2]").arg(error, id));
            }
            continue;
        }
        if (!sendSecureControlPayload(state, before.target, payload)) continue;

        const auto after = m_fileTransfers.transfer(id);
        CpxFileTransferManager::Event progress;
        progress.id = id; progress.fileName = after.fileName;
        progress.transferred = after.transferred; progress.total = after.total;
        progress.percent = after.total > 0 ? static_cast<int>((after.transferred * 100) / after.total) : 100;
        appendTransferProgress(progress, QStringLiteral("Upload"), peer);
        if (finished) {
            refreshTransferWindow(id, QStringLiteral("Upload"), peer, QStringLiteral("Verifying"));
            logTransfer(QStringLiteral("Finished transmitting %1; waiting for receiver SHA-256 confirmation [%2]")
                            .arg(after.fileName, id));
        }
    }
}

bool MainWindow::sendPrivateText(BackendState *state,
                                 const QString &target,
                                 const QString &text,
                                 ChatWindow *window)
{
    if (!state || !state->backend || !state->connected) return false;

    if (m_options.encryptedDmEnabled && m_secureReady
        && m_secure.hasSession(state->profileId, target)) {
        QString error;
        const QString frame = m_secure.encrypt(state->profileId, target, text, &error);
        if (frame.isEmpty()) {
            if (window) window->appendMessage(QStringLiteral("[error] [secure] %1").arg(error));
            return false;
        }
        if (state->backend->settings().protocol == ConnectionSettings::Protocol::Irc
            && frame.toUtf8().size() > 400) {
            if (window) window->appendMessage(QStringLiteral(
                "[error] [secure] Encrypted IRC message is too long; split it into shorter messages."));
            return false;
        }

        m_outgoingSecureFrames.insert(state->profileId + QChar(0x1f)
            + target.toCaseFolded() + QChar(0x1f) + frame);
        state->backend->sendPrivateMessage(target, frame);
        if (window) {
            const QString me = state->identity.isEmpty()
                ? (state->backend->settings().username.isEmpty()
                       ? QStringLiteral("me")
                       : state->backend->settings().username)
                : state->identity;
            window->appendMessage(QStringLiteral("<%1> [secure] %2").arg(me, text));
        }
        return true;
    }

    state->backend->sendPrivateMessage(target, text);
    return true;
}

void MainWindow::handleConversationMessage(ChatWindow *window, const QString &text)
{
    if (!window) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !state->backend || !state->connected) return;

    const QString trimmedCommand = text.trimmed();
    const QString command = trimmedCommand.toCaseFolded();
    if (window->kind() != QStringLiteral("terminal")) {
        if (command == QStringLiteral("/version") || command.startsWith(QStringLiteral("/version "))) {
            QString target = trimmedCommand.mid(QStringLiteral("/version").size()).trimmed();
            if (target.isEmpty() && window->kind() == QStringLiteral("im")) target = window->target();
            if (target.isEmpty()) {
                window->appendMessage(QStringLiteral("[version] Usage: /version USER (or run /version in a PM)."));
                return;
            }
            requestClientVersion(state, target);
            return;
        }
        if (command == QStringLiteral("/options")) { showOptionsDialog(); return; }
        if (command == QStringLiteral("/help")) { showHelpDialog(); return; }
        if (command == QStringLiteral("/fingerprint")) { selectState(state); showSelectedFingerprint(); return; }
        if (window->kind() == QStringLiteral("im") || window->kind() == QStringLiteral("chat")) {
            if (command == QStringLiteral("/secure")) { startSecureSession(window); return; }
            if (command == QStringLiteral("/securestatus")) { showSecureStatus(window); return; }
            if (command == QStringLiteral("/secureoff")) { closeSecureSession(window); return; }
            if (window->kind() == QStringLiteral("im")) {
                if (command == QStringLiteral("/trust")) { trustSecurePeer(window); return; }
                if (command == QStringLiteral("/untrust")) { untrustSecurePeer(window); return; }
            }
        }
    }

    // IRC conversation input uses the same slash-command parser as the CLI.
    // WaffleHouse-local commands above keep priority. Recognized IRC commands
    // bypass CPX room encryption; unknown /text deliberately falls through and
    // is sent as ordinary (and, when enabled, secure-room-encrypted) chat text.
    if (state->backend->settings().protocol == ConnectionSettings::Protocol::Irc
        && window->kind() != QStringLiteral("terminal")) {
        if (auto *irc = qobject_cast<IrcBackend *>(state->backend)) {
            const QString roomContext = window->kind() == QStringLiteral("chat")
                ? window->target() : QString();
            if (irc->handleSlashCommand(roomContext, text)) return;
        }
    }

    if (window->kind() == QStringLiteral("chat")) {
        if (m_secureRooms.hasRoom(state->profileId, window->target())) {
            QString error;
            const QString frame = m_secureRooms.encrypt(state->profileId, window->target(), text, &error);
            if (frame.isEmpty()) {
                window->appendMessage(QStringLiteral("[error] [secure-room] %1").arg(error));
                return;
            }
            if (state->backend->settings().protocol == ConnectionSettings::Protocol::Irc
                && frame.toUtf8().size() > 400) {
                window->appendMessage(QStringLiteral(
                    "[error] [secure-room] Encrypted IRC room message is too long; split it into shorter messages."));
                return;
            }
            state->backend->sendRoomMessage(window->target(), frame);
        } else {
            state->backend->sendRoomMessage(window->target(), text);
        }
    } else if (window->kind() == QStringLiteral("terminal")) {
        state->backend->sendPrivateMessage(window->target(), text);
    } else {
        sendPrivateText(state, window->target(), text, window);
    }
}

void MainWindow::updateConversationSecurity(ChatWindow *window)
{
    if (!window || !m_secureReady) {
        if (window) window->setSecurityState(false, false);
        return;
    }
    BackendState *state = stateById(window->backendId());
    if (!state) {
        window->setSecurityState(false, false);
        return;
    }

    if (window->kind() == QStringLiteral("chat")) {
        const bool active = m_secureRooms.hasRoom(state->profileId, window->target());
        window->setSecurityState(active, true,
                                 active ? m_secureRooms.keyId(state->profileId, window->target()) : QString());
        return;
    }

    if (window->kind() != QStringLiteral("im")
        || !m_secure.hasSession(state->profileId, window->target())) {
        window->setSecurityState(false, false);
        return;
    }
    const QString peer = m_secure.peerFingerprint(state->profileId, window->target());
    const QString trusted = trustedFingerprint(state, window->target());
    window->setSecurityState(true, !peer.isEmpty() && trusted == peer,
                             peer, m_secure.localFingerprint(state->profileId));
}

void MainWindow::handleConnected(ChatBackend *backend,
                                 const QString &identity,
                                 const QString &endpoint)
{
    BackendState *state = stateFor(backend);
    if (!state) {
        return;
    }

    state->connecting = false;
    state->connected = true;
    state->identity = identity;
    state->endpoint = endpoint;
    updateConnectionItem(state);
    appendActivity(backend,
                   QStringLiteral("Connected as %1 — %2").arg(identity, endpoint));
    refreshBuddyList();
    selectState(state);
    updateActions();
    statusBar()->showMessage(
        QStringLiteral("%1 connected").arg(backend->protocolName()), 4000);

    if (backend->settings().protocol == ConnectionSettings::Protocol::Telnet) {
        ChatWindow *terminal = ensureConversationWindow(
            backend, QStringLiteral("terminal"), backend->settings().server, true);
        if (terminal) terminal->setBackendOnline(true);
    }

    // Successful login always brings the Buddy List to the front.
    showBuddyWindow();
}

void MainWindow::handleDisconnected(ChatBackend *backend, const QString &reason)
{
    BackendState *state = stateFor(backend);
    if (!state) {
        return;
    }

    const bool failedWhileConnecting = state->connecting;
    state->connecting = false;
    state->connected = false;
    state->onlineBuddies.clear();
    state->presenceState = QStringLiteral("ONLINE");
    state->presenceMessage.clear();
    state->idleSeconds = 0;
    state->autoPresenceState.clear();

    if (failedWhileConnecting && state->secretRequired) {
        ConnectionSettings cleared = state->backend->settings();
        if (!cleared.savePassword) {
            cleared.password.clear();
            if (auto *sip = qobject_cast<SipBackend *>(state->backend)) {
                // Clearing a failed session secret must not reconfigure PJSUA2
                // from inside a disconnect/error callback.
                sip->clearSessionPassword();
            } else {
                state->backend->setConnectionSettings(cleared);
            }
        }
        state->hasSessionSecret = cleared.savePassword && !cleared.password.isEmpty();
    }

    if (!state->profileId.isEmpty()) {
        m_secure.closeConnection(state->profileId);
        m_secureRooms.closeConnection(state->profileId);
    }
    const QString roomPrefix = backend->id() + QStringLiteral("|chat|");
    for (auto it = m_closedRoomKeys.begin(); it != m_closedRoomKeys.end();) {
        if (it->startsWith(roomPrefix)) it = m_closedRoomKeys.erase(it);
        else ++it;
    }
    closeBackendWindows(backend);
    updateConnectionItem(state);
    appendActivity(backend, QStringLiteral("Disconnected: %1").arg(reason));
    refreshBuddyList();
    updateActions();
}

void MainWindow::handleEvent(ChatBackend *backend,
                             const QString &kind,
                             const QString &target,
                             const QString &text)
{
    BackendState *state = stateFor(backend);
    if (!state) return;

    if (kind == QStringLiteral("version-request")) {
        if (auto *irc = qobject_cast<IrcBackend *>(backend); irc && !target.isEmpty()) {
            const QString ctcp = QString(QChar(0x01))
                + QStringLiteral("VERSION WaffleHouse-Client %1").arg(appVersionString())
                + QChar(0x01);
            irc->sendRaw(QStringLiteral("NOTICE %1 :%2").arg(target, ctcp));
        }
        return;
    }
    if (kind == QStringLiteral("version")) {
        m_pendingVersionQueries.remove(state->profileId + QChar(0x1f) + target.toCaseFolded());
        QString report = text.trimmed();
        if (backend->settings().protocol == ConnectionSettings::Protocol::Irc
            && !report.contains(QStringLiteral("WaffleHouse"), Qt::CaseInsensitive)) {
            report = QStringLiteral("IRC client reports: %1 (not identified as WaffleHouse-Client)").arg(report);
        }
        const QString line = QStringLiteral("[version] %1: %2").arg(target, report);
        ChatWindow *versionWindow = m_windows.value(conversationKey(backend, QStringLiteral("im"), target));
        if (versionWindow) versionWindow->appendMessage(line);
        else appendActivity(backend, line);
        statusBar()->showMessage(line, 7000);
        return;
    }

    if (kind == QStringLiteral("status") || target.isEmpty()) {
        appendActivity(backend, text);
        return;
    }
    if (!state->connected) return;
    if (kind == QStringLiteral("chat")
        && m_closedRoomKeys.contains(conversationKey(backend, kind, target))) {
        return;
    }

    if (kind == QStringLiteral("im")) {
        const QString payload = imPayload(text);
        const QString outgoingToken = state->profileId + QChar(0x1f)
            + target.toCaseFolded() + QChar(0x1f) + payload;
        if (m_outgoingUnsecuredFileFrames.remove(outgoingToken)) return;
        QString filePayload;
        if (WaffleFileTransport::unwrapUnsecured(payload, filePayload)) {
            ChatWindow *window = ensureConversationWindow(backend, kind, target, true);
            if (window) handleFileTransferPayload(state, target, filePayload, window, false);
            return;
        }
    }

    if (kind == QStringLiteral("im") && m_secureReady) {
        const QString payload = imPayload(text);
        const QString outgoingToken = state->profileId + QChar(0x1f)
            + target.toCaseFolded() + QChar(0x1f) + payload;
        if (m_outgoingSecureFrames.remove(outgoingToken)) {
            return;
        }

        if (SecureChannelManager::looksLikeFrame(payload)) {
            ChatWindow *window = ensureConversationWindow(backend, kind, target, true);
            if (!window) return;

            if (!m_options.encryptedDmEnabled) {
                window->appendMessage(QStringLiteral(
                    "[secure] Encrypted DM frame ignored because encrypted DMs are disabled."));
                return;
            }

            const auto result = m_secure.processIncoming(
                state->profileId, target, payload, m_options.autoReplySecure);

            if (!result.replyFrame.isEmpty()) {
                m_outgoingSecureFrames.insert(state->profileId + QChar(0x1f)
                    + target.toCaseFolded() + QChar(0x1f) + result.replyFrame);
                backend->sendPrivateMessage(target, result.replyFrame);
            }

            const QString capsFrame = m_secure.capabilitiesFrame(state->profileId, target);
            if (!capsFrame.isEmpty()) {
                m_outgoingSecureFrames.insert(state->profileId + QChar(0x1f)
                    + target.toCaseFolded() + QChar(0x1f) + capsFrame);
                backend->sendPrivateMessage(target, capsFrame);
            }

            if (result.kind == SecureChannelManager::IncomingKind::Decrypted) {
                if (handleSecureRoomKeyOffer(state, target, result.plaintext)) {
                    updateConversationSecurity(window);
                    return;
                }
                if (handleFileTransferPayload(state, target, result.plaintext, window, true)) {
                    updateConversationSecurity(window);
                    return;
                }
                QString prefix = imSpeakerPrefix(text);
                if (prefix.isEmpty()) {
                    prefix = QStringLiteral("<%1> ").arg(targetDisplayName(state, kind, target));
                }
                window->appendMessage(prefix + QStringLiteral("[secure] ") + result.plaintext);
                if (const auto event = NotificationManager::classifyIncoming(
                        state->backend->settings(), state->identity, kind, text)) {
                    if (!NotificationManager::play(*event, false)) QApplication::beep();
                    if (m_trayIcon && m_trayIcon->isVisible()) {
                        m_trayIcon->showMessage(NotificationManager::displayName(*event),
                                                targetDisplayName(state, kind, target),
                                                QSystemTrayIcon::Information, 3000);
                    }
                }
                updateConversationSecurity(window);
                return;
            }

            if (result.kind == SecureChannelManager::IncomingKind::Error) {
                window->appendMessage(QStringLiteral("[error] [secure] %1").arg(result.notice));
                updateConversationSecurity(window);
                return;
            }

            if (result.kind == SecureChannelManager::IncomingKind::Control) {
                QString notice = result.notice;
                if (!result.peerFingerprint.isEmpty()) {
                    const QString trusted = trustedFingerprint(state, target);
                    if (!trusted.isEmpty() && trusted != result.peerFingerprint) {
                        window->appendMessage(
                            QStringLiteral("[error] [secure] TRUST WARNING: fingerprint changed. Trusted %1; received %2")
                                .arg(trusted, result.peerFingerprint));
                        m_secure.closeSession(state->profileId, target);
                        updateConversationSecurity(window);
                        return;
                    }
                    if (trusted == result.peerFingerprint) {
                        notice += QStringLiteral(" [trusted]");
                    } else {
                        notice += QStringLiteral(" [UNVERIFIED — compare fingerprints before trusting]");
                    }
                }
                if (!notice.isEmpty()) {
                    if (m_options.showSecureFingerprints || result.peerFingerprint.isEmpty()) {
                        window->appendMessage(QStringLiteral("[secure] %1").arg(notice));
                    } else {
                        window->appendMessage(QStringLiteral("[secure] Secure session established."));
                    }
                }
                updateConversationSecurity(window);
                flushPendingSecureRoomKeys(state, target);
                return;
            }
        }
    }

    if (kind == QStringLiteral("chat") && m_secureReady) {
        const QString payload = imPayload(text);
        if (SecureRoomManager::looksLikeFrame(payload)) {
            ChatWindow *window = ensureConversationWindow(backend, kind, target, true);
            if (!window) return;
            const auto result = m_secureRooms.processIncoming(state->profileId, target, payload);
            if (result.kind == SecureRoomManager::IncomingKind::Decrypted) {
                QString prefix = imSpeakerPrefix(text);
                if (prefix.isEmpty()) prefix = QStringLiteral("<room> ");
                window->appendMessage(prefix + QStringLiteral("[secure-room] ") + result.plaintext);
                updateConversationSecurity(window);
                if (const auto event = NotificationManager::classifyIncoming(
                        state->backend->settings(), state->identity, kind, text)) {
                    if (!NotificationManager::play(*event, false)) QApplication::beep();
                }
                return;
            }
            if (result.kind == SecureRoomManager::IncomingKind::Error) {
                window->appendMessage(QStringLiteral("[error] [secure-room] %1").arg(result.notice));
                return;
            }
        }

        if (m_secureRooms.hasRoom(state->profileId, target)) {
            const QString prefix = imSpeakerPrefix(text);
            if (!prefix.isEmpty()) {
                ChatWindow *window = ensureConversationWindow(backend, kind, target, true);
                if (window) {
                    window->appendMessage(prefix + QStringLiteral("[plaintext] ") + payload);
                    return;
                }
            }
        }
    }

    ChatWindow *window = ensureConversationWindow(backend, kind, target, true);
    if (window) window->appendMessage(text);
    if (const auto event = NotificationManager::classifyIncoming(
            state->backend->settings(), state->identity, kind, text)) {
        if (!NotificationManager::play(*event, false)) QApplication::beep();
        if (m_trayIcon && m_trayIcon->isVisible()) {
            m_trayIcon->showMessage(NotificationManager::displayName(*event),
                                    targetDisplayName(state, kind, target),
                                    QSystemTrayIcon::Information, 3000);
        }
    }
}

void MainWindow::handleMembers(ChatBackend *backend,
                               const QString &room,
                               const QString &action,
                               const QStringList &names)
{
    BackendState *state = stateFor(backend);
    if (!state || !state->connected) {
        return;
    }
    if (m_closedRoomKeys.contains(conversationKey(backend, QStringLiteral("chat"), room))) {
        return;
    }

    ChatWindow *window = ensureConversationWindow(
        backend, QStringLiteral("chat"), room, false);
    if (window) {
        window->updateMembers(action, names);
        if ((action == QStringLiteral("add") || action == QStringLiteral("remove"))
            && m_secureRooms.hasRoom(state->profileId, room)
            && m_secureRooms.locallyOwned(state->profileId, room)) {
            QString error;
            if (m_secureRooms.createOrRotate(state->profileId, room, &error)) {
                window->appendMessage(QStringLiteral(
                    "[secure-room] Membership changed; rotated shared key to %1 and redistributing it to current members.")
                    .arg(m_secureRooms.keyId(state->profileId, room)));
                distributeSecureRoomKeyToMembers(state, window);
                updateConversationSecurity(window);
            } else {
                window->appendMessage(QStringLiteral("[error] [secure-room] Key rotation failed: %1").arg(error));
            }
        }
    }
}

void MainWindow::handleTargetNamed(ChatBackend *backend,
                                   const QString &kind,
                                   const QString &target,
                                   const QString &displayName)
{
    BackendState *state = stateFor(backend);
    if (!state) {
        return;
    }
    state->targetNames.insert(QStringLiteral("%1|%2").arg(kind, target), displayName);
    const QString key = conversationKey(backend, kind, target);
    if (ChatWindow *window = m_windows.value(key, nullptr)) {
        window->setDisplayName(displayName);
    }
}

void MainWindow::handleRoomDiscovered(ChatBackend *backend,
                                      const QString &roomId,
                                      const QString &displayName)
{
    BackendState *state = stateFor(backend);
    if (!state) {
        return;
    }
    state->discoveredRooms.insert(roomId, displayName);
    state->targetNames.insert(QStringLiteral("chat|%1").arg(roomId), displayName);
}

void MainWindow::handleBuddyList(ChatBackend *backend, const QStringList &names)
{
    BackendState *state = stateFor(backend);
    if (!state || !state->backend) {
        return;
    }
    const bool localSipContacts =
        state->backend->settings().protocol == ConnectionSettings::Protocol::Sip;
    if (!state->connected && !localSipContacts) {
        return;
    }
    state->buddies.clear();
    for (const QString &name : names) {
        if (!name.trimmed().isEmpty()) {
            state->buddies.insert(name.trimmed());
        }
    }
    if (state->backend
        && (state->backend->settings().protocol == ConnectionSettings::Protocol::Irc
            || state->backend->settings().protocol == ConnectionSettings::Protocol::Sip)) {
        saveConnections();
    }
    refreshBuddyList();
}

void MainWindow::handleBuddyPresence(ChatBackend *backend,
                                     const QString &name,
                                     bool online)
{
    BackendState *state = stateFor(backend);
    if (!state || !state->connected || name.trimmed().isEmpty()) {
        return;
    }

    const QString key = name.toCaseFolded();
    if (online) {
        state->onlineBuddies.insert(key);
        state->buddies.insert(name);
    } else {
        state->onlineBuddies.remove(key);
    }
    refreshBuddyList();
}

void MainWindow::handleBackendError(ChatBackend *backend,
                                    const QString &context,
                                    const QString &message)
{
    appendActivity(backend, QStringLiteral("[error] %1: %2").arg(context, message));
    statusBar()->showMessage(QStringLiteral("%1: %2").arg(context, message), 8000);
}

void MainWindow::handleConversationClosing(ChatWindow *window)
{
    if (!window) return;
    BackendState *state = stateById(window->backendId());
    if (!state || !state->backend) return;

    if (window->kind() == QStringLiteral("im") && !state->profileId.isEmpty()) {
        m_secure.closeSession(state->profileId, window->target());
    }

    if (state->connected && window->kind() == QStringLiteral("chat")) {
        m_secureRooms.closeRoom(state->profileId, window->target());
        m_closedRoomKeys.insert(conversationKey(state->backend, QStringLiteral("chat"), window->target()));
        state->backend->leaveRoom(window->target());
    } else if (state->connected && window->kind() == QStringLiteral("terminal")) {
        state->connected = false;
        state->connecting = false;
        updateConnectionItem(state);
        refreshBuddyList();
        updateActions();
        state->backend->stop();
    }
}

