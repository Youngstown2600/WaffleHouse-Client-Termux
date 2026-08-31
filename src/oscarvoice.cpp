#include "oscarvoice.h"

#include <QHostAddress>
#include <QMetaObject>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QUdpSocket>

#include <algorithm>
#include <cstring>

namespace {
constexpr qsizetype kHeaderSize = 11;
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

QString paError(PaError error)
{
    const char *text = Pa_GetErrorText(error);
    return QString::fromUtf8(text ? text : "unknown PortAudio error");
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

qsizetype OscarVoiceSession::voiceFrameBytes() const
{
    // 20 ms, mono signed 16-bit PCM. Unlike the old fixed 640-byte Qt path,
    // this stays correct if peers negotiate 8/44.1/48 kHz instead of 16 kHz.
    return std::max<qsizetype>(2, static_cast<qsizetype>((m_sampleRate / 50) * 2));
}

bool OscarVoiceSession::configureAudio(int requestedSampleRate, QString *error)
{
    if (!m_portAudioInitialized) {
        const PaError init = Pa_Initialize();
        if (init != paNoError) {
            if (error) *error = QStringLiteral("PortAudio initialization failed: %1").arg(paError(init));
            return false;
        }
        m_portAudioInitialized = true;
    }

    const PaDeviceIndex inputDevice = Pa_GetDefaultInputDevice();
    const PaDeviceIndex outputDevice = Pa_GetDefaultOutputDevice();
    if (inputDevice == paNoDevice || outputDevice == paNoDevice) {
        if (error) *error = QStringLiteral("No usable default Termux microphone/speaker device was found. Run wafflehouse-audio-preflight and check Android microphone permission.");
        resetAudio();
        return false;
    }

    const PaDeviceInfo *inputInfo = Pa_GetDeviceInfo(inputDevice);
    const PaDeviceInfo *outputInfo = Pa_GetDeviceInfo(outputDevice);
    if (!inputInfo || !outputInfo) {
        if (error) *error = QStringLiteral("PortAudio could not inspect the default Termux audio devices.");
        resetAudio();
        return false;
    }

    PaStreamParameters inputParams{};
    inputParams.device = inputDevice;
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = inputInfo->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outputParams{};
    outputParams.device = outputDevice;
    outputParams.channelCount = 1;
    outputParams.sampleFormat = paInt16;
    outputParams.suggestedLatency = outputInfo->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    QList<int> rates;
    if (requestedSampleRate > 0) rates << requestedSampleRate;
    else rates << 16000 << 48000 << 44100 << 8000;

    int chosenRate = 0;
    for (const int rate : rates) {
        if (Pa_IsFormatSupported(&inputParams, &outputParams, static_cast<double>(rate)) == paFormatIsSupported) {
            chosenRate = rate;
            break;
        }
    }
    if (chosenRate <= 0) {
        if (error) *error = QStringLiteral("The default Termux input/output devices have no common mono PCM16 voice format.");
        resetAudio();
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_audioMutex);
        m_captureBuffer.clear();
        m_playbackBuffer.clear();
    }
    m_sampleRate = chosenRate;
    const unsigned long framesPerBuffer = static_cast<unsigned long>(std::max(80, chosenRate / 50));

    PaError pa = Pa_OpenStream(&m_stream,
                               &inputParams,
                               &outputParams,
                               static_cast<double>(chosenRate),
                               framesPerBuffer,
                               paClipOff,
                               &OscarVoiceSession::portAudioCallback,
                               this);
    if (pa != paNoError) {
        if (error) *error = QStringLiteral("Could not open Termux OSCAR voice audio stream: %1").arg(paError(pa));
        m_stream = nullptr;
        resetAudio();
        return false;
    }

    pa = Pa_StartStream(m_stream);
    if (pa != paNoError) {
        if (error) *error = QStringLiteral("Could not start Termux OSCAR voice audio stream: %1").arg(paError(pa));
        resetAudio();
        return false;
    }
    return true;
}

bool OscarVoiceSession::prepare(int requestedSampleRate, QString *error)
{
    if (m_prepared.load()) {
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
    m_prepared.store(true);
    emit statusChanged(QStringLiteral("Termux PortAudio voice ready on UDP %1 at %2 Hz.")
                           .arg(localPort()).arg(m_sampleRate));
    return true;
}

bool OscarVoiceSession::start(const QString &peer,
                              const QString &remoteAddress,
                              quint16 remotePort,
                              QString *error)
{
    if (!m_prepared.load() && !prepare(0, error)) return false;
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
    m_active.store(true);
    emit activeChanged(true);
    emit statusChanged(QStringLiteral("OSCAR voice connected to %1 at %2:%3.")
                           .arg(peer, m_remoteAddress).arg(m_remotePort));
    return true;
}

void OscarVoiceSession::stop()
{
    const bool wasActive = m_active.exchange(false);
    m_prepared.store(false);
    m_muted.store(false);
    m_peer.clear();
    m_remoteAddress.clear();
    m_remotePort = 0;
    m_sequence = 0;
    resetAudio();
    if (m_socket) m_socket->close();
    if (wasActive) emit activeChanged(false);
}

void OscarVoiceSession::resetAudio()
{
    if (m_stream) {
        const PaError active = Pa_IsStreamActive(m_stream);
        if (active == 1) Pa_StopStream(m_stream);
        Pa_CloseStream(m_stream);
        m_stream = nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(m_audioMutex);
        m_captureBuffer.clear();
        m_playbackBuffer.clear();
    }
    if (m_portAudioInitialized) {
        Pa_Terminate();
        m_portAudioInitialized = false;
    }
    m_sampleRate = 0;
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
    m_muted.store(muted);
    emit statusChanged(muted ? QStringLiteral("OSCAR voice microphone muted.")
                             : QStringLiteral("OSCAR voice microphone unmuted."));
}

int OscarVoiceSession::portAudioCallback(const void *inputBuffer,
                                         void *outputBuffer,
                                         unsigned long framesPerBuffer,
                                         const PaStreamCallbackTimeInfo *,
                                         PaStreamCallbackFlags,
                                         void *userData)
{
    auto *self = static_cast<OscarVoiceSession *>(userData);
    if (!self) return paAbort;

    const qsizetype bytes = static_cast<qsizetype>(framesPerBuffer * sizeof(qint16));
    bool queuedCapture = false;
    {
        std::lock_guard<std::mutex> guard(self->m_audioMutex);

        if (outputBuffer) {
            std::memset(outputBuffer, 0, static_cast<size_t>(bytes));
            const qsizetype available = std::min(bytes, self->m_playbackBuffer.size());
            if (available > 0) {
                std::memcpy(outputBuffer, self->m_playbackBuffer.constData(), static_cast<size_t>(available));
                self->m_playbackBuffer.remove(0, available);
            }
        }

        if (inputBuffer && self->m_active.load() && !self->m_muted.load()) {
            self->m_captureBuffer.append(static_cast<const char *>(inputBuffer), bytes);
            queuedCapture = self->m_captureBuffer.size() >= self->voiceFrameBytes();
        }
    }

    if (queuedCapture) {
        QMetaObject::invokeMethod(self, "captureReady", Qt::QueuedConnection);
    }
    return paContinue;
}

void OscarVoiceSession::captureReady()
{
    if (!m_active.load() || m_muted.load()) return;

    for (;;) {
        QByteArray chunk;
        {
            std::lock_guard<std::mutex> guard(m_audioMutex);
            const qsizetype frameBytes = voiceFrameBytes();
            if (m_captureBuffer.size() < frameBytes) break;
            chunk = m_captureBuffer.left(frameBytes);
            m_captureBuffer.remove(0, frameBytes);
        }
        sendAudioChunk(chunk);
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
        if (m_active.load()) {
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
        if (rate != m_sampleRate || channels != 1 || !m_stream) continue;
        const QByteArray audio = packet.mid(kHeaderSize);
        if (audio.isEmpty()) continue;

        std::lock_guard<std::mutex> guard(m_audioMutex);
        // Cap queued playback to roughly one second. If Android stalls audio,
        // dropping old voice is preferable to accumulating seconds of latency.
        const qsizetype maxQueued = std::max<qsizetype>(voiceFrameBytes(), static_cast<qsizetype>(m_sampleRate * 2));
        if (m_playbackBuffer.size() + audio.size() > maxQueued) {
            const qsizetype drop = (m_playbackBuffer.size() + audio.size()) - maxQueued;
            m_playbackBuffer.remove(0, std::min(drop, m_playbackBuffer.size()));
        }
        m_playbackBuffer += audio;
    }
}
