#pragma once

#include "backend.h"
#include "oscarprotocol.h"

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QThread>
#include <QVariantMap>

#include <memory>

class OscarBackend : public ChatBackend {
    Q_OBJECT
public:
    explicit OscarBackend(ConnectionSettings settings, QObject *parent = nullptr);
    ~OscarBackend() override;

    QString protocolName() const override { return QStringLiteral("AIM/OSCAR"); }

    void start() override;
    void stop() override;
    void sendPrivateMessage(const QString &target, const QString &message) override;
    void joinRoom(const QString &room, bool privateRoom = false) override;
    void sendRoomMessage(const QString &room, const QString &message) override;
    void leaveRoom(const QString &room) override;
    void changePassword(const QString &currentPassword,
                        const QString &newPassword) override;
    void sendRaw(const QString &family,
                 const QString &subtype = QString(),
                 const QString &hexBody = QString()) override;
    void addBuddy(const QString &name) override;
    void removeBuddy(const QString &name) override;

    // Native AIM/OSCAR presence controls. These enqueue work onto the OSCAR
    // worker thread just like IM/chat operations.
    void setAwayMessage(const QString &message);
    void setAfkMessage(const QString &message);
    void setIdleSeconds(quint32 seconds);
    void setBack();
    void requestClientVersion(const QString &target);
    void setProfile(const QString &profile);
    void refreshServerCapabilities();
    void requestUserInfo(const QString &target);

    // Persistent, redacted OSCAR audit/wire log for this account.
    // The file is created only when Login Audit or Full Wire Trace is enabled.
    QString auditLogPath() const;

    // OSCAR 3.4r1 full user feature surface.  Every operation is checked
    // against the foodgroups advertised by OSERVICE__HOST_ONLINE before it is
    // placed on the worker queue.  GUI/CLI callers can use supportsFamily()
    // to grey out commands the current server does not provide.
    bool supportsFamily(quint16 family) const;
    bool peerAdvertisesCapability(const QString &target, const QByteArray &capability) const;
    void requestDirectoryInfo(const QString &target);
    void setDirectoryInfo(const QVariantMap &fields);
    void findByEmail(const QString &email);
    void inviteByEmail(const QString &email, const QString &message);
    void addPermit(const QString &target);
    void removePermit(const QString &target);
    void addDeny(const QString &target);
    void removeDeny(const QString &target);
    void addTemporaryPermit(const QString &target);
    void removeTemporaryPermit(const QString &target);
    void requestAuthorization(const QString &target, const QString &message);
    void respondAuthorization(const QString &target, bool accept, const QString &message = QString());
    void preAuthorize(const QString &target, const QString &message = QString());
    void removeMeFromBuddyList(const QString &target);
    void addTemporaryBuddy(const QString &target);
    void removeTemporaryBuddy(const QString &target);
    void requestWatcherList();
    void retrieveStoredMessages();
    void sendTypingNotification(const QString &target, quint16 event);
    void requestAccountInfo();
    void changeAccountEmail(const QString &email);
    void changeFormattedScreenName(const QString &formattedName);
    void confirmAccount();
    void deleteAccount();
    void setPrivacyFlags(quint32 flags);

    // WaffleHouse voice uses OSCAR channel-2 rendezvous for call signaling and
    // a WaffleHouse-specific PCM/UDP media payload.  This deliberately does
    // not advertise compatibility with the proprietary legacy AIM Talk codec.
    void proposeVoice(const QString &target,
                      const QString &cookieHex,
                      const QString &localAddress,
                      quint16 localPort,
                      int sampleRate);
    void acceptVoice(const QString &target,
                     const QString &cookieHex,
                     const QString &localAddress,
                     quint16 localPort,
                     int sampleRate);
    void cancelVoice(const QString &target,
                     const QString &cookieHex,
                     quint16 reason = 0x0001);

signals:
    void presenceChanged(const QString &state, const QString &message, quint32 idleSeconds);
    void serverCapabilitiesChanged(const QStringList &features,
                                   const QStringList &familyIds,
                                   bool profileSupported,
                                   int maxProfileLength);
    void profileChanged(const QString &profile);
    // Rich native presence from BUDDY__ARRIVED/DEPARTED notifications.
    // The generic ChatBackend signal remains for protocol-neutral online/offline.
    void buddyNativePresenceChanged(const QString &name, const QVariantMap &presence);
    void userInfoReceived(const QString &target, const QVariantMap &info);
    void directoryInfoReceived(const QString &target, const QVariantMap &info);
    void lookupResultsReceived(const QString &query, const QStringList &results);
    void accountInfoReceived(const QVariantMap &info);
    void watcherListReceived(const QStringList &users);
    void authorizationRequestReceived(const QString &from, const QString &message);
    void authorizationResponseReceived(const QString &from, bool accepted, const QString &message);
    void buddyAddedYou(const QString &from);
    void typingNotificationReceived(const QString &from, quint16 event);
    void oscarNoticeReceived(const QString &kind, const QString &text);
    void featureOperationResult(const QString &operation, bool success, const QString &message);
    void voiceInviteReceived(const QString &from,
                             const QString &cookieHex,
                             const QString &remoteAddress,
                             quint16 remotePort,
                             int sampleRate,
                             int channels,
                             const QString &invitation);
    void voiceInviteAccepted(const QString &from,
                             const QString &cookieHex,
                             const QString &remoteAddress,
                             quint16 remotePort,
                             int sampleRate,
                             int channels);
    void voiceInviteCancelled(const QString &from,
                              const QString &cookieHex,
                              quint16 reason);
    void legacyVoiceInviteReceived(const QString &from, const QString &cookieHex);

private:
    enum class CommandType {
        SendIm,
        JoinRoom,
        SendRoom,
        LeaveRoom,
        Password,
        Raw,
        AddBuddy,
        RemoveBuddy,
        SetAway,
        SetAfk,
        SetIdle,
        SetBack,
        SetProfile,
        RefreshCapabilities,
        RequestUserInfo,
        RequestDirectoryInfo,
        SetDirectoryInfo,
        FindByEmail,
        InviteByEmail,
        PrivacyListAction,
        AuthorizationRequest,
        AuthorizationResponse,
        PreAuthorize,
        RemoveMe,
        TemporaryBuddy,
        WatcherList,
        RetrieveStoredMessages,
        TypingNotification,
        RequestAccountInfo,
        ChangeAccountEmail,
        ChangeFormattedName,
        ConfirmAccount,
        DeleteAccount,
        SetPrivacyFlags,
        VoicePropose,
        VoiceAccept,
        VoiceCancel,
    };

    struct Command {
        CommandType type{};
        QString a{};
        QString b{};
        QString c{};
        bool flag = false;
        quint32 number = 0;
        QString d{};
        quint32 number2 = 0;
        quint32 number3 = 0;
        QVariantMap map{};
    };

    struct ChatSession {
        Oscar::RoomInfo room;
        std::unique_ptr<Oscar::FlapConnection> connection;
    };

    struct ServiceRedirect {
        QString host;
        quint16 port = 0;
        QByteArray cookie;
    };

    struct FeedbagItem {
        QString name;
        quint16 groupId = 0;
        quint16 itemId = 0;
        quint16 classId = 0;
        QByteArray data;
    };

    void enqueue(Command command);
    QList<Command> takeCommands();
    void run();

    QPair<QString, QByteArray> authenticate();
    void expectGreeting(Oscar::FlapConnection &connection);
    void bootstrapService(Oscar::FlapConnection &connection,
                          const QByteArray &cookie,
                          bool bosBootstrap = false);
    ServiceRedirect requestService(quint16 family,
                                   const QList<Oscar::Tlv> &extraTlvs = {});
    Oscar::FlapConnection &ensureChatNav();
    Oscar::Snac request(Oscar::FlapConnection &connection,
                        quint16 family,
                        quint16 subtype,
                        const QByteArray &body,
                        int timeoutMs = 10000);

    void processCommand(const Command &command);
    void processIncoming(Oscar::FlapConnection &connection);
    void dispatchSnac(Oscar::FlapConnection &connection,
                      const Oscar::Snac &snac);
    void dispatchBos(const Oscar::Snac &snac);
    void dispatchChat(Oscar::FlapConnection &connection,
                      const Oscar::Snac &snac);

    void doSendIm(const QString &recipient, const QString &message, bool echo = true);
    void doJoinRoom(const QString &name, bool privateRoom);
    void doSendRoom(const QString &name, const QString &message);
    void doLeaveRoom(const QString &name);
    void doChangePassword(const QString &currentPassword,
                          const QString &newPassword);
    void doRaw(const QString &family,
               const QString &subtype,
               const QString &hexBody);
    void doAddBuddy(const QString &name);
    void doRemoveBuddy(const QString &name);
    void doSetAway(const QString &message, bool afk);
    void doSetIdle(quint32 seconds);
    void doSetBack();
    void doSetProfile(const QString &profile);
    void doRequestUserInfo(const QString &target);
    void doRequestDirectoryInfo(const QString &target);
    void doSetDirectoryInfo(const QVariantMap &fields);
    void doFindByEmail(const QString &email);
    void doInviteByEmail(const QString &email, const QString &message);
    void doPrivacyListAction(quint16 subtype, const QString &target, const QString &label);
    void doAuthorizationRequest(const QString &target, const QString &message);
    void doAuthorizationResponse(const QString &target, bool accept, const QString &message);
    void doPreAuthorize(const QString &target, const QString &message);
    void doRemoveMe(const QString &target);
    void doTemporaryBuddy(const QString &target, bool add);
    void doRequestWatcherList();
    void doRetrieveStoredMessages();
    void doTypingNotification(const QString &target, quint16 event);
    void doRequestAccountInfo();
    void doChangeAccountEmail(const QString &email);
    void doChangeFormattedName(const QString &formattedName);
    void doConfirmAccount();
    void doDeleteAccount();
    void doSetPrivacyFlags(quint32 flags);
    void doVoiceRendezvous(quint16 messageType,
                           const QString &target,
                           const QString &cookieHex,
                           const QString &localAddress,
                           quint16 localPort,
                           int sampleRate,
                           quint16 cancelReason = 0);
    void advertiseClientCapabilities();
    void discoverBosCapabilities();
    QString fetchOwnProfile();
    void emitPresence();
    void loadBuddyList();
    QList<FeedbagItem> parseFeedbagItems(const QByteArray &body) const;
    QStringList feedbagBuddyNames() const;
    QByteArray marshalFeedbagItem(const FeedbagItem &item) const;
    QByteArray marshalFeedbagItems(const QList<FeedbagItem> &items) const;
    QByteArray withGroupMembers(const QByteArray &data, const QList<quint16> &itemIds) const;
    quint16 nextFeedbagGroupId() const;
    quint16 nextFeedbagItemId(quint16 groupId) const;
    void checkFeedbagAck(const Oscar::Snac &reply, int expectedItems, const QString &operation) const;
    void persistFeedbagChanges(const QList<FeedbagItem> &adds,
                               const QList<FeedbagItem> &mods,
                               const QList<FeedbagItem> &dels);

    QPair<QString, quint16> bosRedirectEndpoint(const QString &advertised,
                                                quint16 fallbackPort) const;
    QPair<QString, quint16> serviceRedirectEndpoint(const QString &advertised,
                                                    quint16 fallbackPort = 5190) const;
    void protocolLog(const QString &text);
    [[noreturn]] void fail(const QString &message) const;

    QThread *m_thread = nullptr;
    QMutex m_commandMutex;
    QQueue<Command> m_commands;

    std::unique_ptr<Oscar::FlapConnection> m_bos;
    std::unique_ptr<Oscar::FlapConnection> m_chatNav;
    QHash<QString, std::shared_ptr<ChatSession>> m_chats;
    QSet<QString> m_buddies;
    QList<FeedbagItem> m_feedbagItems;

    QString m_presenceState = QStringLiteral("ONLINE");
    QString m_presenceMessage;
    quint32 m_idleSeconds = 0;
    QHash<QString, QString> m_peerClientHints;
    mutable QMutex m_capabilityMutex;
    QSet<quint16> m_serverFamilies;
    QHash<QString, QSet<QByteArray>> m_peerCapabilities;
    int m_maxProfileLength = 0;
};
