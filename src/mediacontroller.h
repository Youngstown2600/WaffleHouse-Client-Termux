#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QJsonArray;
class QJsonValue;
class QSocketNotifier;
class QProcess;
class QTimer;

class MediaController final : public QObject
{
    Q_OBJECT
public:
    explicit MediaController(QObject *parent = nullptr);
    ~MediaController() override;

    bool backendAvailable() const;
    QString backendExecutable() const;
    QString backendVersion() const { return m_backendVersion; }
    QString nowPlaying() const { return m_title; }
    QString currentSource() const { return m_source; }
    double positionSeconds() const { return m_position; }
    double durationSeconds() const { return m_duration; }
    int volume() const { return m_volume; }
    bool paused() const { return m_paused; }
    bool muted() const { return m_muted; }
    bool idle() const { return m_idle; }
    QString repeatMode() const { return m_repeatMode; }
    bool shuffleEnabled() const { return m_shuffle; }
    QVector<double> equalizerGains() const { return m_eq; }

    QString statusText() const;
    QStringList statusLines() const;

public slots:
    bool play(const QString &source);
    bool enqueue(const QString &source);
    bool loadPlaylist(const QString &pathOrUrl, bool replace = true);
    void playPlaylistIndex(int index);
    void removePlaylistIndex(int index);
    void clearPlaylist();
    void pause();
    void resume();
    void togglePause();
    void stop();
    void next();
    void previous();
    void seekRelative(double seconds);
    void seekAbsolute(double seconds);
    void setVolume(int percent);
    void setMuted(bool muted);
    void toggleMuted();
    void setShuffle(bool enabled);
    void setRepeatMode(const QString &mode);
    void setEqualizerBand(int band, double gainDb);
    void resetEqualizer();
    void shutdown();

signals:
    void readyChanged(bool ready);
    void nowPlayingChanged(const QString &title);
    void sourceChanged(const QString &source);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void pauseChanged(bool paused);
    void volumeChanged(int percent);
    void muteChanged(bool muted);
    void idleChanged(bool idle);
    void playlistChanged();
    void playlistEntriesChanged(const QStringList &sources,
                                const QStringList &titles,
                                int currentIndex);
    void statusMessage(const QString &message);
    void errorMessage(const QString &message);

private slots:
    void connectIpc();
    void readIpc();
    void processFinished(int exitCode);
    void refreshObservedProperties();
    void drainBackendOutput();

private:
    bool ensureBackend();
    bool startBackendProcess(bool compatibilityMode, QString *failureDetail = nullptr);
    void stopFailedBackend();
    void probeBackendVersion();
    bool sendCommand(const QStringList &parts, const QString &description = QString());
    bool sendJsonCommand(const QJsonArray &command, const QString &description = QString());
    bool ipcConnected() const { return m_ipcFd >= 0; }
    bool connectNativeIpc(QString *error = nullptr);
    bool writeIpc(const QByteArray &wire, QString *error = nullptr);
    void closeIpcSocket();
    void setProperty(const QString &name, const QJsonValue &value);
    void observeProperties();
    void rebuildEqualizer();
    void parseIpcLine(const QByteArray &line);
    bool sendLoadFile(const QString &source, const QString &flags);
    void refreshPlaylistSnapshot(const QJsonValue &data);
    QString buildIpcPath();
    void cleanupIpcPath();
    static bool looksLikeVideoFile(const QString &source);

    QProcess *m_process = nullptr;
    int m_ipcFd = -1;
    QSocketNotifier *m_ipcNotifier = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QString m_mpv;
    QString m_backendVersion;
    QString m_ipcPath;
    QString m_ipcDir;
    QString m_lastBackendError;
    QString m_lastSocketError;
    QByteArray m_ipcBuffer;
    QString m_title;
    QString m_source;
    QStringList m_playlistSources;

    double m_position = 0.0;
    double m_duration = 0.0;
    int m_volume = 80;
    int m_playlistIndex = -1;
    bool m_paused = false;
    bool m_muted = false;
    bool m_idle = true;
    bool m_shuffle = false;
    bool m_observing = false;
    bool m_shuttingDown = false;
    QString m_repeatMode = QStringLiteral("off");
    QVector<double> m_eq = QVector<double>(10, 0.0);
    qint64 m_nextRequestId = 1;
    QHash<qint64, QString> m_pendingRequests;
};
