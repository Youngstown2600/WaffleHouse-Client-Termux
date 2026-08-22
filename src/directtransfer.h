#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <sodium.h>

class QFile;
class QTcpServer;
class QTcpSocket;

class CpxDirectTransferManager final : public QObject {
    Q_OBJECT
public:
    struct ListenResult {
        quint16 port = 0;
        QStringList hosts;
    };

    explicit CpxDirectTransferManager(QObject *parent = nullptr);
    ~CpxDirectTransferManager() override;

    bool prepareIncoming(const QString &transferId,
                         const QString &partPath,
                         qint64 totalBytes,
                         qint64 resumeOffset,
                         const QByteArray &key,
                         ListenResult &result,
                         QString *error = nullptr);

    bool startOutgoing(const QString &transferId,
                       const QString &path,
                       qint64 totalBytes,
                       qint64 resumeOffset,
                       const QStringList &hosts,
                       quint16 port,
                       const QByteArray &key,
                       QString *error = nullptr);

    void cancel(const QString &transferId);
    bool isActive(const QString &transferId) const;

signals:
    void progress(const QString &transferId, qint64 transferred, qint64 total, bool outgoing);
    void incomingFinished(const QString &transferId);
    void outgoingFinished(const QString &transferId);
    void failed(const QString &transferId, const QString &reason, bool outgoing);

private:
    struct IncomingSession {
        QString id;
        QString partPath;
        qint64 total = 0;
        qint64 received = 0;
        QByteArray key;
        QTcpServer *server = nullptr;
        QTcpSocket *socket = nullptr;
        QFile *file = nullptr;
        QByteArray inputBuffer;
        crypto_secretstream_xchacha20poly1305_state streamState{};
        bool streamReady = false;
        bool finished = false;
        bool cancelled = false;
    };

    struct OutgoingSession {
        QString id;
        QString path;
        qint64 total = 0;
        qint64 sent = 0;
        QByteArray key;
        QStringList hosts;
        quint16 port = 0;
        int hostIndex = -1;
        QTcpSocket *socket = nullptr;
        QFile *file = nullptr;
        crypto_secretstream_xchacha20poly1305_state streamState{};
        bool streamReady = false;
        bool finalQueued = false;
        bool finished = false;
        bool cancelled = false;
    };

    static QStringList localTransferHosts();
    static QByteArray encodeFrame(const QByteArray &ciphertext);
    static bool decodeLength(const QByteArray &buffer, quint32 &length);

    void acceptIncomingSocket(const QString &transferId);
    void readIncoming(const QString &transferId);
    void failIncoming(const QString &transferId, const QString &reason);
    void cleanupIncoming(const QString &transferId);

    void tryNextOutgoingHost(const QString &transferId);
    void outgoingConnected(const QString &transferId);
    void pumpOutgoing(const QString &transferId);
    void failOutgoing(const QString &transferId, const QString &reason);
    void cleanupOutgoing(const QString &transferId);

    QHash<QString, IncomingSession *> m_incoming;
    QHash<QString, OutgoingSession *> m_outgoing;
};
