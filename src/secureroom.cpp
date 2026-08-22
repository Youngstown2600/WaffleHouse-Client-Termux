#include "secureroom.h"

#include <sodium.h>

namespace {
const QString RoomPrefix = QStringLiteral("[[CPXROOM1:");
const QString KeyPrefix = QStringLiteral("[[CPXROOMKEY1:");
const QString FrameSuffix = QStringLiteral("]]" );
constexpr int MaxSeenNonces = 512;

QString b64(const QByteArray &value)
{
    return QString::fromLatin1(value.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray fromB64(const QString &value)
{
    return QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding);
}
}

bool SecureRoomManager::initialize(QString *error)
{
    if (m_initialized) return true;
    if (sodium_init() < 0) {
        if (error) *error = QStringLiteral("libsodium initialization failed for secure rooms");
        return false;
    }
    m_initialized = true;
    return true;
}

QString SecureRoomManager::roomKey(const QString &connectionId, const QString &room) const
{
    return connectionId + QChar(0x1f) + room.trimmed().toCaseFolded();
}

QByteArray SecureRoomManager::associatedData(const QString &connectionId,
                                             const QString &room,
                                             const QString &id) const
{
    // connectionId is deliberately NOT part of the authenticated wire data.
    // It is a local WaffleHouse profile identifier and will be different on
    // Client A and Client B. It is used only to namespace local room state.
    // The room name + shared key id are the stable values both peers know.
    (void)connectionId;
    return QByteArray("CPXROOM1|")
        + room.trimmed().toCaseFolded().toUtf8() + '|' + id.toUtf8();
}

bool SecureRoomManager::createOrRotate(const QString &connectionId,
                                       const QString &room,
                                       QString *error)
{
    if (!m_initialized || connectionId.trimmed().isEmpty() || room.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("secure-room manager is not ready");
        return false;
    }

    RoomState &state = m_rooms[roomKey(connectionId, room)];
    if (!state.key.isEmpty()) sodium_memzero(state.key.data(), static_cast<size_t>(state.key.size()));
    state.key.resize(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    randombytes_buf(state.key.data(), static_cast<size_t>(state.key.size()));

    QByteArray idBytes(8, '\0');
    randombytes_buf(idBytes.data(), static_cast<size_t>(idBytes.size()));
    state.keyId = b64(idBytes);
    state.localOwner = true;
    state.seenNonces.clear();
    state.seenNonceOrder.clear();
    return true;
}

bool SecureRoomManager::hasRoom(const QString &connectionId, const QString &room) const
{
    const auto it = m_rooms.constFind(roomKey(connectionId, room));
    return it != m_rooms.constEnd()
        && it->key.size() == crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        && !it->keyId.isEmpty();
}

bool SecureRoomManager::locallyOwned(const QString &connectionId, const QString &room) const
{
    const auto it = m_rooms.constFind(roomKey(connectionId, room));
    return it != m_rooms.constEnd() && it->localOwner;
}

QString SecureRoomManager::keyId(const QString &connectionId, const QString &room) const
{
    const auto it = m_rooms.constFind(roomKey(connectionId, room));
    return it == m_rooms.constEnd() ? QString() : it->keyId;
}

QString SecureRoomManager::keyOffer(const QString &connectionId,
                                    const QString &room,
                                    QString *error) const
{
    const auto it = m_rooms.constFind(roomKey(connectionId, room));
    if (it == m_rooms.constEnd()
        || it->key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || it->keyId.isEmpty()) {
        if (error) *error = QStringLiteral("no secure room key exists for %1").arg(room);
        return {};
    }

    return QStringLiteral("[[CPXROOMKEY1:%1:%2:%3]]")
        .arg(b64(room.toUtf8()), it->keyId, b64(it->key));
}

bool SecureRoomManager::installKeyOffer(const QString &connectionId,
                                        const QString &offer,
                                        QString *room,
                                        QString *id,
                                        QString *error)
{
    const QString trimmed = offer.trimmed();
    if (!looksLikeKeyOffer(trimmed)) {
        if (error) *error = QStringLiteral("not a secure-room key offer");
        return false;
    }

    const QString body = trimmed.mid(KeyPrefix.size(),
                                     trimmed.size() - KeyPrefix.size() - FrameSuffix.size());
    const QStringList parts = body.split(QLatin1Char(':'));
    if (parts.size() != 3) {
        if (error) *error = QStringLiteral("malformed secure-room key offer");
        return false;
    }

    const QString decodedRoom = QString::fromUtf8(fromB64(parts.at(0))).trimmed();
    const QString decodedId = parts.at(1).trimmed();
    const QByteArray decodedKey = fromB64(parts.at(2));
    if (connectionId.trimmed().isEmpty() || decodedRoom.isEmpty() || decodedId.isEmpty()
        || decodedKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        if (error) *error = QStringLiteral("invalid secure-room key offer");
        return false;
    }

    RoomState &state = m_rooms[roomKey(connectionId, decodedRoom)];
    if (!state.key.isEmpty()) sodium_memzero(state.key.data(), static_cast<size_t>(state.key.size()));
    state.key = decodedKey;
    state.keyId = decodedId;
    state.localOwner = false;
    state.seenNonces.clear();
    state.seenNonceOrder.clear();

    if (room) *room = decodedRoom;
    if (id) *id = decodedId;
    return true;
}

QString SecureRoomManager::encrypt(const QString &connectionId,
                                   const QString &room,
                                   const QString &plaintext,
                                   QString *error) const
{
    const auto it = m_rooms.constFind(roomKey(connectionId, room));
    if (it == m_rooms.constEnd()
        || it->key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        if (error) *error = QStringLiteral("secure room %1 is not active").arg(room);
        return {};
    }

    const QByteArray message = plaintext.toUtf8();
    const QByteArray aad = associatedData(connectionId, room, it->keyId);
    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, '\0');
    randombytes_buf(nonce.data(), static_cast<size_t>(nonce.size()));

    QByteArray cipher(message.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long cipherLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipher.data()), &cipherLen,
        reinterpret_cast<const unsigned char *>(message.constData()), static_cast<unsigned long long>(message.size()),
        reinterpret_cast<const unsigned char *>(aad.constData()), static_cast<unsigned long long>(aad.size()),
        nullptr,
        reinterpret_cast<const unsigned char *>(nonce.constData()),
        reinterpret_cast<const unsigned char *>(it->key.constData()));
    if (rc != 0) {
        if (error) *error = QStringLiteral("secure-room encryption failed");
        return {};
    }
    cipher.resize(static_cast<qsizetype>(cipherLen));
    return QStringLiteral("[[CPXROOM1:%1:%2:%3]]").arg(it->keyId, b64(nonce), b64(cipher));
}

void SecureRoomManager::rememberNonce(RoomState &state, const QString &nonceToken)
{
    state.seenNonces.insert(nonceToken);
    state.seenNonceOrder.push_back(nonceToken);
    while (state.seenNonceOrder.size() > MaxSeenNonces) {
        const QString oldest = state.seenNonceOrder.takeFirst();
        state.seenNonces.remove(oldest);
    }
}

SecureRoomManager::IncomingResult SecureRoomManager::processIncoming(
    const QString &connectionId,
    const QString &room,
    const QString &payload)
{
    IncomingResult result;
    const QString trimmed = payload.trimmed();
    if (!looksLikeFrame(trimmed)) return result;

    const QString body = trimmed.mid(RoomPrefix.size(),
                                     trimmed.size() - RoomPrefix.size() - FrameSuffix.size());
    const QStringList parts = body.split(QLatin1Char(':'));
    if (parts.size() != 3) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("malformed secure-room frame");
        return result;
    }

    auto it = m_rooms.find(roomKey(connectionId, room));
    if (it == m_rooms.end()) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("secure-room ciphertext received, but no room key is installed");
        return result;
    }

    result.keyId = parts.at(0);
    if (result.keyId != it->keyId) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("secure-room key mismatch (message %1, local %2)")
            .arg(result.keyId, it->keyId);
        return result;
    }

    const QString nonceToken = parts.at(1);
    if (it->seenNonces.contains(nonceToken)) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("replayed secure-room message rejected");
        return result;
    }

    const QByteArray nonce = fromB64(nonceToken);
    const QByteArray cipher = fromB64(parts.at(2));
    if (nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
        || cipher.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("invalid secure-room message encoding");
        return result;
    }

    const QByteArray aad = associatedData(connectionId, room, it->keyId);
    QByteArray plaintext(cipher.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long plaintextLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(plaintext.data()), &plaintextLen,
        nullptr,
        reinterpret_cast<const unsigned char *>(cipher.constData()), static_cast<unsigned long long>(cipher.size()),
        reinterpret_cast<const unsigned char *>(aad.constData()), static_cast<unsigned long long>(aad.size()),
        reinterpret_cast<const unsigned char *>(nonce.constData()),
        reinterpret_cast<const unsigned char *>(it->key.constData()));
    if (rc != 0) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("secure-room message authentication failed");
        return result;
    }

    plaintext.resize(static_cast<qsizetype>(plaintextLen));
    rememberNonce(*it, nonceToken);
    result.kind = IncomingKind::Decrypted;
    result.plaintext = QString::fromUtf8(plaintext);
    return result;
}

void SecureRoomManager::closeRoom(const QString &connectionId, const QString &room)
{
    auto it = m_rooms.find(roomKey(connectionId, room));
    if (it == m_rooms.end()) return;
    if (!it->key.isEmpty()) sodium_memzero(it->key.data(), static_cast<size_t>(it->key.size()));
    m_rooms.erase(it);
}

void SecureRoomManager::closeConnection(const QString &connectionId)
{
    const QString prefix = connectionId + QChar(0x1f);
    QStringList keys;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it.key().startsWith(prefix)) keys << it.key();
    }
    for (const QString &key : keys) {
        auto it = m_rooms.find(key);
        if (it == m_rooms.end()) continue;
        if (!it->key.isEmpty()) sodium_memzero(it->key.data(), static_cast<size_t>(it->key.size()));
        m_rooms.erase(it);
    }
}

bool SecureRoomManager::looksLikeFrame(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.startsWith(RoomPrefix) && trimmed.endsWith(FrameSuffix);
}

bool SecureRoomManager::looksLikeKeyOffer(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.startsWith(KeyPrefix) && trimmed.endsWith(FrameSuffix);
}
