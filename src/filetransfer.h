#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

class CpxFileTransferManager {
public:
    enum class MessageType {
        Invalid,
        Offer,
        Accept,
        Fallback,
        Decline,
        Resume,
        Data,
        Ack,
        Done,
        Complete,
        Cancel,
    };

    enum class EventKind {
        None,
        OfferReceived,
        Accepted,
        Fallback,
        Declined,
        ResumeRequested,
        Progress,
        Completed,
        Cancelled,
        Error,
    };

    struct Message {
        MessageType type = MessageType::Invalid;
        QString id;
        QString fileName;
        qint64 size = 0;
        QByteArray sha256;
        qint64 offset = 0;
        QByteArray data;
        QString reason;
        bool reliable = false;
        bool direct = false;
        quint16 directPort = 0;
        QStringList directHosts;
    };

    struct Event {
        EventKind kind = EventKind::None;
        QString id;
        QString target;
        QString fileName;
        QString path;
        QString reason;
        QString replyPayload;
        qint64 transferred = 0;
        qint64 total = 0;
        int percent = -1;
        bool outgoing = false;
        bool direct = false;
        quint16 directPort = 0;
        QStringList directHosts;
    };

    struct TransferInfo {
        QString id;
        QString target;
        QString fileName;
        QString path;
        qint64 transferred = 0;
        qint64 total = 0;
        bool outgoing = false;
        bool accepted = false;
        bool complete = false;
        bool direct = false;
        bool resumable = false;
        QString status;
    };

    static bool looksLikeMessage(const QString &plaintext);
    static bool parseMessage(const QString &plaintext, Message &message, QString *error = nullptr);
    static QString sanitizeFileName(const QString &name);

    bool createOffer(const QString &target,
                     const QString &path,
                     QString &transferId,
                     QString &payload,
                     QString *error = nullptr,
                     bool reliable = false);

    Event processIncoming(const QString &target, const QString &payload);

    QString acceptIncoming(const QString &transferId,
                           const QString &destinationPath,
                           QString *error = nullptr,
                           quint16 directPort = 0,
                           const QStringList &directHosts = {});
    QString declineIncoming(const QString &transferId,
                            const QString &reason = QStringLiteral("declined"));
    QString cancel(const QString &transferId,
                   const QString &reason = QStringLiteral("cancelled"));
    QString requestResume(const QString &transferId, QString *error = nullptr);
    QString resumeIncoming(const QString &transferId,
                           QString *error = nullptr,
                           quint16 directPort = 0,
                           const QStringList &directHosts = {});
    bool canResume(const QString &transferId) const;
    bool clearTransfer(const QString &transferId, QString *error = nullptr);
    QString requestOutgoingRelayFallback(const QString &transferId,
                                         const QString &reason);
    QString fallbackIncomingToRelay(const QString &transferId);
    void updateDirectProgress(const QString &transferId, qint64 transferred, bool outgoing);
    bool finalizeIncomingDirect(const QString &transferId, QString *error = nullptr);
    QString completionPayload(const QString &transferId) const;
    void markOutgoingDirectSent(const QString &transferId);

    QString nextOutgoingPayload(const QString &transferId,
                                int rawChunkBytes,
                                bool *finished = nullptr,
                                QString *error = nullptr,
                                int minimumSendIntervalMs = 0);

    QStringList activeOutgoingIds() const;
    QList<TransferInfo> transfers() const;
    TransferInfo transfer(const QString &transferId) const;

private:
    struct Outgoing {
        QString id;
        QString target;
        QString path;
        QString fileName;
        qint64 size = 0;
        QByteArray sha256;
        qint64 offset = 0;
        QByteArray inFlightData;
        qint64 inFlightOffset = 0;
        qint64 lastPayloadSentMs = 0;
        int retryCount = 0;
        int doneRetryCount = 0;
        bool reliable = false;
        bool direct = false;
        quint16 directPort = 0;
        QStringList directHosts;
        bool accepted = false;
        bool doneSent = false;
        bool complete = false;
        QString status = QStringLiteral("offered");
    };

    struct Incoming {
        QString id;
        QString target;
        QString fileName;
        QString destinationPath;
        QString partPath;
        qint64 size = 0;
        QByteArray sha256;
        qint64 received = 0;
        bool reliable = false;
        bool direct = false;
        bool accepted = false;
        bool complete = false;
        int lastReportedPercent = -1;
        QString status = QStringLiteral("offered");
    };

    static QString encodeName(const QString &value);
    static QString decodeName(const QString &value);
    static QByteArray sha256File(const QString &path, QString *error = nullptr);
    static QString makePayload(const QStringList &parts);
    static int progressPercent(qint64 value, qint64 total);

    QHash<QString, Outgoing> m_outgoing;
    QHash<QString, Incoming> m_incoming;
};
