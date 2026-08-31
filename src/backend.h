#pragma once

#include "appbranding.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <atomic>

struct ConnectionSettings {
    enum class Protocol {
        Oscar = 0,
        Irc = 1,
        Telnet = 3,
        Sip = 4,
        Unknown = 255,
    };

    Protocol protocol = Protocol::Unknown;
    QString server;
    quint16 port = 5190;
    QString username;
    QString password;
    // When true, the password may be persisted in the local QSettings profile.
    // QSettings does not encrypt this value; the UI makes that clear before opt-in.
    bool savePassword = false;

    // AIM/OSCAR network compatibility profile. "nina" reproduces the
    // stock-AIM behavior used after NINAPatcher redirects AIM to NINA.
    // "auto" also enables that profile automatically for *.nina.chat hosts.
    QString networkProfile = QStringLiteral("auto");
    QString arsHost;
    quint16 arsPort = 5190;

    // AIM/OSCAR-specific BOS redirect overrides and diagnostic mode.
    // Secondary OSCAR service redirects always follow the server-advertised host.
    QString redirectHost;
    quint16 redirectPort = 0;
    // off = no OSCAR diagnostics; login = credential-safe login/bootstrap audit;
    // full = login audit plus credential-safe FLAP/SNAC wire tracing.
    QString oscarDebugMode = QStringLiteral("off");
    // Locally retained AIM profile. Some private/revival OSCAR servers keep
    // LOCATE profile data only for the current BOS session, so replay it after
    // each successful login when the user has explicitly saved one.
    QString oscarProfile;

    // IRC-specific settings.
    QString realName = appDefaultRealName();
    bool tls = false;
    // IRC buddy/watch names are stored locally and checked with standard ISON.
    QStringList ircBuddies;
    // SIP contacts are local WaffleHouse dial targets (not a server-side SIP buddy roster).
    QStringList sipContacts;

    // Telnet/MUD/BBS settings. username is used as an optional profile/session
    // label for Telnet connections rather than being transmitted automatically.
    QString telnetTerminalType = QStringLiteral("ANSI");
    int telnetColumns = 80;
    int telnetRows = 24;
    bool telnetAutoFit = true;

    // SIP/VoIP settings. SIP is a first-class saved WaffleHouse connection type.
    // username/password/savePassword are shared with the normal connection model.
    QString sipProfileName;
    QString sipDomain;
    QString sipRegistrar;
    QString sipAuthUsername;
    QString sipDisplayName;
    QString sipOutboundProxy;
    QString sipCallerIdDomain;
    QString sipDialPrefix;
    QString sipStunServer;
    QString sipTransport = QStringLiteral("udp");
    QString sipIdentityMode = QStringLiteral("from");
    QString sipCompatibility = QStringLiteral("auto");
    quint16 sipLocalPort = 5060;
    quint32 sipRegistrationExpires = 300;
    bool sipUseIce = false;
    bool sipEnableSrtp = false;

    bool debug = false;
};

class ChatBackend : public QObject {
    Q_OBJECT
public:
    explicit ChatBackend(ConnectionSettings settings, QObject *parent = nullptr);
    ~ChatBackend() override;

    const ConnectionSettings &settings() const { return m_settings; }
    void setPassword(const QString &password) { m_settings.password = password; }
    virtual void setConnectionSettings(const ConnectionSettings &settings);
    const QString &id() const { return m_id; }
    virtual QString protocolName() const = 0;

    virtual void start() = 0;
    virtual void stop() = 0;

    // Non-blocking first phase of application shutdown. This is used by the
    // terminal frontend so every worker can see the stop request before any
    // individual backend is joined/destroyed.
    void requestStop() { m_stopRequested.store(true); }

    // These methods must be safe to call from the GUI thread.
    virtual void sendPrivateMessage(const QString &target, const QString &message) = 0;
    virtual void joinRoom(const QString &room, bool privateRoom = false) = 0;
    virtual void sendRoomMessage(const QString &room, const QString &message) = 0;
    virtual void leaveRoom(const QString &room) = 0;

    virtual void changePassword(const QString &currentPassword,
                                const QString &newPassword);
    virtual void sendRaw(const QString &a,
                         const QString &b = QString(),
                         const QString &c = QString());
    virtual void changeNickname(const QString &newNick);
    virtual void addBuddy(const QString &name);
    virtual void removeBuddy(const QString &name);
    virtual void setTerminalSize(int columns, int rows);
    virtual void sendTerminalInput(const QByteArray &bytes);

signals:
    void eventReceived(const QString &kind,
                       const QString &target,
                       const QString &text);
    void membersChanged(const QString &room,
                        const QString &action,
                        const QStringList &names);
    void targetNamed(const QString &kind,
                     const QString &target,
                     const QString &displayName);
    void roomDiscovered(const QString &roomId,
                        const QString &displayName);
    void buddyListChanged(const QStringList &names);
    void buddyPresenceChanged(const QString &name,
                              bool online);
    void connected(const QString &identity,
                   const QString &endpoint);
    void disconnected(const QString &reason);
    void backendError(const QString &context,
                      const QString &message);

protected:
    ConnectionSettings m_settings;
    QString m_id;
    std::atomic_bool m_stopRequested{false};
};
