#include "filetransfer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>

namespace {
QString prefix()
{
    return QString(QChar(0x1e)) + QStringLiteral("CPXFILE1|");
}

QString b64url(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding
                                              | QByteArray::OmitTrailingEquals));
}

QByteArray fromB64url(const QString &text)
{
    return QByteArray::fromBase64(text.toLatin1(), QByteArray::Base64UrlEncoding);
}

constexpr qint64 MaxAdvertisedFileSize = 1024LL * 1024LL * 1024LL; // 1 GiB guardrail.
constexpr qint64 AckRetryMs = 5000;
constexpr int MaxChunkRetries = 12;
} // namespace

bool CpxFileTransferManager::looksLikeMessage(const QString &plaintext)
{
    return plaintext.startsWith(prefix());
}

QString CpxFileTransferManager::encodeName(const QString &value)
{
    return b64url(value.toUtf8());
}

QString CpxFileTransferManager::decodeName(const QString &value)
{
    return QString::fromUtf8(fromB64url(value));
}

QString CpxFileTransferManager::makePayload(const QStringList &parts)
{
    return prefix() + parts.join(QLatin1Char('|'));
}

QString CpxFileTransferManager::sanitizeFileName(const QString &name)
{
    QString clean = QFileInfo(name).fileName().trimmed();
    if (clean.isEmpty() || clean == QStringLiteral(".") || clean == QStringLiteral("..")) {
        clean = QStringLiteral("received-file.bin");
    }
    clean.replace(QChar(0), QLatin1Char('_'));
    return clean;
}

QByteArray CpxFileTransferManager::sha256File(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(256 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            if (error) *error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
            return {};
        }
        hash.addData(block);
    }
    return hash.result();
}

int CpxFileTransferManager::progressPercent(qint64 value, qint64 total)
{
    if (total <= 0) return 100;
    return static_cast<int>(std::clamp<qint64>((value * 100) / total, 0, 100));
}

bool CpxFileTransferManager::parseMessage(const QString &plaintext,
                                          Message &message,
                                          QString *error)
{
    message = {};
    if (!looksLikeMessage(plaintext)) {
        if (error) *error = QStringLiteral("not a CPX file-transfer message");
        return false;
    }
    const QString body = plaintext.mid(prefix().size());
    const QStringList parts = body.split(QLatin1Char('|'), Qt::KeepEmptyParts);
    if (parts.size() < 2) {
        if (error) *error = QStringLiteral("malformed CPX file-transfer message");
        return false;
    }
    const QString type = parts.at(0).toUpper();
    message.id = parts.at(1).trimmed();
    if (message.id.isEmpty()) {
        if (error) *error = QStringLiteral("file-transfer message is missing its transfer ID");
        return false;
    }

    bool ok = false;
    if ((type == QStringLiteral("OFFER") || type == QStringLiteral("OFFER2"))
        && parts.size() == 5) {
        message.type = MessageType::Offer;
        message.reliable = type == QStringLiteral("OFFER2");
        message.fileName = sanitizeFileName(decodeName(parts.at(2)));
        message.size = parts.at(3).toLongLong(&ok);
        message.sha256 = QByteArray::fromHex(parts.at(4).toLatin1());
        if (!ok || message.size < 0 || message.size > MaxAdvertisedFileSize
            || message.sha256.size() != 32) {
            if (error) *error = QStringLiteral("invalid file offer metadata");
            return false;
        }
        return true;
    }
    if ((type == QStringLiteral("ACCEPT") && parts.size() == 3)
        || (type == QStringLiteral("ACCEPTD") && parts.size() == 5)) {
        message.type = MessageType::Accept;
        message.direct = type == QStringLiteral("ACCEPTD");
        message.offset = parts.at(2).toLongLong(&ok);
        if (!ok || message.offset < 0) {
            if (error) *error = QStringLiteral("invalid file resume offset");
            return false;
        }
        if (message.direct) {
            bool portOk = false;
            const uint port = parts.at(3).toUInt(&portOk);
            message.directPort = portOk && port <= 65535 ? static_cast<quint16>(port) : 0;
            const QString hostsText = QString::fromUtf8(fromB64url(parts.at(4)));
            for (const QString &host : hostsText.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const QString clean = host.trimmed();
                if (!clean.isEmpty() && clean.size() <= 255) message.directHosts << clean;
            }
            message.directHosts.removeDuplicates();
            if (message.directPort == 0 || message.directHosts.isEmpty()) {
                if (error) *error = QStringLiteral("invalid direct file-transfer endpoint");
                return false;
            }
        }
        return true;
    }
    if (type == QStringLiteral("FALLBACK") && parts.size() == 3) {
        message.type = MessageType::Fallback;
        message.reason = decodeName(parts.at(2));
        return true;
    }
    if (type == QStringLiteral("DECLINE") && parts.size() == 3) {
        message.type = MessageType::Decline;
        message.reason = decodeName(parts.at(2));
        return true;
    }
    if (type == QStringLiteral("RESUME") && parts.size() == 2) {
        message.type = MessageType::Resume;
        return true;
    }
    if (type == QStringLiteral("DATA") && parts.size() == 4) {
        message.type = MessageType::Data;
        message.offset = parts.at(2).toLongLong(&ok);
        message.data = fromB64url(parts.at(3));
        if (!ok || message.offset < 0 || message.data.isEmpty()) {
            if (error) *error = QStringLiteral("invalid file data chunk");
            return false;
        }
        return true;
    }
    if (type == QStringLiteral("ACK") && parts.size() == 3) {
        message.type = MessageType::Ack;
        message.offset = parts.at(2).toLongLong(&ok);
        if (!ok || message.offset < 0) {
            if (error) *error = QStringLiteral("invalid file-transfer acknowledgment");
            return false;
        }
        return true;
    }
    if (type == QStringLiteral("DONE") && parts.size() == 2) {
        message.type = MessageType::Done;
        return true;
    }
    if (type == QStringLiteral("COMPLETE") && parts.size() == 2) {
        message.type = MessageType::Complete;
        return true;
    }
    if (type == QStringLiteral("CANCEL") && parts.size() == 3) {
        message.type = MessageType::Cancel;
        message.reason = decodeName(parts.at(2));
        return true;
    }

    if (error) *error = QStringLiteral("unknown or malformed CPX file-transfer message");
    return false;
}

bool CpxFileTransferManager::createOffer(const QString &target,
                                         const QString &path,
                                         QString &transferId,
                                         QString &payload,
                                         QString *error,
                                         bool reliable)
{
    transferId.clear();
    payload.clear();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        if (error) *error = QStringLiteral("file is not readable: %1").arg(path);
        return false;
    }
    if (info.size() < 0 || info.size() > MaxAdvertisedFileSize) {
        if (error) *error = QStringLiteral("file is larger than the 1 GiB CPX file-transfer limit");
        return false;
    }
    QString hashError;
    const QByteArray hash = sha256File(path, &hashError);
    if (hash.size() != 32) {
        if (error) *error = hashError.isEmpty() ? QStringLiteral("could not hash file") : hashError;
        return false;
    }

    Outgoing transfer;
    transfer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    transfer.target = target;
    transfer.path = info.absoluteFilePath();
    transfer.fileName = sanitizeFileName(info.fileName());
    transfer.size = info.size();
    transfer.sha256 = hash;
    transfer.reliable = reliable;
    m_outgoing.insert(transfer.id, transfer);

    transferId = transfer.id;
    payload = makePayload({reliable ? QStringLiteral("OFFER2") : QStringLiteral("OFFER"), transfer.id,
                           encodeName(transfer.fileName), QString::number(transfer.size),
                           QString::fromLatin1(transfer.sha256.toHex())});
    return true;
}

CpxFileTransferManager::Event CpxFileTransferManager::processIncoming(const QString &target,
                                                                      const QString &payload)
{
    Event event;
    Message message;
    QString parseError;
    if (!parseMessage(payload, message, &parseError)) {
        event.kind = EventKind::Error;
        event.reason = parseError;
        return event;
    }
    event.id = message.id;
    event.target = target;

    if (message.type == MessageType::Offer) {
        Incoming transfer;
        transfer.id = message.id;
        transfer.target = target;
        transfer.fileName = message.fileName;
        transfer.size = message.size;
        transfer.sha256 = message.sha256;
        transfer.reliable = message.reliable;
        m_incoming.insert(transfer.id, transfer);
        event.kind = EventKind::OfferReceived;
        event.fileName = transfer.fileName;
        event.total = transfer.size;
        return event;
    }

    if (message.type == MessageType::Accept) {
        auto it = m_outgoing.find(message.id);
        if (it == m_outgoing.end() || it->target.compare(target, Qt::CaseInsensitive) != 0) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("accept received for an unknown file transfer");
            return event;
        }
        if (it->complete) {
            const QString terminalState = it->status.toCaseFolded();
            const bool resumable = terminalState.contains(QStringLiteral("cancel"))
                || terminalState.contains(QStringLiteral("failed"))
                || terminalState.contains(QStringLiteral("error"));
            if (!resumable) {
                event.kind = EventKind::None;
                return event;
            }
            it->complete = false;
            it->doneSent = false;
            it->doneRetryCount = 0;
        }
        if (message.offset > it->size) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("peer requested an invalid resume offset");
            return event;
        }
        it->offset = message.offset;
        it->inFlightData.clear();
        it->retryCount = 0;
        it->accepted = true;
        it->direct = message.direct;
        it->directPort = message.directPort;
        it->directHosts = message.directHosts;
        it->status = message.direct
            ? QStringLiteral("direct connection starting")
            : (message.offset > 0 ? QStringLiteral("resuming relay") : QStringLiteral("sending by relay"));
        event.kind = EventKind::Accepted;
        event.outgoing = true;
        event.direct = message.direct;
        event.directPort = message.directPort;
        event.directHosts = message.directHosts;
        event.fileName = it->fileName;
        event.transferred = it->offset;
        event.total = it->size;
        event.percent = progressPercent(it->offset, it->size);
        return event;
    }

    if (message.type == MessageType::Ack) {
        auto it = m_outgoing.find(message.id);
        if (it == m_outgoing.end() || !it->reliable
            || it->target.compare(target, Qt::CaseInsensitive) != 0) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("acknowledgment received for an unknown reliable file transfer");
            return event;
        }
        if (message.offset > it->size) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("peer acknowledged beyond the advertised file size");
            return event;
        }
        event.outgoing = true;
        event.fileName = it->fileName;
        event.total = it->size;

        if (message.offset < it->offset) {
            event.kind = EventKind::None; // duplicate/late cumulative ACK
            return event;
        }
        if (it->inFlightData.isEmpty()) {
            if (message.offset == it->offset) {
                event.kind = EventKind::None;
                return event;
            }
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("unexpected file-transfer acknowledgment offset");
            return event;
        }

        const qint64 expected = it->inFlightOffset + it->inFlightData.size();
        if (message.offset == it->inFlightOffset) {
            // Receiver is asking us to retransmit the current chunk.
            it->lastPayloadSentMs = 0;
            event.kind = EventKind::None;
            return event;
        }
        if (message.offset != expected) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("unexpected file-transfer acknowledgment offset");
            return event;
        }

        it->offset = message.offset;
        it->inFlightData.clear();
        it->retryCount = 0;
        it->status = QStringLiteral("sending");
        event.kind = EventKind::Progress;
        event.transferred = it->offset;
        event.percent = progressPercent(it->offset, it->size);
        return event;
    }

    if (message.type == MessageType::Complete) {
        auto it = m_outgoing.find(message.id);
        if (it == m_outgoing.end() || !it->reliable
            || it->target.compare(target, Qt::CaseInsensitive) != 0) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("completion received for an unknown reliable file transfer");
            return event;
        }
        it->complete = true;
        it->status = QStringLiteral("complete");
        event.kind = EventKind::Completed;
        event.outgoing = true;
        event.fileName = it->fileName;
        event.transferred = it->size;
        event.total = it->size;
        event.percent = 100;
        return event;
    }

    if (message.type == MessageType::Fallback) {
        auto incoming = m_incoming.find(message.id);
        if (incoming == m_incoming.end()
            || incoming->target.compare(target, Qt::CaseInsensitive) != 0
            || !incoming->accepted) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("relay fallback requested for an unknown incoming transfer");
            return event;
        }
        incoming->direct = false;
        incoming->status = QStringLiteral("receiving by relay fallback");
        event.kind = EventKind::Fallback;
        event.fileName = incoming->fileName;
        event.transferred = incoming->received;
        event.total = incoming->size;
        event.reason = message.reason;
        event.replyPayload = makePayload({QStringLiteral("ACCEPT"), message.id,
                                          QString::number(incoming->received)});
        return event;
    }

    if (message.type == MessageType::Resume) {
        auto it = m_incoming.find(message.id);
        if (it == m_incoming.end()
            || it->target.compare(target, Qt::CaseInsensitive) != 0
            || it->destinationPath.isEmpty()) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("resume requested for an unknown or unaccepted incoming transfer");
            return event;
        }
        const QString terminalState = it->status.toCaseFolded();
        if (!it->complete
            || !(terminalState.contains(QStringLiteral("cancel"))
                 || terminalState.contains(QStringLiteral("failed"))
                 || terminalState.contains(QStringLiteral("error")))) {
            event.kind = EventKind::None;
            return event;
        }
        event.kind = EventKind::ResumeRequested;
        event.fileName = it->fileName;
        event.path = it->destinationPath;
        event.transferred = it->received;
        event.total = it->size;
        return event;
    }

    if (message.type == MessageType::Decline) {
        auto it = m_outgoing.find(message.id);
        if (it != m_outgoing.end()) {
            it->complete = true;
            it->status = QStringLiteral("declined");
            event.fileName = it->fileName;
            event.total = it->size;
        }
        event.kind = EventKind::Declined;
        event.reason = message.reason;
        return event;
    }

    if (message.type == MessageType::Cancel) {
        if (auto out = m_outgoing.find(message.id); out != m_outgoing.end()) {
            out->complete = true;
            out->status = QStringLiteral("cancelled");
            event.fileName = out->fileName;
            event.total = out->size;
        }
        if (auto in = m_incoming.find(message.id); in != m_incoming.end()) {
            in->complete = true;
            in->status = QStringLiteral("cancelled");
            event.fileName = in->fileName;
            event.path = in->partPath;
            event.transferred = in->received;
            event.total = in->size;
        }
        event.kind = EventKind::Cancelled;
        event.reason = message.reason;
        return event;
    }

    auto it = m_incoming.find(message.id);
    if (it == m_incoming.end() || it->target.compare(target, Qt::CaseInsensitive) != 0) {
        event.kind = EventKind::Error;
        event.reason = QStringLiteral("file data received for an unknown transfer");
        return event;
    }
    event.fileName = it->fileName;
    event.total = it->size;
    event.path = it->destinationPath;

    if (!it->accepted) {
        event.kind = EventKind::Error;
        event.reason = QStringLiteral("file data received before the transfer was accepted");
        return event;
    }

    if (it->complete && it->reliable && message.type == MessageType::Done) {
        // The COMPLETE control frame may itself be lost.  A repeated DONE is
        // therefore an idempotent request for the final confirmation, not an
        // attempt to hash a .cpxpart file that has already been renamed.
        event.kind = EventKind::None;
        event.replyPayload = makePayload({QStringLiteral("COMPLETE"), message.id});
        return event;
    }

    if (message.type == MessageType::Data) {
        if (message.offset + message.data.size() > it->size) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("oversized file-transfer chunk rejected");
            return event;
        }

        if (it->reliable && message.offset != it->received) {
            // Stop-and-wait reliable mode is cumulative.  A duplicate chunk
            // means our ACK was lost; a future-offset chunk means the sender
            // missed our last ACK.  Neither condition should kill the whole
            // transfer or the chat session: simply advertise the next byte we
            // still expect and let the sender retransmit.
            event.kind = EventKind::None;
            event.replyPayload = makePayload({QStringLiteral("ACK"), message.id,
                                              QString::number(it->received)});
            return event;
        }

        if (!it->reliable && message.offset != it->received) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("out-of-order file-transfer chunk rejected");
            return event;
        }

        QFile part(it->partPath);
        if (!part.open(QIODevice::ReadWrite)) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("cannot write %1: %2").arg(it->partPath, part.errorString());
            return event;
        }
        if (!part.seek(message.offset) || part.write(message.data) != message.data.size()) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("failed writing file-transfer chunk: %1").arg(part.errorString());
            return event;
        }
        part.close();
        it->received += message.data.size();
        it->status = QStringLiteral("receiving");
        if (it->reliable) {
            event.replyPayload = makePayload({QStringLiteral("ACK"), message.id,
                                              QString::number(it->received)});
        }
        event.kind = EventKind::Progress;
        event.transferred = it->received;
        event.percent = progressPercent(it->received, it->size);
        return event;
    }

    if (message.type == MessageType::Done) {
        if (it->received != it->size) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("transfer ended early (%1 of %2 bytes received)")
                               .arg(it->received).arg(it->size);
            return event;
        }
        QString hashError;
        const QByteArray actual = sha256File(it->partPath, &hashError);
        if (actual != it->sha256) {
            event.kind = EventKind::Error;
            event.reason = hashError.isEmpty()
                ? QStringLiteral("SHA-256 verification failed; partial file retained for retry/resume")
                : hashError;
            return event;
        }
        if (QFile::exists(it->destinationPath) && !QFile::remove(it->destinationPath)) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("cannot replace existing destination file %1").arg(it->destinationPath);
            return event;
        }
        if (!QFile::rename(it->partPath, it->destinationPath)) {
            event.kind = EventKind::Error;
            event.reason = QStringLiteral("verified transfer but could not finalize %1").arg(it->destinationPath);
            return event;
        }
        it->complete = true;
        it->status = QStringLiteral("complete");
        event.kind = EventKind::Completed;
        if (it->reliable) {
            event.replyPayload = makePayload({QStringLiteral("COMPLETE"), message.id});
        }
        event.transferred = it->received;
        event.percent = 100;
        event.path = it->destinationPath;
        return event;
    }

    event.kind = EventKind::Error;
    event.reason = QStringLiteral("unexpected file-transfer state");
    return event;
}

QString CpxFileTransferManager::acceptIncoming(const QString &transferId,
                                               const QString &destinationPath,
                                               QString *error,
                                               quint16 directPort,
                                               const QStringList &directHosts)
{
    auto it = m_incoming.find(transferId);
    if (it == m_incoming.end()) {
        if (error) *error = QStringLiteral("unknown incoming transfer ID %1").arg(transferId);
        return {};
    }
    if (it->complete) {
        const QString state = it->status.toCaseFolded();
        const bool resumable = state.contains(QStringLiteral("cancel"))
            || state.contains(QStringLiteral("failed"))
            || state.contains(QStringLiteral("error"));
        if (!resumable) {
            if (error) *error = QStringLiteral("incoming transfer %1 is already finished").arg(transferId);
            return {};
        }
    }
    const QFileInfo destInfo(destinationPath);
    if (destinationPath.trimmed().isEmpty() || !QDir().mkpath(destInfo.absolutePath())) {
        if (error) *error = QStringLiteral("cannot create destination directory");
        return {};
    }
    it->destinationPath = destInfo.absoluteFilePath();
    it->partPath = it->destinationPath + QStringLiteral(".cpxpart");

    qint64 resumeOffset = 0;
    QFileInfo partInfo(it->partPath);
    if (partInfo.exists()) {
        if (!partInfo.isFile() || partInfo.size() > it->size) {
            QFile::remove(it->partPath);
        } else {
            resumeOffset = partInfo.size();
        }
    }
    if (!QFile::exists(it->partPath)) {
        QFile part(it->partPath);
        if (!part.open(QIODevice::WriteOnly)) {
            if (error) *error = QStringLiteral("cannot create partial file %1: %2")
                                    .arg(it->partPath, part.errorString());
            return {};
        }
        part.close();
    }
    it->received = resumeOffset;
    it->accepted = true;
    it->complete = false;
    it->lastReportedPercent = -1;
    QStringList cleanHosts;
    for (const QString &host : directHosts) {
        const QString clean = host.trimmed();
        if (!clean.isEmpty() && clean.size() <= 255) cleanHosts << clean;
    }
    cleanHosts.removeDuplicates();
    it->direct = directPort > 0 && !cleanHosts.isEmpty();
    it->status = it->direct
        ? QStringLiteral("waiting for direct connection")
        : (resumeOffset > 0 ? QStringLiteral("resuming relay") : QStringLiteral("receiving by relay"));
    if (it->direct) {
        return makePayload({QStringLiteral("ACCEPTD"), transferId, QString::number(resumeOffset),
                            QString::number(directPort), b64url(cleanHosts.join(QLatin1Char('\n')).toUtf8())});
    }
    return makePayload({QStringLiteral("ACCEPT"), transferId, QString::number(resumeOffset)});
}

QString CpxFileTransferManager::declineIncoming(const QString &transferId, const QString &reason)
{
    auto it = m_incoming.find(transferId);
    if (it != m_incoming.end()) {
        it->complete = true;
        it->status = QStringLiteral("declined");
    }
    return makePayload({QStringLiteral("DECLINE"), transferId, encodeName(reason)});
}

QString CpxFileTransferManager::cancel(const QString &transferId, const QString &reason)
{
    if (auto out = m_outgoing.find(transferId); out != m_outgoing.end()) {
        out->complete = true;
        out->status = QStringLiteral("cancelled");
    }
    if (auto in = m_incoming.find(transferId); in != m_incoming.end()) {
        in->complete = true;
        in->status = QStringLiteral("cancelled");
    }
    return makePayload({QStringLiteral("CANCEL"), transferId, encodeName(reason)});
}

QString CpxFileTransferManager::requestResume(const QString &transferId, QString *error)
{
    auto it = m_outgoing.find(transferId);
    if (it == m_outgoing.end()) {
        if (error) *error = QStringLiteral("unknown outgoing transfer ID %1").arg(transferId);
        return {};
    }
    if (!canResume(transferId)) {
        if (error) *error = QStringLiteral("transfer %1 is not resumable").arg(transferId);
        return {};
    }
    it->complete = false;
    it->accepted = false;
    it->direct = false;
    it->directPort = 0;
    it->directHosts.clear();
    it->inFlightData.clear();
    it->retryCount = 0;
    it->doneSent = false;
    it->doneRetryCount = 0;
    it->lastPayloadSentMs = 0;
    it->status = QStringLiteral("resume requested");
    return makePayload({QStringLiteral("RESUME"), transferId});
}

QString CpxFileTransferManager::resumeIncoming(const QString &transferId,
                                               QString *error,
                                               quint16 directPort,
                                               const QStringList &directHosts)
{
    auto it = m_incoming.find(transferId);
    if (it == m_incoming.end() || it->destinationPath.isEmpty()) {
        if (error) *error = QStringLiteral("unknown or unaccepted incoming transfer ID %1").arg(transferId);
        return {};
    }
    const QString terminalState = it->status.toCaseFolded();
    if (it->complete
        && !(terminalState.contains(QStringLiteral("cancel"))
             || terminalState.contains(QStringLiteral("failed"))
             || terminalState.contains(QStringLiteral("error")))) {
        if (error) *error = QStringLiteral("transfer %1 is already complete and cannot be resumed").arg(transferId);
        return {};
    }
    it->complete = false;
    return acceptIncoming(transferId, it->destinationPath, error, directPort, directHosts);
}

bool CpxFileTransferManager::canResume(const QString &transferId) const
{
    if (auto it = m_outgoing.constFind(transferId); it != m_outgoing.constEnd()) {
        const QString state = it->status.toCaseFolded();
        if (!(state.contains(QStringLiteral("cancel"))
              || state.contains(QStringLiteral("failed"))
              || state.contains(QStringLiteral("error")))) return false;
        const QFileInfo info(it->path);
        return info.exists() && info.isFile() && info.isReadable() && info.size() == it->size;
    }
    if (auto it = m_incoming.constFind(transferId); it != m_incoming.constEnd()) {
        const QString state = it->status.toCaseFolded();
        if (!(state.contains(QStringLiteral("cancel"))
              || state.contains(QStringLiteral("failed"))
              || state.contains(QStringLiteral("error")))) return false;
        if (it->destinationPath.isEmpty() || it->partPath.isEmpty()) return false;
        const QFileInfo part(it->partPath);
        return !part.exists() || (part.isFile() && part.size() >= 0 && part.size() <= it->size);
    }
    return false;
}

bool CpxFileTransferManager::clearTransfer(const QString &transferId, QString *error)
{
    bool found = false;
    if (auto it = m_incoming.find(transferId); it != m_incoming.end()) {
        found = true;
        // Never delete a successfully finalized download. Only discard the
        // resumable .cpxpart file for incomplete/cancelled transfers.
        if (it->status.compare(QStringLiteral("complete"), Qt::CaseInsensitive) != 0
            && !it->partPath.isEmpty() && QFile::exists(it->partPath)
            && !QFile::remove(it->partPath)) {
            if (error) *error = QStringLiteral("could not remove partial file %1").arg(it->partPath);
            return false;
        }
        m_incoming.erase(it);
    }
    if (auto it = m_outgoing.find(transferId); it != m_outgoing.end()) {
        found = true;
        m_outgoing.erase(it);
    }
    if (!found && error) *error = QStringLiteral("unknown transfer ID %1").arg(transferId);
    return found;
}

QString CpxFileTransferManager::requestOutgoingRelayFallback(const QString &transferId,
                                                               const QString &reason)
{
    auto it = m_outgoing.find(transferId);
    if (it == m_outgoing.end()) return {};
    it->direct = false;
    it->accepted = false;
    it->inFlightData.clear();
    it->retryCount = 0;
    it->doneSent = false;
    it->doneRetryCount = 0;
    it->status = QStringLiteral("waiting for relay resume");
    return makePayload({QStringLiteral("FALLBACK"), transferId, encodeName(reason)});
}

QString CpxFileTransferManager::fallbackIncomingToRelay(const QString &transferId)
{
    auto it = m_incoming.find(transferId);
    if (it == m_incoming.end() || !it->accepted) return {};
    it->direct = false;
    it->status = QStringLiteral("receiving by relay fallback");
    return makePayload({QStringLiteral("ACCEPT"), transferId, QString::number(it->received)});
}

void CpxFileTransferManager::updateDirectProgress(const QString &transferId,
                                                  qint64 transferred,
                                                  bool outgoing)
{
    if (outgoing) {
        auto it = m_outgoing.find(transferId);
        if (it == m_outgoing.end()) return;
        it->offset = std::clamp<qint64>(transferred, 0, it->size);
        it->status = QStringLiteral("sending direct");
        return;
    }
    auto it = m_incoming.find(transferId);
    if (it == m_incoming.end()) return;
    it->received = std::clamp<qint64>(transferred, 0, it->size);
    it->status = QStringLiteral("receiving direct");
}

bool CpxFileTransferManager::finalizeIncomingDirect(const QString &transferId, QString *error)
{
    auto it = m_incoming.find(transferId);
    if (it == m_incoming.end() || !it->accepted) {
        if (error) *error = QStringLiteral("unknown incoming direct transfer");
        return false;
    }
    if (it->received != it->size) {
        if (error) *error = QStringLiteral("direct transfer ended early (%1 of %2 bytes received)")
                                .arg(it->received).arg(it->size);
        return false;
    }
    QString hashError;
    const QByteArray actual = sha256File(it->partPath, &hashError);
    if (actual != it->sha256) {
        if (error) *error = hashError.isEmpty()
            ? QStringLiteral("SHA-256 verification failed; partial file retained for resume")
            : hashError;
        return false;
    }
    if (QFile::exists(it->destinationPath) && !QFile::remove(it->destinationPath)) {
        if (error) *error = QStringLiteral("cannot replace existing destination file %1")
                                .arg(it->destinationPath);
        return false;
    }
    if (!QFile::rename(it->partPath, it->destinationPath)) {
        if (error) *error = QStringLiteral("verified transfer but could not finalize %1")
                                .arg(it->destinationPath);
        return false;
    }
    it->complete = true;
    it->direct = false;
    it->status = QStringLiteral("complete");
    return true;
}

QString CpxFileTransferManager::completionPayload(const QString &transferId) const
{
    if (transferId.trimmed().isEmpty()) return {};
    return makePayload({QStringLiteral("COMPLETE"), transferId});
}

void CpxFileTransferManager::markOutgoingDirectSent(const QString &transferId)
{
    auto it = m_outgoing.find(transferId);
    if (it == m_outgoing.end()) return;
    it->offset = it->size;
    it->status = QStringLiteral("sent direct; awaiting receiver verification");
}

QString CpxFileTransferManager::nextOutgoingPayload(const QString &transferId,
                                                    int rawChunkBytes,
                                                    bool *finished,
                                                    QString *error,
                                                    int minimumSendIntervalMs)
{
    if (finished) *finished = false;
    auto it = m_outgoing.find(transferId);
    if (it == m_outgoing.end()) {
        if (error) *error = QStringLiteral("unknown outgoing transfer ID %1").arg(transferId);
        return {};
    }
    if (!it->accepted || it->complete) return {};

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    minimumSendIntervalMs = std::clamp(minimumSendIntervalMs, 0, 5000);

    if (it->reliable) {
        if (!it->inFlightData.isEmpty()) {
            if (it->lastPayloadSentMs > 0 && now - it->lastPayloadSentMs < AckRetryMs) {
                return {};
            }
            if (it->retryCount >= MaxChunkRetries) {
                it->complete = true;
                it->status = QStringLiteral("failed: acknowledgment timeout");
                if (error) *error = QStringLiteral(
                    "file-transfer chunk was not acknowledged after %1 attempts; transfer stopped without closing the chat connection")
                    .arg(MaxChunkRetries);
                return {};
            }
            ++it->retryCount;
            it->lastPayloadSentMs = now;
            return makePayload({QStringLiteral("DATA"), transferId,
                                QString::number(it->inFlightOffset), b64url(it->inFlightData)});
        }

        if (it->offset >= it->size) {
            if (it->doneSent) {
                if (now - it->lastPayloadSentMs < AckRetryMs) return {};
                if (it->doneRetryCount >= MaxChunkRetries) {
                    it->complete = true;
                    it->status = QStringLiteral("failed: completion unconfirmed");
                    if (error) *error = QStringLiteral(
                        "receiver did not confirm file verification after %1 attempts")
                        .arg(MaxChunkRetries);
                    return {};
                }
                ++it->doneRetryCount;
                it->lastPayloadSentMs = now;
                return makePayload({QStringLiteral("DONE"), transferId});
            }
            if (it->lastPayloadSentMs > 0
                && now - it->lastPayloadSentMs < minimumSendIntervalMs) return {};
            it->doneSent = true;
            it->doneRetryCount = 1;
            it->lastPayloadSentMs = now;
            it->status = QStringLiteral("sent; awaiting receiver verification");
            return makePayload({QStringLiteral("DONE"), transferId});
        }

        if (it->lastPayloadSentMs > 0
            && now - it->lastPayloadSentMs < minimumSendIntervalMs) return {};

        rawChunkBytes = std::clamp(rawChunkBytes, 32, 4096);
        QFile file(it->path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(it->offset)) {
            if (error) *error = QStringLiteral("cannot read outgoing file %1: %2")
                                    .arg(it->path, file.errorString());
            return {};
        }
        const QByteArray data = file.read(std::min<qint64>(rawChunkBytes, it->size - it->offset));
        if (data.isEmpty() && it->offset < it->size) {
            if (error) *error = QStringLiteral("unexpected end of outgoing file");
            return {};
        }
        it->inFlightOffset = it->offset;
        it->inFlightData = data;
        it->retryCount = 1;
        it->lastPayloadSentMs = now;
        it->status = QStringLiteral("sending; awaiting acknowledgment");
        return makePayload({QStringLiteral("DATA"), transferId,
                            QString::number(it->inFlightOffset), b64url(it->inFlightData)});
    }

    // Legacy CPXFILE1 mode for older peers.  Keep it compatible, but pace it
    // so a large transfer cannot dump thousands of queued IMs into OSCAR/IRC.
    if (it->lastPayloadSentMs > 0
        && now - it->lastPayloadSentMs < minimumSendIntervalMs) return {};

    if (it->offset >= it->size) {
        if (!it->doneSent) {
            it->doneSent = true;
            it->complete = true;
            it->status = QStringLiteral("sent; awaiting receiver verification");
            it->lastPayloadSentMs = now;
            if (finished) *finished = true;
            return makePayload({QStringLiteral("DONE"), transferId});
        }
        if (finished) *finished = true;
        return {};
    }

    rawChunkBytes = std::clamp(rawChunkBytes, 32, 4096);
    QFile file(it->path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(it->offset)) {
        if (error) *error = QStringLiteral("cannot read outgoing file %1: %2")
                                .arg(it->path, file.errorString());
        return {};
    }
    const QByteArray data = file.read(std::min<qint64>(rawChunkBytes, it->size - it->offset));
    if (data.isEmpty() && it->offset < it->size) {
        if (error) *error = QStringLiteral("unexpected end of outgoing file");
        return {};
    }
    const qint64 offset = it->offset;
    it->offset += data.size();
    it->lastPayloadSentMs = now;
    it->status = QStringLiteral("sending (legacy relay mode)");
    return makePayload({QStringLiteral("DATA"), transferId, QString::number(offset), b64url(data)});
}

QStringList CpxFileTransferManager::activeOutgoingIds() const
{
    QStringList ids;
    for (auto it = m_outgoing.constBegin(); it != m_outgoing.constEnd(); ++it) {
        if (it->accepted && !it->complete && !it->direct) ids << it.key();
    }
    return ids;
}

QList<CpxFileTransferManager::TransferInfo> CpxFileTransferManager::transfers() const
{
    QList<TransferInfo> out;
    out.reserve(m_outgoing.size() + m_incoming.size());
    for (auto it = m_outgoing.constBegin(); it != m_outgoing.constEnd(); ++it) {
        TransferInfo info;
        info.id = it->id; info.target = it->target; info.fileName = it->fileName;
        info.path = it->path; info.transferred = it->offset; info.total = it->size;
        info.outgoing = true; info.accepted = it->accepted; info.complete = it->complete;
        info.direct = it->direct; info.resumable = canResume(it.key()); info.status = it->status; out.append(info);
    }
    for (auto it = m_incoming.constBegin(); it != m_incoming.constEnd(); ++it) {
        TransferInfo info;
        info.id = it->id; info.target = it->target; info.fileName = it->fileName;
        info.path = it->destinationPath; info.transferred = it->received; info.total = it->size;
        info.outgoing = false; info.accepted = it->accepted; info.complete = it->complete;
        info.direct = it->direct; info.resumable = canResume(it.key()); info.status = it->status; out.append(info);
    }
    return out;
}

CpxFileTransferManager::TransferInfo CpxFileTransferManager::transfer(const QString &transferId) const
{
    if (auto it = m_outgoing.constFind(transferId); it != m_outgoing.constEnd()) {
        TransferInfo info;
        info.id = it->id; info.target = it->target; info.fileName = it->fileName;
        info.path = it->path; info.transferred = it->offset; info.total = it->size;
        info.outgoing = true; info.accepted = it->accepted; info.complete = it->complete;
        info.direct = it->direct; info.resumable = canResume(transferId); info.status = it->status; return info;
    }
    if (auto it = m_incoming.constFind(transferId); it != m_incoming.constEnd()) {
        TransferInfo info;
        info.id = it->id; info.target = it->target; info.fileName = it->fileName;
        info.path = it->destinationPath; info.transferred = it->received; info.total = it->size;
        info.outgoing = false; info.accepted = it->accepted; info.complete = it->complete;
        info.direct = it->direct; info.resumable = canResume(transferId); info.status = it->status; return info;
    }
    return {};
}
