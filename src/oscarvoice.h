#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

#include <atomic>
#include <mutex>

#include <portaudio.h>

class QUdpSocket;

// WaffleHouse-to-WaffleHouse audio transport whose call setup is signaled over
// OSCAR channel-2 rendezvous. The Termux build intentionally uses PortAudio
// instead of Qt Multimedia so the native CLI does not depend on Qt6Gui or the
// Android/X11 Qt multimedia stack.
class OscarVoiceSession final : public QObject {
    Q_OBJECT
public:
    explicit OscarVoiceSession(QObject *parent = nullptr);
    ~OscarVoiceSession() override;

    bool prepare(int requestedSampleRate = 0, QString *error = nullptr);
    bool start(const QString &peer,
               const QString &remoteAddress,
               quint16 remotePort,
               QString *error = nullptr);
    void stop();

    bool isPrepared() const { return m_prepared.load(); }
    bool isActive() const { return m_active.load(); }
    QString peer() const { return m_peer; }
    QString remoteAddress() const { return m_remoteAddress; }
    quint16 remotePort() const { return m_remotePort; }
    quint16 localPort() const;
    QString localAddress() const;
    int sampleRate() const { return m_sampleRate; }
    int channelCount() const { return 1; }

    void setMuted(bool muted);
    bool muted() const { return m_muted.load(); }

signals:
    void statusChanged(const QString &message);
    void errorOccurred(const QString &message);
    void activeChanged(bool active);

private slots:
    void captureReady();
    void datagramsReady();

private:
    static int portAudioCallback(const void *inputBuffer,
                                 void *outputBuffer,
                                 unsigned long framesPerBuffer,
                                 const PaStreamCallbackTimeInfo *timeInfo,
                                 PaStreamCallbackFlags statusFlags,
                                 void *userData);

    void resetAudio();
    bool configureAudio(int requestedSampleRate, QString *error);
    void sendAudioChunk(const QByteArray &chunk);
    qsizetype voiceFrameBytes() const;

    QUdpSocket *m_socket = nullptr;
    PaStream *m_stream = nullptr;
    bool m_portAudioInitialized = false;

    mutable std::mutex m_audioMutex;
    QByteArray m_captureBuffer;
    QByteArray m_playbackBuffer;

    QString m_peer;
    QString m_remoteAddress;
    quint16 m_remotePort = 0;
    quint32 m_sequence = 0;
    int m_sampleRate = 0;
    std::atomic_bool m_prepared{false};
    std::atomic_bool m_active{false};
    std::atomic_bool m_muted{false};
};
