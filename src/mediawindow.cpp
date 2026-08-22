#include "mediawindow.h"
#include "mediacontroller.h"
#include "appbranding.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDesktopServices>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

MediaWindow::MediaWindow(QWidget *parent)
    : QMainWindow(parent),
      m_media(new MediaController(this))
{
    setWindowTitle(QStringLiteral("WaffleHouse Media Center — %1").arg(appDisplayName()));
    // Keep the media center as a real independent window even though MainWindow
    // owns its lifetime. This avoids desktop/WM differences on Linux and FreeBSD.
    setWindowFlag(Qt::Window, true);
    resize(780, 580);
    setMinimumSize(640, 460);
    setAcceptDrops(true);
    setAttribute(Qt::WA_DeleteOnClose, false);

    buildUi();

    connect(m_media, &MediaController::nowPlayingChanged, this, [this](const QString &title) {
        m_title->setText(title.isEmpty() ? QStringLiteral("Nothing playing") : title);
    });
    connect(m_media, &MediaController::sourceChanged, this, [this](const QString &source) {
        m_source->setText(source);
    });
    connect(m_media, &MediaController::positionChanged, this, [this](double value) {
        m_position = value;
        if (!m_draggingSeek && m_duration > 0.0) {
            m_seek->setValue(qBound(0, qRound((m_position / m_duration) * 1000.0), 1000));
        }
        updateTime();
    });
    connect(m_media, &MediaController::durationChanged, this, [this](double value) {
        m_duration = value;
        if (!m_draggingSeek && m_duration <= 0.0) m_seek->setValue(0);
        updateTime();
    });
    connect(m_media, &MediaController::volumeChanged, this, [this](int value) {
        if (m_volume->value() != value) m_volume->setValue(value);
    });
    connect(m_media, &MediaController::playlistEntriesChanged,
            this, &MediaWindow::syncPlaylist);
    connect(m_media, &MediaController::errorMessage, this, [this](const QString &message) {
        statusBar()->showMessage(message, 10000);
    });
    connect(m_media, &MediaController::statusMessage, this, [this](const QString &message) {
        statusBar()->showMessage(message, 7000);
    });

    QString backend = m_media->backendAvailable()
        ? QStringLiteral("mpv backend ready")
        : QStringLiteral("mpv not found — install mpv");
    if (!m_media->backendVersion().isEmpty()) {
        backend += QStringLiteral(" (%1)").arg(m_media->backendVersion());
    }
    m_backend->setText(backend);
}

void MediaWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(10);

    auto *header = new QGroupBox(QStringLiteral("Now Playing"), central);
    auto *headerLayout = new QVBoxLayout(header);
    m_title = new QLabel(QStringLiteral("Nothing playing"), header);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(m_title);

    m_source = new QLabel(QStringLiteral(
        "Local files, SHOUTcast/Icecast, HTTP/HLS streams, and playlists"), header);
    m_source->setWordWrap(true);
    m_source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(m_source);

    auto *seekRow = new QHBoxLayout;
    m_seek = new QSlider(Qt::Horizontal, header);
    m_seek->setRange(0, 1000);
    m_time = new QLabel(QStringLiteral("00:00 / 00:00"), header);
    seekRow->addWidget(m_seek, 1);
    seekRow->addWidget(m_time);
    headerLayout->addLayout(seekRow);

    auto *controls = new QHBoxLayout;
    auto *prev = new QPushButton(QStringLiteral("Previous"), header);
    auto *play = new QPushButton(QStringLiteral("Play"), header);
    auto *pause = new QPushButton(QStringLiteral("Pause"), header);
    auto *stop = new QPushButton(QStringLiteral("Stop"), header);
    auto *next = new QPushButton(QStringLiteral("Next"), header);
    auto *mute = new QPushButton(QStringLiteral("Mute"), header);
    controls->addWidget(prev);
    controls->addWidget(play);
    controls->addWidget(pause);
    controls->addWidget(stop);
    controls->addWidget(next);
    controls->addWidget(mute);
    controls->addStretch(1);

    controls->addWidget(new QLabel(QStringLiteral("Volume"), header));
    m_volume = new QSlider(Qt::Horizontal, header);
    m_volume->setRange(0, 150);
    m_volume->setValue(80);
    m_volume->setMaximumWidth(130);
    controls->addWidget(m_volume);
    headerLayout->addLayout(controls);

    auto *modeRow = new QHBoxLayout;
    m_shuffle = new QCheckBox(QStringLiteral("Shuffle"), header);
    m_repeat = new QComboBox(header);
    m_repeat->addItems({QStringLiteral("Repeat Off"), QStringLiteral("Repeat One"), QStringLiteral("Repeat All")});
    m_backend = new QLabel(header);
    modeRow->addWidget(m_shuffle);
    modeRow->addWidget(m_repeat);
    modeRow->addStretch(1);
    modeRow->addWidget(m_backend);
    headerLayout->addLayout(modeRow);
    outer->addWidget(header);

    auto *playlistBox = new QGroupBox(QStringLiteral("Playlist / Queue"), central);
    auto *playlistLayout = new QVBoxLayout(playlistBox);
    m_playlist = new QListWidget(playlistBox);
    m_playlist->setSelectionMode(QAbstractItemView::ExtendedSelection);
    playlistLayout->addWidget(m_playlist, 1);

    auto *playlistButtons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add Files"), playlistBox);
    auto *stream = new QPushButton(QStringLiteral("Stream URL"), playlistBox);
    auto *shoutcast = new QPushButton(QStringLiteral("SHOUTcast Search"), playlistBox);
    auto *internetList = new QPushButton(QStringLiteral("Playlist URL"), playlistBox);
    auto *loadList = new QPushButton(QStringLiteral("Load Playlist"), playlistBox);
    auto *saveList = new QPushButton(QStringLiteral("Save Playlist"), playlistBox);
    auto *remove = new QPushButton(QStringLiteral("Remove"), playlistBox);
    auto *clear = new QPushButton(QStringLiteral("Clear Queue"), playlistBox);
    playlistButtons->addWidget(add);
    playlistButtons->addWidget(stream);
    playlistButtons->addWidget(shoutcast);
    playlistButtons->addWidget(internetList);
    playlistButtons->addWidget(loadList);
    playlistButtons->addWidget(saveList);
    playlistButtons->addStretch(1);
    playlistButtons->addWidget(remove);
    playlistButtons->addWidget(clear);
    playlistLayout->addLayout(playlistButtons);
    outer->addWidget(playlistBox, 1);

    auto *eqBox = new QGroupBox(QStringLiteral("10-Band Equalizer"), central);
    auto *eqLayout = new QHBoxLayout(eqBox);
    static const char *labels[10] = {"60", "170", "310", "600", "1K", "3K", "6K", "12K", "14K", "16K"};
    for (int i = 0; i < 10; ++i) {
        auto *column = new QVBoxLayout;
        auto *gain = new QLabel(QStringLiteral("0"), eqBox);
        gain->setAlignment(Qt::AlignCenter);
        auto *slider = new QSlider(Qt::Vertical, eqBox);
        slider->setRange(-12, 12);
        slider->setValue(0);
        slider->setInvertedAppearance(true);
        auto *freq = new QLabel(QString::fromLatin1(labels[i]), eqBox);
        freq->setAlignment(Qt::AlignCenter);
        column->addWidget(gain);
        column->addWidget(slider, 1);
        column->addWidget(freq);
        eqLayout->addLayout(column);
        m_eqSliders.append(slider);
        connect(slider, &QSlider::valueChanged, this, [this, i, gain](int value) {
            gain->setText(QString::number(value));
            m_media->setEqualizerBand(i, value);
        });
    }
    auto *flat = new QPushButton(QStringLiteral("Flat"), eqBox);
    eqLayout->addWidget(flat, 0, Qt::AlignBottom);
    outer->addWidget(eqBox);

    setCentralWidget(central);

    connect(prev, &QPushButton::clicked, m_media, &MediaController::previous);
    connect(play, &QPushButton::clicked, this, [this] {
        if (m_media->idle()) {
            m_media->resume();
        } else if (m_playlist->currentRow() >= 0) {
            playSelected();
        } else {
            m_media->resume();
        }
    });
    connect(pause, &QPushButton::clicked, m_media, &MediaController::pause);
    connect(stop, &QPushButton::clicked, m_media, &MediaController::stop);
    connect(next, &QPushButton::clicked, m_media, &MediaController::next);
    connect(mute, &QPushButton::clicked, m_media, &MediaController::toggleMuted);
    connect(m_volume, &QSlider::valueChanged, m_media, &MediaController::setVolume);
    connect(m_shuffle, &QCheckBox::toggled, m_media, &MediaController::setShuffle);
    connect(m_repeat, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_media->setRepeatMode(index == 1 ? QStringLiteral("one")
                                         : index == 2 ? QStringLiteral("all")
                                                      : QStringLiteral("off"));
    });
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_draggingSeek = true; });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_draggingSeek = false;
        if (m_duration > 0.0) m_media->seekAbsolute(m_duration * m_seek->value() / 1000.0);
    });
    connect(add, &QPushButton::clicked, this, &MediaWindow::openMediaFiles);
    connect(stream, &QPushButton::clicked, this, &MediaWindow::openStreamDialog);
    connect(shoutcast, &QPushButton::clicked, this, &MediaWindow::searchShoutcastDirectory);
    connect(internetList, &QPushButton::clicked, this, &MediaWindow::openInternetPlaylistDialog);
    connect(loadList, &QPushButton::clicked, this, &MediaWindow::openPlaylistDialog);
    connect(saveList, &QPushButton::clicked, this, &MediaWindow::savePlaylistDialog);
    connect(remove, &QPushButton::clicked, this, &MediaWindow::removeSelected);
    connect(clear, &QPushButton::clicked, this, &MediaWindow::clearPlaylist);
    connect(flat, &QPushButton::clicked, this, [this] {
        for (QSlider *slider : m_eqSliders) slider->setValue(0);
        m_media->resetEqualizer();
    });
    connect(m_playlist, &QListWidget::itemDoubleClicked, this, [this] { playSelected(); });
}

void MediaWindow::showAndRaise()
{
    show();
    raise();
    activateWindow();
}

void MediaWindow::openMediaFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Open media"),
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        QStringLiteral(
            "Media (*.mp3 *.flac *.ogg *.opus *.wav *.aac *.m4a *.wma "
            "*.avi *.mp4 *.m4v *.mkv *.mov *.webm *.mpeg *.mpg *.ts *.m2ts *.wmv *.flv);;"
            "Audio (*.mp3 *.flac *.ogg *.opus *.wav *.aac *.m4a *.wma);;"
            "Video (*.avi *.mp4 *.m4v *.mkv *.mov *.webm *.mpeg *.mpg *.ts *.m2ts *.wmv *.flv);;"
            "All files (*)"));
    addSources(files, true);
}

void MediaWindow::openStreamDialog()
{
    bool ok = false;
    const QString url = QInputDialog::getText(
        this, QStringLiteral("Open internet stream"),
        QStringLiteral("SHOUTcast / Icecast / HTTP(S) / HLS media URL:"),
        QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || url.isEmpty()) return;
    m_media->play(url);
}

void MediaWindow::searchShoutcastDirectory()
{
    bool ok = false;
    const QString query = QInputDialog::getText(
        this, QStringLiteral("Search SHOUTcast Directory"),
        QStringLiteral("Station, artist, or genre:"),
        QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || query.isEmpty()) return;

    const QByteArray encoded = QByteArrayLiteral("https://directory.shoutcast.com/Search?query=")
        + QUrl::toPercentEncoding(query);
    const QUrl url = QUrl::fromEncoded(encoded);
    if (!QDesktopServices::openUrl(url)) {
        QMessageBox::information(this, QStringLiteral("SHOUTcast Directory"),
                                 QStringLiteral("Open this URL in a browser:\n%1").arg(url.toString()));
    } else {
        statusBar()->showMessage(QStringLiteral("Opened SHOUTcast directory search for: %1").arg(query), 5000);
    }
}

void MediaWindow::openInternetPlaylistDialog()
{
    bool ok = false;
    const QString url = QInputDialog::getText(
        this, QStringLiteral("Open internet playlist"),
        QStringLiteral("M3U / PLS playlist URL (use Stream URL for HLS .m3u8 manifests):"),
        QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || url.isEmpty()) return;
    m_media->loadPlaylist(url, true);
}

void MediaWindow::openPlaylistDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load playlist"), {},
        QStringLiteral("Playlists (*.m3u *.m3u8 *.pls);;All files (*)"));
    if (path.isEmpty()) return;
    m_media->loadPlaylist(path, true);
}

void MediaWindow::savePlaylistDialog()
{
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save playlist"), {},
        QStringLiteral("M3U8 Playlist (*.m3u8)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) path += QStringLiteral(".m3u8");

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Playlist"), file.errorString());
        return;
    }

    QTextStream out(&file);
    out << "#EXTM3U\n";
    for (int i = 0; i < m_playlist->count(); ++i) {
        out << m_playlist->item(i)->data(Qt::UserRole).toString() << '\n';
    }
    statusBar()->showMessage(QStringLiteral("Saved playlist: %1").arg(path), 5000);
}

void MediaWindow::addSources(const QStringList &sources, bool playFirst)
{
    QStringList added;
    for (const QString &source : sources) {
        if (!source.trimmed().isEmpty()) added << source.trimmed();
    }
    if (added.isEmpty()) return;

    if (playFirst) {
        m_media->play(added.first());
        for (int i = 1; i < added.size(); ++i) {
            m_media->enqueue(added.at(i));
        }
    } else {
        for (const QString &source : added) {
            m_media->enqueue(source);
        }
    }
}

void MediaWindow::syncPlaylist(const QStringList &sources,
                               const QStringList &titles,
                               int currentIndex)
{
    if (m_syncingPlaylist) return;
    m_syncingPlaylist = true;
    const QSignalBlocker blocker(m_playlist);
    m_playlist->clear();
    for (int i = 0; i < sources.size(); ++i) {
        const QString title = i < titles.size() && !titles.at(i).isEmpty()
            ? titles.at(i) : sources.at(i);
        auto *item = new QListWidgetItem(title, m_playlist);
        item->setData(Qt::UserRole, sources.at(i));
        item->setToolTip(sources.at(i));
    }
    if (currentIndex >= 0 && currentIndex < m_playlist->count()) {
        m_playlist->setCurrentRow(currentIndex);
    }
    m_syncingPlaylist = false;
}

void MediaWindow::playSelected()
{
    const int row = m_playlist->currentRow();
    if (row < 0) return;
    m_media->playPlaylistIndex(row);
}

void MediaWindow::removeSelected()
{
    QList<int> rows;
    for (QListWidgetItem *item : m_playlist->selectedItems()) {
        rows << m_playlist->row(item);
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) m_media->removePlaylistIndex(row);
}

void MediaWindow::clearPlaylist()
{
    m_media->clearPlaylist();
}

QString MediaWindow::timeText(double seconds) const
{
    const int total = qMax(0, qRound(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

void MediaWindow::updateTime()
{
    m_time->setText(QStringLiteral("%1 / %2").arg(timeText(m_position), timeText(m_duration)));
}

void MediaWindow::closeEvent(QCloseEvent *event)
{
    hide();
    event->ignore();
}

void MediaWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MediaWindow::dropEvent(QDropEvent *event)
{
    QStringList sources;
    for (const QUrl &url : event->mimeData()->urls()) {
        sources << (url.isLocalFile() ? url.toLocalFile() : url.toString());
    }
    addSources(sources, true);
    event->acceptProposedAction();
}
