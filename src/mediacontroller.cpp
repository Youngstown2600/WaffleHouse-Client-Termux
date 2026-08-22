#include "mediacontroller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QIODevice>
#include <QSocketNotifier>
#include <QThread>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtGlobal>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
QString boolText(bool value)
{
    return value ? QStringLiteral("on") : QStringLiteral("off");
}
}

MediaController::MediaController(QObject *parent)
    : QObject(parent)
{
    m_mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"));
    probeBackendVersion();

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &MediaController::drainBackendOutput);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &MediaController::drainBackendOutput);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { processFinished(code); });

    m_connectTimer = new QTimer(this);
    m_connectTimer->setInterval(100);
    connect(m_connectTimer, &QTimer::timeout, this, &MediaController::connectIpc);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, &MediaController::refreshObservedProperties);
}

MediaController::~MediaController()
{
    shutdown();
}

bool MediaController::backendAvailable() const
{
    return !m_mpv.isEmpty();
}

QString MediaController::backendExecutable() const
{
    return m_mpv;
}

void MediaController::probeBackendVersion()
{
    if (m_mpv.isEmpty()) return;

    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(m_mpv, {QStringLiteral("--version")});
    if (!probe.waitForStarted(700) || !probe.waitForFinished(1200)) {
        probe.kill();
        probe.waitForFinished(200);
        return;
    }

    const QString output = QString::fromLocal8Bit(probe.readAll()).trimmed();
    const QRegularExpression rx(QStringLiteral("\\bmpv\\s+(\\d+)\\.(\\d+)(?:\\.(\\d+))?"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rx.match(output);
    if (!match.hasMatch()) return;

    const int major = match.captured(1).toInt();
    const int minor = match.captured(2).toInt();
    const QString patch = match.captured(3).isEmpty() ? QStringLiteral("0") : match.captured(3);
    m_backendVersion = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

QString MediaController::statusText() const
{
    const QString title = m_title.isEmpty()
        ? (m_source.isEmpty() ? QStringLiteral("Nothing loaded") : m_source)
        : m_title;
    const QString state = m_idle ? QStringLiteral("idle")
                                 : (m_paused ? QStringLiteral("paused") : QStringLiteral("playing"));
    return QStringLiteral("%1 | %2 | %3% | mute %4")
        .arg(title, state)
        .arg(m_volume)
        .arg(boolText(m_muted));
}

QStringList MediaController::statusLines() const
{
    QString backend = backendAvailable() ? m_mpv : QStringLiteral("mpv not found");
    if (!m_backendVersion.isEmpty()) backend += QStringLiteral(" (mpv %1)").arg(m_backendVersion);

    QStringList out;
    out << QStringLiteral("Backend: %1").arg(backend)
        << QStringLiteral("Now playing: %1")
               .arg(m_title.isEmpty() ? QStringLiteral("<nothing>") : m_title)
        << QStringLiteral("Source: %1")
               .arg(m_source.isEmpty() ? QStringLiteral("<none>") : m_source)
        << QStringLiteral("Position: %1 / %2 sec")
               .arg(m_position, 0, 'f', 1)
               .arg(m_duration, 0, 'f', 1)
        << QStringLiteral("State: %1 | volume %2% | mute %3")
               .arg(m_idle ? QStringLiteral("idle")
                           : (m_paused ? QStringLiteral("paused") : QStringLiteral("playing")))
               .arg(m_volume)
               .arg(boolText(m_muted))
        << QStringLiteral("Playlist entries: %1 | current index: %2")
               .arg(m_playlistSources.size()).arg(m_playlistIndex)
        << QStringLiteral("Shuffle: %1 | repeat: %2")
               .arg(boolText(m_shuffle), m_repeatMode);
    return out;
}

QString MediaController::buildIpcPath()
{
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QDir::tempPath();

    // Use an unpredictable per-process directory rather than a shared predictable
    // /tmp/wafflehouse-client path. This matters on FreeBSD/Unix desktops where
    // Qt may not expose RuntimeLocation: another local user must not be able to
    // pre-create or observe the mpv IPC socket directory.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    // Keep this deliberately short. sockaddr_un.sun_path is only about 104-108
    // bytes on common FreeBSD/Linux targets, and a long XDG runtime path can
    // otherwise make mpv fail to bind the socket even though QProcess started.
    m_ipcDir = QDir(runtime).filePath(
        QStringLiteral("wr-%1-%2")
            .arg(QCoreApplication::applicationPid()).arg(token));

    if (!QDir().mkpath(m_ipcDir)
        || !QFile::setPermissions(m_ipcDir,
                                  QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner)) {
        QDir(m_ipcDir).removeRecursively();
        m_ipcDir.clear();
        return {};
    }
    return QDir(m_ipcDir).filePath(QStringLiteral("m.sock"));
}

void MediaController::cleanupIpcPath()
{
    if (!m_ipcPath.isEmpty()) QFile::remove(m_ipcPath);
    if (!m_ipcDir.isEmpty()) QDir(m_ipcDir).removeRecursively();
    m_ipcPath.clear();
    m_ipcDir.clear();
}

bool MediaController::ensureBackend()
{
    if (!backendAvailable()) {
        emit errorMessage(QStringLiteral(
            "mpv was not found. Install mpv and retry."));
        return false;
    }

    if (m_process->state() != QProcess::NotRunning && ipcConnected()) {
        return true;
    }

    // A half-started backend is not useful. Tear it down before a clean retry.
    if (m_process->state() != QProcess::NotRunning) stopFailedBackend();

    QString normalFailure;
    if (startBackendProcess(false, &normalFailure)) return true;

    // Some long-term-support distributions and older FreeBSD packages carry an
    // mpv build that understands JSON IPC but not every newer command-line
    // hardening option. Retry with the small, long-established option set before
    // declaring media unavailable. The actual media commands remain identical.
    QString compatibilityFailure;
    if (startBackendProcess(true, &compatibilityFailure)) {
        emit statusMessage(QStringLiteral(
            "Media engine ready in mpv compatibility mode (%1).")
                               .arg(m_backendVersion.isEmpty()
                                        ? QStringLiteral("version unavailable")
                                        : QStringLiteral("mpv %1").arg(m_backendVersion)));
        return true;
    }

    QStringList details;
    if (!normalFailure.isEmpty())
        details << QStringLiteral("normal startup: %1").arg(normalFailure);
    if (!compatibilityFailure.isEmpty())
        details << QStringLiteral("compatibility retry: %1").arg(compatibilityFailure);
    emit errorMessage(QStringLiteral("mpv started, but its local control socket did not become ready.%1")
                          .arg(details.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" %1").arg(details.join(QStringLiteral(" | ")))));
    return false;
}

bool MediaController::startBackendProcess(bool compatibilityMode, QString *failureDetail)
{
    cleanupIpcPath();
    m_ipcPath = buildIpcPath();
    if (m_ipcPath.isEmpty()) {
        if (failureDetail)
            *failureDetail = QStringLiteral("unable to create a private runtime directory");
        return false;
    }

    m_lastBackendError.clear();
    m_lastSocketError.clear();
    m_ipcBuffer.clear();
    m_shuttingDown = false;

    QStringList args = {
        QStringLiteral("--no-config"),
        QStringLiteral("--ytdl=no"),
        QStringLiteral("--idle=yes"),
        QStringLiteral("--no-terminal"),
        QStringLiteral("--force-window=no"),
        QStringLiteral("--input-ipc-server=%1").arg(m_ipcPath),
        QStringLiteral("--volume=%1").arg(m_volume)
    };
    if (!compatibilityMode) {
        // These are quality-of-life/safety options, not requirements for the IPC
        // protocol. If a packaged mpv does not know one, ensureBackend() retries
        // without them instead of disabling all WaffleHouse Media playback.
        args.insert(3, QStringLiteral("--input-terminal=no"));
        args.insert(4, QStringLiteral("--load-unsafe-playlists=no"));
        args.insert(5, QStringLiteral("--really-quiet"));
        args.insert(6, QStringLiteral("--audio-display=no"));
        args.insert(7, QStringLiteral("--keep-open=no"));
        args.insert(8, QStringLiteral("--save-position-on-quit=no"));
    }

    m_process->start(m_mpv, args);
    if (!m_process->waitForStarted(2500)) {
        if (failureDetail)
            *failureDetail = QStringLiteral("could not start mpv: %1").arg(m_process->errorString());
        cleanupIpcPath();
        return false;
    }
    m_connectTimer->start();

    // Connecting is the authoritative readiness test. Do not gate this on
    // QFileInfo::exists(): Unix-domain socket nodes can race that test and have
    // platform-specific stat behavior. Slow machines get up to five seconds.
    for (int i = 0; i < 50 && !ipcConnected(); ++i) {
        if (m_process->state() == QProcess::NotRunning) {
            drainBackendOutput();
            break;
        }
        connectIpc();
        if (ipcConnected()) break;
        drainBackendOutput();
        QThread::msleep(100);
    }

    if (ipcConnected()) return true;

    drainBackendOutput();
    QStringList details;
    if (!m_lastSocketError.isEmpty())
        details << QStringLiteral("socket: %1").arg(m_lastSocketError);
    if (!m_lastBackendError.isEmpty())
        details << QStringLiteral("mpv: %1").arg(m_lastBackendError);
    if (details.isEmpty())
        details << QStringLiteral("socket path %1 never accepted a connection").arg(m_ipcPath);
    if (failureDetail) *failureDetail = details.join(QStringLiteral("; "));

    stopFailedBackend();
    return false;
}

void MediaController::stopFailedBackend()
{
    m_connectTimer->stop();
    closeIpcSocket();
    m_shuttingDown = true;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(750)) {
            m_process->kill();
            m_process->waitForFinished(300);
        }
    }
    m_shuttingDown = false;
    cleanupIpcPath();
}

void MediaController::connectIpc()
{
    if (ipcConnected()) {
        m_connectTimer->stop();
        return;
    }
    if (m_ipcPath.isEmpty()) return;

    QString error;
    if (!connectNativeIpc(&error)) {
        if (!error.isEmpty()) m_lastSocketError = error;
        return;
    }

    m_connectTimer->stop();
    m_lastSocketError.clear();
    m_observing = false;
    observeProperties();
    setVolume(m_volume);
    setMuted(m_muted);
    emit readyChanged(true);
    emit statusMessage(QStringLiteral("Media engine ready."));
}

bool MediaController::connectNativeIpc(QString *error)
{
    if (ipcConnected()) return true;
    if (m_ipcPath.isEmpty()) {
        if (error) *error = QStringLiteral("IPC socket path is empty");
        return false;
    }

    const QByteArray encodedPath = QFile::encodeName(m_ipcPath);
    sockaddr_un address{};
    if (encodedPath.isEmpty() || encodedPath.size() >= static_cast<int>(sizeof(address.sun_path))) {
        if (error) {
            *error = QStringLiteral("IPC socket path is invalid or too long (%1 bytes): %2")
                         .arg(encodedPath.size()).arg(m_ipcPath);
        }
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = QStringLiteral("socket(AF_UNIX) failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    // Never leak the controller socket into unrelated child processes.
    const int fdFlags = ::fcntl(fd, F_GETFD);
    if (fdFlags >= 0) (void)::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);

#if defined(Q_OS_FREEBSD)
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, encodedPath.constData(), static_cast<size_t>(encodedPath.size()));
    address.sun_path[encodedPath.size()] = '\0';
    const socklen_t addressLength = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + static_cast<size_t>(encodedPath.size()) + 1U);
#if defined(Q_OS_FREEBSD)
    address.sun_len = static_cast<unsigned char>(addressLength);
#endif

    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), addressLength) != 0) {
        const int savedErrno = errno;
        ::close(fd);
        if (error) {
            *error = QStringLiteral("connect(%1) failed: %2")
                         .arg(m_ipcPath, QString::fromLocal8Bit(std::strerror(savedErrno)));
        }
        return false;
    }

    m_ipcFd = fd;
    m_ipcNotifier = new QSocketNotifier(static_cast<qintptr>(m_ipcFd), QSocketNotifier::Read, this);
    connect(m_ipcNotifier, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { readIpc(); });
    return true;
}

void MediaController::closeIpcSocket()
{
    if (m_ipcNotifier) {
        m_ipcNotifier->setEnabled(false);
        m_ipcNotifier->deleteLater();
        m_ipcNotifier = nullptr;
    }
    if (m_ipcFd >= 0) {
        ::close(m_ipcFd);
        m_ipcFd = -1;
    }
    m_observing = false;
}

bool MediaController::writeIpc(const QByteArray &wire, QString *error)
{
    if (!ipcConnected()) {
        if (error) *error = QStringLiteral("media IPC socket is not connected");
        return false;
    }

    qsizetype offset = 0;
    while (offset < wire.size()) {
#if defined(Q_OS_LINUX)
        const ssize_t written = ::send(m_ipcFd, wire.constData() + offset,
                                       static_cast<size_t>(wire.size() - offset), MSG_NOSIGNAL);
#else
        const ssize_t written = ::send(m_ipcFd, wire.constData() + offset,
                                       static_cast<size_t>(wire.size() - offset), 0);
#endif
        if (written > 0) {
            offset += static_cast<qsizetype>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (error) {
            *error = QStringLiteral("IPC write failed: %1")
                         .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        closeIpcSocket();
        return false;
    }
    return true;
}

bool MediaController::sendJsonCommand(const QJsonArray &command, const QString &description)
{
    if (!ensureBackend()) return false;
    const qint64 requestId = m_nextRequestId++;
    QJsonObject object;
    object.insert(QStringLiteral("command"), command);
    object.insert(QStringLiteral("request_id"), static_cast<double>(requestId));
    const QByteArray wire = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    QString ipcError;
    if (!writeIpc(wire, &ipcError)) {
        emit errorMessage(QStringLiteral("Could not send media command to mpv: %1").arg(ipcError));
        return false;
    }
    m_pendingRequests.insert(requestId,
        description.isEmpty() ? (command.isEmpty() ? QStringLiteral("command") : command.at(0).toString()) : description);
    return true;
}

bool MediaController::sendCommand(const QStringList &parts, const QString &description)
{
    QJsonArray command;
    for (const QString &part : parts) command.append(part);
    return sendJsonCommand(command, description);
}

void MediaController::setProperty(const QString &name, const QJsonValue &value)
{
    QJsonArray command;
    command.append(QStringLiteral("set_property"));
    command.append(name);
    command.append(value);
    sendJsonCommand(command, QStringLiteral("set %1").arg(name));
}

bool MediaController::sendLoadFile(const QString &source, const QString &flags)
{
    const QString wireSource = source.trimmed();
    if (wireSource.isEmpty()) return false;
    return sendCommand({QStringLiteral("loadfile"), wireSource, flags},
                       QStringLiteral("load media"));
}

bool MediaController::looksLikeVideoFile(const QString &source)
{
    if (source.contains(QStringLiteral("://"))) return false;
    static const QStringList extensions = {
        QStringLiteral("avi"), QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("webm"),
        QStringLiteral("mpeg"), QStringLiteral("mpg"), QStringLiteral("ts"),
        QStringLiteral("m2ts"), QStringLiteral("wmv"), QStringLiteral("flv")
    };
    return extensions.contains(QFileInfo(source).suffix().toCaseFolded());
}


bool MediaController::play(const QString &source)
{
    const QString clean = source.trimmed();
    if (clean.isEmpty()) {
        emit errorMessage(QStringLiteral("No media source was supplied."));
        return false;
    }
    if (!ensureBackend()) return false;
    if (!sendLoadFile(clean, QStringLiteral("replace"))) return false;

    m_source = clean;
    m_title = QFileInfo(clean).fileName();
    if (m_title.isEmpty()) m_title = clean;
    m_position = 0.0;
    m_duration = 0.0;
    m_idle = false;
    emit idleChanged(false);
    emit sourceChanged(m_source);
    emit nowPlayingChanged(m_title);
    emit statusMessage(looksLikeVideoFile(clean)
        ? QStringLiteral("Playing video in mpv's video window: %1").arg(m_title)
        : QStringLiteral("Playing: %1").arg(m_title));
    return true;
}

bool MediaController::enqueue(const QString &source)
{
    const QString clean = source.trimmed();
    if (clean.isEmpty() || !ensureBackend()) return false;
    const bool ok = sendLoadFile(clean, QStringLiteral("append-play"));
    if (ok) emit playlistChanged();
    return ok;
}

bool MediaController::loadPlaylist(const QString &pathOrUrl, bool replace)
{
    const QString clean = pathOrUrl.trimmed();
    if (clean.isEmpty() || !ensureBackend()) return false;
    const bool ok = sendCommand({
        QStringLiteral("loadlist"),
        clean,
        replace ? QStringLiteral("replace") : QStringLiteral("append")
    }, QStringLiteral("load playlist"));
    if (ok) {
        m_source = clean;
        emit sourceChanged(clean);
        emit playlistChanged();
        emit statusMessage(QStringLiteral("Loaded playlist: %1").arg(clean));
    }
    return ok;
}

void MediaController::playPlaylistIndex(int index)
{
    if (index < 0) return;
    sendCommand({QStringLiteral("playlist-play-index"), QString::number(index)},
                QStringLiteral("play playlist index"));
}

void MediaController::removePlaylistIndex(int index)
{
    if (index < 0) return;
    sendCommand({QStringLiteral("playlist-remove"), QString::number(index)},
                QStringLiteral("remove playlist entry"));
}

void MediaController::clearPlaylist()
{
    sendCommand({QStringLiteral("playlist-clear")}, QStringLiteral("clear playlist"));
}

void MediaController::pause() { setProperty(QStringLiteral("pause"), true); }
void MediaController::resume()
{
    if (m_idle && !m_playlistSources.isEmpty()) {
        // Stop intentionally preserves the queue. A subsequent Play should be
        // deterministic even though mpv clears its active playlist position:
        // restart from the first queued item without requiring a GUI selection.
        sendCommand({QStringLiteral("playlist-play-index"), QStringLiteral("0")},
                    QStringLiteral("play first queued item after stop"));
        return;
    }
    setProperty(QStringLiteral("pause"), false);
}
void MediaController::togglePause() { sendCommand({QStringLiteral("cycle"), QStringLiteral("pause")}); }
void MediaController::stop()
{
    // mpv's plain `stop` clears the playlist. Client/API users want transport
    // Stop semantics instead: unload playback but preserve the queue.
    sendCommand({QStringLiteral("stop"), QStringLiteral("keep-playlist")},
                QStringLiteral("stop playback and keep playlist"));
    emit statusMessage(QStringLiteral("Playback stopped; playlist preserved."));
}
void MediaController::next() { sendCommand({QStringLiteral("playlist-next"), QStringLiteral("force")}); }
void MediaController::previous() { sendCommand({QStringLiteral("playlist-prev"), QStringLiteral("force")}); }

void MediaController::seekRelative(double seconds)
{
    sendCommand({QStringLiteral("seek"), QString::number(seconds, 'f', 3), QStringLiteral("relative")});
}

void MediaController::seekAbsolute(double seconds)
{
    sendCommand({QStringLiteral("seek"), QString::number(qMax(0.0, seconds), 'f', 3), QStringLiteral("absolute")});
}

void MediaController::setVolume(int percent)
{
    m_volume = qBound(0, percent, 150);
    setProperty(QStringLiteral("volume"), m_volume);
    emit volumeChanged(m_volume);
}

void MediaController::setMuted(bool muted)
{
    m_muted = muted;
    setProperty(QStringLiteral("mute"), muted);
    emit muteChanged(muted);
}

void MediaController::toggleMuted()
{
    setMuted(!m_muted);
}

void MediaController::setShuffle(bool enabled)
{
    if (!ensureBackend()) return;
    if (enabled == m_shuffle) return;
    sendCommand({enabled ? QStringLiteral("playlist-shuffle")
                         : QStringLiteral("playlist-unshuffle")});
    m_shuffle = enabled;
    emit statusMessage(QStringLiteral("Shuffle %1.").arg(enabled ? QStringLiteral("enabled")
                                                                 : QStringLiteral("disabled")));
}

void MediaController::setRepeatMode(const QString &mode)
{
    const QString normalized = mode.trimmed().toCaseFolded();
    if (normalized != QStringLiteral("off")
        && normalized != QStringLiteral("one")
        && normalized != QStringLiteral("all")) {
        emit errorMessage(QStringLiteral("Repeat mode must be off, one, or all."));
        return;
    }

    m_repeatMode = normalized;
    setProperty(QStringLiteral("loop-file"),
                normalized == QStringLiteral("one") ? QJsonValue(QStringLiteral("inf"))
                                                    : QJsonValue(QStringLiteral("no")));
    setProperty(QStringLiteral("loop-playlist"),
                normalized == QStringLiteral("all") ? QJsonValue(QStringLiteral("inf"))
                                                    : QJsonValue(QStringLiteral("no")));
    emit statusMessage(QStringLiteral("Repeat mode: %1").arg(normalized));
}

void MediaController::setEqualizerBand(int band, double gainDb)
{
    if (band < 0 || band >= m_eq.size()) {
        emit errorMessage(QStringLiteral("EQ band must be 0 through 9."));
        return;
    }
    m_eq[band] = qBound(-12.0, gainDb, 12.0);
    rebuildEqualizer();
}

void MediaController::resetEqualizer()
{
    for (double &value : m_eq) value = 0.0;
    sendCommand({QStringLiteral("af"), QStringLiteral("clr"), QString()},
                QStringLiteral("reset equalizer"));
    emit statusMessage(QStringLiteral("Media equalizer reset to flat."));
}

void MediaController::rebuildEqualizer()
{
    static const int frequencies[10] = {
        60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000
    };

    QStringList filters;
    for (int i = 0; i < m_eq.size(); ++i) {
        filters << QStringLiteral("equalizer=f=%1:t=q:w=1:g=%2")
                       .arg(frequencies[i])
                       .arg(m_eq[i], 0, 'f', 1);
    }
    sendCommand({
        QStringLiteral("af"),
        QStringLiteral("set"),
        QStringLiteral("lavfi=[%1]").arg(filters.join(QLatin1Char(',')))
    }, QStringLiteral("update equalizer"));
    emit statusMessage(QStringLiteral("10-band media equalizer updated."));
}

void MediaController::observeProperties()
{
    if (m_observing || !ipcConnected()) return;
    const QStringList names = {
        QStringLiteral("time-pos"), QStringLiteral("duration"), QStringLiteral("pause"),
        QStringLiteral("media-title"), QStringLiteral("volume"), QStringLiteral("mute"),
        QStringLiteral("idle-active"), QStringLiteral("path"), QStringLiteral("playlist-pos"),
        QStringLiteral("playlist")
    };
    int id = 100;
    for (const QString &name : names) {
        QJsonArray command;
        command.append(QStringLiteral("observe_property"));
        command.append(id++);
        command.append(name);
        sendJsonCommand(command, QStringLiteral("observe %1").arg(name));
    }
    m_observing = true;
    m_refreshTimer->start();
}

void MediaController::refreshObservedProperties()
{
    if (!ipcConnected()) return;
    if (m_process->state() == QProcess::NotRunning) {
        m_refreshTimer->stop();
        emit readyChanged(false);
    }
}

void MediaController::readIpc()
{
    if (!ipcConnected()) return;

    char buffer[8192];
    while (true) {
        const ssize_t received = ::recv(m_ipcFd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (received > 0) {
            m_ipcBuffer.append(buffer, static_cast<qsizetype>(received));
            continue;
        }
        if (received == 0) {
            closeIpcSocket();
            emit readyChanged(false);
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        m_lastSocketError = QStringLiteral("IPC read failed: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno)));
        closeIpcSocket();
        emit readyChanged(false);
        break;
    }

    while (true) {
        const int newline = m_ipcBuffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = m_ipcBuffer.left(newline);
        m_ipcBuffer.remove(0, newline + 1);
        parseIpcLine(line);
    }
}

void MediaController::refreshPlaylistSnapshot(const QJsonValue &data)
{
    if (!data.isArray()) return;
    const QJsonArray array = data.toArray();
    QStringList sources;
    QStringList titles;
    int current = -1;
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) continue;
        const QJsonObject entry = array.at(i).toObject();
        QString source = entry.value(QStringLiteral("filename")).toString();
        QString title = entry.value(QStringLiteral("title")).toString();
        if (title.isEmpty()) {
            title = QFileInfo(source).fileName();
            if (title.isEmpty()) title = source;
        }
        sources << source;
        titles << title;
        if (entry.value(QStringLiteral("current")).toBool(false)) current = i;
    }
    m_playlistSources = sources;
    m_playlistIndex = current;
    emit playlistEntriesChanged(sources, titles, current);
    emit playlistChanged();
}

void MediaController::parseIpcLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return;

    const QJsonObject object = doc.object();
    if (object.contains(QStringLiteral("request_id"))) {
        const qint64 requestId = static_cast<qint64>(object.value(QStringLiteral("request_id")).toDouble());
        const QString description = m_pendingRequests.take(requestId);
        const QString result = object.value(QStringLiteral("error")).toString();
        if (!description.isEmpty() && !result.isEmpty() && result != QStringLiteral("success")) {
            emit errorMessage(QStringLiteral("mpv command '%1' failed: %2").arg(description, result));
        }
    }

    const QString event = object.value(QStringLiteral("event")).toString();
    if (event == QStringLiteral("end-file")
        && object.value(QStringLiteral("reason")).toString() == QStringLiteral("error")) {
        QString detail = object.value(QStringLiteral("file_error")).toString().trimmed();
        if (detail.isEmpty()) detail = object.value(QStringLiteral("error")).toString().trimmed();
        if (detail.isEmpty()) detail = QStringLiteral("unknown media/network error");
        emit errorMessage(QStringLiteral("mpv could not play the media stream: %1").arg(detail));
        return;
    }
    if (event != QStringLiteral("property-change")) return;

    const QString name = object.value(QStringLiteral("name")).toString();
    const QJsonValue data = object.value(QStringLiteral("data"));

    if (name == QStringLiteral("time-pos") && data.isDouble()) {
        m_position = data.toDouble();
        emit positionChanged(m_position);
    } else if (name == QStringLiteral("duration") && data.isDouble()) {
        m_duration = data.toDouble();
        emit durationChanged(m_duration);
    } else if (name == QStringLiteral("pause") && data.isBool()) {
        m_paused = data.toBool();
        emit pauseChanged(m_paused);
    } else if (name == QStringLiteral("media-title") && data.isString()) {
        m_title = data.toString();
        emit nowPlayingChanged(m_title);
    } else if (name == QStringLiteral("volume") && data.isDouble()) {
        m_volume = qBound(0, qRound(data.toDouble()), 150);
        emit volumeChanged(m_volume);
    } else if (name == QStringLiteral("mute") && data.isBool()) {
        m_muted = data.toBool();
        emit muteChanged(m_muted);
    } else if (name == QStringLiteral("idle-active") && data.isBool()) {
        m_idle = data.toBool();
        emit idleChanged(m_idle);
        if (m_idle) {
            m_position = 0.0;
            m_duration = 0.0;
            emit positionChanged(0.0);
            emit durationChanged(0.0);
        }
    } else if (name == QStringLiteral("path") && data.isString()) {
        m_source = data.toString();
        emit sourceChanged(m_source);
    } else if (name == QStringLiteral("playlist-pos") && data.isDouble()) {
        m_playlistIndex = qRound(data.toDouble());
    } else if (name == QStringLiteral("playlist")) {
        refreshPlaylistSnapshot(data);
    }
}

void MediaController::drainBackendOutput()
{
    const QByteArray out = m_process->readAllStandardOutput();
    const QByteArray err = m_process->readAllStandardError();
    Q_UNUSED(out);
    if (!err.trimmed().isEmpty()) {
        m_lastBackendError = QString::fromLocal8Bit(err).trimmed().right(2000);
    }
}

void MediaController::processFinished(int exitCode)
{
    m_connectTimer->stop();
    m_refreshTimer->stop();
    m_observing = false;
    closeIpcSocket();
    cleanupIpcPath();
    emit readyChanged(false);
    if (!m_shuttingDown && exitCode != 0) {
        emit errorMessage(m_lastBackendError.isEmpty()
            ? QStringLiteral("mpv exited unexpectedly with status %1.").arg(exitCode)
            : QStringLiteral("mpv exited unexpectedly with status %1: %2")
                  .arg(exitCode).arg(m_lastBackendError));
    }
}

void MediaController::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    m_connectTimer->stop();
    m_refreshTimer->stop();
    if (ipcConnected()) {
        // Ask mpv to exit cleanly, then close our native Unix-domain socket.
        QJsonArray command;
        command.append(QStringLiteral("quit"));
        QJsonObject object;
        object.insert(QStringLiteral("command"), command);
        const QByteArray wire = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        QString ignored;
        (void)writeIpc(wire, &ignored);
        closeIpcSocket();
    }
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(1000)) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
    cleanupIpcPath();
}
