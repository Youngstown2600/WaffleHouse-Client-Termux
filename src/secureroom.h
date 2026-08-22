#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

class SecureRoomManager {
public:
    enum class IncomingKind {
        NotSecureRoom,
        Decrypted,
        Error,
    };

    struct IncomingResult {
        IncomingKind kind = IncomingKind::NotSecureRoom;
        QString plaintext;
        QString notice;
        QString keyId;
    };

    bool initialize(QString *error = nullptr);

    bool createOrRotate(const QString &connectionId,
                        const QString &room,
                        QString *error = nullptr);
    bool hasRoom(const QString &connectionId, const QString &room) const;
    bool locallyOwned(const QString &connectionId, const QString &room) const;
    QString keyId(const QString &connectionId, const QString &room) const;

    QString keyOffer(const QString &connectionId,
                     const QString &room,
                     QString *error = nullptr) const;
    bool installKeyOffer(const QString &connectionId,
                         const QString &offer,
                         QString *room = nullptr,
                         QString *keyId = nullptr,
                         QString *error = nullptr);

    QString encrypt(const QString &connectionId,
                    const QString &room,
                    const QString &plaintext,
                    QString *error = nullptr) const;
    IncomingResult processIncoming(const QString &connectionId,
                                   const QString &room,
                                   const QString &payload);

    void closeRoom(const QString &connectionId, const QString &room);
    void closeConnection(const QString &connectionId);

    static bool looksLikeFrame(const QString &text);
    static bool looksLikeKeyOffer(const QString &text);

private:
    struct RoomState {
        QByteArray key;
        QString keyId;
        bool localOwner = false;
        QSet<QString> seenNonces;
        QStringList seenNonceOrder;
    };

    QString roomKey(const QString &connectionId, const QString &room) const;
    QByteArray associatedData(const QString &connectionId,
                              const QString &room,
                              const QString &keyId) const;
    void rememberNonce(RoomState &state, const QString &nonceToken);

    QHash<QString, RoomState> m_rooms;
    bool m_initialized = false;
};
