#pragma once

#include "backend.h"
#include "oscarprotocol.h"

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QThread>

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

signals:
    void presenceChanged(const QString &state, const QString &message, quint32 idleSeconds);

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
    };

    struct Command {
        CommandType type;
        QString a;
        QString b;
        QString c;
        bool flag = false;
        quint32 number = 0;
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
                          bool addIcbmParams = false);
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

    QPair<QString, quint16> redirectEndpoint(const QString &advertised,
                                             quint16 fallbackPort) const;
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
};
