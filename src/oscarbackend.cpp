#include "oscarbackend.h"
#include "appbranding.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <utility>

using namespace Oscar;

namespace {

QString normalizedOscarDebugMode(const ConnectionSettings &settings)
{
    const QString mode = settings.oscarDebugMode.trimmed().toCaseFolded();
    if (mode == QStringLiteral("login") || mode == QStringLiteral("full")) return mode;
    // Backward compatibility: an older saved profile with generic debug enabled
    // keeps the old full OSCAR trace behavior until the user picks a new mode.
    if (settings.debug) return QStringLiteral("full");
    return QStringLiteral("off");
}

bool isNinaNetwork(const ConnectionSettings &settings)
{
    if (settings.networkProfile.compare(QStringLiteral("nina"), Qt::CaseInsensitive) == 0)
        return true;
    const QString host = settings.server.trimmed().toCaseFolded();
    return host == QStringLiteral("login.oscar.nina.chat")
        || host.endsWith(QStringLiteral(".oscar.nina.chat"))
        || host.endsWith(QStringLiteral(".nina.chat"));
}


bool oscarLoginAuditEnabled(const ConnectionSettings &settings)
{
    const QString mode = normalizedOscarDebugMode(settings);
    return mode == QStringLiteral("login") || mode == QStringLiteral("full");
}

bool oscarWireTraceEnabled(const ConnectionSettings &settings)
{
    return normalizedOscarDebugMode(settings) == QStringLiteral("full");
}

QString oscarTlvInventory(const QList<Tlv> &items)
{
    QStringList out;
    for (const Tlv &item : items) {
        QString suffix;
        if (item.type == TLV_AUTH_COOKIE || item.type == TLV_PASSWORD_HASH)
            suffix = QStringLiteral("=<redacted>");
        else if (item.type == TLV_LOGIN_ERROR && item.value.size() >= 2) {
            const quint16 code = readU16(item.value, 0);
            suffix = QStringLiteral("=0x%1(%2)")
                         .arg(code, 4, 16, QLatin1Char('0'))
                         .arg(authErrorDescription(code));
        }
        out << QStringLiteral("0x%1[%2]%3")
                   .arg(item.type, 4, 16, QLatin1Char('0'))
                   .arg(item.value.size())
                   .arg(suffix);
    }
    return out.join(QStringLiteral(", "));
}

QString oscarFamilyIdList(const QList<quint16> &families)
{
    QStringList out;
    for (const quint16 family : families)
        out << QStringLiteral("0x%1").arg(family, 4, 16, QLatin1Char('0'));
    return out.join(QStringLiteral(", "));
}

QString oscarFamilyName(quint16 family)
{
    switch (family) {
    case FAM_OSERVICE: return QStringLiteral("Generic Service / BOS");
    case FAM_LOCATE: return QStringLiteral("Locate / Profiles / User Info");
    case FAM_BUDDY: return QStringLiteral("Buddy Presence");
    case FAM_ICBM: return QStringLiteral("Instant Messaging / Rendezvous (ICBM)");
    case FAM_ADVERT: return QStringLiteral("Legacy Advertising");
    case FAM_INVITE: return QStringLiteral("AIM Service Invitations");
    case FAM_ADMIN: return QStringLiteral("Account Administration");
    case FAM_POPUP: return QStringLiteral("Server Popups / Notices");
    case FAM_PERMIT_DENY: return QStringLiteral("Privacy / Permit-Deny");
    case FAM_USER_LOOKUP: return QStringLiteral("User Lookup / Email Search");
    case FAM_STATS: return QStringLiteral("Usage Statistics");
    case FAM_TRANSLATE: return QStringLiteral("Legacy Translation Service");
    case FAM_CHATNAV: return QStringLiteral("Chat Navigation");
    case FAM_CHAT: return QStringLiteral("Chat Rooms");
    case FAM_ODIR: return QStringLiteral("Legacy Online Directory");
    case FAM_BART: return QStringLiteral("Buddy Art / Icons (BART)");
    case FAM_FEEDBAG: return QStringLiteral("SSI / Feedbag Buddy List + Authorization");
    case FAM_ICQ: return QStringLiteral("ICQ Extensions");
    case FAM_BUCP: return QStringLiteral("BUCP Authentication");
    case FAM_ALERT: return QStringLiteral("Alerts / Notifications");
    case FAM_PLUGIN: return QStringLiteral("Legacy Plugin Service");
    case FAM_UNNAMED_24: return QStringLiteral("OSCAR Family 0x0024");
    case FAM_MDIR: return QStringLiteral("Modern Directory (MDIR)");
    case FAM_ARS: return QStringLiteral("AOL Rendezvous Relay Service (ARS)");
    default:
        return QStringLiteral("Unknown / server-specific family");
    }
}

const QByteArray &waffleVoiceCapability()
{
    // Private WaffleHouse capability UUID.  A private UUID keeps our open PCM/UDP
    // media transport distinct from the proprietary legacy AIM Talk framing.
    static const QByteArray value = QByteArray::fromHex("574846564f4943458001574146464c45");
    return value;
}

const QByteArray &legacyAimVoiceCapability()
{
    static const QByteArray value = QByteArray::fromHex("094613414c7f11d18222444553540000");
    return value;
}

QByteArray waffleAdvertisedCapabilities()
{
    QByteArray capabilities;
    capabilities += waffleVoiceCapability();
    capabilities += QByteArray::fromHex("0946134e4c7f11d18222444553540000"); // UTF-8 messaging
    capabilities += QByteArray::fromHex("748f2420628711d18222444553540000"); // native OSCAR chat
    return capabilities;
}

QString capabilityName(const QByteArray &uuid)
{
    static const QHash<QByteArray, QString> names = {
        {QByteArray::fromHex("094600004c7f11d18222444553540000"), QStringLiteral("Short capability blocks")},
        {QByteArray::fromHex("094600014c7f11d18222444553540000"), QStringLiteral("Secure IM")},
        {QByteArray::fromHex("094600024c7f11d18222444553540000"), QStringLiteral("XHTML IM")},
        {QByteArray::fromHex("094601014c7f11d18222444553540000"), QStringLiteral("RTC video")},
        {QByteArray::fromHex("094601024c7f11d18222444553540000"), QStringLiteral("Camera")},
        {QByteArray::fromHex("094601034c7f11d18222444553540000"), QStringLiteral("Microphone")},
        {QByteArray::fromHex("094601044c7f11d18222444553540000"), QStringLiteral("RTC audio")},
        {legacyAimVoiceCapability(), QStringLiteral("Legacy AIM Voice / Talk")},
        {QByteArray::fromHex("094613434c7f11d18222444553540000"), QStringLiteral("OSCAR file transfer")},
        {QByteArray::fromHex("094613454c7f11d18222444553540000"), QStringLiteral("Direct IM")},
        {QByteArray::fromHex("094613464c7f11d18222444553540000"), QStringLiteral("Buddy icon / avatar")},
        {QByteArray::fromHex("094613484c7f11d18222444553540000"), QStringLiteral("File sharing / receive file")},
        {QByteArray::fromHex("0946134d4c7f11d18222444553540000"), QStringLiteral("AIM/ICQ interoperability")},
        {QByteArray::fromHex("0946134e4c7f11d18222444553540000"), QStringLiteral("UTF-8 messaging")},
        {QByteArray::fromHex("748f2420628711d18222444553540000"), QStringLiteral("Chat")},
        {waffleVoiceCapability(), QStringLiteral("WaffleHouse OSCAR Voice")},
    };
    return names.value(uuid, QStringLiteral("Unknown capability"));
}


QString capabilityCategory(const QByteArray &uuid)
{
    if (uuid == waffleVoiceCapability()) return QStringLiteral("WaffleHouse extensions");

    static const QSet<QByteArray> legacyAim = {
        legacyAimVoiceCapability(),
        QByteArray::fromHex("094601014c7f11d18222444553540000"), // RTC video
        QByteArray::fromHex("094601024c7f11d18222444553540000"), // camera
        QByteArray::fromHex("094601034c7f11d18222444553540000"), // microphone
        QByteArray::fromHex("094601044c7f11d18222444553540000"), // RTC audio
        QByteArray::fromHex("094613434c7f11d18222444553540000"), // file transfer
        QByteArray::fromHex("094613454c7f11d18222444553540000"), // Direct IM
        QByteArray::fromHex("094613464c7f11d18222444553540000"), // buddy icon
        QByteArray::fromHex("094613484c7f11d18222444553540000"), // file sharing
    };
    if (legacyAim.contains(uuid)) return QStringLiteral("Legacy AIM / rendezvous");

    if (capabilityName(uuid) != QStringLiteral("Unknown capability"))
        return QStringLiteral("Standard OSCAR");
    return QStringLiteral("Unknown / client-specific");
}

QList<QByteArray> splitCapabilities(const QByteArray &raw)
{
    QList<QByteArray> result;
    for (qsizetype i = 0; i + 16 <= raw.size(); i += 16) result.append(raw.mid(i, 16));
    return result;
}

QList<QByteArray> shortCapabilities(const QByteArray &raw)
{
    QList<QByteArray> result;
    const QByteArray suffix = QByteArray::fromHex("4c7f11d18222444553540000");
    for (qsizetype i = 0; i + 2 <= raw.size(); i += 2) {
        QByteArray full = QByteArray::fromHex("0946");
        full += raw.mid(i, 2);
        full += suffix;
        result.append(full);
    }
    return result;
}

QStringList describeCapabilities(QList<QByteArray> caps)
{
    QStringList lines;
    QSet<QByteArray> seen;
    for (const QByteArray &cap : caps) {
        if (cap.size() != 16 || seen.contains(cap)) continue;
        seen.insert(cap);
        lines.append(QStringLiteral("%1  %2")
                         .arg(capabilityName(cap), QString::fromLatin1(cap.toHex())));
    }
    lines.sort(Qt::CaseInsensitive);
    return lines;
}

QByteArray ipv4Bytes(const QString &text)
{
    QHostAddress address(text);
    if (address.protocol() != QAbstractSocket::IPv4Protocol) return {};
    QByteArray out;
    appendU32(out, address.toIPv4Address());
    return out;
}

QString ipv4Text(const QByteArray &raw)
{
    if (raw.size() < 4) return {};
    return QHostAddress(readU32(raw, 0)).toString();
}
}

OscarBackend::OscarBackend(ConnectionSettings settings, QObject *parent)
    : ChatBackend(std::move(settings), parent)
{
}

OscarBackend::~OscarBackend()
{
    m_stopRequested = true;
    if (m_thread && m_thread != QThread::currentThread()) {
        m_thread->wait();
    }
}

void OscarBackend::start()
{
    if (m_thread) {
        return;
    }
    m_stopRequested = false;
    m_thread = QThread::create([this] { run(); });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, [this] { m_thread = nullptr; }, Qt::QueuedConnection);
    m_thread->start();
}

void OscarBackend::stop()
{
    m_stopRequested = true;
    QThread *thread = m_thread;
    if (thread && thread != QThread::currentThread()) {
        thread->wait(15000);
    }
}

void OscarBackend::enqueue(Command command)
{
    QMutexLocker locker(&m_commandMutex);
    m_commands.enqueue(std::move(command));
}

QList<OscarBackend::Command> OscarBackend::takeCommands()
{
    QList<Command> result;
    QMutexLocker locker(&m_commandMutex);
    while (!m_commands.isEmpty()) {
        result.push_back(m_commands.dequeue());
    }
    return result;
}

void OscarBackend::sendPrivateMessage(const QString &target, const QString &message)
{
    enqueue({CommandType::SendIm, target, message, {}, false});
}

void OscarBackend::joinRoom(const QString &room, bool privateRoom)
{
    enqueue({CommandType::JoinRoom, room, {}, {}, privateRoom});
}

void OscarBackend::sendRoomMessage(const QString &room, const QString &message)
{
    enqueue({CommandType::SendRoom, room, message, {}, false});
}

void OscarBackend::leaveRoom(const QString &room)
{
    enqueue({CommandType::LeaveRoom, room, {}, {}, false});
}

void OscarBackend::changePassword(const QString &currentPassword,
                                  const QString &newPassword)
{
    enqueue({CommandType::Password, currentPassword, newPassword, {}, false});
}

void OscarBackend::sendRaw(const QString &family,
                           const QString &subtype,
                           const QString &hexBody)
{
    enqueue({CommandType::Raw, family, subtype, hexBody, false});
}

void OscarBackend::addBuddy(const QString &name)
{
    enqueue({CommandType::AddBuddy, name.trimmed(), {}, {}, false});
}

void OscarBackend::removeBuddy(const QString &name)
{
    enqueue({CommandType::RemoveBuddy, name.trimmed(), {}, {}, false});
}

void OscarBackend::setAwayMessage(const QString &message)
{
    enqueue({CommandType::SetAway, message.trimmed(), {}, {}, false, 0});
}

void OscarBackend::setAfkMessage(const QString &message)
{
    enqueue({CommandType::SetAfk, message.trimmed(), {}, {}, true, 0});
}

void OscarBackend::setIdleSeconds(quint32 seconds)
{
    enqueue({CommandType::SetIdle, {}, {}, {}, false, seconds});
}

void OscarBackend::setBack()
{
    enqueue({CommandType::SetBack, {}, {}, {}, false, 0});
}

void OscarBackend::requestClientVersion(const QString &target)
{
    const QString clean = target.trimmed();
    if (clean.isEmpty()) return;
    enqueue({CommandType::SendIm, clean, QStringLiteral("[[WHVER:Q]]"), {}, true, 0});
}

void OscarBackend::setProfile(const QString &profile)
{
    // Keep the user's chosen profile in the account settings as well as sending
    // it to BOS. Several revival/private OSCAR servers do not persist LOCATE
    // profile state across sessions; replaying this value makes WaffleHouse
    // behave like a stable classic client without depending on server storage.
    m_settings.oscarProfile = profile;
    enqueue({CommandType::SetProfile, profile, {}, {}, false, 0});
}

void OscarBackend::refreshServerCapabilities()
{
    enqueue({CommandType::RefreshCapabilities, {}, {}, {}, false, 0});
}

void OscarBackend::requestUserInfo(const QString &target)
{
    const QString clean = target.trimmed();
    if (clean.isEmpty()) return;
    enqueue({CommandType::RequestUserInfo, clean});
}

bool OscarBackend::supportsFamily(quint16 family) const
{
    QMutexLocker locker(&m_capabilityMutex);
    return m_serverFamilies.contains(family);
}

bool OscarBackend::peerAdvertisesCapability(const QString &target, const QByteArray &capability) const
{
    QMutexLocker locker(&m_capabilityMutex);
    return m_peerCapabilities.value(target.trimmed().toCaseFolded()).contains(capability);
}

void OscarBackend::requestDirectoryInfo(const QString &target)
{
    const QString clean = target.trimmed();
    if (clean.isEmpty()) return;
    enqueue({CommandType::RequestDirectoryInfo, clean});
}

void OscarBackend::setDirectoryInfo(const QVariantMap &fields)
{
    Command command; command.type = CommandType::SetDirectoryInfo; command.map = fields; enqueue(std::move(command));
}

void OscarBackend::findByEmail(const QString &email)
{
    const QString clean = email.trimmed(); if (clean.isEmpty()) return;
    enqueue({CommandType::FindByEmail, clean});
}

void OscarBackend::inviteByEmail(const QString &email, const QString &message)
{
    const QString clean = email.trimmed(); if (clean.isEmpty()) return;
    enqueue({CommandType::InviteByEmail, clean, message.trimmed()});
}

void OscarBackend::addPermit(const QString &target) { enqueue({CommandType::PrivacyListAction, target.trimmed(), QStringLiteral("permit"), {}, false, PD_ADD_PERMIT}); }
void OscarBackend::removePermit(const QString &target) { enqueue({CommandType::PrivacyListAction, target.trimmed(), QStringLiteral("unpermit"), {}, false, PD_REMOVE_PERMIT}); }
void OscarBackend::addDeny(const QString &target) { enqueue({CommandType::PrivacyListAction, target.trimmed(), QStringLiteral("block"), {}, false, PD_ADD_DENY}); }
void OscarBackend::removeDeny(const QString &target) { enqueue({CommandType::PrivacyListAction, target.trimmed(), QStringLiteral("unblock"), {}, false, PD_REMOVE_DENY}); }
void OscarBackend::addTemporaryPermit(const QString &target) { enqueue({CommandType::PrivacyListAction, target.trimmed(), QStringLiteral("temporary permit"), {}, false, PD_ADD_TEMP_PERMIT}); }
void OscarBackend::removeTemporaryPermit(const QString &target) { enqueue({CommandType::PrivacyListAction, target.trimmed(), QStringLiteral("remove temporary permit"), {}, false, PD_REMOVE_TEMP_PERMIT}); }

void OscarBackend::requestAuthorization(const QString &target, const QString &message)
{
    enqueue({CommandType::AuthorizationRequest, target.trimmed(), message.trimmed()});
}

void OscarBackend::respondAuthorization(const QString &target, bool accept, const QString &message)
{
    enqueue({CommandType::AuthorizationResponse, target.trimmed(), message.trimmed(), {}, accept});
}

void OscarBackend::preAuthorize(const QString &target, const QString &message)
{
    enqueue({CommandType::PreAuthorize, target.trimmed(), message.trimmed()});
}

void OscarBackend::removeMeFromBuddyList(const QString &target)
{
    enqueue({CommandType::RemoveMe, target.trimmed()});
}

void OscarBackend::addTemporaryBuddy(const QString &target)
{
    enqueue({CommandType::TemporaryBuddy, target.trimmed(), {}, {}, true});
}

void OscarBackend::removeTemporaryBuddy(const QString &target)
{
    enqueue({CommandType::TemporaryBuddy, target.trimmed(), {}, {}, false});
}

void OscarBackend::requestWatcherList() { enqueue({CommandType::WatcherList}); }
void OscarBackend::retrieveStoredMessages() { enqueue({CommandType::RetrieveStoredMessages}); }

void OscarBackend::sendTypingNotification(const QString &target, quint16 event)
{
    enqueue({CommandType::TypingNotification, target.trimmed(), {}, {}, false, event});
}

void OscarBackend::requestAccountInfo() { enqueue({CommandType::RequestAccountInfo}); }
void OscarBackend::changeAccountEmail(const QString &email) { enqueue({CommandType::ChangeAccountEmail, email.trimmed()}); }
void OscarBackend::changeFormattedScreenName(const QString &formattedName) { enqueue({CommandType::ChangeFormattedName, formattedName.trimmed()}); }
void OscarBackend::confirmAccount() { enqueue({CommandType::ConfirmAccount}); }
void OscarBackend::deleteAccount() { enqueue({CommandType::DeleteAccount}); }
void OscarBackend::setPrivacyFlags(quint32 flags) { enqueue({CommandType::SetPrivacyFlags, {}, {}, {}, false, flags}); }

void OscarBackend::proposeVoice(const QString &target,
                                const QString &cookieHex,
                                const QString &localAddress,
                                quint16 localPort,
                                int sampleRate)
{
    Command command;
    command.type = CommandType::VoicePropose;
    command.a = target.trimmed();
    command.b = cookieHex;
    command.c = localAddress;
    command.number = localPort;
    command.number2 = static_cast<quint32>(sampleRate);
    enqueue(std::move(command));
}

void OscarBackend::acceptVoice(const QString &target,
                               const QString &cookieHex,
                               const QString &localAddress,
                               quint16 localPort,
                               int sampleRate)
{
    Command command;
    command.type = CommandType::VoiceAccept;
    command.a = target.trimmed();
    command.b = cookieHex;
    command.c = localAddress;
    command.number = localPort;
    command.number2 = static_cast<quint32>(sampleRate);
    enqueue(std::move(command));
}

void OscarBackend::cancelVoice(const QString &target,
                               const QString &cookieHex,
                               quint16 reason)
{
    Command command;
    command.type = CommandType::VoiceCancel;
    command.a = target.trimmed();
    command.b = cookieHex;
    command.number = reason;
    enqueue(std::move(command));
}

QString OscarBackend::auditLogPath() const
{
    QString stem = QStringLiteral("%1-%2-%3")
                       .arg(m_settings.username.trimmed())
                       .arg(m_settings.server.trimmed())
                       .arg(m_settings.port);
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    if (stem.isEmpty()) stem = QStringLiteral("oscar-account");

    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/share/WaffleHouse-Client");
    return QDir(base).filePath(QStringLiteral("logs/oscar-%1.log").arg(stem));
}

void OscarBackend::protocolLog(const QString &text)
{
    // protocolLog() is fed only redacted OSCAR diagnostic strings.  Persist the
    // same safe text shown in Activity whenever OSCAR audit logging is enabled.
    if (oscarLoginAuditEnabled(m_settings)) {
        const QString path = auditLogPath();
        const QFileInfo info(path);
        QDir().mkpath(info.absolutePath());

        // Full wire trace can grow quickly. Keep one previous 5 MiB generation
        // instead of allowing an unattended client to consume unlimited space.
        if (info.exists() && info.size() >= 5 * 1024 * 1024) {
            const QString oldPath = path + QStringLiteral(".old");
            QFile::remove(oldPath);
            QFile::rename(path, oldPath);
        }

        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                << QLatin1Char(' ') << text << QLatin1Char('\n');
        }
    }

    emit eventReceived(QStringLiteral("status"), QString(), text);
}

[[noreturn]] void OscarBackend::fail(const QString &message) const
{
    throw ProtocolError(message);
}

QPair<QString, quint16> OscarBackend::bosRedirectEndpoint(const QString &advertised,
                                                          quint16 fallbackPort) const
{
    auto endpoint = parseEndpoint(advertised, fallbackPort);
    if (!m_settings.redirectHost.trimmed().isEmpty()) {
        endpoint.first = m_settings.redirectHost.trimmed();
    }
    if (m_settings.redirectPort != 0) {
        endpoint.second = m_settings.redirectPort;
    }
    return endpoint;
}

QPair<QString, quint16> OscarBackend::serviceRedirectEndpoint(const QString &advertised,
                                                              quint16 fallbackPort) const
{
    // Secondary OSCAR services are authoritative redirects from BOS. Never
    // rewrite ChatNav/Chat/Admin/BART/etc. through the login/BOS override.
    return parseEndpoint(advertised, fallbackPort);
}

void OscarBackend::expectGreeting(FlapConnection &connection)
{
    const FlapFrame frame = connection.receiveFlap();
    if (frame.channel != FLAP_SIGNON) {
        fail(QStringLiteral("%1: expected FLAP signon greeting, got channel %2")
                 .arg(connection.label())
                 .arg(frame.channel));
    }
    if (oscarLoginAuditEnabled(m_settings) && frame.payload.size() >= 4) {
        protocolLog(QStringLiteral("[oscar-login] %1 server FLAP version=%2")
                        .arg(connection.label())
                        .arg(readU32(frame.payload, 0)));
    }
}

QPair<QString, QByteArray> OscarBackend::authenticate()
{
    const bool audit = oscarLoginAuditEnabled(m_settings);
    if (audit) {
        protocolLog(QStringLiteral("[oscar-login] START target=%1:%2 screen-name=%3 mode=%4")
                        .arg(m_settings.server)
                        .arg(m_settings.port)
                        .arg(m_settings.username)
                        .arg(normalizedOscarDebugMode(m_settings)));
        if (!m_settings.redirectHost.trimmed().isEmpty() || m_settings.redirectPort != 0) {
            protocolLog(QStringLiteral("[oscar-login] Redirect override host=%1 port=%2")
                            .arg(m_settings.redirectHost.trimmed().isEmpty() ? QStringLiteral("<server-advertised>") : m_settings.redirectHost)
                            .arg(m_settings.redirectPort == 0 ? QStringLiteral("<server-advertised>") : QString::number(m_settings.redirectPort)));
        }
        protocolLog(QStringLiteral("[oscar-login] Secrets are redacted: password, password hash, auth challenge material, and BOS/service cookies are never printed."));
    }

    FlapConnection auth(m_settings.server,
                        m_settings.port,
                        QStringLiteral("auth"),
                        oscarWireTraceEnabled(m_settings),
                        [this](const QString &s) { protocolLog(s); });
    if (audit) protocolLog(QStringLiteral("[oscar-login] TCP connect -> auth server"));
    auth.connectToHost();
    if (audit) protocolLog(QStringLiteral("[oscar-login] TCP connected; waiting for FLAP signon greeting"));
    expectGreeting(auth);
    auth.sendSignon();
    if (audit) protocolLog(QStringLiteral("[oscar-login] Sent FLAP SIGNON version=1"));

    auth.sendSnac(FAM_BUCP,
                  BUCP_CHALLENGE_REQUEST,
                  tlv(TLV_SCREEN_NAME, m_settings.username));
    if (audit) protocolLog(QStringLiteral("[oscar-login] Sent BUCP 0x0017/0x0006 challenge request for '%1'").arg(m_settings.username));

    QByteArray authKey;
    while (true) {
        const Snac snac = auth.receiveSnac();
        if (audit) {
            protocolLog(QStringLiteral("[oscar-login] auth <= SNAC %1/%2 flags=0x%3 req=%4 len=%5")
                            .arg(snac.family, 4, 16, QLatin1Char('0'))
                            .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                            .arg(snac.flags, 4, 16, QLatin1Char('0'))
                            .arg(snac.requestId)
                            .arg(snac.body.size()));
        }
        if (snac.family == FAM_BUCP && snac.subtype == BUCP_CHALLENGE_RESPONSE) {
            if (snac.body.size() < 2) {
                fail(QStringLiteral("truncated BUCP challenge response"));
            }

            // BUCP challenge responses exist in both a classic 16-bit-length
            // form and a revival/private-server 32-bit-length form. NINA has
            // used the latter in deployments. Accept either representation so
            // the password digest is built from the actual challenge bytes.
            qsizetype prefixBytes = 2;
            quint32 length = readU16(snac.body, 0);
            QString lengthForm = QStringLiteral("u16");
            if (snac.body.size() >= 4) {
                const quint32 length32 = readU32(snac.body, 0);
                const bool valid32 = length32 > 0
                    && static_cast<quint64>(4) + length32 <= static_cast<quint64>(snac.body.size());
                const bool valid16 = static_cast<quint64>(2) + length <= static_cast<quint64>(snac.body.size());
                if (valid32 && (length == 0 || !valid16)) {
                    length = length32;
                    prefixBytes = 4;
                    lengthForm = QStringLiteral("u32");
                }
            }
            if (length == 0 || static_cast<quint64>(prefixBytes) + length > static_cast<quint64>(snac.body.size())) {
                fail(QStringLiteral("truncated or empty BUCP auth key"));
            }
            authKey = snac.body.mid(prefixBytes, static_cast<qsizetype>(length));
            if (audit) protocolLog(QStringLiteral("[oscar-login] Received BUCP challenge key: <redacted:%1 bytes> length-form=%2")
                                       .arg(authKey.size()).arg(lengthForm));
            break;
        }

        if (snac.family == FAM_BUCP && snac.subtype == BUCP_LOGIN_RESPONSE) {
            qsizetype offset = 0;
            const auto items = parseTlvs(snac.body, offset);
            if (audit) protocolLog(QStringLiteral("[oscar-login] Early login response TLVs: %1").arg(oscarTlvInventory(items)));
            const QByteArray error = firstTlv(items, TLV_LOGIN_ERROR);
            if (error.size() >= 2) {
                const quint16 code = readU16(error, 0);
                const QString url = QString::fromUtf8(firstTlv(items, TLV_ERROR_URL));
                if (audit) protocolLog(QStringLiteral("[oscar-login] AUTH REJECTED code=0x%1 meaning='%2'%3")
                                           .arg(code, 4, 16, QLatin1Char('0'))
                                           .arg(authErrorDescription(code))
                                           .arg(url.isEmpty() ? QString() : QStringLiteral(" url=") + url));
                fail(QStringLiteral("login challenge rejected, OSCAR error 0x%1 (%2)%3")
                         .arg(code, 4, 16, QLatin1Char('0'))
                         .arg(authErrorDescription(code))
                         .arg(url.isEmpty() ? QString() : QStringLiteral("; ") + url));
            }
        }
        if (snac.family == FAM_BUCP && snac.subtype == BUCP_SECURID_REQUEST) {
            if (audit) protocolLog(QStringLiteral("[oscar-login] Server requested SecurID/second-factor continuation (BUCP 0x0017/0x000A)."));
            fail(QStringLiteral("OSCAR server requires SecurID/second-factor authentication; this client does not yet implement BUCP SecurID continuation"));
        }
        if (snac.family == FAM_BUCP && snac.subtype == BUCP_ERR) {
            const quint16 code = snac.body.size() >= 2 ? readU16(snac.body, 0) : 0xffff;
            if (audit) protocolLog(QStringLiteral("[oscar-login] BUCP error SNAC code=0x%1").arg(code, 4, 16, QLatin1Char('0')));
            fail(QStringLiteral("BUCP challenge failed with error 0x%1").arg(code, 4, 16, QLatin1Char('0')));
        }
    }

    const QByteArray hashedPassword = passwordHash(m_settings.password, authKey);
    QByteArray body;
    body += tlv(TLV_SCREEN_NAME, m_settings.username);
    body += tlv(TLV_PASSWORD_HASH, hashedPassword);
    body += tlv(TLV_CLIENT_ID, QStringLiteral("%1 C++ OSCAR client").arg(appDisplayName()));
    body += tlv(TLV_MULTI_CONN, QByteArray(1, char(1)));
    if (audit) {
        protocolLog(QStringLiteral("[oscar-login] Sending BUCP 0x0017/0x0002 login: screen-name='%1', password-hash=<redacted:%2 bytes>, client='%3 C++ OSCAR client', multi-conn=1")
                        .arg(m_settings.username)
                        .arg(hashedPassword.size())
                        .arg(appDisplayName()));
    }
    auth.sendSnac(FAM_BUCP, BUCP_LOGIN_REQUEST, body);

    while (true) {
        const Snac snac = auth.receiveSnac();
        if (audit) {
            protocolLog(QStringLiteral("[oscar-login] auth <= SNAC %1/%2 flags=0x%3 req=%4 len=%5")
                            .arg(snac.family, 4, 16, QLatin1Char('0'))
                            .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                            .arg(snac.flags, 4, 16, QLatin1Char('0'))
                            .arg(snac.requestId)
                            .arg(snac.body.size()));
        }
        if (snac.family == FAM_BUCP && snac.subtype == BUCP_SECURID_REQUEST) {
            if (audit) protocolLog(QStringLiteral("[oscar-login] Server requested SecurID/second-factor continuation after password validation."));
            fail(QStringLiteral("OSCAR server requires SecurID/second-factor authentication; this client does not yet implement BUCP SecurID continuation"));
        }
        if (snac.family == FAM_BUCP && snac.subtype == BUCP_ERR) {
            const quint16 code = snac.body.size() >= 2 ? readU16(snac.body, 0) : 0xffff;
            if (audit) protocolLog(QStringLiteral("[oscar-login] BUCP error SNAC code=0x%1").arg(code, 4, 16, QLatin1Char('0')));
            fail(QStringLiteral("BUCP login failed with error 0x%1").arg(code, 4, 16, QLatin1Char('0')));
        }
        if (snac.family != FAM_BUCP || snac.subtype != BUCP_LOGIN_RESPONSE) {
            continue;
        }

        qsizetype offset = 0;
        const auto items = parseTlvs(snac.body, offset);
        if (audit) protocolLog(QStringLiteral("[oscar-login] Login response TLVs: %1").arg(oscarTlvInventory(items)));
        const QByteArray error = firstTlv(items, TLV_LOGIN_ERROR);
        if (error.size() >= 2) {
            const quint16 code = readU16(error, 0);
            const QString url = QString::fromUtf8(firstTlv(items, TLV_ERROR_URL));
            if (audit) protocolLog(QStringLiteral("[oscar-login] AUTH FAILED code=0x%1 meaning='%2'%3")
                                       .arg(code, 4, 16, QLatin1Char('0'))
                                       .arg(authErrorDescription(code))
                                       .arg(url.isEmpty() ? QString() : QStringLiteral(" url=") + url));
            fail(QStringLiteral("login failed, OSCAR error 0x%1 (%2)%3")
                     .arg(code, 4, 16, QLatin1Char('0'))
                     .arg(authErrorDescription(code))
                     .arg(url.isEmpty() ? QString() : QStringLiteral("; ") + url));
        }

        const QByteArray hostRaw = firstTlv(items, TLV_RECONNECT_HOST);
        const QByteArray cookie = firstTlv(items, TLV_AUTH_COOKIE);
        if (hostRaw.isEmpty() || cookie.isEmpty()) {
            if (audit) protocolLog(QStringLiteral("[oscar-login] Login response is missing redirect data: BOS-host-bytes=%1 cookie-bytes=%2")
                                       .arg(hostRaw.size()).arg(cookie.size()));
            fail(QStringLiteral("login succeeded but BOS host/cookie are missing"));
        }

        if (audit) {
            protocolLog(QStringLiteral("[oscar-login] AUTH SUCCESS BOS=%1 auth-cookie=<redacted:%2 bytes>")
                            .arg(QString::fromUtf8(hostRaw)).arg(cookie.size()));
        }
        auth.close();
        return {QString::fromUtf8(hostRaw), cookie};
    }
}

void OscarBackend::bootstrapService(FlapConnection &connection,
                                    const QByteArray &cookie,
                                    bool bosBootstrap)
{
    const bool audit = oscarLoginAuditEnabled(m_settings);
    const auto auditBootstrapSnac = [this, &connection, audit](const Snac &snac, const QString &stage) {
        if (audit) {
            protocolLog(QStringLiteral("[oscar-login] %1 %2 <= SNAC %3/%4 flags=0x%5 req=%6 len=%7")
                            .arg(connection.label(), stage)
                            .arg(snac.family, 4, 16, QLatin1Char('0'))
                            .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                            .arg(snac.flags, 4, 16, QLatin1Char('0'))
                            .arg(snac.requestId)
                            .arg(snac.body.size()));
        }
        if (snac.family == FAM_OSERVICE && snac.subtype == 0x0001) {
            const quint16 code = snac.body.size() >= 2 ? readU16(snac.body, 0) : 0xffff;
            throw ProtocolError(QStringLiteral("%1: OSCAR service error during %2: 0x%3")
                                    .arg(connection.label(), stage)
                                    .arg(code, 4, 16, QLatin1Char('0')));
        }
    };
    if (audit) protocolLog(QStringLiteral("[oscar-login] Bootstrap %1 -> %2:%3")
                               .arg(connection.label(), connection.host())
                               .arg(connection.port()));
    connection.connectToHost();

    // OSCAR service endpoints are not unanimous about who speaks first after
    // TCP connect. Some classic/private implementations send a FLAP SIGNON
    // greeting immediately, while NINA's documented BOSS flow permits the
    // client to send its cookie-bearing FLAP SIGNON as the first frame. Do a
    // short opportunistic greeting read, but never sit on the socket waiting
    // for a greeting while the server is waiting for our cookie.
    bool consumedGreeting = false;
    if (connection.waitForData(250)) {
        expectGreeting(connection);
        consumedGreeting = true;
    }
    QList<Tlv> signonTlvs{Tlv{TLV_AUTH_COOKIE, cookie}};
    // NINA's BOSS Stage-2 sign-on requires OSERVICE__MULTICONN_FLAGS (0x004A)
    // alongside the login cookie.  This is also what multi-instance-capable
    // classic AIM clients send.  Do not add it to secondary service cookies,
    // whose documented sign-on is cookie-only.
    if (bosBootstrap) {
        signonTlvs.push_back(Tlv{TLV_MULTI_CONN, QByteArray(1, char(1))});
    }
    connection.sendSignon(signonTlvs);
    if (audit) protocolLog(QStringLiteral("[oscar-login] %1 sent service-cookie FLAP SIGNON <redacted:%2 bytes> multi-conn=%3 startup=%4")
                               .arg(connection.label()).arg(cookie.size())
                               .arg(bosBootstrap ? QStringLiteral("1") : QStringLiteral("n/a"))
                               .arg(consumedGreeting ? QStringLiteral("server-greeting-first")
                                                     : QStringLiteral("client-signon-first")));

    const auto receiveBootstrapSnac = [&connection](const QString &stage) -> Snac {
        try {
            // receiveSnac() deliberately skips FLAP SIGNON/KEEPALIVE frames.
            // On NINA BOSS this consumes the required server FLAP SIGNON reply
            // before returning the following OSERVICE__HOST_ONLINE SNAC.
            return connection.receiveSnac();
        } catch (const ProtocolError &e) {
            throw ProtocolError(QStringLiteral("%1: %2 while waiting for %3 from %4:%5")
                                    .arg(connection.label(), QString::fromUtf8(e.what()), stage, connection.host())
                                    .arg(connection.port()));
        }
    };

    while (true) {
        const Snac snac = receiveBootstrapSnac(QStringLiteral("HOST_ONLINE"));
        auditBootstrapSnac(snac, QStringLiteral("HOST_ONLINE"));
        if (snac.family == FAM_OSERVICE && snac.subtype == OS_HOST_ONLINE) {
            if (snac.body.size() % 2 != 0) {
                fail(QStringLiteral("odd HostOnline family list"));
            }
            connection.families.clear();
            for (qsizetype i = 0; i < snac.body.size(); i += 2) {
                connection.families.push_back(readU16(snac.body, i));
            }
            break;
        }
    }
    if (audit) protocolLog(QStringLiteral("[oscar-login] %1 HOST_ONLINE families (%2): %3")
                               .arg(connection.label())
                               .arg(connection.families.size())
                               .arg(oscarFamilyIdList(connection.families)));

    const bool ninaNetwork = isNinaNetwork(m_settings);

    // NINAPatcher does not replace AIM's OSCAR engine; it redirects stock AIM
    // to NINA endpoints.  For the NINA profile, therefore, advertise the
    // classic WinAIM/libfaim family set rather than inventing capabilities for
    // every family the server happens to announce.
    const auto clientImplementsFamily = [](quint16 family) -> bool {
        switch (family) {
        case FAM_OSERVICE:
        case FAM_LOCATE:
        case FAM_BUDDY:
        case FAM_ICBM:
        case FAM_INVITE:
        case FAM_ADMIN:
        case FAM_POPUP:
        case FAM_PERMIT_DENY:
        case FAM_USER_LOOKUP:
        case FAM_STATS:
        case FAM_TRANSLATE:
        case FAM_CHATNAV:
        case FAM_CHAT:
        case FAM_ODIR:
        case FAM_BART:
        case FAM_FEEDBAG:
        case FAM_ALERT:
            return true;
        default:
            return false;
        }
    };

    const auto familyVersion = [ninaNetwork](quint16 family) -> quint16 {
        if (ninaNetwork) {
            // Classic patched AIM compatibility.  WinAIM/libfaim-era clients
            // negotiate OSERVICE v3 and SSI/FEEDBAG v2; all other advertised
            // BOS families use v1.  NINA supports these legacy clients by
            // design, and NINAPatcher leaves these protocol versions intact.
            if (family == FAM_OSERVICE) return 3;
            if (family == FAM_FEEDBAG) return 2;
            return 1;
        }
        // Keep the newer profile for generic/custom revival servers.
        if (family == FAM_OSERVICE) return 4;
        if (family == FAM_FEEDBAG) return 3;
        return 1;
    };

    QList<quint16> negotiatedFamilies;
    QByteArray versions;
    for (const quint16 family : connection.families) {
        if (!clientImplementsFamily(family)) {
            if (audit) protocolLog(QStringLiteral("[oscar-login] %1 skipping unsupported HOST_ONLINE family 0x%2")
                                       .arg(connection.label())
                                       .arg(family, 4, 16, QLatin1Char('0')));
            continue;
        }
        negotiatedFamilies.push_back(family);
        appendU16(versions, family);
        appendU16(versions, familyVersion(family));
    }
    if (!negotiatedFamilies.contains(FAM_OSERVICE)) {
        fail(QStringLiteral("HOST_ONLINE did not offer OSCAR Generic Service"));
    }

    // Stock AIM/libfaim uses a normal generated SNAC id here.  More
    // importantly, it dispatches HOST_VERSIONS by family/subtype and does not
    // require the server to echo that request id.  NINA has hosted multiple
    // generations of AIM servers/clients, so be equally tolerant.
    const quint32 versionsRequest = connection.sendSnac(FAM_OSERVICE, OS_CLIENT_VERSIONS, versions);
    if (audit) protocolLog(QStringLiteral("[oscar-login] %1 CLIENT_VERSIONS sent: records=%2 request-id=%3 body=%4")
                               .arg(connection.label())
                               .arg(negotiatedFamilies.size())
                               .arg(versionsRequest)
                               .arg(QString::fromLatin1(versions.toHex())));

    while (true) {
        const Snac snac = receiveBootstrapSnac(QStringLiteral("HOST_VERSIONS"));
        auditBootstrapSnac(snac, QStringLiteral("HOST_VERSIONS"));
        if (snac.family == FAM_OSERVICE && snac.subtype == OS_HOST_VERSIONS) {
            if (audit && snac.requestId != versionsRequest) {
                protocolLog(QStringLiteral("[oscar-login] %1 accepting HOST_VERSIONS with non-echoed request-id=%2 (sent=%3), matching stock AIM behavior")
                                .arg(connection.label()).arg(snac.requestId).arg(versionsRequest));
            }
            break;
        }
    }
    if (audit) protocolLog(QStringLiteral("[oscar-login] %1 family-version negotiation complete").arg(connection.label()));

    const quint32 rateRequest = connection.sendSnac(FAM_OSERVICE, OS_RATE_QUERY);
    QList<quint16> rateIds;
    while (true) {
        const Snac snac = receiveBootstrapSnac(QStringLiteral("RATE_REPLY"));
        auditBootstrapSnac(snac, QStringLiteral("RATE_REPLY"));
        if (snac.family == FAM_OSERVICE && snac.subtype == OS_RATE_REPLY) {
            if (audit && snac.requestId != rateRequest) {
                protocolLog(QStringLiteral("[oscar-login] %1 accepting RATE_REPLY with non-echoed request-id=%2 (sent=%3)")
                                .arg(connection.label()).arg(snac.requestId).arg(rateRequest));
            }
            if (snac.body.size() >= 2) {
                const quint16 count = readU16(snac.body, 0);

                // OSCAR rate-parameter records exist in both the older
                // 30-byte form and the later 35-byte form (which appends the
                // last-arrival delta and current state byte).  Validate the
                // rate-group tail to determine which layout this server sent
                // instead of stepping through the reply with a hard-coded
                // record size and accidentally ACKing bytes from inside a
                // previous record as bogus class IDs.
                auto hasValidRateGroups = [&](qsizetype offset) {
                    for (quint16 i = 0; i < count; ++i) {
                        if (offset + 4 > snac.body.size()) return false;
                        const quint16 members = readU16(snac.body, offset + 2);
                        offset += 4;
                        const qsizetype memberBytes = static_cast<qsizetype>(members) * 4;
                        if (memberBytes < 0 || offset + memberBytes > snac.body.size()) return false;
                        offset += memberBytes;
                    }
                    return offset == snac.body.size();
                };

                int recordBytes = 0;
                const qsizetype v3Tail = 2 + static_cast<qsizetype>(count) * 35;
                const qsizetype v2Tail = 2 + static_cast<qsizetype>(count) * 30;
                if (v3Tail <= snac.body.size() && hasValidRateGroups(v3Tail)) recordBytes = 35;
                else if (v2Tail <= snac.body.size() && hasValidRateGroups(v2Tail)) recordBytes = 30;
                else if (v3Tail <= snac.body.size()) recordBytes = 35;
                else if (v2Tail <= snac.body.size()) recordBytes = 30;

                qsizetype offset = 2;
                for (quint16 i = 0; i < count && recordBytes > 0; ++i) {
                    if (offset + recordBytes > snac.body.size()) break;
                    rateIds.push_back(readU16(snac.body, offset));
                    offset += recordBytes;
                }
            }
            break;
        }
    }
    if (audit) protocolLog(QStringLiteral("[oscar-login] %1 rate negotiation returned %2 class(es)")
                               .arg(connection.label()).arg(rateIds.size()));

    if (!rateIds.isEmpty()) {
        QByteArray ack;
        for (const quint16 rateId : rateIds) {
            appendU16(ack, rateId);
        }
        connection.sendSnac(FAM_OSERVICE, OS_RATE_ACK, ack);
    }

    if (bosBootstrap && connection.families.contains(FAM_ICBM)) {
        QByteArray params;
        appendU16(params, ICBM_CHANNEL_IM);
        appendU32(params, 3);
        appendU16(params, 8000);
        appendU16(params, 999);
        appendU16(params, 999);
        appendU32(params, 0);
        connection.sendSnac(FAM_ICBM, ICBM_ADD_PARAMS, params);

        // Channel 2 is OSCAR's rendezvous channel (Direct IM, file transfer,
        // voice and other peer services).  Register it separately so BOS can
        // deliver WaffleHouse voice invitations alongside normal channel-1 IMs.
        QByteArray rendezvousParams;
        appendU16(rendezvousParams, ICBM_CHANNEL_RENDEZVOUS);
        appendU32(rendezvousParams, 3);
        appendU16(rendezvousParams, 8000);
        appendU16(rendezvousParams, 999);
        appendU16(rendezvousParams, 999);
        appendU32(rendezvousParams, 0);
        connection.sendSnac(FAM_ICBM, ICBM_ADD_PARAMS, rendezvousParams);
    }

    const auto classicToolInfo = [](quint16 family) -> QPair<quint16, quint16> {
        switch (family) {
        case FAM_ADMIN:
        case FAM_CHATNAV:
        case FAM_CHAT:
        case FAM_ODIR:
        case FAM_BART:
        case FAM_ALERT:
            return {0x0010, 0x0629};
        case FAM_POPUP:
        case FAM_STATS:
        case FAM_TRANSLATE:
            return {0x0104, 0x0001};
        default:
            return {0x0110, 0x0629};
        }
    };

    QByteArray online;
    for (const quint16 family : negotiatedFamilies) {
        appendU16(online, family);
        appendU16(online, familyVersion(family));
        if (ninaNetwork) {
            const auto tool = classicToolInfo(family);
            appendU16(online, tool.first);
            appendU16(online, tool.second);
        } else {
            appendU16(online, 0x0110);
            appendU16(online, 0x0001);
        }
    }
    connection.sendSnac(FAM_OSERVICE, OS_CLIENT_ONLINE, online);
    if (audit) protocolLog(QStringLiteral("[oscar-login] %1 CLIENT_ONLINE sent; OSCAR service bootstrap complete")
                               .arg(connection.label()));
}

Snac OscarBackend::request(FlapConnection &connection,
                           quint16 family,
                           quint16 subtype,
                           const QByteArray &body,
                           int timeoutMs)
{
    const quint32 requestId = connection.sendSnac(family, subtype, body);
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        const int remaining = std::max(1, timeoutMs - static_cast<int>(timer.elapsed()));
        const Snac snac = connection.receiveSnac(remaining);
        if (snac.requestId == requestId) {
            return snac;
        }
        dispatchSnac(connection, snac);
    }

    fail(QStringLiteral("%1: timed out waiting for reply to %2/%3")
             .arg(connection.label())
             .arg(family, 4, 16, QLatin1Char('0'))
             .arg(subtype, 4, 16, QLatin1Char('0')));
}

OscarBackend::ServiceRedirect OscarBackend::requestService(
    quint16 family,
    const QList<Tlv> &extraTlvs)
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    QByteArray body;
    appendU16(body, family);
    for (const auto &item : extraTlvs) {
        body += tlv(item.type, item.value);
    }

    const Snac snac = request(*m_bos, FAM_OSERVICE, OS_SERVICE_REQUEST, body);
    if (snac.family == FAM_OSERVICE && snac.subtype == 0x0001) {
        const int code = snac.body.size() >= 2 ? readU16(snac.body, 0) : -1;
        fail(QStringLiteral("service request failed, OSCAR error 0x%1")
                 .arg(code, 4, 16, QLatin1Char('0')));
    }
    if (snac.family != FAM_OSERVICE || snac.subtype != OS_SERVICE_RESPONSE) {
        fail(QStringLiteral("unexpected service reply %1/%2")
                 .arg(snac.family, 4, 16, QLatin1Char('0'))
                 .arg(snac.subtype, 4, 16, QLatin1Char('0')));
    }

    qsizetype offset = 0;
    const auto items = parseTlvs(snac.body, offset);
    const QByteArray hostRaw = firstTlv(items, TLV_RECONNECT_HOST);
    const QByteArray cookie = firstTlv(items, TLV_AUTH_COOKIE);
    if (hostRaw.isEmpty() || cookie.isEmpty()) {
        fail(QStringLiteral("service redirect missing host or cookie"));
    }

    const auto endpoint = serviceRedirectEndpoint(QString::fromUtf8(hostRaw), 5190);
    return {endpoint.first, endpoint.second, cookie};
}

FlapConnection &OscarBackend::ensureChatNav()
{
    if (m_chatNav && m_chatNav->isConnected()) {
        return *m_chatNav;
    }

    const ServiceRedirect redirect = requestService(FAM_CHATNAV);
    m_chatNav = std::make_unique<FlapConnection>(
        redirect.host,
        redirect.port,
        QStringLiteral("ChatNav"),
        oscarWireTraceEnabled(m_settings),
        [this](const QString &s) { protocolLog(s); });
    bootstrapService(*m_chatNav, redirect.cookie);
    return *m_chatNav;
}

void OscarBackend::doSendIm(const QString &recipient, const QString &message, bool echo)
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    const QByteArray name = recipient.toUtf8();
    if (name.size() > 255) {
        fail(QStringLiteral("recipient screen name is too long"));
    }

    QByteArray body;
    const quint64 cookie = (static_cast<quint64>(QRandomGenerator::global()->generate64()));
    appendU64(body, cookie);
    appendU16(body, ICBM_CHANNEL_IM);
    body += lp8(name);
    body += tlv(ICBM_TLV_IM_DATA, marshalIcbmFragments(message));
    body += tlv(ICBM_TLV_ACK, QByteArray());
    m_bos->sendSnac(FAM_ICBM, ICBM_MSG_TO_HOST, body);
    if (echo) {
        emit eventReceived(QStringLiteral("im"), recipient,
                           QStringLiteral("<%1> %2").arg(m_settings.username, message));
    }
}

void OscarBackend::doJoinRoom(const QString &name, bool privateRoom)
{
    FlapConnection &nav = ensureChatNav();
    const quint16 exchange = privateRoom ? PRIVATE_EXCHANGE : PUBLIC_EXCHANGE;

    QByteArray roomRequest;
    appendU16(roomRequest, exchange);
    roomRequest += lp8(QStringLiteral("create"));
    appendU16(roomRequest, 0);
    appendU8(roomRequest, 2);
    roomRequest += tlvBlock({Tlv{CHAT_ROOM_TLV_NAME, name.toUtf8()}});

    const Snac reply = request(nav, FAM_CHATNAV, CHATNAV_CREATE_ROOM, roomRequest);
    if (reply.family == FAM_CHATNAV && reply.subtype == 0x0001) {
        const int code = reply.body.size() >= 2 ? readU16(reply.body, 0) : -1;
        fail(QStringLiteral("ChatNav could not resolve '%1', OSCAR error 0x%2")
                 .arg(name)
                 .arg(code, 4, 16, QLatin1Char('0')));
    }
    if (reply.family != FAM_CHATNAV || reply.subtype != CHATNAV_NAV_INFO) {
        fail(QStringLiteral("unexpected ChatNav reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }

    qsizetype offset = 0;
    const auto items = parseTlvs(reply.body, offset);
    const QByteArray roomBlob = firstTlv(items, 0x0004);
    if (roomBlob.isEmpty()) {
        fail(QStringLiteral("ChatNav response contains no room-info TLV"));
    }

    qsizetype roomOffset = 0;
    const RoomInfo room = parseRoomInfo(roomBlob, roomOffset, true);

    QByteArray shortRoom;
    appendU16(shortRoom, room.exchange);
    shortRoom += lp8(room.cookie);
    appendU16(shortRoom, room.instance);

    const ServiceRedirect redirect = requestService(
        FAM_CHAT,
        {Tlv{0x0001, shortRoom}});

    auto connection = std::make_unique<FlapConnection>(
        redirect.host,
        redirect.port,
        QStringLiteral("chat:%1").arg(room.name()),
        oscarWireTraceEnabled(m_settings),
        [this](const QString &s) { protocolLog(s); });
    bootstrapService(*connection, redirect.cookie);

    const QString key = room.name().toCaseFolded();
    auto session = std::make_shared<ChatSession>();
    session->room = room;
    session->connection = std::move(connection);
    m_chats.insert(key, std::move(session));

    emit eventReceived(
        QStringLiteral("chat"),
        room.name(),
        QStringLiteral("*** joined '%1' (exchange=%2, cookie=%3)")
            .arg(room.name())
            .arg(room.exchange)
            .arg(room.cookie));
}

void OscarBackend::doSendRoom(const QString &name, const QString &message)
{
    const QString key = name.toCaseFolded();
    auto it = m_chats.find(key);
    if (it == m_chats.end() || !it.value()) {
        fail(QStringLiteral("not joined to chat '%1'").arg(name));
    }

    const QString rendered = QStringLiteral(
        "<HTML><BODY BGCOLOR=\"#ffffff\"><FONT LANG=\"0\">%1</FONT></BODY></HTML>")
                                 .arg(message.toHtmlEscaped());
    const QByteArray encoding = message.toLatin1() == message.toUtf8()
                              ? QByteArray("us-ascii")
                              : QByteArray("utf-8");

    QByteArray messageInfo;
    messageInfo += tlv(CHAT_MSG_TLV_ENCODING, encoding);
    messageInfo += tlv(CHAT_MSG_TLV_LANG, QByteArray("en"));
    messageInfo += tlv(CHAT_MSG_TLV_TEXT, rendered.toUtf8());

    QByteArray body;
    appendU64(body, QRandomGenerator::global()->generate64());
    appendU16(body, 3);
    body += tlv(CHAT_TLV_PUBLIC, QByteArray());
    body += tlv(CHAT_TLV_REFLECT, QByteArray());
    body += tlv(CHAT_TLV_MESSAGE_INFO, messageInfo);

    it.value()->connection->sendSnac(FAM_CHAT, CHAT_MSG_TO_HOST, body);
}

void OscarBackend::doLeaveRoom(const QString &name)
{
    const QString key = name.toCaseFolded();
    auto it = m_chats.find(key);
    if (it == m_chats.end()) {
        return;
    }
    if (it.value() && it.value()->connection) {
        it.value()->connection->close();
    }
    m_chats.erase(it);
    emit membersChanged(name, QStringLiteral("replace"), {});
}

void OscarBackend::doChangePassword(const QString &currentPassword,
                                    const QString &newPassword)
{
    if (currentPassword.isEmpty() || newPassword.isEmpty()) {
        fail(QStringLiteral("passwords cannot be empty"));
    }

    const ServiceRedirect redirect = requestService(FAM_ADMIN);
    FlapConnection admin(redirect.host,
                         redirect.port,
                         QStringLiteral("Admin"),
                         oscarWireTraceEnabled(m_settings),
                         [this](const QString &s) { protocolLog(s); });
    bootstrapService(admin, redirect.cookie);

    QByteArray body;
    body += tlv(ADMIN_TLV_NEW_PASSWORD, newPassword);
    body += tlv(ADMIN_TLV_OLD_PASSWORD, currentPassword);

    const Snac reply = request(admin, FAM_ADMIN, ADMIN_INFO_CHANGE_REQUEST, body);
    if (reply.family == FAM_ADMIN && reply.subtype == 0x0001) {
        const int code = reply.body.size() >= 2 ? readU16(reply.body, 0) : -1;
        fail(QStringLiteral("Admin service error 0x%1")
                 .arg(code, 4, 16, QLatin1Char('0')));
    }
    if (reply.family != FAM_ADMIN || reply.subtype != ADMIN_INFO_CHANGE_REPLY) {
        fail(QStringLiteral("unexpected Admin reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }
    if (reply.body.size() < 4) {
        fail(QStringLiteral("truncated Admin password-change reply"));
    }

    qsizetype offset = 4;
    const quint16 count = readU16(reply.body, 2);
    const auto items = parseTlvs(reply.body, offset, static_cast<int>(count));
    const QByteArray error = firstTlv(items, ADMIN_TLV_ERROR_CODE);
    if (!error.isEmpty()) {
        const int code = error.size() >= 2 ? readU16(error, 0) : -1;
        fail(QStringLiteral("password change failed: Admin error 0x%1")
                 .arg(code, 4, 16, QLatin1Char('0')));
    }

    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[account] password changed successfully"));
    admin.close();
}

void OscarBackend::doRaw(const QString &familyText,
                         const QString &subtypeText,
                         const QString &hexBody)
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    bool familyOk = false;
    bool subtypeOk = false;
    const quint16 family = familyText.toUShort(&familyOk, 0);
    const quint16 subtype = subtypeText.toUShort(&subtypeOk, 0);
    if (!familyOk || !subtypeOk) {
        fail(QStringLiteral("family and subtype must be numeric, e.g. 0x01 and 0x16"));
    }

    QString compact = hexBody;
    compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (compact.size() % 2 != 0
        || compact.contains(QRegularExpression(QStringLiteral("[^0-9A-Fa-f]")))) {
        fail(QStringLiteral("raw SNAC body must be hexadecimal bytes"));
    }
    const QByteArray body = compact.isEmpty() ? QByteArray() : QByteArray::fromHex(compact.toLatin1());
    const quint32 requestId = m_bos->sendSnac(family, subtype, body);
    emit eventReceived(
        QStringLiteral("status"), QString(),
        QStringLiteral("[raw] sent %1/%2 req=%3")
            .arg(family, 4, 16, QLatin1Char('0'))
            .arg(subtype, 4, 16, QLatin1Char('0'))
            .arg(requestId));
}


QList<OscarBackend::FeedbagItem> OscarBackend::parseFeedbagItems(const QByteArray &body) const
{
    QList<FeedbagItem> items;
    if (body.size() < 3) {
        return items;
    }

    qsizetype offset = 0;
    offset += 1; // feedbag version
    const quint16 count = readU16(body, offset);
    offset += 2;

    for (quint16 i = 0; i < count; ++i) {
        if (offset + 2 > body.size()) {
            break;
        }
        const quint16 nameLength = readU16(body, offset);
        offset += 2;
        if (offset + nameLength + 8 > body.size()) {
            break;
        }

        FeedbagItem item;
        item.name = QString::fromUtf8(body.mid(offset, nameLength));
        offset += nameLength;
        item.groupId = readU16(body, offset);
        offset += 2;
        item.itemId = readU16(body, offset);
        offset += 2;
        item.classId = readU16(body, offset);
        offset += 2;

        if (offset + 2 > body.size()) {
            break;
        }
        const quint16 tlvBytes = readU16(body, offset);
        offset += 2;
        if (offset + tlvBytes > body.size()) {
            break;
        }
        item.data = body.mid(offset, tlvBytes);
        offset += tlvBytes;
        items.push_back(item);
    }

    return items;
}

QStringList OscarBackend::feedbagBuddyNames() const
{
    QStringList names;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_BUDDY && !item.name.isEmpty()) {
            names.push_back(item.name);
        }
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QByteArray OscarBackend::marshalFeedbagItem(const FeedbagItem &item) const
{
    QByteArray out;
    const QByteArray name = item.name.toUtf8();
    appendU16(out, static_cast<quint16>(name.size()));
    out += name;
    appendU16(out, item.groupId);
    appendU16(out, item.itemId);
    appendU16(out, item.classId);
    appendU16(out, static_cast<quint16>(item.data.size()));
    out += item.data;
    return out;
}

QByteArray OscarBackend::marshalFeedbagItems(const QList<FeedbagItem> &items) const
{
    QByteArray out;
    for (const FeedbagItem &item : items) {
        out += marshalFeedbagItem(item);
    }
    return out;
}

QByteArray OscarBackend::withGroupMembers(const QByteArray &data, const QList<quint16> &itemIds) const
{
    QList<Tlv> tlvs;
    if (!data.isEmpty()) {
        qsizetype offset = 0;
        try {
            tlvs = parseTlvs(data, offset);
        } catch (const std::exception &) {
            // Preserve operation even if an unusual group TLV block cannot be parsed.
            // The member-list TLV is the only value the client needs to replace here.
            tlvs.clear();
        }
    }

    QList<Tlv> rebuilt;
    for (const Tlv &entry : tlvs) {
        if (entry.type != FEEDBAG_TLV_GROUP_MEMBERS) {
            rebuilt.push_back(entry);
        }
    }

    if (!itemIds.isEmpty()) {
        QByteArray members;
        for (const quint16 id : itemIds) {
            appendU16(members, id);
        }
        rebuilt.push_back(Tlv{FEEDBAG_TLV_GROUP_MEMBERS, members});
    }

    QByteArray out;
    for (const Tlv &entry : rebuilt) {
        out += tlv(entry.type, entry.value);
    }
    return out;
}

quint16 OscarBackend::nextFeedbagGroupId() const
{
    QSet<quint16> used;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_GROUP) {
            used.insert(item.groupId);
        }
    }
    for (quint32 candidate = 1; candidate < 0xFFFF; ++candidate) {
        if (!used.contains(static_cast<quint16>(candidate))) {
            return static_cast<quint16>(candidate);
        }
    }
    fail(QStringLiteral("no free Feedbag group IDs remain"));
}

quint16 OscarBackend::nextFeedbagItemId(quint16 groupId) const
{
    QSet<quint16> used;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.groupId == groupId) {
            used.insert(item.itemId);
        }
    }
    for (quint32 candidate = 1; candidate < 0xFFFF; ++candidate) {
        if (!used.contains(static_cast<quint16>(candidate))) {
            return static_cast<quint16>(candidate);
        }
    }
    fail(QStringLiteral("no free Feedbag item IDs remain"));
}

void OscarBackend::checkFeedbagAck(const Snac &reply,
                                   int expectedItems,
                                   const QString &operation) const
{
    if (reply.family != FAM_FEEDBAG || reply.subtype != FEEDBAG_STATUS) {
        fail(QStringLiteral("%1: unexpected Feedbag reply %2/%3")
                 .arg(operation)
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }
    if (reply.body.size() < expectedItems * 2) {
        fail(QStringLiteral("%1: truncated Feedbag acknowledgement").arg(operation));
    }
    for (int i = 0; i < expectedItems; ++i) {
        const quint16 result = readU16(reply.body, i * 2);
        if (result != 0) {
            fail(QStringLiteral("%1: server rejected item %2 with SSI status 0x%3")
                     .arg(operation)
                     .arg(i + 1)
                     .arg(result, 4, 16, QLatin1Char('0')));
        }
    }
}

void OscarBackend::persistFeedbagChanges(const QList<FeedbagItem> &adds,
                                         const QList<FeedbagItem> &mods,
                                         const QList<FeedbagItem> &dels)
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG)) {
        fail(QStringLiteral("server does not advertise persistent Feedbag/SSI support"));
    }

    m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_EDIT_START, QByteArray());
    try {
        if (!dels.isEmpty()) {
            const Snac ack = request(*m_bos, FAM_FEEDBAG, FEEDBAG_DELETE,
                                     marshalFeedbagItems(dels), 5000);
            checkFeedbagAck(ack, dels.size(), QStringLiteral("buddy delete"));
        }
        if (!adds.isEmpty()) {
            const Snac ack = request(*m_bos, FAM_FEEDBAG, FEEDBAG_ADD,
                                     marshalFeedbagItems(adds), 5000);
            checkFeedbagAck(ack, adds.size(), QStringLiteral("buddy add"));
        }
        if (!mods.isEmpty()) {
            const Snac ack = request(*m_bos, FAM_FEEDBAG, FEEDBAG_MODIFY,
                                     marshalFeedbagItems(mods), 5000);
            checkFeedbagAck(ack, mods.size(), QStringLiteral("group update"));
        }
        m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_EDIT_END, QByteArray());
    } catch (...) {
        try {
            m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_EDIT_END, QByteArray());
        } catch (...) {
        }
        throw;
    }
}

void OscarBackend::loadBuddyList()
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG)) {
        protocolLog(QStringLiteral("[buddy] server did not advertise Feedbag/SSI"));
        return;
    }

    try {
        const Snac reply = request(*m_bos, FAM_FEEDBAG, FEEDBAG_QUERY, QByteArray(), 5000);
        if (reply.family != FAM_FEEDBAG || reply.subtype != FEEDBAG_REPLY) {
            protocolLog(QStringLiteral("[buddy] unexpected feedbag reply %1/%2")
                            .arg(reply.family, 4, 16, QLatin1Char('0'))
                            .arg(reply.subtype, 4, 16, QLatin1Char('0')));
            return;
        }

        m_feedbagItems = parseFeedbagItems(reply.body);
        const QStringList names = feedbagBuddyNames();
        m_buddies.clear();
        for (const QString &name : names) {
            m_buddies.insert(name);
        }
        emit buddyListChanged(names);

        // Activate SSI/Feedbag for presence notifications.
        m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_USE, QByteArray());
        protocolLog(QStringLiteral("[buddy] loaded %1 persistent buddies").arg(names.size()));
    } catch (const std::exception &e) {
        protocolLog(QStringLiteral("[buddy] could not load feedbag: %1")
                        .arg(QString::fromUtf8(e.what())));
    }
}

void OscarBackend::doAddBuddy(const QString &name)
{
    if (!m_bos || name.trimmed().isEmpty()) {
        return;
    }
    const QString cleaned = name.trimmed();

    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_BUDDY
            && item.name.compare(cleaned, Qt::CaseInsensitive) == 0) {
            emit eventReceived(QStringLiteral("status"), QString(),
                               QStringLiteral("[buddy] %1 is already in the persistent buddy list").arg(cleaned));
            return;
        }
    }

    if (!m_bos->families.contains(FAM_FEEDBAG)) {
        // Legacy fallback: useful for presence during this session, but not persistent.
        m_bos->sendSnac(FAM_BUDDY, BUDDY_ADD, lp8(cleaned));
        m_buddies.insert(cleaned);
        QStringList names = m_buddies.values();
        names.sort(Qt::CaseInsensitive);
        emit buddyListChanged(names);
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[buddy] server lacks SSI; added %1 for this session only").arg(cleaned));
        return;
    }

    QList<FeedbagItem> adds;
    QList<FeedbagItem> mods;

    int groupIndex = -1;
    for (int i = 0; i < m_feedbagItems.size(); ++i) {
        const FeedbagItem &item = m_feedbagItems.at(i);
        if (item.classId == FEEDBAG_CLASS_GROUP && item.groupId != 0 && item.itemId == 0) {
            groupIndex = i;
            break;
        }
    }

    quint16 groupId = 0;
    if (groupIndex >= 0) {
        groupId = m_feedbagItems.at(groupIndex).groupId;
    } else {
        groupId = nextFeedbagGroupId();
    }

    FeedbagItem buddy;
    buddy.name = cleaned;
    buddy.groupId = groupId;
    buddy.itemId = nextFeedbagItemId(groupId);
    buddy.classId = FEEDBAG_CLASS_BUDDY;

    if (groupIndex >= 0) {
        FeedbagItem group = m_feedbagItems.at(groupIndex);
        QList<quint16> memberIds;
        for (const FeedbagItem &item : m_feedbagItems) {
            if (item.classId == FEEDBAG_CLASS_BUDDY && item.groupId == groupId) {
                memberIds.push_back(item.itemId);
            }
        }
        memberIds.push_back(buddy.itemId);
        group.data = withGroupMembers(group.data, memberIds);
        adds.push_back(buddy);
        mods.push_back(group);
    } else {
        FeedbagItem group;
        group.name = QStringLiteral("Buddies");
        group.groupId = groupId;
        group.itemId = 0;
        group.classId = FEEDBAG_CLASS_GROUP;
        group.data = withGroupMembers(QByteArray(), {buddy.itemId});

        int masterIndex = -1;
        for (int i = 0; i < m_feedbagItems.size(); ++i) {
            const FeedbagItem &item = m_feedbagItems.at(i);
            if (item.classId == FEEDBAG_CLASS_GROUP && item.groupId == 0 && item.itemId == 0) {
                masterIndex = i;
                break;
            }
        }

        if (masterIndex >= 0) {
            FeedbagItem master = m_feedbagItems.at(masterIndex);
            QList<quint16> groupIds;
            for (const FeedbagItem &item : m_feedbagItems) {
                if (item.classId == FEEDBAG_CLASS_GROUP && item.groupId != 0 && item.itemId == 0) {
                    groupIds.push_back(item.groupId);
                }
            }
            groupIds.push_back(groupId);
            master.data = withGroupMembers(master.data, groupIds);
            mods.push_back(master);
        } else {
            FeedbagItem master;
            master.groupId = 0;
            master.itemId = 0;
            master.classId = FEEDBAG_CLASS_GROUP;
            master.data = withGroupMembers(QByteArray(), {groupId});
            adds.push_back(master);
        }

        adds.push_back(group);
        adds.push_back(buddy);
    }

    persistFeedbagChanges(adds, mods, {});
    loadBuddyList();
    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[buddy] persistently added %1").arg(cleaned));
}

void OscarBackend::doRemoveBuddy(const QString &name)
{
    if (!m_bos || name.trimmed().isEmpty()) {
        return;
    }
    const QString cleaned = name.trimmed();

    if (!m_bos->families.contains(FAM_FEEDBAG)) {
        m_bos->sendSnac(FAM_BUDDY, BUDDY_REMOVE, lp8(cleaned));
        for (auto it = m_buddies.begin(); it != m_buddies.end();) {
            if (it->compare(cleaned, Qt::CaseInsensitive) == 0) {
                it = m_buddies.erase(it);
            } else {
                ++it;
            }
        }
        QStringList names = m_buddies.values();
        names.sort(Qt::CaseInsensitive);
        emit buddyListChanged(names);
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[buddy] server lacks SSI; removed %1 for this session only").arg(cleaned));
        return;
    }

    QList<FeedbagItem> dels;
    QSet<quint16> affectedGroups;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_BUDDY
            && item.name.compare(cleaned, Qt::CaseInsensitive) == 0) {
            dels.push_back(item);
            affectedGroups.insert(item.groupId);
        }
    }

    if (dels.isEmpty()) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[buddy] %1 is not in the persistent buddy list").arg(cleaned));
        return;
    }

    QList<FeedbagItem> mods;
    for (const quint16 groupId : affectedGroups) {
        for (const FeedbagItem &item : m_feedbagItems) {
            if (item.classId != FEEDBAG_CLASS_GROUP
                || item.groupId != groupId || item.itemId != 0) {
                continue;
            }
            FeedbagItem group = item;
            QList<quint16> remaining;
            for (const FeedbagItem &candidate : m_feedbagItems) {
                if (candidate.classId != FEEDBAG_CLASS_BUDDY
                    || candidate.groupId != groupId) {
                    continue;
                }
                if (candidate.name.compare(cleaned, Qt::CaseInsensitive) != 0) {
                    remaining.push_back(candidate.itemId);
                }
            }
            group.data = withGroupMembers(group.data, remaining);
            mods.push_back(group);
            break;
        }
    }

    persistFeedbagChanges({}, mods, dels);
    loadBuddyList();
    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[buddy] persistently removed %1").arg(cleaned));
}

void OscarBackend::emitPresence()
{
    emit presenceChanged(m_presenceState, m_presenceMessage, m_idleSeconds);
    QString summary = m_presenceState;
    if (m_idleSeconds > 0) {
        summary += QStringLiteral(" + IDLE %1s").arg(m_idleSeconds);
    }
    if (!m_presenceMessage.isEmpty()) {
        summary += QStringLiteral(" — %1").arg(m_presenceMessage);
    }
    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[presence] %1").arg(summary));
}

void OscarBackend::doSetAway(const QString &message, bool afk)
{
    if (!m_bos) fail(QStringLiteral("not logged in"));

    QString clean = message.trimmed();
    if (clean.isEmpty()) clean = afk ? QStringLiteral("AFK") : QStringLiteral("Away");
    QString wireText = afk && !clean.startsWith(QStringLiteral("[AFK]"), Qt::CaseInsensitive)
        ? QStringLiteral("[AFK] %1").arg(clean)
        : clean;
    const QString html = QStringLiteral("<HTML><BODY>%1</BODY></HTML>")
                             .arg(wireText.toHtmlEscaped());

    QByteArray body;
    body += tlv(LOCATE_TLV_UNAVAILABLE_TYPE,
                QByteArray("text/x-aolrtf; charset=\"us-ascii\""));
    body += tlv(LOCATE_TLV_UNAVAILABLE_DATA, html.toLatin1());
    m_bos->sendSnac(FAM_LOCATE, LOCATE_SET_INFO, body);

    m_presenceState = afk ? QStringLiteral("AFK") : QStringLiteral("AWAY");
    m_presenceMessage = clean;
    emitPresence();
}

void OscarBackend::doSetIdle(quint32 seconds)
{
    if (!m_bos) fail(QStringLiteral("not logged in"));
    if (!m_bos->families.contains(FAM_OSERVICE))
        fail(QStringLiteral("OSCAR Generic Service family is unavailable; cannot advertise native idle state"));

    // Native AIM/OSCAR idle state: OSERVICE__IDLE_NOTIFICATION (SNAC 0x0001/0x0011)
    // carries one uint32 containing seconds since the user's last activity. A zero
    // value means active. The server increments non-zero values after receipt.
    QByteArray body;
    appendU32(body, seconds);
    if (oscarLoginAuditEnabled(m_settings)) {
        protocolLog(QStringLiteral("[oscar-idle] native SNAC 0x0001/0x0011 -> idleSeconds=%1 (%2)")
                        .arg(seconds)
                        .arg(seconds == 0 ? QStringLiteral("ACTIVE") : QStringLiteral("IDLE")));
    }
    m_bos->sendSnac(FAM_OSERVICE, OS_IDLE_NOTIFICATION, body);
    m_idleSeconds = seconds;
    emitPresence();
}

void OscarBackend::doSetBack()
{
    if (!m_bos) fail(QStringLiteral("not logged in"));

    // Empty UNAVAILABLE tags clear the classic AIM away message.
    QByteArray locateBody;
    locateBody += tlv(LOCATE_TLV_UNAVAILABLE_TYPE, QByteArray());
    locateBody += tlv(LOCATE_TLV_UNAVAILABLE_DATA, QByteArray());
    m_bos->sendSnac(FAM_LOCATE, LOCATE_SET_INFO, locateBody);

    QByteArray idleBody;
    appendU32(idleBody, 0);
    if (oscarLoginAuditEnabled(m_settings))
        protocolLog(QStringLiteral("[oscar-idle] native SNAC 0x0001/0x0011 -> idleSeconds=0 (ACTIVE)"));
    m_bos->sendSnac(FAM_OSERVICE, OS_IDLE_NOTIFICATION, idleBody);

    m_presenceState = QStringLiteral("ONLINE");
    m_presenceMessage.clear();
    m_idleSeconds = 0;
    emitPresence();
}

void OscarBackend::discoverBosCapabilities()
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    QStringList features;
    QStringList familyIds;
    const QList<quint16> families = m_bos->families;
    {
        QMutexLocker locker(&m_capabilityMutex);
        m_serverFamilies.clear();
        for (const quint16 family : families) m_serverFamilies.insert(family);
    }
    for (const quint16 family : families) {
        const QString name = oscarFamilyName(family);
        familyIds.append(QStringLiteral("0x%1 — %2")
                             .arg(family, 4, 16, QLatin1Char('0'))
                             .arg(name));
        features.append(name);
    }

    // Ask the Buddy foodgroup for complete initial arrival/departure state.
    // Do not claim BART/icon support here; WaffleHouse only requests the native
    // INITIAL_DEPARTS behavior it actually consumes.
    if (families.contains(FAM_BUDDY)) {
        QByteArray flags;
        appendU16(flags, BUDDY_RIGHTS_FLAG_INITIAL_DEPARTS);
        // Fire-and-forget: older/private BOS implementations sometimes advertise
        // Buddy but never answer the rights query. Do not add a login timeout for
        // an optional capability hint; any reply is harmlessly processed later.
        m_bos->sendSnac(FAM_BUDDY, BUDDY_RIGHTS_QUERY,
                        tlv(BUDDY_RIGHTS_TLV_FLAGS, flags));
    }

    // Ask for the server-authoritative copy of our own online USERINFO.  The
    // response is asynchronous OSERVICE 01/0F and is consumed by dispatchBos;
    // private servers that ignore the optional query cannot stall login.
    if (families.contains(FAM_OSERVICE)) {
        m_bos->sendSnac(FAM_OSERVICE, OS_USER_INFO_QUERY, QByteArray());
    }

    const bool profileSupported = families.contains(FAM_LOCATE);
    m_maxProfileLength = 0;
    if (profileSupported) {
        try {
            const Snac rights = request(*m_bos, FAM_LOCATE, LOCATE_RIGHTS_QUERY, QByteArray(), 5000);
            if (rights.family == FAM_LOCATE && rights.subtype == LOCATE_RIGHTS_REPLY) {
                qsizetype offset = 0;
                const QList<Tlv> rightsTlvs = parseTlvs(rights.body, offset);
                const QByteArray maxSig = firstTlv(rightsTlvs, 0x0001);
                if (maxSig.size() >= 2) {
                    m_maxProfileLength = readU16(maxSig, 0);
                }
            } else if (rights.family == FAM_LOCATE && rights.subtype == 0x0001) {
                protocolLog(QStringLiteral("[OSCAR capabilities] Locate family advertised, but rights query returned an error."));
            }
        } catch (const std::exception &e) {
            // Capability inspection must never tear down an otherwise healthy login.
            protocolLog(QStringLiteral("[OSCAR capabilities] Locate rights query unavailable: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
    }

    features.removeDuplicates();
    if (profileSupported) {
        try {
            advertiseClientCapabilities();
        } catch (const std::exception &e) {
            protocolLog(QStringLiteral("[OSCAR capabilities] Could not advertise WaffleHouse user capabilities: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
    }
    emit serverCapabilitiesChanged(features, familyIds, profileSupported, m_maxProfileLength);

    if (profileSupported) {
        try {
            if (!m_settings.oscarProfile.isEmpty()) {
                doSetProfile(m_settings.oscarProfile);
                protocolLog(QStringLiteral("[OSCAR profile] Replayed locally saved profile after BOS login (%1 byte(s)).")
                                .arg(m_settings.oscarProfile.toUtf8().size()));
            } else {
                emit profileChanged(fetchOwnProfile());
            }
        } catch (const std::exception &e) {
            protocolLog(QStringLiteral("[OSCAR profile] Could not restore/read current profile: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
    }
}

QString OscarBackend::fetchOwnProfile()
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE)) {
        return {};
    }

    QByteArray body;
    appendU16(body, 0x0001); // classic LOCATE profile query
    body += lp8(m_settings.username);
    const Snac reply = request(*m_bos, FAM_LOCATE, LOCATE_USER_INFO_QUERY, body, 5000);
    if (reply.family == FAM_LOCATE && reply.subtype == 0x0001) {
        fail(QStringLiteral("server rejected AIM profile query"));
    }
    if (reply.family != FAM_LOCATE || reply.subtype != LOCATE_USER_INFO_REPLY) {
        fail(QStringLiteral("unexpected AIM profile reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }

    qsizetype offset = 0;
    (void)parseUserInfo(reply.body, offset);
    const QList<Tlv> profileTlvs = parseTlvs(reply.body, offset);
    return stripAimHtml(QString::fromUtf8(firstTlv(profileTlvs, LOCATE_TLV_PROFILE_DATA)));
}

void OscarBackend::doSetProfile(const QString &profile)
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE)) {
        fail(QStringLiteral("this OSCAR server does not advertise Locate/profile support"));
    }

    const QByteArray profileBytes = profile.toUtf8();
    if (m_maxProfileLength > 0 && profileBytes.size() > m_maxProfileLength) {
        fail(QStringLiteral("AIM profile is %1 bytes; this server allows at most %2 bytes")
                 .arg(profileBytes.size())
                 .arg(m_maxProfileLength));
    }

    QByteArray body;
    body += tlv(LOCATE_TLV_PROFILE_TYPE,
                QByteArrayLiteral("text/x-aolrtf; charset=\"utf-8\""));
    body += tlv(LOCATE_TLV_PROFILE_DATA, profileBytes);
    // Include our client capabilities in the same LOCATE_SET_INFO so profile
    // edits cannot accidentally drop the WaffleHouse voice/UTF-8 advertisement
    // on servers that treat SET_INFO as a replacement record.
    body += tlv(LOCATE_TLV_CAPABILITIES, waffleAdvertisedCapabilities());
    m_bos->sendSnac(FAM_LOCATE, LOCATE_SET_INFO, body);
    emit profileChanged(profile);
    protocolLog(QStringLiteral("[AIM profile] Profile updated (%1 byte(s)).")
                    .arg(profileBytes.size()));
}

void OscarBackend::advertiseClientCapabilities()
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE)) return;

    // WaffleHouse messages are UTF-8 capable; advertise that standard OSCAR
    // capability alongside our namespaced voice service.
    m_bos->sendSnac(FAM_LOCATE,
                    LOCATE_SET_INFO,
                    tlv(LOCATE_TLV_CAPABILITIES, waffleAdvertisedCapabilities()));
}

void OscarBackend::doRequestUserInfo(const QString &target)
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE)) {
        fail(QStringLiteral("this OSCAR server does not advertise Locate/user-info support"));
    }
    const QString clean = target.trimmed();
    if (clean.isEmpty()) fail(QStringLiteral("AIM screen name is empty"));

    Snac reply;
    bool gotReply = false;
    try {
        QByteArray body;
        // Query2 flags: profile/signature + away message + capabilities + HTML info.
        appendU32(body, 0x00000407);
        body += lp8(clean);
        reply = request(*m_bos, FAM_LOCATE, LOCATE_USER_INFO_QUERY2, body, 6000);
        gotReply = reply.family == FAM_LOCATE && reply.subtype == LOCATE_USER_INFO_REPLY;
    } catch (const std::exception &e) {
        protocolLog(QStringLiteral("[AIM user info] Query2 unavailable for %1: %2")
                        .arg(clean, QString::fromUtf8(e.what())));
    }

    if (!gotReply) {
        QByteArray body;
        appendU16(body, 0x0001);
        body += lp8(clean);
        reply = request(*m_bos, FAM_LOCATE, LOCATE_USER_INFO_QUERY, body, 6000);
    }
    if (reply.family == FAM_LOCATE && reply.subtype == 0x0001) {
        fail(QStringLiteral("AIM user-info lookup for '%1' was rejected by the server").arg(clean));
    }
    if (reply.family != FAM_LOCATE || reply.subtype != LOCATE_USER_INFO_REPLY) {
        fail(QStringLiteral("unexpected AIM user-info reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }

    qsizetype offset = 0;
    const UserInfo user = parseUserInfo(reply.body, offset);
    const QList<Tlv> locateTlvs = parseTlvs(reply.body, offset);

    auto generic = [&user](quint16 type) { return firstTlv(user.tlvs, type); };
    auto u16Value = [](const QByteArray &raw) -> int {
        return raw.size() >= 2 ? static_cast<int>(readU16(raw, 0)) : -1;
    };
    auto u32Value = [](const QByteArray &raw) -> qint64 {
        return raw.size() >= 4 ? static_cast<qint64>(readU32(raw, 0)) : -1;
    };

    QVariantMap info;
    info.insert(QStringLiteral("screenName"), user.name.isEmpty() ? clean : user.name);
    info.insert(QStringLiteral("warningRaw"), static_cast<int>(user.warningLevel));
    info.insert(QStringLiteral("warningPercent"), static_cast<double>(user.warningLevel) / 10.0);
    const QByteArray flagsLowRaw = generic(USERINFO_TLV_FLAGS);
    const QByteArray flagsHighRaw = generic(USERINFO_TLV_FLAGS2);
    const int flagsLow = u16Value(flagsLowRaw);
    quint64 userFlags = flagsLow >= 0 ? static_cast<quint64>(flagsLow) : 0;
    if (!flagsHighRaw.isEmpty()) {
        quint64 upper = 0;
        for (const char byte : flagsHighRaw) upper = (upper << 8) | static_cast<quint8>(byte);
        userFlags |= (upper << 16);
    }
    info.insert(QStringLiteral("userFlagsSupplied"), flagsLow >= 0);
    info.insert(QStringLiteral("userFlags"), QVariant::fromValue<qulonglong>(userFlags));
    info.insert(QStringLiteral("signonTime"), u32Value(generic(USERINFO_TLV_SIGNON_TIME)));
    info.insert(QStringLiteral("idleMinutes"), u16Value(generic(USERINFO_TLV_IDLE_TIME)));
    info.insert(QStringLiteral("memberSince"), u32Value(generic(USERINFO_TLV_MEMBER_SINCE)));
    const qint64 statusRaw = u32Value(generic(USERINFO_TLV_STATUS));
    info.insert(QStringLiteral("statusSupplied"), statusRaw >= 0);
    info.insert(QStringLiteral("statusRaw"), statusRaw);
    info.insert(QStringLiteral("onlineSeconds"), u32Value(generic(USERINFO_TLV_ONLINE_TIME)));

    const QString profile = stripAimHtml(QString::fromUtf8(firstTlv(locateTlvs, LOCATE_TLV_PROFILE_DATA)));
    const QString away = stripAimHtml(QString::fromUtf8(firstTlv(locateTlvs, LOCATE_TLV_UNAVAILABLE_DATA)));
    info.insert(QStringLiteral("profile"), profile);
    info.insert(QStringLiteral("awayMessage"), away);
    const bool awayByClass = (userFlags & USER_FLAG_UNAVAILABLE) != 0;
    const quint32 statusBits = statusRaw >= 0 ? static_cast<quint32>(statusRaw) : 0;
    QString nativePresence = QStringLiteral("Online");
    if (statusRaw >= 0 && (statusBits & USER_STATUS_INVISIBLE)) nativePresence = QStringLiteral("Invisible");
    else if (statusRaw >= 0 && (statusBits & USER_STATUS_DND)) nativePresence = QStringLiteral("Do Not Disturb");
    else if (statusRaw >= 0 && (statusBits & USER_STATUS_NA)) nativePresence = QStringLiteral("Not Available");
    else if (statusRaw >= 0 && (statusBits & USER_STATUS_BUSY)) nativePresence = QStringLiteral("Busy");
    else if (!away.trimmed().isEmpty() || awayByClass || (statusRaw >= 0 && (statusBits & USER_STATUS_AWAY))) nativePresence = QStringLiteral("Away");
    else if (statusRaw >= 0 && (statusBits & USER_STATUS_FREE_FOR_CHAT)) nativePresence = QStringLiteral("Free for Chat");
    const int idleMinutes = u16Value(generic(USERINFO_TLV_IDLE_TIME));
    info.insert(QStringLiteral("idleSeconds"), idleMinutes > 0 ? idleMinutes * 60 : 0);
    info.insert(QStringLiteral("presence"), nativePresence);

    QList<QByteArray> caps = splitCapabilities(generic(USERINFO_TLV_CAPABILITIES));
    caps += shortCapabilities(generic(USERINFO_TLV_SHORT_CAPABILITIES));
    caps += splitCapabilities(firstTlv(locateTlvs, LOCATE_TLV_CAPABILITIES));
    const QStringList capDescriptions = describeCapabilities(caps);
    QStringList capHex;
    QStringList standardCaps;
    QStringList legacyCaps;
    QStringList waffleCaps;
    QStringList unknownCaps;
    QSet<QByteArray> uniqueCaps;
    int rawCapabilityEntries = 0;
    bool legacyVoice = false;
    bool waffleVoice = false;
    bool directIm = false;
    bool fileTransfer = false;
    bool buddyIcon = false;
    for (const QByteArray &cap : caps) {
        if (cap.size() != 16) continue;
        ++rawCapabilityEntries;
        const QString hex = QString::fromLatin1(cap.toHex());
        if (!capHex.contains(hex)) capHex.append(hex);
        if (!uniqueCaps.contains(cap)) {
            uniqueCaps.insert(cap);
            const QString entry = QStringLiteral("%1  %2").arg(capabilityName(cap), hex);
            const QString category = capabilityCategory(cap);
            if (category == QStringLiteral("Standard OSCAR")) standardCaps.append(entry);
            else if (category == QStringLiteral("Legacy AIM / rendezvous")) legacyCaps.append(entry);
            else if (category == QStringLiteral("WaffleHouse extensions")) waffleCaps.append(entry);
            else unknownCaps.append(entry);
        }
        legacyVoice = legacyVoice || cap == legacyAimVoiceCapability();
        waffleVoice = waffleVoice || cap == waffleVoiceCapability();
        directIm = directIm || cap == QByteArray::fromHex("094613454c7f11d18222444553540000");
        fileTransfer = fileTransfer
            || cap == QByteArray::fromHex("094613434c7f11d18222444553540000")
            || cap == QByteArray::fromHex("094613484c7f11d18222444553540000");
        buddyIcon = buddyIcon || cap == QByteArray::fromHex("094613464c7f11d18222444553540000");
    }
    standardCaps.sort(Qt::CaseInsensitive);
    legacyCaps.sort(Qt::CaseInsensitive);
    waffleCaps.sort(Qt::CaseInsensitive);
    unknownCaps.sort(Qt::CaseInsensitive);
    info.insert(QStringLiteral("capabilities"), capDescriptions);
    info.insert(QStringLiteral("capabilityHex"), capHex);
    info.insert(QStringLiteral("capabilityCount"), uniqueCaps.size());
    info.insert(QStringLiteral("rawCapabilityEntries"), rawCapabilityEntries);
    info.insert(QStringLiteral("standardCapabilities"), standardCaps);
    info.insert(QStringLiteral("legacyCapabilities"), legacyCaps);
    info.insert(QStringLiteral("waffleCapabilities"), waffleCaps);
    info.insert(QStringLiteral("unknownCapabilities"), unknownCaps);
    info.insert(QStringLiteral("legacyVoice"), legacyVoice);
    info.insert(QStringLiteral("waffleVoice"), waffleVoice);
    info.insert(QStringLiteral("directIm"), directIm);
    info.insert(QStringLiteral("fileTransfer"), fileTransfer);
    info.insert(QStringLiteral("buddyIcon"), buddyIcon);
    info.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    {
        QMutexLocker locker(&m_capabilityMutex);
        m_peerCapabilities.insert(clean.toCaseFolded(), uniqueCaps);
        if (!user.name.isEmpty()) m_peerCapabilities.insert(user.name.toCaseFolded(), uniqueCaps);
    }
    emit userInfoReceived(clean, info);
}

void OscarBackend::doRequestDirectoryInfo(const QString &target)
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE))
        fail(QStringLiteral("this OSCAR server does not advertise Locate/directory support"));
    const QString clean = target.trimmed();
    if (clean.isEmpty()) fail(QStringLiteral("AIM screen name is empty"));

    const Snac reply = request(*m_bos, FAM_LOCATE, LOCATE_GET_DIR_INFO, lp8(clean), 6000);
    if (reply.family == FAM_LOCATE && reply.subtype == 0x0001)
        fail(QStringLiteral("directory lookup for '%1' was rejected by the server").arg(clean));
    if (reply.family != FAM_LOCATE || reply.subtype != LOCATE_GET_DIR_REPLY)
        fail(QStringLiteral("unexpected directory reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));

    qsizetype offset = 0;
    QVariantMap info;
    if (reply.body.size() >= 2) {
        info.insert(QStringLiteral("status"), static_cast<int>(readU16(reply.body, 0)));
        offset = 2;
    }
    const QList<Tlv> items = parseTlvs(reply.body, offset);
    const struct { quint16 id; const char *key; } fields[] = {
        {0x0001, "firstName"}, {0x0002, "lastName"}, {0x0003, "middleName"},
        {0x0004, "maidenName"}, {0x0006, "country"}, {0x0007, "state"},
        {0x0008, "city"}, {0x000C, "nickname"}, {0x000D, "zip"},
        {0x0021, "street"}
    };
    for (const auto &field : fields) {
        const QByteArray raw = firstTlv(items, field.id);
        if (!raw.isEmpty()) info.insert(QString::fromLatin1(field.key), QString::fromUtf8(raw));
    }
    info.insert(QStringLiteral("rawTlvCount"), items.size());
    info.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    emit directoryInfoReceived(clean, info);
}

void OscarBackend::doSetDirectoryInfo(const QVariantMap &fields)
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE))
        fail(QStringLiteral("this OSCAR server does not advertise Locate/directory support"));
    QByteArray body;
    const struct { quint16 id; const char *key; } map[] = {
        {0x0001, "firstName"}, {0x0002, "lastName"}, {0x0003, "middleName"},
        {0x0004, "maidenName"}, {0x0006, "country"}, {0x0007, "state"},
        {0x0008, "city"}, {0x000C, "nickname"}, {0x000D, "zip"},
        {0x0021, "street"}
    };
    for (const auto &field : map) {
        if (!fields.contains(QString::fromLatin1(field.key))) continue;
        body += tlv(field.id, fields.value(QString::fromLatin1(field.key)).toString().trimmed());
    }
    const Snac reply = request(*m_bos, FAM_LOCATE, LOCATE_SET_DIR_INFO, body, 6000);
    const bool ok = reply.family == FAM_LOCATE && reply.subtype == LOCATE_SET_DIR_REPLY
                    && (reply.body.size() < 2 || readU16(reply.body, 0) == 0);
    emit featureOperationResult(QStringLiteral("Set AIM directory information"), ok,
                                ok ? QStringLiteral("Directory information updated.")
                                   : QStringLiteral("The server rejected the directory update."));
    if (!ok) fail(QStringLiteral("AIM directory update failed"));
}

void OscarBackend::doFindByEmail(const QString &email)
{
    if (!m_bos || !m_bos->families.contains(FAM_LOCATE))
        fail(QStringLiteral("this OSCAR server does not advertise Locate/email-search support"));
    const QByteArray address = email.trimmed().toUtf8();
    if (address.isEmpty()) fail(QStringLiteral("email address is empty"));
    QByteArray body;
    appendU16(body, static_cast<quint16>(std::min<qsizetype>(address.size(), 0xffff)));
    body += address.left(0xffff);
    const Snac reply = request(*m_bos, FAM_LOCATE, LOCATE_FIND_LIST_BY_EMAIL, body, 6000);
    if (reply.family == FAM_LOCATE && reply.subtype == 0x0001)
        fail(QStringLiteral("AIM email lookup was rejected by the server"));
    if (reply.family != FAM_LOCATE || reply.subtype != LOCATE_FIND_LIST_REPLY)
        fail(QStringLiteral("unexpected AIM email lookup reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));

    QStringList names;
    qsizetype offset = 0;
    while (offset < reply.body.size()) {
        const qsizetype before = offset;
        try {
            const UserInfo item = parseUserInfo(reply.body, offset);
            if (!item.name.trimmed().isEmpty()) names.append(item.name.trimmed());
        } catch (...) {
            offset = before;
            break;
        }
        if (offset <= before) break;
    }
    names.removeDuplicates();
    emit lookupResultsReceived(email.trimmed(), names);
}

void OscarBackend::doInviteByEmail(const QString &email, const QString &message)
{
    if (!m_bos || !m_bos->families.contains(FAM_INVITE))
        fail(QStringLiteral("this OSCAR server does not advertise AIM invitation support"));
    QByteArray body;
    body += tlv(INVITE_TLV_EMAIL, email.trimmed());
    body += tlv(INVITE_TLV_PERSONAL_TEXT, message);
    const Snac reply = request(*m_bos, FAM_INVITE, INVITE_REQUEST_QUERY, body, 6000);
    const bool ok = reply.family == FAM_INVITE && reply.subtype == INVITE_REQUEST_REPLY;
    emit featureOperationResult(QStringLiteral("AIM service invitation"), ok,
                                ok ? QStringLiteral("Invitation submitted to the OSCAR server.")
                                   : QStringLiteral("The OSCAR server rejected the invitation."));
    if (!ok) fail(QStringLiteral("AIM invitation failed"));
}

void OscarBackend::doPrivacyListAction(quint16 subtype, const QString &target, const QString &label)
{
    if (!m_bos || !m_bos->families.contains(FAM_PERMIT_DENY))
        fail(QStringLiteral("this OSCAR server does not advertise Permit/Deny privacy support"));
    const QString clean = target.trimmed();
    if (clean.isEmpty()) fail(QStringLiteral("AIM screen name is empty"));
    m_bos->sendSnac(FAM_PERMIT_DENY, subtype, lp8(clean));
    emit featureOperationResult(label, true, QStringLiteral("%1: %2").arg(label, clean));
}

void OscarBackend::doAuthorizationRequest(const QString &target, const QString &message)
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG))
        fail(QStringLiteral("this OSCAR server does not advertise Feedbag authorization support"));
    const QByteArray reason = message.toUtf8();
    QByteArray body = lp8(target.trimmed());
    appendU16(body, static_cast<quint16>(std::min<qsizetype>(reason.size(), 0xffff)));
    body += reason.left(0xffff);
    appendU16(body, 0);
    m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_REQUEST_AUTHORIZE_TO_HOST, body);
    emit featureOperationResult(QStringLiteral("Authorization request"), true,
                                QStringLiteral("Authorization request sent to %1.").arg(target.trimmed()));
}

void OscarBackend::doAuthorizationResponse(const QString &target, bool accept, const QString &message)
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG))
        fail(QStringLiteral("this OSCAR server does not advertise Feedbag authorization support"));
    const QByteArray reason = message.toUtf8();
    QByteArray body = lp8(target.trimmed());
    appendU8(body, accept ? 1 : 0);
    appendU16(body, static_cast<quint16>(std::min<qsizetype>(reason.size(), 0xffff)));
    body += reason.left(0xffff);
    appendU16(body, 0);
    m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_RESPOND_AUTHORIZE_TO_HOST, body);
    emit featureOperationResult(QStringLiteral("Authorization response"), true,
                                QStringLiteral("Authorization %1 for %2.").arg(accept ? QStringLiteral("accepted") : QStringLiteral("denied"), target.trimmed()));
}

void OscarBackend::doPreAuthorize(const QString &target, const QString &message)
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG))
        fail(QStringLiteral("this OSCAR server does not advertise Feedbag authorization support"));
    const QByteArray reason = message.toUtf8();
    QByteArray body = lp8(target.trimmed());
    appendU16(body, static_cast<quint16>(std::min<qsizetype>(reason.size(), 0xffff)));
    body += reason.left(0xffff);
    appendU16(body, 0);
    m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_PRE_AUTHORIZE_BUDDY, body);
    emit featureOperationResult(QStringLiteral("Pre-authorize buddy"), true,
                                QStringLiteral("Pre-authorized %1.").arg(target.trimmed()));
}

void OscarBackend::doRemoveMe(const QString &target)
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG))
        fail(QStringLiteral("this OSCAR server does not advertise Feedbag support"));
    m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_REMOVE_ME, lp8(target.trimmed()));
    emit featureOperationResult(QStringLiteral("Remove me from buddy list"), true,
                                QStringLiteral("Requested removal from %1's buddy list.").arg(target.trimmed()));
}

void OscarBackend::doTemporaryBuddy(const QString &target, bool add)
{
    if (!m_bos || !m_bos->families.contains(FAM_BUDDY))
        fail(QStringLiteral("this OSCAR server does not advertise Buddy service support"));
    const QString clean = target.trimmed();
    if (clean.isEmpty()) fail(QStringLiteral("AIM screen name is empty"));
    m_bos->sendSnac(FAM_BUDDY, add ? BUDDY_ADD_TEMP : BUDDY_REMOVE_TEMP, lp8(clean));
    emit featureOperationResult(add ? QStringLiteral("Temporary buddy watch") : QStringLiteral("Remove temporary buddy watch"),
                                true, clean);
}

void OscarBackend::doRequestWatcherList()
{
    if (!m_bos || !m_bos->families.contains(FAM_BUDDY))
        fail(QStringLiteral("this OSCAR server does not advertise Buddy watcher-list support"));
    const Snac reply = request(*m_bos, FAM_BUDDY, BUDDY_WATCHER_LIST_QUERY, QByteArray(), 6000);
    if (reply.family != FAM_BUDDY || reply.subtype != BUDDY_WATCHER_LIST_RESPONSE)
        fail(QStringLiteral("unexpected watcher-list reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    QStringList users;
    qsizetype offset = 0;
    while (offset < reply.body.size()) {
        const quint8 len = static_cast<quint8>(reply.body.at(offset++));
        if (offset + len > reply.body.size()) break;
        users.append(QString::fromUtf8(reply.body.mid(offset, len)));
        offset += len;
    }
    users.removeDuplicates();
    emit watcherListReceived(users);
}

void OscarBackend::doRetrieveStoredMessages()
{
    if (!m_bos || !m_bos->families.contains(FAM_ICBM))
        fail(QStringLiteral("this OSCAR server does not advertise ICBM/stored-message support"));
    m_bos->sendSnac(FAM_ICBM, ICBM_SIN_RETRIEVE, QByteArray());
    emit featureOperationResult(QStringLiteral("Stored messages"), true,
                                QStringLiteral("Requested server-stored/offline messages."));
}

void OscarBackend::doTypingNotification(const QString &target, quint16 event)
{
    if (!m_bos || !m_bos->families.contains(FAM_ICBM)) return;
    const QString clean = target.trimmed();
    if (clean.isEmpty()) return;
    QByteArray body(8, '\0');
    const quint64 cookie = QRandomGenerator::global()->generate64();
    for (int i = 0; i < 8; ++i) body[i] = static_cast<char>((cookie >> ((7 - i) * 8)) & 0xff);
    appendU16(body, ICBM_CHANNEL_IM);
    body += lp8(clean);
    appendU16(body, event);
    m_bos->sendSnac(FAM_ICBM, ICBM_CLIENT_EVENT, body);
}

void OscarBackend::doRequestAccountInfo()
{
    if (!m_bos || !m_bos->families.contains(FAM_ADMIN))
        fail(QStringLiteral("this OSCAR server does not advertise Account Administration support"));
    const Snac reply = request(*m_bos, FAM_ADMIN, ADMIN_INFO_QUERY, QByteArray(), 6000);
    if (reply.family != FAM_ADMIN || reply.subtype != ADMIN_INFO_REPLY)
        fail(QStringLiteral("unexpected account-info reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    qsizetype offset = 0;
    const QList<Tlv> items = parseTlvs(reply.body, offset);
    QVariantMap info;
    const QByteArray screen = firstTlv(items, ADMIN_TLV_SCREEN_NAME);
    const QByteArray email = firstTlv(items, ADMIN_TLV_EMAIL);
    const QByteArray reg = firstTlv(items, ADMIN_TLV_REG_STATUS);
    const QByteArray err = firstTlv(items, ADMIN_TLV_ERROR_CODE);
    if (!screen.isEmpty()) info.insert(QStringLiteral("screenName"), QString::fromUtf8(screen));
    if (!email.isEmpty()) info.insert(QStringLiteral("email"), QString::fromUtf8(email));
    if (!reg.isEmpty()) info.insert(QStringLiteral("registrationStatus"), reg.size() >= 2 ? readU16(reg, 0) : static_cast<quint8>(reg.at(0)));
    if (!err.isEmpty()) info.insert(QStringLiteral("errorCode"), err.size() >= 2 ? readU16(err, 0) : static_cast<quint8>(err.at(0)));
    info.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    emit accountInfoReceived(info);
}

static bool adminReplyOk(const Snac &reply, quint16 expectedSubtype)
{
    if (reply.family != FAM_ADMIN || reply.subtype != expectedSubtype) return false;
    qsizetype offset = 0;
    try {
        const QList<Tlv> items = parseTlvs(reply.body, offset);
        const QByteArray err = firstTlv(items, ADMIN_TLV_ERROR_CODE);
        if (!err.isEmpty()) return false;
    } catch (...) {
        // Some classic servers return an empty success body.
    }
    return true;
}

void OscarBackend::doChangeAccountEmail(const QString &email)
{
    if (!m_bos || !m_bos->families.contains(FAM_ADMIN)) fail(QStringLiteral("Account Administration is unavailable"));
    const Snac reply = request(*m_bos, FAM_ADMIN, ADMIN_INFO_CHANGE_REQUEST, tlv(ADMIN_TLV_EMAIL, email.trimmed()), 6000);
    const bool ok = adminReplyOk(reply, ADMIN_INFO_CHANGE_REPLY);
    emit featureOperationResult(QStringLiteral("Change account email"), ok, ok ? QStringLiteral("Account email updated.") : QStringLiteral("Account email change rejected."));
    if (!ok) fail(QStringLiteral("AIM account email change failed"));
}

void OscarBackend::doChangeFormattedName(const QString &formattedName)
{
    if (!m_bos || !m_bos->families.contains(FAM_ADMIN)) fail(QStringLiteral("Account Administration is unavailable"));
    const Snac reply = request(*m_bos, FAM_ADMIN, ADMIN_INFO_CHANGE_REQUEST, tlv(ADMIN_TLV_SCREEN_NAME, formattedName.trimmed()), 6000);
    const bool ok = adminReplyOk(reply, ADMIN_INFO_CHANGE_REPLY);
    emit featureOperationResult(QStringLiteral("Change formatted screen name"), ok, ok ? QStringLiteral("Formatted screen name updated.") : QStringLiteral("Formatted screen-name change rejected."));
    if (!ok) fail(QStringLiteral("AIM formatted screen-name change failed"));
}

void OscarBackend::doConfirmAccount()
{
    if (!m_bos || !m_bos->families.contains(FAM_ADMIN)) fail(QStringLiteral("Account Administration is unavailable"));
    const Snac reply = request(*m_bos, FAM_ADMIN, ADMIN_ACCOUNT_CONFIRM_REQUEST, QByteArray(), 6000);
    const bool ok = adminReplyOk(reply, ADMIN_ACCOUNT_CONFIRM_REPLY);
    emit featureOperationResult(QStringLiteral("Confirm AIM account"), ok, ok ? QStringLiteral("Account confirmation request submitted.") : QStringLiteral("Account confirmation request rejected."));
    if (!ok) fail(QStringLiteral("AIM account confirmation request failed"));
}

void OscarBackend::doDeleteAccount()
{
    if (!m_bos || !m_bos->families.contains(FAM_ADMIN)) fail(QStringLiteral("Account Administration is unavailable"));
    const Snac reply = request(*m_bos, FAM_ADMIN, ADMIN_ACCOUNT_DELETE_REQUEST, QByteArray(), 6000);
    const bool ok = adminReplyOk(reply, ADMIN_ACCOUNT_DELETE_REPLY);
    emit featureOperationResult(QStringLiteral("Delete AIM account"), ok, ok ? QStringLiteral("Account deletion request accepted by the server.") : QStringLiteral("Account deletion request rejected."));
    if (!ok) fail(QStringLiteral("AIM account deletion request failed"));
}

void OscarBackend::doSetPrivacyFlags(quint32 flags)
{
    if (!m_bos || !m_bos->families.contains(FAM_OSERVICE)) fail(QStringLiteral("OSCAR Generic Service is unavailable"));
    QByteArray body; appendU32(body, flags);
    m_bos->sendSnac(FAM_OSERVICE, OS_SET_PRIVACY_FLAGS, body);
    emit featureOperationResult(QStringLiteral("Privacy flags"), true, QStringLiteral("OSCAR privacy flags updated to 0x%1.").arg(flags, 8, 16, QLatin1Char('0')));
}

void OscarBackend::doVoiceRendezvous(quint16 messageType,
                                     const QString &target,
                                     const QString &cookieHex,
                                     const QString &localAddress,
                                     quint16 localPort,
                                     int sampleRate,
                                     quint16 cancelReason)
{
    if (!m_bos || !m_bos->families.contains(FAM_ICBM)) fail(QStringLiteral("OSCAR ICBM service is unavailable"));
    const QByteArray cookie = QByteArray::fromHex(cookieHex.toLatin1());
    if (cookie.size() != 8) fail(QStringLiteral("invalid OSCAR voice rendezvous cookie"));
    if (target.trimmed().isEmpty()) fail(QStringLiteral("OSCAR voice target is empty"));

    QByteArray rendezvous;
    appendU16(rendezvous, messageType);
    rendezvous += cookie;
    rendezvous += waffleVoiceCapability();

    if (messageType == RENDEZVOUS_PROPOSE || messageType == RENDEZVOUS_ACCEPT) {
        const QByteArray ip = ipv4Bytes(localAddress);
        if (ip.size() != 4 || localPort == 0) fail(QStringLiteral("invalid local OSCAR voice endpoint"));
        rendezvous += tlv(RENDEZVOUS_TLV_IP, ip);
        rendezvous += tlv(RENDEZVOUS_TLV_REQUESTER_IP, ip);
        QByteArray port;
        appendU16(port, localPort);
        rendezvous += tlv(RENDEZVOUS_TLV_PORT, port);
        if (messageType == RENDEZVOUS_PROPOSE) {
            QByteArray seq;
            appendU16(seq, 1);
            rendezvous += tlv(RENDEZVOUS_TLV_SEQUENCE, seq);
            rendezvous += tlv(RENDEZVOUS_TLV_INVITATION,
                              QByteArrayLiteral("WaffleHouse-Client OSCAR voice chat"));
        }
        QByteArray extension = QByteArrayLiteral("WHV1");
        appendU16(extension, static_cast<quint16>(std::clamp(sampleRate, 8000, 65535)));
        appendU8(extension, 1);
        rendezvous += tlv(RENDEZVOUS_TLV_WAFFLE_VOICE, extension);
    } else {
        QByteArray reason;
        appendU16(reason, cancelReason);
        rendezvous += tlv(RENDEZVOUS_TLV_CANCEL_REASON, reason);
    }

    QByteArray body;
    body += cookie;
    appendU16(body, ICBM_CHANNEL_RENDEZVOUS);
    body += lp8(target.trimmed());
    body += tlv(ICBM_TLV_RENDEZVOUS, rendezvous);
    m_bos->sendSnac(FAM_ICBM, ICBM_MSG_TO_HOST, body);
}

void OscarBackend::processCommand(const Command &command)
{
    try {
        switch (command.type) {
        case CommandType::SendIm:
            doSendIm(command.a, command.b, !command.flag);
            if (command.flag) {
                const QString hint = m_peerClientHints.value(command.a.toCaseFolded());
                if (!hint.isEmpty()) emit eventReceived(QStringLiteral("version"), command.a, hint);
            }
            break;
        case CommandType::JoinRoom:
            doJoinRoom(command.a, command.flag);
            break;
        case CommandType::SendRoom:
            doSendRoom(command.a, command.b);
            break;
        case CommandType::LeaveRoom:
            doLeaveRoom(command.a);
            break;
        case CommandType::Password:
            doChangePassword(command.a, command.b);
            break;
        case CommandType::Raw:
            doRaw(command.a, command.b, command.c);
            break;
        case CommandType::AddBuddy:
            doAddBuddy(command.a);
            break;
        case CommandType::RemoveBuddy:
            doRemoveBuddy(command.a);
            break;
        case CommandType::SetAway:
            doSetAway(command.a, false);
            break;
        case CommandType::SetAfk:
            doSetAway(command.a, true);
            break;
        case CommandType::SetIdle:
            doSetIdle(command.number);
            break;
        case CommandType::SetBack:
            doSetBack();
            break;
        case CommandType::SetProfile:
            doSetProfile(command.a);
            break;
        case CommandType::RefreshCapabilities:
            discoverBosCapabilities();
            break;
        case CommandType::RequestUserInfo:
            doRequestUserInfo(command.a);
            break;
        case CommandType::RequestDirectoryInfo:
            doRequestDirectoryInfo(command.a);
            break;
        case CommandType::SetDirectoryInfo:
            doSetDirectoryInfo(command.map);
            break;
        case CommandType::FindByEmail:
            doFindByEmail(command.a);
            break;
        case CommandType::InviteByEmail:
            doInviteByEmail(command.a, command.b);
            break;
        case CommandType::PrivacyListAction:
            doPrivacyListAction(static_cast<quint16>(command.number), command.a, command.b);
            break;
        case CommandType::AuthorizationRequest:
            doAuthorizationRequest(command.a, command.b);
            break;
        case CommandType::AuthorizationResponse:
            doAuthorizationResponse(command.a, command.flag, command.b);
            break;
        case CommandType::PreAuthorize:
            doPreAuthorize(command.a, command.b);
            break;
        case CommandType::RemoveMe:
            doRemoveMe(command.a);
            break;
        case CommandType::TemporaryBuddy:
            doTemporaryBuddy(command.a, command.flag);
            break;
        case CommandType::WatcherList:
            doRequestWatcherList();
            break;
        case CommandType::RetrieveStoredMessages:
            doRetrieveStoredMessages();
            break;
        case CommandType::TypingNotification:
            doTypingNotification(command.a, static_cast<quint16>(command.number));
            break;
        case CommandType::RequestAccountInfo:
            doRequestAccountInfo();
            break;
        case CommandType::ChangeAccountEmail:
            doChangeAccountEmail(command.a);
            break;
        case CommandType::ChangeFormattedName:
            doChangeFormattedName(command.a);
            break;
        case CommandType::ConfirmAccount:
            doConfirmAccount();
            break;
        case CommandType::DeleteAccount:
            doDeleteAccount();
            break;
        case CommandType::SetPrivacyFlags:
            doSetPrivacyFlags(command.number);
            break;
        case CommandType::VoicePropose:
            doVoiceRendezvous(RENDEZVOUS_PROPOSE, command.a, command.b, command.c,
                              static_cast<quint16>(command.number), static_cast<int>(command.number2));
            break;
        case CommandType::VoiceAccept:
            doVoiceRendezvous(RENDEZVOUS_ACCEPT, command.a, command.b, command.c,
                              static_cast<quint16>(command.number), static_cast<int>(command.number2));
            break;
        case CommandType::VoiceCancel:
            doVoiceRendezvous(RENDEZVOUS_CANCEL, command.a, command.b, QString(), 0, 0,
                              static_cast<quint16>(command.number));
            break;
        }
    } catch (const std::exception &e) {
        emit backendError(QStringLiteral("AIM/OSCAR"), QString::fromUtf8(e.what()));
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[error] %1").arg(QString::fromUtf8(e.what())));
    }
}

void OscarBackend::dispatchBos(const Snac &snac)
{
    if (snac.family == FAM_ICBM && snac.subtype == ICBM_CLIENT_EVENT) {
        // cookie[8], channel[2], screen-name[LP8], event[2]
        if (snac.body.size() >= 13) {
            qsizetype offset = 10;
            const quint8 len = static_cast<quint8>(snac.body.at(offset++));
            if (offset + len + 2 <= snac.body.size()) {
                const QString from = QString::fromUtf8(snac.body.mid(offset, len));
                offset += len;
                const quint16 event = readU16(snac.body, offset);
                // Typing is transient UI state, not a chat event.  Consumers
                // use the dedicated signal so GUI/CLI conversation containers
                // are never created just to display a typing notification.
                emit typingNotificationReceived(from, event);
            }
        }
        return;
    }

    if (snac.family == FAM_FEEDBAG && snac.subtype == FEEDBAG_REQUEST_AUTHORIZE_TO_CLIENT) {
        qsizetype offset = 0;
        if (snac.body.isEmpty()) return;
        const quint8 len = static_cast<quint8>(snac.body.at(offset++));
        if (offset + len > snac.body.size()) return;
        const QString from = QString::fromUtf8(snac.body.mid(offset, len)); offset += len;
        QString message;
        if (offset + 2 <= snac.body.size()) {
            const quint16 msgLen = readU16(snac.body, offset); offset += 2;
            if (offset + msgLen <= snac.body.size()) message = QString::fromUtf8(snac.body.mid(offset, msgLen));
        }
        emit authorizationRequestReceived(from, message);
        emit eventReceived(QStringLiteral("authorization"), from,
                           QStringLiteral("%1 requests buddy-list authorization%2")
                               .arg(from, message.isEmpty() ? QString() : QStringLiteral(": %1").arg(message)));
        return;
    }

    if (snac.family == FAM_FEEDBAG && snac.subtype == FEEDBAG_RESPOND_AUTHORIZE_TO_CLIENT) {
        qsizetype offset = 0;
        if (snac.body.isEmpty()) return;
        const quint8 len = static_cast<quint8>(snac.body.at(offset++));
        if (offset + len + 1 > snac.body.size()) return;
        const QString from = QString::fromUtf8(snac.body.mid(offset, len)); offset += len;
        const bool accepted = static_cast<quint8>(snac.body.at(offset++)) != 0;
        QString message;
        if (offset + 2 <= snac.body.size()) {
            const quint16 msgLen = readU16(snac.body, offset); offset += 2;
            if (offset + msgLen <= snac.body.size()) message = QString::fromUtf8(snac.body.mid(offset, msgLen));
        }
        emit authorizationResponseReceived(from, accepted, message);
        emit eventReceived(QStringLiteral("authorization"), from,
                           QStringLiteral("%1 %2 your buddy-list authorization request%3")
                               .arg(from, accepted ? QStringLiteral("accepted") : QStringLiteral("denied"),
                                    message.isEmpty() ? QString() : QStringLiteral(": %1").arg(message)));
        return;
    }

    if (snac.family == FAM_FEEDBAG && snac.subtype == FEEDBAG_BUDDY_ADDED) {
        qsizetype offset = 0;
        if (snac.body.size() >= 7 && static_cast<quint8>(snac.body.at(0)) == 0) offset = 6;
        if (offset < snac.body.size()) {
            const quint8 len = static_cast<quint8>(snac.body.at(offset++));
            if (offset + len <= snac.body.size()) {
                const QString from = QString::fromUtf8(snac.body.mid(offset, len));
                emit buddyAddedYou(from);
                emit eventReceived(QStringLiteral("buddy"), from,
                                   QStringLiteral("%1 added you to their buddy list").arg(from));
            }
        }
        return;
    }

    if (snac.family == FAM_OSERVICE && snac.subtype == OS_USER_INFO_UPDATE) {
        try {
            qsizetype offset = 0;
            const UserInfo self = parseUserInfo(snac.body, offset);
            const QByteArray idleRaw = firstTlv(self.tlvs, USERINFO_TLV_IDLE_TIME);
            const int idleMinutes = idleRaw.size() >= 2 ? static_cast<int>(readU16(idleRaw, 0)) : 0;
            const QByteArray flagsRaw = firstTlv(self.tlvs, USERINFO_TLV_FLAGS);
            const quint16 flags = flagsRaw.size() >= 2 ? readU16(flagsRaw, 0) : 0;
            const QByteArray statusRaw = firstTlv(self.tlvs, USERINFO_TLV_STATUS);
            const qint64 status = statusRaw.size() >= 4 ? static_cast<qint64>(readU32(statusRaw, 0)) : -1;
            if (oscarWireTraceEnabled(m_settings)) {
                protocolLog(QStringLiteral("[oscar-self] server USER_INFO_UPDATE name=%1 warning=%2 idle=%3m flags=0x%4 status=%5")
                                .arg(self.name)
                                .arg(static_cast<double>(self.warningLevel) / 10.0, 0, 'f', 1)
                                .arg(idleMinutes)
                                .arg(flags, 4, 16, QLatin1Char('0'))
                                .arg(status >= 0 ? QStringLiteral("0x%1").arg(status, 8, 16, QLatin1Char('0'))
                                                 : QStringLiteral("not supplied")));
            }
        } catch (const std::exception &e) {
            protocolLog(QStringLiteral("[oscar-self] USER_INFO_UPDATE parse error: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
        return;
    }

    if (snac.family == FAM_OSERVICE && snac.subtype == OS_EVIL_NOTIFICATION) {
        if (snac.body.size() < 2) {
            protocolLog(QStringLiteral("[OSCAR warning] truncated EVIL_NOTIFICATION"));
            return;
        }
        const quint16 warningRaw = readU16(snac.body, 0);
        QString actor;
        if (snac.body.size() > 2) {
            try {
                qsizetype offset = 2;
                const UserInfo source = parseUserInfo(snac.body, offset);
                actor = source.name;
            } catch (...) {
                // The warning level is still useful when the optional USERINFO is malformed.
            }
        }
        const QString text = actor.isEmpty()
            ? QStringLiteral("Your OSCAR warning level changed to %1%.").arg(static_cast<double>(warningRaw) / 10.0, 0, 'f', 1)
            : QStringLiteral("Your OSCAR warning level changed to %1% (reported by %2).")
                  .arg(static_cast<double>(warningRaw) / 10.0, 0, 'f', 1)
                  .arg(actor);
        emit oscarNoticeReceived(QStringLiteral("warning"), text);
        emit eventReceived(QStringLiteral("status"), QString(), text);
        return;
    }

    if (snac.family == FAM_OSERVICE && (snac.subtype == OS_MOTD || snac.subtype == OS_WELL_KNOWN_URLS)) {
        QStringList parts;
        qsizetype offset = 0;
        try {
            const QList<Tlv> items = parseTlvs(snac.body, offset);
            for (const Tlv &item : items) {
                const QString text = stripAimHtml(QString::fromUtf8(item.value)).trimmed();
                if (!text.isEmpty()) parts.append(text);
            }
        } catch (...) {
            const QString raw = stripAimHtml(QString::fromUtf8(snac.body)).trimmed();
            if (!raw.isEmpty()) parts.append(raw);
        }
        const QString kind = snac.subtype == OS_MOTD ? QStringLiteral("motd") : QStringLiteral("urls");
        const QString text = parts.join(QStringLiteral("\n"));
        if (!text.isEmpty()) { emit oscarNoticeReceived(kind, text); emit eventReceived(QStringLiteral("status"), QString(), text); }
        return;
    }

    if (snac.family == FAM_BUDDY && snac.subtype == BUDDY_REJECT_NOTIFICATION) {
        QStringList users;
        qsizetype offset = 0;
        while (offset < snac.body.size()) {
            const quint8 len = static_cast<quint8>(snac.body.at(offset++));
            if (offset + len > snac.body.size()) break;
            users.append(QString::fromUtf8(snac.body.mid(offset, len)));
            offset += len;
        }
        if (!users.isEmpty()) {
            const QString text = QStringLiteral("The server rejected presence watches for: %1")
                                     .arg(users.join(QStringLiteral(", ")));
            emit oscarNoticeReceived(QStringLiteral("buddy-watch"), text);
            emit eventReceived(QStringLiteral("buddy"), QString(), text);
        }
        return;
    }

    if (snac.family == FAM_BUDDY && snac.subtype == BUDDY_WATCHER_NOTIFICATION) {
        QStringList users;
        qsizetype offset = 0;
        while (offset < snac.body.size()) {
            const quint8 len = static_cast<quint8>(snac.body.at(offset++));
            if (offset + len > snac.body.size()) break;
            users.append(QString::fromUtf8(snac.body.mid(offset, len))); offset += len;
        }
        if (!users.isEmpty()) emit eventReceived(QStringLiteral("buddy"), QString(),
                                                  QStringLiteral("Watcher notification: %1").arg(users.join(QStringLiteral(", "))));
        return;
    }

    if (snac.family == FAM_ICBM && snac.subtype == ICBM_MISSED_CALLS) {
        protocolLog(QStringLiteral("[OSCAR] The server reports missed ICBM message(s)/call(s)."));
        return;
    }
    if (snac.family == FAM_ICBM && snac.subtype == ICBM_SIN_REPLY) {
        protocolLog(QStringLiteral("[OSCAR] Server-stored message retrieval completed."));
        return;
    }

    if (snac.family == FAM_ICBM && snac.subtype == ICBM_MSG_TO_CLIENT) {
        if (snac.body.size() < 10) {
            protocolLog(QStringLiteral("[event error] truncated incoming ICBM"));
            return;
        }

        const quint16 channel = readU16(snac.body, 8);
        qsizetype offset = 10;
        const UserInfo sender = parseUserInfo(snac.body, offset);
        const auto items = parseTlvs(snac.body, offset);

        if (channel == ICBM_CHANNEL_RENDEZVOUS) {
            const QByteArray blob = firstTlv(items, ICBM_TLV_RENDEZVOUS);
            if (blob.size() < 26) {
                protocolLog(QStringLiteral("[OSCAR rendezvous] Truncated service request from %1.").arg(sender.name));
                return;
            }
            const quint16 messageType = readU16(blob, 0);
            const QByteArray cookie = blob.mid(2, 8);
            const QByteArray service = blob.mid(10, 16);
            const QString cookieHex = QString::fromLatin1(cookie.toHex());

            if (service == legacyAimVoiceCapability()) {
                emit legacyVoiceInviteReceived(sender.name, cookieHex);
                protocolLog(QStringLiteral("[OSCAR voice] %1 advertised a legacy AIM Talk invitation; "
                                           "the proprietary legacy media codec is not enabled.").arg(sender.name));
                return;
            }
            if (service != waffleVoiceCapability()) {
                if (oscarWireTraceEnabled(m_settings)) {
                    protocolLog(QStringLiteral("[OSCAR rendezvous] %1 service %2 from %3")
                                    .arg(messageType)
                                    .arg(QString::fromLatin1(service.toHex()), sender.name));
                }
                return;
            }

            qsizetype rvOffset = 26;
            const QList<Tlv> rv = parseTlvs(blob, rvOffset);
            QString remoteAddress = ipv4Text(firstTlv(rv, RENDEZVOUS_TLV_VERIFIED_IP));
            if (remoteAddress.isEmpty()) remoteAddress = ipv4Text(firstTlv(rv, RENDEZVOUS_TLV_IP));
            if (remoteAddress.isEmpty()) remoteAddress = ipv4Text(firstTlv(rv, RENDEZVOUS_TLV_REQUESTER_IP));
            const QByteArray portRaw = firstTlv(rv, RENDEZVOUS_TLV_PORT);
            const quint16 remotePort = portRaw.size() >= 2 ? readU16(portRaw, 0) : 0;
            const QByteArray ext = firstTlv(rv, RENDEZVOUS_TLV_WAFFLE_VOICE);
            int sampleRate = 16000;
            int channels = 1;
            if (ext.size() >= 7 && ext.left(4) == QByteArrayLiteral("WHV1")) {
                sampleRate = readU16(ext, 4);
                channels = static_cast<unsigned char>(ext.at(6));
            }
            const QString invitation = QString::fromUtf8(firstTlv(rv, RENDEZVOUS_TLV_INVITATION));
            const QByteArray reasonRaw = firstTlv(rv, RENDEZVOUS_TLV_CANCEL_REASON);
            const quint16 reason = reasonRaw.size() >= 2 ? readU16(reasonRaw, 0) : 0;

            if (messageType == RENDEZVOUS_PROPOSE) {
                emit voiceInviteReceived(sender.name, cookieHex, remoteAddress, remotePort,
                                         sampleRate, channels, invitation);
            } else if (messageType == RENDEZVOUS_ACCEPT) {
                emit voiceInviteAccepted(sender.name, cookieHex, remoteAddress, remotePort,
                                         sampleRate, channels);
            } else if (messageType == RENDEZVOUS_CANCEL || messageType == RENDEZVOUS_REJECT) {
                emit voiceInviteCancelled(sender.name, cookieHex,
                                          messageType == RENDEZVOUS_REJECT ? 0xffff : reason);
            }
            return;
        }

        if (channel != ICBM_CHANNEL_IM) {
            if (oscarWireTraceEnabled(m_settings)) {
                protocolLog(QStringLiteral("[oscar-wire] Unsupported OSCAR ICBM channel %1 from %2")
                                .arg(channel).arg(sender.name));
            }
            return;
        }

        const QByteArray messageBlob = firstTlv(items, ICBM_TLV_IM_DATA);
        const QString message = messageBlob.isEmpty()
                              ? QStringLiteral("<non-text ICBM>")
                              : stripAimHtml(parseIcbmMessage(messageBlob));
        const QString peerKey = sender.name.toCaseFolded();
        if (message == QStringLiteral("[[WHVER:Q]]")) {
            doSendIm(sender.name,
                     QStringLiteral("[[WHVER:R:%1]]").arg(appVersionString()),
                     false);
            return;
        }
        if (message.startsWith(QStringLiteral("[[WHVER:R:")) && message.endsWith(QStringLiteral("]]"))) {
            const QString version = message.mid(10, message.size() - 12).trimmed();
            const QString reported = version.isEmpty()
                ? QStringLiteral("WaffleHouse-Client (version unavailable)")
                : QStringLiteral("WaffleHouse-Client %1").arg(version);
            m_peerClientHints.insert(peerKey, reported);
            emit eventReceived(QStringLiteral("version"), sender.name, reported);
            return;
        }
        if (message.startsWith(QStringLiteral("[[CPX3:")) && !m_peerClientHints.contains(peerKey)) {
            m_peerClientHints.insert(peerKey,
                QStringLiteral("Legacy CPX3-compatible client detected; exact WaffleHouse-Client version unavailable"));
        }
        emit eventReceived(QStringLiteral("im"), sender.name,
                           QStringLiteral("<%1> %2").arg(sender.name, message));
        return;
    }

    if (snac.family == FAM_ICBM && snac.subtype == ICBM_HOST_ACK) {
        if (oscarWireTraceEnabled(m_settings)) {
            protocolLog(QStringLiteral("[oscar-wire] IM acknowledged by host"));
        }
        return;
    }

    if (snac.family == FAM_ICBM
        && (snac.subtype == 0x0001 || snac.subtype == ICBM_CLIENT_ERROR)) {
        const int code = snac.body.size() >= 2 ? readU16(snac.body, 0) : -1;
        protocolLog(QStringLiteral("[IM error] OSCAR error 0x%1")
                        .arg(code, 4, 16, QLatin1Char('0')));
        return;
    }

    if (snac.family == FAM_BUDDY
        && (snac.subtype == BUDDY_ONCOMING || snac.subtype == BUDDY_OFFGOING)) {
        try {
            qsizetype offset = 0;
            const bool online = snac.subtype == BUDDY_ONCOMING;

            // BUDDY__ARRIVED/DEPARTED may batch more than one UserInfo
            // record in a single notification.  Consume the whole body so a
            // burst at sign-on cannot make us silently lose buddies after the
            // first record.
            while (offset < snac.body.size()) {
                const qsizetype before = offset;
                const UserInfo user = parseUserInfo(snac.body, offset);
                if (offset <= before) {
                    throw std::runtime_error("BUDDY presence parser made no progress");
                }
                if (user.name.isEmpty()) continue;

                emit buddyPresenceChanged(user.name, online);

                QVariantMap native;
                native.insert(QStringLiteral("online"), online);
                native.insert(QStringLiteral("warningRaw"), static_cast<int>(user.warningLevel));
                native.insert(QStringLiteral("warningPercent"), static_cast<double>(user.warningLevel) / 10.0);

                quint64 userFlags = 0;
                const QByteArray lowRaw = firstTlv(user.tlvs, USERINFO_TLV_FLAGS);
                if (lowRaw.size() >= 2) userFlags = readU16(lowRaw, 0);
                const QByteArray highRaw = firstTlv(user.tlvs, USERINFO_TLV_FLAGS2);
                if (!highRaw.isEmpty()) {
                    quint64 upper = 0;
                    for (const char byte : highRaw) upper = (upper << 8) | static_cast<quint8>(byte);
                    userFlags |= (upper << 16);
                }
                native.insert(QStringLiteral("userFlags"), QVariant::fromValue<qulonglong>(userFlags));

                const QByteArray idleRaw = firstTlv(user.tlvs, USERINFO_TLV_IDLE_TIME);
                const int idleMinutes = idleRaw.size() >= 2 ? static_cast<int>(readU16(idleRaw, 0)) : 0;
                native.insert(QStringLiteral("idleMinutes"), idleMinutes);
                native.insert(QStringLiteral("idleSeconds"), online && idleMinutes > 0 ? idleMinutes * 60 : 0);

                const QByteArray statusRawBytes = firstTlv(user.tlvs, USERINFO_TLV_STATUS);
                const bool statusSupplied = statusRawBytes.size() >= 4;
                const quint32 statusBits = statusSupplied ? readU32(statusRawBytes, 0) : 0;
                native.insert(QStringLiteral("statusSupplied"), statusSupplied);
                native.insert(QStringLiteral("statusRaw"), statusSupplied ? static_cast<qint64>(statusBits) : -1);

                const QByteArray signonRaw = firstTlv(user.tlvs, USERINFO_TLV_SIGNON_TIME);
                const QByteArray memberRaw = firstTlv(user.tlvs, USERINFO_TLV_MEMBER_SINCE);
                if (signonRaw.size() >= 4) native.insert(QStringLiteral("signonTime"), static_cast<qint64>(readU32(signonRaw, 0)));
                if (memberRaw.size() >= 4) native.insert(QStringLiteral("memberSince"), static_cast<qint64>(readU32(memberRaw, 0)));

                QString state = online ? QStringLiteral("Online") : QStringLiteral("Offline");
                if (online) {
                    if (statusSupplied && (statusBits & USER_STATUS_INVISIBLE)) state = QStringLiteral("Invisible");
                    else if (statusSupplied && (statusBits & USER_STATUS_DND)) state = QStringLiteral("Do Not Disturb");
                    else if (statusSupplied && (statusBits & USER_STATUS_NA)) state = QStringLiteral("Not Available");
                    else if (statusSupplied && (statusBits & USER_STATUS_BUSY)) state = QStringLiteral("Busy");
                    else if ((userFlags & USER_FLAG_UNAVAILABLE)
                             || (statusSupplied && (statusBits & USER_STATUS_AWAY))) state = QStringLiteral("Away");
                    else if (statusSupplied && (statusBits & USER_STATUS_FREE_FOR_CHAT)) state = QStringLiteral("Free for Chat");
                    else if (idleMinutes > 0) state = QStringLiteral("Idle");
                }
                native.insert(QStringLiteral("state"), state);

                // Capabilities in an arrival packet are authoritative when the TLV
                // is present. If omitted, retain the last known values as OSCAR
                // specifies. Short capabilities are expanded to their UUID form.
                bool capsSupplied = false;
                QList<QByteArray> caps;
                for (const Tlv &item : user.tlvs) {
                    if (item.type == USERINFO_TLV_CAPABILITIES) { capsSupplied = true; caps += splitCapabilities(item.value); }
                    else if (item.type == USERINFO_TLV_SHORT_CAPABILITIES) { capsSupplied = true; caps += shortCapabilities(item.value); }
                }
                if (capsSupplied) {
                    QSet<QByteArray> uniqueCaps;
                    for (const QByteArray &cap : caps) if (cap.size() == 16) uniqueCaps.insert(cap);
                    {
                        QMutexLocker locker(&m_capabilityMutex);
                        m_peerCapabilities.insert(user.name.toCaseFolded(), uniqueCaps);
                    }
                    native.insert(QStringLiteral("capabilities"), describeCapabilities(caps));
                }

                emit buddyNativePresenceChanged(user.name, native);
                if (oscarWireTraceEnabled(m_settings)) {
                    protocolLog(QStringLiteral("[oscar-presence] %1 => %2 idle=%3m flags=0x%4 status=%5")
                                    .arg(user.name, state)
                                    .arg(idleMinutes)
                                    .arg(userFlags, 0, 16)
                                    .arg(statusSupplied ? QStringLiteral("0x%1").arg(statusBits, 8, 16, QLatin1Char('0'))
                                                       : QStringLiteral("not supplied")));
                }
            }
        } catch (const std::exception &e) {
            protocolLog(QStringLiteral("[buddy] presence parse error: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
        return;
    }

    if (oscarWireTraceEnabled(m_settings)) {
        protocolLog(QStringLiteral("[oscar-wire] BOS: unhandled SNAC %1/%2 req=%3 body=%4")
                        .arg(snac.family, 4, 16, QLatin1Char('0'))
                        .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                        .arg(snac.requestId)
                        .arg(QString::fromLatin1(snac.body.toHex())));
    }
}

void OscarBackend::dispatchChat(FlapConnection &connection, const Snac &snac)
{
    const QString roomName = connection.label().startsWith(QStringLiteral("chat:"))
                           ? connection.label().mid(5)
                           : connection.label();

    if (snac.family == FAM_CHAT && snac.subtype == CHAT_MSG_TO_CLIENT) {
        if (snac.body.size() < 10) {
            protocolLog(QStringLiteral("[chat event error] truncated chat message"));
            return;
        }

        qsizetype offset = 10;
        const auto items = parseTlvs(snac.body, offset);
        const QByteArray senderBlob = firstTlv(items, CHAT_TLV_SENDER);
        const QByteArray messageBlob = firstTlv(items, CHAT_TLV_MESSAGE_INFO);

        QString sender = QStringLiteral("?");
        if (!senderBlob.isEmpty()) {
            qsizetype senderOffset = 0;
            sender = parseUserInfo(senderBlob, senderOffset).name;
        }

        QString text = QStringLiteral("<non-text chat message>");
        if (!messageBlob.isEmpty()) {
            qsizetype nestedOffset = 0;
            const auto nested = parseTlvs(messageBlob, nestedOffset);
            const QByteArray textBlob = firstTlv(nested, CHAT_MSG_TLV_TEXT);
            if (!textBlob.isEmpty()) {
                text = stripAimHtml(QString::fromUtf8(textBlob));
            }
        }

        emit eventReceived(QStringLiteral("chat"), roomName,
                           QStringLiteral("<%1> %2").arg(sender, text));
        return;
    }

    if (snac.family == FAM_CHAT
        && (snac.subtype == CHAT_USERS_JOINED || snac.subtype == CHAT_USERS_LEFT)) {
        qsizetype offset = 0;
        QStringList names;
        while (offset < snac.body.size()) {
            const qsizetype previous = offset;
            names.push_back(parseUserInfo(snac.body, offset).name);
            if (offset <= previous) {
                break;
            }
        }

        if (!names.isEmpty()) {
            const bool joined = snac.subtype == CHAT_USERS_JOINED;
            emit membersChanged(roomName,
                                joined ? QStringLiteral("add") : QStringLiteral("remove"),
                                names);
            emit eventReceived(QStringLiteral("chat"), roomName,
                               QStringLiteral("*** %1 %2")
                                   .arg(names.join(QStringLiteral(", ")),
                                        joined ? QStringLiteral("joined") : QStringLiteral("left")));
        }
        return;
    }

    if (snac.family == FAM_CHAT && snac.subtype == CHAT_ROOM_INFO) {
        if (oscarWireTraceEnabled(m_settings)) {
            qsizetype offset = 0;
            const RoomInfo room = parseRoomInfo(snac.body, offset, true);
            protocolLog(QStringLiteral("[oscar-wire] chat room info: %1 exchange=%2 cookie=%3")
                            .arg(room.name())
                            .arg(room.exchange)
                            .arg(room.cookie));
        }
        return;
    }

    if (oscarWireTraceEnabled(m_settings)) {
        protocolLog(QStringLiteral("[oscar-wire] %1: unhandled SNAC %2/%3 req=%4 body=%5")
                        .arg(connection.label())
                        .arg(snac.family, 4, 16, QLatin1Char('0'))
                        .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                        .arg(snac.requestId)
                        .arg(QString::fromLatin1(snac.body.toHex())));
    }
}

void OscarBackend::dispatchSnac(FlapConnection &connection, const Snac &snac)
{
    if (&connection == m_bos.get()) {
        dispatchBos(snac);
        return;
    }
    if (connection.label().startsWith(QStringLiteral("chat:"))) {
        dispatchChat(connection, snac);
        return;
    }

    if (oscarWireTraceEnabled(m_settings)) {
        protocolLog(QStringLiteral("[oscar-wire] %1: unhandled SNAC %2/%3 req=%4 body=%5")
                        .arg(connection.label())
                        .arg(snac.family, 4, 16, QLatin1Char('0'))
                        .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                        .arg(snac.requestId)
                        .arg(QString::fromLatin1(snac.body.toHex())));
    }
}

void OscarBackend::processIncoming(FlapConnection &connection)
{
    while (connection.hasData()) {
        dispatchSnac(connection, connection.receiveSnac(1000));
    }
}

void OscarBackend::run()
{
    QString disconnectReason = QStringLiteral("signed off");

    try {
        const auto authResult = authenticate();
        const auto endpoint = bosRedirectEndpoint(authResult.first, m_settings.port);

        m_bos = std::make_unique<FlapConnection>(
            endpoint.first,
            endpoint.second,
            QStringLiteral("BOS"),
            oscarWireTraceEnabled(m_settings),
            [this](const QString &s) { protocolLog(s); });
        bootstrapService(*m_bos, authResult.second, true);
        discoverBosCapabilities();
        loadBuddyList();

        m_presenceState = QStringLiteral("ONLINE");
        m_presenceMessage.clear();
        m_idleSeconds = 0;
        emit presenceChanged(m_presenceState, m_presenceMessage, m_idleSeconds);

        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[online] signed on as %1 via %2:%3")
                               .arg(m_settings.username, endpoint.first)
                               .arg(endpoint.second));
        emit connected(m_settings.username,
                       QStringLiteral("%1:%2").arg(endpoint.first).arg(endpoint.second));

        while (!m_stopRequested) {
            for (const Command &command : takeCommands()) {
                processCommand(command);
            }

            if (m_bos && !m_bos->isConnected()) {
                fail(QStringLiteral("BOS connection closed"));
            }

            if (m_bos && m_bos->isConnected()) {
                // Let the BOS socket block briefly so this loop does not spin.
                // receiveSnac() itself is only called when bytes are available.
                // QTcpSocket::waitForReadyRead pumps the socket in this worker thread.
                if (!m_bos->hasData()) {
                    m_bos->waitForData(20);
                }
                processIncoming(*m_bos);
            }

            if (m_chatNav && m_chatNav->isConnected()) {
                if (!m_chatNav->hasData()) {
                    m_chatNav->waitForData(1);
                }
                processIncoming(*m_chatNav);
            }

            for (auto it = m_chats.begin(); it != m_chats.end(); ++it) {
                if (it.value() && it.value()->connection && it.value()->connection->isConnected()) {
                    if (!it.value()->connection->hasData()) {
                        it.value()->connection->waitForData(1);
                    }
                    processIncoming(*it.value()->connection);
                }
            }
        }
    } catch (const std::exception &e) {
        disconnectReason = QString::fromUtf8(e.what());
        emit backendError(QStringLiteral("AIM/OSCAR connection"), disconnectReason);
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[connection] %1").arg(disconnectReason));
    }

    m_chats.clear();
    {
        QMutexLocker locker(&m_capabilityMutex);
        m_serverFamilies.clear();
        m_peerCapabilities.clear();
    }
    m_maxProfileLength = 0;
    if (m_chatNav) {
        m_chatNav->close();
        m_chatNav.reset();
    }
    if (m_bos) {
        m_bos->close();
        m_bos.reset();
    }

    emit eventReceived(QStringLiteral("status"), QString(), QStringLiteral("[offline] signed off"));
    emit disconnected(disconnectReason);
}
