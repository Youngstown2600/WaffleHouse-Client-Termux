#pragma once

#include <QObject>
#include <QString>

class QAudioSink;
class QAudioSource;
class QIODevice;
class QUdpSocket;

// WaffleHouse-to-WaffleHouse audio transport whose call setup is signaled over
// OSCAR channel-2 rendezvous.  The audio payload is intentionally namespaced
// to WaffleHouse-Client; it does not claim compatibility with the proprietary
// media framing used by vintage AIM Talk clients.
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

    bool isPrepared() const { return m_prepared; }
    bool isActive() const { return m_active; }
    QString peer() const { return m_peer; }
    QString remoteAddress() const { return m_remoteAddress; }
    quint16 remotePort() const { return m_remotePort; }
    quint16 localPort() const;
    QString localAddress() const;
    int sampleRate() const { return m_sampleRate; }
    int channelCount() const { return 1; }

    void setMuted(bool muted);
    bool muted() const { return m_muted; }

signals:
    void statusChanged(const QString &message);
    void errorOccurred(const QString &message);
    void activeChanged(bool active);

private slots:
    void captureReady();
    void datagramsReady();

private:
    void resetAudio();
    bool configureAudio(int requestedSampleRate, QString *error);
    void sendAudioChunk(const QByteArray &chunk);

    QUdpSocket *m_socket = nullptr;
    QAudioSource *m_source = nullptr;
    QAudioSink *m_sink = nullptr;
    QIODevice *m_capture = nullptr;
    QIODevice *m_playback = nullptr;
    QByteArray m_captureBuffer;

    QString m_peer;
    QString m_remoteAddress;
    quint16 m_remotePort = 0;
    quint32 m_sequence = 0;
    int m_sampleRate = 0;
    bool m_prepared = false;
    bool m_active = false;
    bool m_muted = false;
};
