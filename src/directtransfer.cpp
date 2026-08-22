#include "directtransfer.h"

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <algorithm>

namespace {
constexpr int ConnectTimeoutMs = 5000;
constexpr qint64 SocketHighWaterBytes = 512 * 1024;
constexpr int PlainChunkBytes = 64 * 1024;
constexpr quint32 MaxCipherFrameBytes = PlainChunkBytes
    + crypto_secretstream_xchacha20poly1305_ABYTES + 1024;
const QByteArray HandshakePrefix("CPXDIR1 ");

QString b64url(const QByteArray &value)
{
    return QString::fromLatin1(value.toBase64(QByteArray::Base64UrlEncoding
                                               | QByteArray::OmitTrailingEquals));
}

QByteArray fromB64url(const QString &value)
{
    return QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding);
}
}

CpxDirectTransferManager::CpxDirectTransferManager(QObject *parent)
    : QObject(parent)
{
}

CpxDirectTransferManager::~CpxDirectTransferManager()
{
    const auto incomingIds = m_incoming.keys();
    for (const QString &id : incomingIds) cancel(id);
    const auto outgoingIds = m_outgoing.keys();
    for (const QString &id : outgoingIds) cancel(id);
}

QStringList CpxDirectTransferManager::localTransferHosts()
{
    QStringList ipv4;
    QStringList ipv6;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.isNull() || address.isLoopback() || address.isLinkLocal()) continue;
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                ipv4 << address.toString();
            } else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
                ipv6 << address.toString();
            }
        }
    }
    ipv4.removeDuplicates();
    ipv6.removeDuplicates();
    QStringList result = ipv4;
    result.append(ipv6);
    if (result.isEmpty()) result << QStringLiteral("127.0.0.1");
    // Keep the encrypted ACCEPTD control frame small enough for IRC relays.
    if (result.size() > 3) result = result.mid(0, 3);
    return result;
}

QByteArray CpxDirectTransferManager::encodeFrame(const QByteArray &ciphertext)
{
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(ciphertext.size()),
                          reinterpret_cast<uchar *>(frame.data()));
    frame.append(ciphertext);
    return frame;
}

bool CpxDirectTransferManager::decodeLength(const QByteArray &buffer, quint32 &length)
{
    if (buffer.size() < 4) return false;
    length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
    return true;
}

bool CpxDirectTransferManager::prepareIncoming(const QString &transferId,
                                               const QString &partPath,
                                               qint64 totalBytes,
                                               qint64 resumeOffset,
                                               const QByteArray &key,
                                               ListenResult &result,
                                               QString *error)
{
    result = {};
    if (transferId.trimmed().isEmpty() || partPath.trimmed().isEmpty()
        || totalBytes < 0 || resumeOffset < 0 || resumeOffset > totalBytes
        || key.size() != crypto_secretstream_xchacha20poly1305_KEYBYTES) {
        if (error) *error = QStringLiteral("invalid direct-transfer listener parameters");
        return false;
    }
    cancel(transferId);

    auto *session = new IncomingSession;
    session->id = transferId;
    session->partPath = partPath;
    session->total = totalBytes;
    session->received = resumeOffset;
    session->key = key;
    session->file = new QFile(partPath, this);
    if (!session->file->open(QIODevice::ReadWrite)) {
        if (error) *error = QStringLiteral("cannot open partial file %1: %2")
                                .arg(partPath, session->file->errorString());
        delete session->file;
        delete session;
        return false;
    }
    if (!session->file->seek(resumeOffset)) {
        if (error) *error = QStringLiteral("cannot seek partial file %1 to resume offset %2")
                                .arg(partPath).arg(resumeOffset);
        session->file->close();
        delete session->file;
        delete session;
        return false;
    }

    session->server = new QTcpServer(this);
    if (!session->server->listen(QHostAddress::Any, 0)) {
        if (error) *error = QStringLiteral("cannot open direct-transfer listener: %1")
                                .arg(session->server->errorString());
        session->file->close();
        delete session->file;
        delete session->server;
        delete session;
        return false;
    }
    m_incoming.insert(transferId, session);
    connect(session->server, &QTcpServer::newConnection, this, [this, transferId]() {
        acceptIncomingSocket(transferId);
    });

    result.port = session->server->serverPort();
    result.hosts = localTransferHosts();
    return true;
}

void CpxDirectTransferManager::acceptIncomingSocket(const QString &transferId)
{
    IncomingSession *session = m_incoming.value(transferId, nullptr);
    if (!session || !session->server) return;
    while (session->server->hasPendingConnections()) {
        QTcpSocket *socket = session->server->nextPendingConnection();
        if (!socket) continue;
        if (session->socket) {
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        session->socket = socket;
        session->server->close();
        connect(socket, &QTcpSocket::readyRead, this, [this, transferId]() {
            readIncoming(transferId);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, transferId]() {
            IncomingSession *s = m_incoming.value(transferId, nullptr);
            if (s && !s->finished && !s->cancelled) {
                failIncoming(transferId, QStringLiteral("direct data connection closed before transfer completion"));
            }
        });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, transferId](QAbstractSocket::SocketError) {
            IncomingSession *s = m_incoming.value(transferId, nullptr);
            if (s && !s->finished && !s->cancelled && s->socket) {
                failIncoming(transferId,
                             QStringLiteral("direct data connection error: %1")
                                 .arg(s->socket->errorString()));
            }
        });
    }
}

void CpxDirectTransferManager::readIncoming(const QString &transferId)
{
    IncomingSession *session = m_incoming.value(transferId, nullptr);
    if (!session || !session->socket || session->cancelled || session->finished) return;
    session->inputBuffer.append(session->socket->readAll());

    if (!session->streamReady) {
        const int newline = session->inputBuffer.indexOf('\n');
        if (newline < 0) {
            if (session->inputBuffer.size() > 2048) {
                failIncoming(transferId, QStringLiteral("invalid direct-transfer handshake"));
            }
            return;
        }
        const QByteArray line = session->inputBuffer.left(newline);
        session->inputBuffer.remove(0, newline + 1);
        if (!line.startsWith(HandshakePrefix)) {
            failIncoming(transferId, QStringLiteral("invalid direct-transfer handshake magic"));
            return;
        }
        const QList<QByteArray> fields = line.mid(HandshakePrefix.size()).split(' ');
        if (fields.size() != 3) {
            failIncoming(transferId, QStringLiteral("malformed direct-transfer handshake"));
            return;
        }
        const QString id = QString::fromUtf8(fields.at(0));
        bool ok = false;
        const qint64 offset = fields.at(1).toLongLong(&ok);
        const QByteArray header = fromB64url(QString::fromLatin1(fields.at(2)));
        if (id != session->id || !ok || offset != session->received
            || header.size() != crypto_secretstream_xchacha20poly1305_HEADERBYTES
            || crypto_secretstream_xchacha20poly1305_init_pull(
                   &session->streamState,
                   reinterpret_cast<const unsigned char *>(header.constData()),
                   reinterpret_cast<const unsigned char *>(session->key.constData())) != 0) {
            failIncoming(transferId, QStringLiteral("direct-transfer handshake authentication failed"));
            return;
        }
        session->streamReady = true;
    }

    while (session->streamReady) {
        quint32 cipherLength = 0;
        if (!decodeLength(session->inputBuffer, cipherLength)) return;
        if (cipherLength < crypto_secretstream_xchacha20poly1305_ABYTES
            || cipherLength > MaxCipherFrameBytes) {
            failIncoming(transferId, QStringLiteral("invalid direct-transfer frame length"));
            return;
        }
        if (session->inputBuffer.size() < 4 + static_cast<int>(cipherLength)) return;
        const QByteArray cipher = session->inputBuffer.mid(4, static_cast<int>(cipherLength));
        session->inputBuffer.remove(0, 4 + static_cast<int>(cipherLength));

        QByteArray plain(static_cast<int>(cipherLength), '\0');
        unsigned long long plainLength = 0;
        unsigned char tag = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(
                &session->streamState,
                reinterpret_cast<unsigned char *>(plain.data()), &plainLength, &tag,
                reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size(),
                nullptr, 0) != 0) {
            failIncoming(transferId, QStringLiteral("direct-transfer frame authentication failed"));
            return;
        }
        plain.resize(static_cast<int>(plainLength));
        if (session->received + plain.size() > session->total) {
            failIncoming(transferId, QStringLiteral("direct-transfer payload exceeded advertised file size"));
            return;
        }
        if (!plain.isEmpty() && session->file->write(plain) != plain.size()) {
            failIncoming(transferId, QStringLiteral("could not write direct-transfer data: %1")
                                         .arg(session->file->errorString()));
            return;
        }
        session->received += plain.size();
        emit progress(transferId, session->received, session->total, false);

        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            if (session->received != session->total) {
                failIncoming(transferId,
                             QStringLiteral("direct transfer ended early (%1 of %2 bytes)")
                                 .arg(session->received).arg(session->total));
                return;
            }
            session->file->flush();
            session->file->close();
            session->finished = true;
            if (session->socket) session->socket->disconnectFromHost();
            emit incomingFinished(transferId);
            QTimer::singleShot(0, this, [this, transferId]() { cleanupIncoming(transferId); });
            return;
        }
        if (tag != crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) {
            failIncoming(transferId, QStringLiteral("unsupported direct-transfer stream tag"));
            return;
        }
    }
}

bool CpxDirectTransferManager::startOutgoing(const QString &transferId,
                                             const QString &path,
                                             qint64 totalBytes,
                                             qint64 resumeOffset,
                                             const QStringList &hosts,
                                             quint16 port,
                                             const QByteArray &key,
                                             QString *error)
{
    if (transferId.trimmed().isEmpty() || hosts.isEmpty() || port == 0
        || totalBytes < 0 || resumeOffset < 0 || resumeOffset > totalBytes
        || key.size() != crypto_secretstream_xchacha20poly1305_KEYBYTES) {
        if (error) *error = QStringLiteral("invalid outgoing direct-transfer parameters");
        return false;
    }
    QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable() || info.size() != totalBytes) {
        if (error) *error = QStringLiteral("outgoing file is no longer readable or changed size");
        return false;
    }
    cancel(transferId);

    auto *session = new OutgoingSession;
    session->id = transferId;
    session->path = info.absoluteFilePath();
    session->total = totalBytes;
    session->sent = resumeOffset;
    session->key = key;
    session->hosts = hosts;
    session->port = port;
    session->file = new QFile(session->path, this);
    if (!session->file->open(QIODevice::ReadOnly) || !session->file->seek(resumeOffset)) {
        if (error) *error = QStringLiteral("cannot open outgoing file for direct transfer: %1")
                                .arg(session->file->errorString());
        if (session->file->isOpen()) session->file->close();
        delete session->file;
        delete session;
        return false;
    }
    m_outgoing.insert(transferId, session);
    tryNextOutgoingHost(transferId);
    return true;
}

void CpxDirectTransferManager::tryNextOutgoingHost(const QString &transferId)
{
    OutgoingSession *session = m_outgoing.value(transferId, nullptr);
    if (!session || session->cancelled || session->finished) return;

    ++session->hostIndex;
    if (session->hostIndex >= session->hosts.size()) {
        failOutgoing(transferId, QStringLiteral("could not establish a direct connection to any advertised peer address"));
        return;
    }
    if (session->socket) {
        session->socket->abort();
        session->socket->deleteLater();
    }
    session->socket = new QTcpSocket(this);
    QTcpSocket *socket = session->socket;
    connect(socket, &QTcpSocket::connected, this, [this, transferId]() {
        outgoingConnected(transferId);
    });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, transferId](qint64) {
        pumpOutgoing(transferId);
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, transferId, socket](QAbstractSocket::SocketError) {
        OutgoingSession *s = m_outgoing.value(transferId, nullptr);
        if (!s || s->cancelled || s->finished || s->socket != socket || s->streamReady) return;
        QTimer::singleShot(0, this, [this, transferId]() { tryNextOutgoingHost(transferId); });
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, transferId, socket]() {
        OutgoingSession *s = m_outgoing.value(transferId, nullptr);
        if (!s || s->cancelled || s->finished || s->socket != socket) return;
        if (s->finalQueued && socket->bytesToWrite() == 0) {
            s->finished = true;
            emit outgoingFinished(transferId);
            QTimer::singleShot(0, this, [this, transferId]() { cleanupOutgoing(transferId); });
        } else if (s->streamReady) {
            failOutgoing(transferId, QStringLiteral("direct data connection closed before the file was fully transmitted"));
        }
    });

    socket->connectToHost(session->hosts.at(session->hostIndex), session->port);
    QTimer::singleShot(ConnectTimeoutMs, this, [this, transferId, socket]() {
        OutgoingSession *s = m_outgoing.value(transferId, nullptr);
        if (!s || s->cancelled || s->finished || s->socket != socket || s->streamReady) return;
        if (socket->state() != QAbstractSocket::ConnectedState) {
            socket->abort();
            tryNextOutgoingHost(transferId);
        }
    });
}

void CpxDirectTransferManager::outgoingConnected(const QString &transferId)
{
    OutgoingSession *session = m_outgoing.value(transferId, nullptr);
    if (!session || !session->socket || session->cancelled || session->finished) return;

    QByteArray header(crypto_secretstream_xchacha20poly1305_HEADERBYTES, '\0');
    if (crypto_secretstream_xchacha20poly1305_init_push(
            &session->streamState,
            reinterpret_cast<unsigned char *>(header.data()),
            reinterpret_cast<const unsigned char *>(session->key.constData())) != 0) {
        failOutgoing(transferId, QStringLiteral("could not initialize encrypted direct-transfer stream"));
        return;
    }
    session->streamReady = true;
    const QByteArray handshake = HandshakePrefix
        + transferId.toUtf8() + ' '
        + QByteArray::number(session->sent) + ' '
        + b64url(header).toLatin1() + '\n';
    if (session->socket->write(handshake) != handshake.size()) {
        failOutgoing(transferId, QStringLiteral("could not write direct-transfer handshake"));
        return;
    }
    pumpOutgoing(transferId);
}

void CpxDirectTransferManager::pumpOutgoing(const QString &transferId)
{
    OutgoingSession *session = m_outgoing.value(transferId, nullptr);
    if (!session || !session->socket || !session->streamReady
        || session->cancelled || session->finished || session->finalQueued) return;

    while (session->socket->bytesToWrite() < SocketHighWaterBytes && !session->finalQueued) {
        QByteArray plain;
        if (session->sent < session->total) {
            plain = session->file->read(std::min<qint64>(PlainChunkBytes,
                                                        session->total - session->sent));
            if (plain.isEmpty()) {
                failOutgoing(transferId, QStringLiteral("unexpected end of file during direct transfer"));
                return;
            }
        }
        const bool final = session->sent + plain.size() >= session->total;
        QByteArray cipher(plain.size() + crypto_secretstream_xchacha20poly1305_ABYTES, '\0');
        unsigned long long cipherLength = 0;
        const unsigned char tag = final
            ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
            : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;
        if (crypto_secretstream_xchacha20poly1305_push(
                &session->streamState,
                reinterpret_cast<unsigned char *>(cipher.data()), &cipherLength,
                reinterpret_cast<const unsigned char *>(plain.constData()), plain.size(),
                nullptr, 0, tag) != 0) {
            failOutgoing(transferId, QStringLiteral("could not encrypt direct-transfer frame"));
            return;
        }
        cipher.resize(static_cast<int>(cipherLength));
        const QByteArray frame = encodeFrame(cipher);
        if (session->socket->write(frame) != frame.size()) {
            failOutgoing(transferId, QStringLiteral("could not queue direct-transfer frame"));
            return;
        }
        session->sent += plain.size();
        emit progress(transferId, session->sent, session->total, true);
        if (final) {
            session->finalQueued = true;
            session->file->close();
            break;
        }
    }

    if (session->finalQueued && session->socket->bytesToWrite() == 0) {
        session->socket->disconnectFromHost();
        if (session->socket->state() == QAbstractSocket::UnconnectedState) {
            session->finished = true;
            emit outgoingFinished(transferId);
            QTimer::singleShot(0, this, [this, transferId]() { cleanupOutgoing(transferId); });
        }
    }
}

void CpxDirectTransferManager::failIncoming(const QString &transferId, const QString &reason)
{
    IncomingSession *session = m_incoming.value(transferId, nullptr);
    if (!session || session->cancelled || session->finished) return;
    session->finished = true;
    if (session->file && session->file->isOpen()) session->file->flush();
    emit failed(transferId, reason, false);
    QTimer::singleShot(0, this, [this, transferId]() { cleanupIncoming(transferId); });
}

void CpxDirectTransferManager::failOutgoing(const QString &transferId, const QString &reason)
{
    OutgoingSession *session = m_outgoing.value(transferId, nullptr);
    if (!session || session->cancelled || session->finished) return;
    session->finished = true;
    emit failed(transferId, reason, true);
    QTimer::singleShot(0, this, [this, transferId]() { cleanupOutgoing(transferId); });
}

void CpxDirectTransferManager::cleanupIncoming(const QString &transferId)
{
    IncomingSession *session = m_incoming.take(transferId);
    if (!session) return;
    if (session->socket) {
        session->socket->abort();
        session->socket->deleteLater();
    }
    if (session->server) {
        session->server->close();
        session->server->deleteLater();
    }
    if (session->file) {
        if (session->file->isOpen()) session->file->close();
        session->file->deleteLater();
    }
    sodium_memzero(session->key.data(), static_cast<size_t>(session->key.size()));
    delete session;
}

void CpxDirectTransferManager::cleanupOutgoing(const QString &transferId)
{
    OutgoingSession *session = m_outgoing.take(transferId);
    if (!session) return;
    if (session->socket) {
        session->socket->abort();
        session->socket->deleteLater();
    }
    if (session->file) {
        if (session->file->isOpen()) session->file->close();
        session->file->deleteLater();
    }
    sodium_memzero(session->key.data(), static_cast<size_t>(session->key.size()));
    delete session;
}

void CpxDirectTransferManager::cancel(const QString &transferId)
{
    if (IncomingSession *session = m_incoming.value(transferId, nullptr)) {
        session->cancelled = true;
        cleanupIncoming(transferId);
    }
    if (OutgoingSession *session = m_outgoing.value(transferId, nullptr)) {
        session->cancelled = true;
        cleanupOutgoing(transferId);
    }
}

bool CpxDirectTransferManager::isActive(const QString &transferId) const
{
    return m_incoming.contains(transferId) || m_outgoing.contains(transferId);
}
