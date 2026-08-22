#include "sipbackend.h"
#include "sipcontroller.h"

#include <exception>

namespace {
std::string s(const QString &v) { return v.toStdString(); }
QString q(const std::string &v) { return QString::fromStdString(v); }
}

trunkmonkey::SipProfile sipProfileFromConnectionSettings(const ConnectionSettings &v)
{
    trunkmonkey::SipProfile p;
    p.name = s(v.sipProfileName.trimmed().isEmpty()
                   ? (v.username.trimmed().isEmpty() ? QStringLiteral("WaffleHouse SIP") : v.username.trimmed())
                   : v.sipProfileName.trimmed());
    p.sipDomain = s(v.sipDomain.trimmed().isEmpty() ? v.server.trimmed() : v.sipDomain.trimmed());
    const auto ensureSipUri = [](QString value) {
        value = value.trimmed();
        if (value.isEmpty() || value.startsWith(QStringLiteral("sip:"), Qt::CaseInsensitive)
            || value.startsWith(QStringLiteral("sips:"), Qt::CaseInsensitive))
            return value;
        return QStringLiteral("sip:") + value;
    };
    p.registrar = s(ensureSipUri(v.sipRegistrar));
    p.username = s(v.username.trimmed());
    p.authUsername = s(v.sipAuthUsername.trimmed());
    p.password = s(v.password);
    p.displayName = s(v.sipDisplayName.trimmed().isEmpty() ? v.username.trimmed() : v.sipDisplayName.trimmed());
    p.outboundProxy = s(ensureSipUri(v.sipOutboundProxy));
    p.callerIdDomain = s(v.sipCallerIdDomain.trimmed());
    p.dialPrefix = s(v.sipDialPrefix.trimmed());
    p.stunServer = s(v.sipStunServer.trimmed());
    try { p.transport = trunkmonkey::transportFromString(s(v.sipTransport)); }
    catch (...) { p.transport = trunkmonkey::Transport::Udp; }
    try { p.identityMode = trunkmonkey::identityModeFromString(s(v.sipIdentityMode)); }
    catch (...) { p.identityMode = trunkmonkey::IdentityMode::From; }
    p.localSipPort = v.sipLocalPort ? v.sipLocalPort : 5060;
    p.registrationExpires = v.sipRegistrationExpires ? v.sipRegistrationExpires : 300;
    p.useIce = v.sipUseIce;
    p.enableSrtp = v.sipEnableSrtp;
    return p;
}

void applySipProfileToConnectionSettings(const trunkmonkey::SipProfile &p,
                                         ConnectionSettings &v)
{
    v.protocol = ConnectionSettings::Protocol::Sip;
    v.sipProfileName = q(p.name);
    v.sipDomain = q(p.sipDomain);
    v.server = q(p.sipDomain);
    v.sipRegistrar = q(p.registrar);
    v.username = q(p.username);
    v.sipAuthUsername = q(p.authUsername);
    v.password = q(p.password);
    v.sipDisplayName = q(p.displayName);
    v.sipOutboundProxy = q(p.outboundProxy);
    v.sipCallerIdDomain = q(p.callerIdDomain);
    v.sipDialPrefix = q(p.dialPrefix);
    v.sipStunServer = q(p.stunServer);
    v.sipTransport = q(trunkmonkey::toString(p.transport));
    v.sipIdentityMode = q(trunkmonkey::toString(p.identityMode));
    v.sipLocalPort = p.localSipPort;
    v.port = p.localSipPort;
    v.sipRegistrationExpires = p.registrationExpires;
    v.sipUseIce = p.useIce;
    v.sipEnableSrtp = p.enableSrtp;
}

SipBackend::SipBackend(ConnectionSettings settings, SipController *controller, QObject *parent)
    : ChatBackend(std::move(settings), parent), m_controller(controller)
{
    // Do not register the PJSUA2 account from inside the QObject constructor.
    // In 2.5 this could emit SipController signals before MainWindow had attached
    // the backend to its state model, which made GUI account creation re-enter UI
    // refresh code with a half-constructed SIP connection. MainWindow calls
    // initializeAccount() only after the backend is fully attached and wired.
    if (m_controller) {
        connect(m_controller, &SipController::accountStateChanged, this,
                [this](const QString &accountId) { if (accountId == id()) syncState(); });
    }
}

bool SipBackend::initializeAccount(QString *error)
{
    if (m_accountInitialized) return true;
    if (!m_controller) {
        if (error) *error = QStringLiteral("Softphone controller unavailable.");
        return false;
    }
    if (!m_controller->addAccount(id(), sipProfileFromConnectionSettings(m_settings), error))
        return false;
    m_accountInitialized = true;
    return true;
}

SipBackend::~SipBackend()
{
    if (m_controller && m_accountInitialized) m_controller->removeAccount(id());
}

void SipBackend::start()
{
    if (!m_controller) { emit backendError(QStringLiteral("SIP"), QStringLiteral("Softphone controller unavailable.")); return; }
    QString error;
    if (!initializeAccount(&error)) {
        emit backendError(QStringLiteral("SIP account"), error);
        emit disconnected(error);
        return;
    }
    m_connectRequested = true;
    if (!m_controller->connectAccount(id(), &error)) {
        m_connectRequested = false;
        emit backendError(QStringLiteral("SIP registration"), error);
        emit disconnected(error);
        return;
    }
    syncState();
}

void SipBackend::stop()
{
    m_connectRequested = false;
    if (m_controller) {
        QString error;
        if (!m_controller->disconnectAccount(id(), &error) && !error.isEmpty())
            emit backendError(QStringLiteral("SIP disconnect"), error);
    }
    if (m_reportedConnected) {
        m_reportedConnected = false;
        emit disconnected(QStringLiteral("SIP account disconnected"));
    } else {
        emit disconnected(QStringLiteral("SIP account offline"));
    }
}

void SipBackend::setConnectionSettings(const ConnectionSettings &settings)
{
    const ConnectionSettings previous = m_settings;
    ChatBackend::setConnectionSettings(settings);
    if (!m_controller) return;

    // Do not implicitly create a PJSUA2 account while merely updating local
    // connection state. In particular, CLI/GUI authentication-error handlers
    // clear an unsaved session password through this method. 2.5.1 used to
    // call initializeAccount() here when account creation had already failed,
    // which emitted another backendError and recursively retried until the CLI
    // exhausted its stack and crashed with SIGSEGV. Account creation is now
    // performed only by initializeAccount()/start() or the GUI attach path.
    if (!m_accountInitialized) {
        emit buddyListChanged(m_settings.sipContacts);
        return;
    }

    QString error;
    if (!m_controller->updateAccount(id(), sipProfileFromConnectionSettings(m_settings), &error)) {
        m_settings = previous;
        emit backendError(QStringLiteral("SIP profile"), error);
        return;
    }
    emit buddyListChanged(m_settings.sipContacts);
}

void SipBackend::clearSessionPassword()
{
    // Error recovery must never re-enter PJSUA2 account creation/reconfiguration.
    // This only removes the in-memory WaffleHouse session secret.
    m_settings.password.clear();
}

void SipBackend::syncState()
{
    if (!m_controller) return;
    const bool online = m_controller->accountRegistered(id());
    if (online && !m_reportedConnected) {
        m_reportedConnected = true;
        const QString identity = m_settings.username + QStringLiteral("@") +
            (m_settings.sipDomain.isEmpty() ? m_settings.server : m_settings.sipDomain);
        emit connected(identity, m_controller->registrationText(id()));
    } else if (!online && m_reportedConnected) {
        m_reportedConnected = false;
        emit disconnected(m_controller->registrationText(id()));
    } else if (!online && m_connectRequested) {
        const QString state = m_controller->registrationText(id());
        if (state.startsWith(QStringLiteral("Not registered"), Qt::CaseInsensitive))
            emit backendError(QStringLiteral("SIP registration"), state);
    }
}

void SipBackend::sendPrivateMessage(const QString &, const QString &) { emit backendError(QStringLiteral("SIP"), QStringLiteral("SIP accounts use voice calling, not instant messages.")); }
void SipBackend::joinRoom(const QString &, bool) { emit backendError(QStringLiteral("SIP"), QStringLiteral("SIP accounts do not join chat rooms.")); }
void SipBackend::sendRoomMessage(const QString &, const QString &) { emit backendError(QStringLiteral("SIP"), QStringLiteral("SIP accounts do not send room messages.")); }
void SipBackend::leaveRoom(const QString &) {}

void SipBackend::addBuddy(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return;
    for (const QString &existing : m_settings.sipContacts)
        if (existing.compare(clean, Qt::CaseInsensitive) == 0) return;
    m_settings.sipContacts.append(clean);
    m_settings.sipContacts.sort(Qt::CaseInsensitive);
    emit buddyListChanged(m_settings.sipContacts);
}

void SipBackend::removeBuddy(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return;
    for (auto it = m_settings.sipContacts.begin(); it != m_settings.sipContacts.end();) {
        if (it->compare(clean, Qt::CaseInsensitive) == 0) it = m_settings.sipContacts.erase(it);
        else ++it;
    }
    emit buddyListChanged(m_settings.sipContacts);
}
