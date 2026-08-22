#include "securechannel.h"
#include "appbranding.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QStandardPaths>

#include <sodium.h>

#include <cstring>
#include <algorithm>

namespace {
const QString FramePrefix = QStringLiteral("[[CPX3:");
const QString FrameSuffix = QStringLiteral("]]" );
const QByteArray IdentityMagic("CPXKEY1\0", 8);
constexpr int FingerprintBytes = 20;
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
} // namespace

bool SecureChannelManager::initialize(QString *error)
{
    if (m_initialized) {
        return true;
    }
    if (sodium_init() < 0) {
        if (error) *error = QStringLiteral("libsodium initialization failed");
        return false;
    }
    if (!loadOrCreateMasterIdentity(error)) {
        return false;
    }
    m_initialized = true;
    return true;
}

bool SecureChannelManager::loadOrCreateMasterIdentity(QString *error)
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDir.isEmpty()) {
        if (error) *error = QStringLiteral("cannot determine %1 config directory").arg(appDisplayName());
        return false;
    }
    if (!QDir().mkpath(configDir)) {
        if (error) *error = QStringLiteral("cannot create config directory %1").arg(configDir);
        return false;
    }

    // A local master keypair is retained only as derivation material; stable
    // per-connection identities are deterministically derived from it.
    const QString path = QDir(configDir).filePath(QStringLiteral("identity.key"));
    QFile file(path);
    const qsizetype expected = IdentityMagic.size()
        + crypto_kx_PUBLICKEYBYTES + crypto_kx_SECRETKEYBYTES;

    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        const QByteArray data = file.readAll();
        file.close();
        if (data.size() == expected && data.startsWith(IdentityMagic)) {
            qsizetype offset = IdentityMagic.size();
            m_masterPublicKey = data.mid(offset, crypto_kx_PUBLICKEYBYTES);
            offset += crypto_kx_PUBLICKEYBYTES;
            m_masterSecretKey = data.mid(offset, crypto_kx_SECRETKEYBYTES);
            return true;
        }
    }

    m_masterPublicKey.resize(crypto_kx_PUBLICKEYBYTES);
    m_masterSecretKey.resize(crypto_kx_SECRETKEYBYTES);
    if (crypto_kx_keypair(reinterpret_cast<unsigned char *>(m_masterPublicKey.data()),
                          reinterpret_cast<unsigned char *>(m_masterSecretKey.data())) != 0) {
        if (error) *error = QStringLiteral("could not generate %1 master identity key").arg(appDisplayName());
        return false;
    }

    QByteArray data = IdentityMagic + m_masterPublicKey + m_masterSecretKey;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error) *error = QStringLiteral("could not completely write %1").arg(path);
        file.close();
        return false;
    }
    file.close();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool SecureChannelManager::deriveIdentity(const QString &connectionId,
                                          Identity &identity,
                                          QString *error) const
{
    if (!m_initialized || connectionId.trimmed().isEmpty()
        || m_masterSecretKey.size() != crypto_kx_SECRETKEYBYTES) {
        if (error) *error = QStringLiteral("invalid %1 connection identity").arg(appDisplayName());
        return false;
    }

    const QByteArray context = QByteArray("WaffleHouse/profile/") + connectionId.toUtf8();
    QByteArray seed(crypto_kx_SEEDBYTES, '\0');
    if (crypto_generichash(
            reinterpret_cast<unsigned char *>(seed.data()), seed.size(),
            reinterpret_cast<const unsigned char *>(context.constData()), context.size(),
            reinterpret_cast<const unsigned char *>(m_masterSecretKey.constData()),
            m_masterSecretKey.size()) != 0) {
        if (error) *error = QStringLiteral("could not derive %1 profile identity seed").arg(appDisplayName());
        return false;
    }

    identity.publicKey.resize(crypto_kx_PUBLICKEYBYTES);
    identity.secretKey.resize(crypto_kx_SECRETKEYBYTES);
    if (crypto_kx_seed_keypair(
            reinterpret_cast<unsigned char *>(identity.publicKey.data()),
            reinterpret_cast<unsigned char *>(identity.secretKey.data()),
            reinterpret_cast<const unsigned char *>(seed.constData())) != 0) {
        sodium_memzero(seed.data(), seed.size());
        if (error) *error = QStringLiteral("could not derive %1 profile identity key").arg(appDisplayName());
        return false;
    }
    sodium_memzero(seed.data(), seed.size());
    return true;
}

QString SecureChannelManager::sessionKey(const QString &connectionId, const QString &target) const
{
    return connectionId + QChar(0x1f) + target.toCaseFolded();
}

QString SecureChannelManager::fingerprintForKey(const QByteArray &publicKey) const
{
    if (publicKey.size() != crypto_kx_PUBLICKEYBYTES) {
        return {};
    }
    QByteArray digest(FingerprintBytes, '\0');
    crypto_generichash(reinterpret_cast<unsigned char *>(digest.data()), digest.size(),
                       reinterpret_cast<const unsigned char *>(publicKey.constData()), publicKey.size(),
                       nullptr, 0);
    const QString hex = QString::fromLatin1(digest.toHex().toUpper());
    QStringList groups;
    for (int i = 0; i < hex.size(); i += 8) {
        groups << hex.mid(i, 8);
    }
    return groups.join(QLatin1Char('-'));
}

QString SecureChannelManager::localFingerprint(const QString &connectionId) const
{
    Identity identity;
    return deriveIdentity(connectionId, identity) ? fingerprintForKey(identity.publicKey) : QString();
}

QString SecureChannelManager::helloFrame(const QString &connectionId) const
{
    Identity identity;
    if (!deriveIdentity(connectionId, identity)) return {};
    return QStringLiteral("[[CPX3:HELLO:%1]]").arg(b64(identity.publicKey));
}

QStringList SecureChannelManager::localCapabilities()
{
    return {QStringLiteral("secure-dm"),
            QStringLiteral("file-transfer"),
            QStringLiteral("file-resume"),
            QStringLiteral("file-ack"),
            QStringLiteral("file-direct-v1"),
            QStringLiteral("secure-room-v1")};
}

QString SecureChannelManager::capabilitiesFrame(const QString &connectionId,
                                                const QString &target)
{
    auto it = m_sessions.find(sessionKey(connectionId, target));
    if (it == m_sessions.end() || !it->established || it->capabilitiesSent) return {};
    it->capabilitiesSent = true;
    const QByteArray payload = localCapabilities().join(QLatin1Char(',')).toUtf8();
    return QStringLiteral("[[CPX3:CAPS:%1]]").arg(b64(payload));
}

QStringList SecureChannelManager::peerCapabilities(const QString &connectionId,
                                                   const QString &target) const
{
    const auto it = m_sessions.constFind(sessionKey(connectionId, target));
    if (it == m_sessions.constEnd()) return {};
    QStringList caps = it->peerCapabilities.values();
    caps.sort(Qt::CaseInsensitive);
    return caps;
}

bool SecureChannelManager::peerSupports(const QString &connectionId,
                                        const QString &target,
                                        const QString &capability) const
{
    const auto it = m_sessions.constFind(sessionKey(connectionId, target));
    return it != m_sessions.constEnd()
        && it->established
        && it->peerCapabilities.contains(capability.trimmed().toCaseFolded());
}


QByteArray SecureChannelManager::fileTransferKey(const QString &connectionId,
                                                 const QString &target,
                                                 const QString &transferId,
                                                 QString *error) const
{
    const auto it = m_sessions.constFind(sessionKey(connectionId, target));
    if (it == m_sessions.constEnd() || !it->established
        || it->rxKey.size() != crypto_kx_SESSIONKEYBYTES
        || it->txKey.size() != crypto_kx_SESSIONKEYBYTES
        || transferId.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("secure session is not ready for direct file transfer");
        return {};
    }

    // Both peers possess the same two directional session keys, but in the
    // opposite rx/tx order. Sort them before hashing so both sides derive the
    // same dedicated direct-file key without exposing either session key.
    QByteArray first = it->rxKey;
    QByteArray second = it->txKey;
    if (second < first) std::swap(first, second);
    QByteArray secret = first + second;
    const QByteArray context = QByteArray("CPX3/direct-file/v1|") + transferId.toUtf8();
    QByteArray key(crypto_secretstream_xchacha20poly1305_KEYBYTES, '\0');
    const int rc = crypto_generichash(
        reinterpret_cast<unsigned char *>(key.data()), key.size(),
        reinterpret_cast<const unsigned char *>(context.constData()), context.size(),
        reinterpret_cast<const unsigned char *>(secret.constData()), secret.size());
    sodium_memzero(first.data(), static_cast<size_t>(first.size()));
    sodium_memzero(second.data(), static_cast<size_t>(second.size()));
    sodium_memzero(secret.data(), static_cast<size_t>(secret.size()));
    if (rc != 0) {
        sodium_memzero(key.data(), static_cast<size_t>(key.size()));
        if (error) *error = QStringLiteral("could not derive direct file-transfer key");
        return {};
    }
    return key;
}

bool SecureChannelManager::deriveSession(const QString &connectionId,
                                         Session &session,
                                         const QByteArray &peerPublicKey,
                                         QString *error) const
{
    Identity identity;
    if (!deriveIdentity(connectionId, identity, error)
        || peerPublicKey.size() != crypto_kx_PUBLICKEYBYTES) {
        if (error && error->isEmpty()) *error = QStringLiteral("invalid CPX3 peer public key");
        return false;
    }
    if (peerPublicKey == identity.publicKey) {
        if (error) *error = QStringLiteral("peer supplied our own secure identity key");
        return false;
    }

    session.peerPublicKey = peerPublicKey;
    session.rxKey.resize(crypto_kx_SESSIONKEYBYTES);
    session.txKey.resize(crypto_kx_SESSIONKEYBYTES);

    const bool clientRole = std::memcmp(identity.publicKey.constData(), peerPublicKey.constData(),
                                        crypto_kx_PUBLICKEYBYTES) < 0;
    int rc = -1;
    if (clientRole) {
        rc = crypto_kx_client_session_keys(
            reinterpret_cast<unsigned char *>(session.rxKey.data()),
            reinterpret_cast<unsigned char *>(session.txKey.data()),
            reinterpret_cast<const unsigned char *>(identity.publicKey.constData()),
            reinterpret_cast<const unsigned char *>(identity.secretKey.constData()),
            reinterpret_cast<const unsigned char *>(peerPublicKey.constData()));
    } else {
        rc = crypto_kx_server_session_keys(
            reinterpret_cast<unsigned char *>(session.rxKey.data()),
            reinterpret_cast<unsigned char *>(session.txKey.data()),
            reinterpret_cast<const unsigned char *>(identity.publicKey.constData()),
            reinterpret_cast<const unsigned char *>(identity.secretKey.constData()),
            reinterpret_cast<const unsigned char *>(peerPublicKey.constData()));
    }
    sodium_memzero(identity.secretKey.data(), identity.secretKey.size());
    if (rc != 0) {
        if (error) *error = QStringLiteral("secure key exchange rejected the peer key");
        sodium_memzero(session.rxKey.data(), session.rxKey.size());
        sodium_memzero(session.txKey.data(), session.txKey.size());
        session.rxKey.clear();
        session.txKey.clear();
        return false;
    }

    session.fingerprint = fingerprintForKey(peerPublicKey);
    session.seenNonces.clear();
    session.seenNonceOrder.clear();
    session.established = true;
    return true;
}

QString SecureChannelManager::beginHandshake(const QString &connectionId,
                                              const QString &target,
                                              QString *notice)
{
    if (!m_initialized || connectionId.isEmpty() || target.trimmed().isEmpty()) {
        if (notice) *notice = QStringLiteral("secure channel is not initialized");
        return {};
    }
    const QString frame = helloFrame(connectionId);
    if (frame.isEmpty()) {
        if (notice) *notice = QStringLiteral("could not derive this connection's secure identity");
        return {};
    }
    Session &session = m_sessions[sessionKey(connectionId, target)];
    session.helloSent = true;
    if (notice) {
        *notice = QStringLiteral("secure handshake sent; local fingerprint %1")
                      .arg(localFingerprint(connectionId));
    }
    return frame;
}

void SecureChannelManager::rememberNonce(Session &session, const QString &nonceToken)
{
    session.seenNonces.insert(nonceToken);
    session.seenNonceOrder.push_back(nonceToken);
    while (session.seenNonceOrder.size() > MaxSeenNonces) {
        const QString oldest = session.seenNonceOrder.takeFirst();
        session.seenNonces.remove(oldest);
    }
}

SecureChannelManager::IncomingResult SecureChannelManager::processIncoming(
    const QString &connectionId,
    const QString &target,
    const QString &payload,
    bool autoReply)
{
    IncomingResult result;
    const QString trimmed = payload.trimmed();
    if (!looksLikeFrame(trimmed)) {
        return result;
    }

    result.kind = IncomingKind::Control;
    const QString body = trimmed.mid(FramePrefix.size(),
                                     trimmed.size() - FramePrefix.size() - FrameSuffix.size());
    const QStringList parts = body.split(QLatin1Char(':'));
    if (parts.isEmpty()) {
        result.kind = IncomingKind::Error;
        result.notice = QStringLiteral("malformed compatible secure frame");
        return result;
    }

    const QString type = parts.at(0).toUpper();
    Session &session = m_sessions[sessionKey(connectionId, target)];

    if (type == QStringLiteral("HELLO")) {
        if (parts.size() != 2) {
            result.kind = IncomingKind::Error;
            result.notice = QStringLiteral("malformed compatible secure HELLO frame");
            return result;
        }
        const QByteArray peerKey = fromB64(parts.at(1));
        const bool alreadyEstablishedWithSameKey =
            session.established && session.peerPublicKey == peerKey;
        const bool changed = session.established && session.peerPublicKey != peerKey;

        if (changed) {
            session.peerCapabilities.clear();
            session.capabilitiesSent = false;
        }

        if (!alreadyEstablishedWithSameKey) {
            QString error;
            if (!deriveSession(connectionId, session, peerKey, &error)) {
                result.kind = IncomingKind::Error;
                result.notice = error;
                return result;
            }
            result.newlyEstablished = true;
        } else {
            // A duplicate HELLO is a harmless handshake refresh.  Do not derive
            // the same keys again because deriveSession() resets replay-nonce
            // history, which would unnecessarily reopen the replay window.
            result.newlyEstablished = false;
            session.capabilitiesSent = false;
        }

        if (!session.pendingPeerCapabilities.isEmpty()) {
            session.peerCapabilities = session.pendingPeerCapabilities;
            session.pendingPeerCapabilities.clear();
            result.capabilitiesUpdated = true;
            result.peerCapabilities = peerCapabilities(connectionId, target);
        }

        result.peerFingerprint = session.fingerprint;
        result.notice = changed
            ? QStringLiteral("peer secure identity key CHANGED; fingerprint %1").arg(session.fingerprint)
            : (alreadyEstablishedWithSameKey
                ? QStringLiteral("secure handshake refreshed; peer fingerprint %1").arg(session.fingerprint)
                : QStringLiteral("secure session established; peer fingerprint %1").arg(session.fingerprint));
        if (autoReply && !session.helloSent) {
            session.helloSent = true;
            result.replyFrame = helloFrame(connectionId);
        }
        return result;
    }

    if (type == QStringLiteral("CAPS")) {
        if (parts.size() != 2) {
            result.kind = IncomingKind::Error;
            result.notice = QStringLiteral("malformed CPX3 capability frame");
            return result;
        }
        const QString decoded = QString::fromUtf8(fromB64(parts.at(1)));
        QSet<QString> caps;
        for (const QString &raw : decoded.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString cap = raw.trimmed().toCaseFolded();
            if (!cap.isEmpty() && cap.size() <= 64) caps.insert(cap);
        }
        if (!session.established) {
            // Relay services can deliver a CAPS frame left in flight from just
            // before a local /secureoff followed by a new HELLO. Keep the
            // capabilities pending, but deliberately keep this race silent:
            // it is neither a protocol error nor useful chat-window output.
            session.pendingPeerCapabilities = caps;
            result.notice.clear();
            return result;
        }
        session.peerCapabilities = caps;
        result.capabilitiesUpdated = true;
        result.peerCapabilities = peerCapabilities(connectionId, target);
        result.notice = result.peerCapabilities.isEmpty()
            ? QStringLiteral("peer advertised no optional CPX capabilities")
            : QStringLiteral("peer capabilities: %1").arg(result.peerCapabilities.join(QStringLiteral(", ")));
        return result;
    }

    if (type == QStringLiteral("MSG")) {
        if (parts.size() != 3 || !session.established) {
            result.kind = IncomingKind::Error;
            result.notice = session.established
                ? QStringLiteral("malformed secure encrypted message")
                : QStringLiteral("encrypted message received before key exchange");
            return result;
        }
        const QString nonceToken = parts.at(1);
        if (session.seenNonces.contains(nonceToken)) {
            result.kind = IncomingKind::Error;
            result.notice = QStringLiteral("replayed secure encrypted message rejected");
            return result;
        }

        const QByteArray nonce = fromB64(nonceToken);
        const QByteArray cipher = fromB64(parts.at(2));
        if (nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
            || cipher.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
            result.kind = IncomingKind::Error;
            result.notice = QStringLiteral("invalid secure encrypted message encoding");
            return result;
        }

        QByteArray plaintext(cipher.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
        unsigned long long plaintextLen = 0;
        const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &plaintextLen,
            nullptr,
            reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size(),
            nullptr, 0,
            reinterpret_cast<const unsigned char *>(nonce.constData()),
            reinterpret_cast<const unsigned char *>(session.rxKey.constData()));
        if (rc != 0) {
            result.kind = IncomingKind::Error;
            result.notice = QStringLiteral("secure encrypted message authentication failed");
            return result;
        }
        plaintext.resize(static_cast<qsizetype>(plaintextLen));
        rememberNonce(session, nonceToken);
        result.kind = IncomingKind::Decrypted;
        result.plaintext = QString::fromUtf8(plaintext);
        result.peerFingerprint = session.fingerprint;
        return result;
    }

    result.kind = IncomingKind::Error;
    result.notice = QStringLiteral("unknown compatible secure frame type %1").arg(type);
    return result;
}

QString SecureChannelManager::encrypt(const QString &connectionId,
                                      const QString &target,
                                      const QString &plaintext,
                                      QString *error)
{
    const auto it = m_sessions.constFind(sessionKey(connectionId, target));
    if (it == m_sessions.constEnd() || !it->established) {
        if (error) *error = QStringLiteral("no secure session with %1; establish a secure session first").arg(target);
        return {};
    }

    const QByteArray message = plaintext.toUtf8();
    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, '\0');
    randombytes_buf(nonce.data(), nonce.size());

    QByteArray cipher(message.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long cipherLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipher.data()), &cipherLen,
        reinterpret_cast<const unsigned char *>(message.constData()), message.size(),
        nullptr, 0, nullptr,
        reinterpret_cast<const unsigned char *>(nonce.constData()),
        reinterpret_cast<const unsigned char *>(it->txKey.constData()));
    if (rc != 0) {
        if (error) *error = QStringLiteral("secure message encryption failed");
        return {};
    }
    cipher.resize(static_cast<qsizetype>(cipherLen));
    return QStringLiteral("[[CPX3:MSG:%1:%2]]").arg(b64(nonce), b64(cipher));
}

bool SecureChannelManager::hasSession(const QString &connectionId, const QString &target) const
{
    const auto it = m_sessions.constFind(sessionKey(connectionId, target));
    return it != m_sessions.constEnd() && it->established;
}

QString SecureChannelManager::peerFingerprint(const QString &connectionId,
                                              const QString &target) const
{
    const auto it = m_sessions.constFind(sessionKey(connectionId, target));
    return it == m_sessions.constEnd() ? QString() : it->fingerprint;
}

void SecureChannelManager::closeSession(const QString &connectionId, const QString &target)
{
    const QString key = sessionKey(connectionId, target);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) return;
    if (!it->rxKey.isEmpty()) sodium_memzero(it->rxKey.data(), it->rxKey.size());
    if (!it->txKey.isEmpty()) sodium_memzero(it->txKey.data(), it->txKey.size());
    m_sessions.erase(it);
}

void SecureChannelManager::closeConnection(const QString &connectionId)
{
    const QString prefix = connectionId + QChar(0x1f);
    QStringList keys;
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.key().startsWith(prefix)) keys << it.key();
    }
    for (const QString &key : keys) {
        auto it = m_sessions.find(key);
        if (it == m_sessions.end()) continue;
        if (!it->rxKey.isEmpty()) sodium_memzero(it->rxKey.data(), it->rxKey.size());
        if (!it->txKey.isEmpty()) sodium_memzero(it->txKey.data(), it->txKey.size());
        m_sessions.erase(it);
    }
}

bool SecureChannelManager::looksLikeFrame(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.startsWith(FramePrefix) && trimmed.endsWith(FrameSuffix);
}
