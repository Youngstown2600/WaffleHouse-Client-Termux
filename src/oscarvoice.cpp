#include "oscarvoice.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QHostAddress>
#include <QIODevice>
#include <QMediaDevices>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QUdpSocket>

namespace {
constexpr qsizetype kHeaderSize = 11;
constexpr qsizetype kPayloadBytes = 640; // 20 ms of 16-kHz mono PCM16; also a safe UDP size.
const QByteArray kMagic = QByteArrayLiteral("WHV1");

void appendU16(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xff));
    out.append(static_cast<char>(value & 0xff));
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xff));
    out.append(static_cast<char>((value >> 16) & 0xff));
    out.append(static_cast<char>((value >> 8) & 0xff));
    out.append(static_cast<char>(value & 0xff));
}

quint16 readU16(const QByteArray &data, qsizetype offset)
{
    return (static_cast<quint16>(static_cast<unsigned char>(data.at(offset))) << 8)
         | static_cast<quint16>(static_cast<unsigned char>(data.at(offset + 1)));
}

QString bestLocalIpv4()
{
    QString fallback;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp)
            || !(iface.flags() & QNetworkInterface::IsRunning)
            || (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isNull()) continue;
            const QString text = address.toString();
            if (fallback.isEmpty()) fallback = text;
            if (!address.isLoopback() && !text.startsWith(QStringLiteral("169.254."))) return text;
        }
    }
    return fallback.isEmpty() ? QStringLiteral("127.0.0.1") : fallback;
}
}

OscarVoiceSession::OscarVoiceSession(QObject *parent)
    : QObject(parent), m_socket(new QUdpSocket(this))
{
    connect(m_socket, &QUdpSocket::readyRead, this, &OscarVoiceSession::datagramsReady);
}

OscarVoiceSession::~OscarVoiceSession()
{
    stop();
}

bool OscarVoiceSession::configureAudio(int requestedSampleRate, QString *error)
{
    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (input.isNull() || output.isNull()) {
        if (error) *error = QStringLiteral("No usable default microphone/speaker device was found.");
        return false;
    }

    QList<int> rates;
    if (requestedSampleRate > 0) {
        rates << requestedSampleRate;
    } else {
        rates << 16000 << 48000 << 44100 << 8000;
    }

    QAudioFormat chosen;
    for (const int rate : rates) {
        QAudioFormat format;
        format.setSampleRate(rate);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        if (input.isFormatSupported(format) && output.isFormatSupported(format)) {
            chosen = format;
            break;
        }
    }
    if (!chosen.isValid()) {
        if (error) *error = QStringLiteral("The default input/output devices have no common mono PCM16 voice format.");
        return false;
    }

    resetAudio();
    m_sampleRate = chosen.sampleRate();
    m_source = new QAudioSource(input, chosen, this);
    m_sink = new QAudioSink(output, chosen, this);
    m_capture = m_source->start();
    m_playback = m_sink->start();
    if (!m_capture || !m_playback) {
        if (error) *error = QStringLiteral("Could not start the OSCAR voice audio devices.");
        resetAudio();
        return false;
    }
    connect(m_capture, &QIODevice::readyRead, this, &OscarVoiceSession::captureReady);
    return true;
}

bool OscarVoiceSession::prepare(int requestedSampleRate, QString *error)
{
    if (m_prepared) {
        if (requestedSampleRate <= 0 || requestedSampleRate == m_sampleRate) return true;
        if (error) *error = QStringLiteral("The peer requested %1 Hz but this voice session is already prepared at %2 Hz.")
                                .arg(requestedSampleRate).arg(m_sampleRate);
        return false;
    }
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        if (error) *error = QStringLiteral("Could not bind OSCAR voice UDP socket: %1").arg(m_socket->errorString());
        return false;
    }
    if (!configureAudio(requestedSampleRate, error)) {
        m_socket->close();
        return false;
    }
    m_prepared = true;
    emit statusChanged(QStringLiteral("Voice audio ready on UDP %1 at %2 Hz.")
                           .arg(localPort()).arg(m_sampleRate));
    return true;
}

bool OscarVoiceSession::start(const QString &peer,
                              const QString &remoteAddress,
                              quint16 remotePort,
                              QString *error)
{
    if (!m_prepared && !prepare(0, error)) return false;
    QHostAddress address;
    if (!address.setAddress(remoteAddress) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        if (error) *error = QStringLiteral("Invalid OSCAR voice peer address: %1").arg(remoteAddress);
        return false;
    }
    if (remotePort == 0) {
        if (error) *error = QStringLiteral("The OSCAR voice peer did not advertise a UDP port.");
        return false;
    }
    m_peer = peer;
    m_remoteAddress = address.toString();
    m_remotePort = remotePort;
    m_active = true;
    emit activeChanged(true);
    emit statusChanged(QStringLiteral("OSCAR voice connected to %1 at %2:%3.")
                           .arg(peer, m_remoteAddress).arg(m_remotePort));
    return true;
}

void OscarVoiceSession::stop()
{
    const bool wasActive = m_active;
    m_active = false;
    m_prepared = false;
    m_peer.clear();
    m_remoteAddress.clear();
    m_remotePort = 0;
    m_captureBuffer.clear();
    m_sequence = 0;
    m_muted = false;
    resetAudio();
    if (m_socket) m_socket->close();
    if (wasActive) emit activeChanged(false);
}

void OscarVoiceSession::resetAudio()
{
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    m_capture = nullptr;
    m_playback = nullptr;
}

quint16 OscarVoiceSession::localPort() const
{
    return m_socket ? m_socket->localPort() : 0;
}

QString OscarVoiceSession::localAddress() const
{
    return bestLocalIpv4();
}

void OscarVoiceSession::setMuted(bool muted)
{
    m_muted = muted;
    emit statusChanged(muted ? QStringLiteral("OSCAR voice microphone muted.")
                             : QStringLiteral("OSCAR voice microphone unmuted."));
}

void OscarVoiceSession::captureReady()
{
    if (!m_capture) return;
    m_captureBuffer += m_capture->readAll();
    while (m_captureBuffer.size() >= kPayloadBytes) {
        const QByteArray chunk = m_captureBuffer.left(kPayloadBytes);
        m_captureBuffer.remove(0, kPayloadBytes);
        if (m_active && !m_muted) sendAudioChunk(chunk);
    }
}

void OscarVoiceSession::sendAudioChunk(const QByteArray &chunk)
{
    QHostAddress address(m_remoteAddress);
    if (address.isNull() || m_remotePort == 0 || chunk.isEmpty()) return;
    QByteArray packet;
    packet.reserve(kHeaderSize + chunk.size());
    packet += kMagic;
    appendU32(packet, ++m_sequence);
    appendU16(packet, static_cast<quint16>(m_sampleRate));
    packet.append(char(1));
    packet += chunk;
    if (m_socket->writeDatagram(packet, address, m_remotePort) < 0) {
        emit errorOccurred(QStringLiteral("OSCAR voice UDP send failed: %1").arg(m_socket->errorString()));
    }
}

void OscarVoiceSession::datagramsReady()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray packet;
        packet.resize(static_cast<qsizetype>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 size = m_socket->readDatagram(packet.data(), packet.size(), &sender, &senderPort);
        if (size < kHeaderSize) continue;
        packet.resize(size);
        if (packet.left(4) != kMagic) continue;
        if (m_active) {
            const QHostAddress expected(m_remoteAddress);
            if (!expected.isNull() && sender != expected) continue;
            if (m_remotePort != 0 && senderPort != m_remotePort) {
                // NATs can rewrite a peer's source port. Once a valid WHV1 packet
                // arrives from the expected address, learn that mapped port.
                m_remotePort = senderPort;
            }
        }
        const int rate = readU16(packet, 8);
        const int channels = static_cast<unsigned char>(packet.at(10));
        if (rate != m_sampleRate || channels != 1 || !m_playback) continue;
        const QByteArray audio = packet.mid(kHeaderSize);
        if (!audio.isEmpty()) m_playback->write(audio);
    }
}
