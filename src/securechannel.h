#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

class SecureChannelManager {
public:
    enum class IncomingKind {
        NotSecure,
        Control,
        Decrypted,
        Error,
    };

    struct IncomingResult {
        IncomingKind kind = IncomingKind::NotSecure;
        QString plaintext;
        QString notice;
        QString replyFrame;
        QString peerFingerprint;
        QStringList peerCapabilities;
        bool newlyEstablished = false;
        bool capabilitiesUpdated = false;
    };

    bool initialize(QString *error = nullptr);
    QString localFingerprint(const QString &connectionId) const;
    QString helloFrame(const QString &connectionId) const;
    QString capabilitiesFrame(const QString &connectionId, const QString &target);
    QStringList peerCapabilities(const QString &connectionId, const QString &target) const;
    bool peerSupports(const QString &connectionId, const QString &target, const QString &capability) const;
    QByteArray fileTransferKey(const QString &connectionId,
                               const QString &target,
                               const QString &transferId,
                               QString *error = nullptr) const;
    static QStringList localCapabilities();

    QString beginHandshake(const QString &connectionId,
                           const QString &target,
                           QString *notice = nullptr);
    IncomingResult processIncoming(const QString &connectionId,
                                   const QString &target,
                                   const QString &payload,
                                   bool autoReply);
    QString encrypt(const QString &connectionId,
                    const QString &target,
                    const QString &plaintext,
                    QString *error = nullptr);

    bool hasSession(const QString &connectionId, const QString &target) const;
    QString peerFingerprint(const QString &connectionId, const QString &target) const;
    void closeSession(const QString &connectionId, const QString &target);
    void closeConnection(const QString &connectionId);

    static bool looksLikeFrame(const QString &text);

private:
    struct Identity {
        QByteArray publicKey;
        QByteArray secretKey;
    };

    struct Session {
        QByteArray peerPublicKey;
        QByteArray rxKey;
        QByteArray txKey;
        QString fingerprint;
        QSet<QString> seenNonces;
        QStringList seenNonceOrder;
        QSet<QString> peerCapabilities;
        QSet<QString> pendingPeerCapabilities;
        bool established = false;
        bool helloSent = false;
        bool capabilitiesSent = false;
    };

    QString sessionKey(const QString &connectionId, const QString &target) const;
    bool deriveIdentity(const QString &connectionId, Identity &identity, QString *error = nullptr) const;
    bool deriveSession(const QString &connectionId,
                       Session &session,
                       const QByteArray &peerPublicKey,
                       QString *error = nullptr) const;
    QString fingerprintForKey(const QByteArray &publicKey) const;
    bool loadOrCreateMasterIdentity(QString *error);
    void rememberNonce(Session &session, const QString &nonceToken);

    QByteArray m_masterPublicKey;
    QByteArray m_masterSecretKey;
    QHash<QString, Session> m_sessions;
    bool m_initialized = false;
};
