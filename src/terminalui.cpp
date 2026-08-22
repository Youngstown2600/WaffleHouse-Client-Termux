#include "terminalui.h"
#include "appbranding.h"
#include "platforminfo.h"
#include "mediacontroller.h"

#include "ircbackend.h"
#include "oscarbackend.h"
#include "telnetbackend.h"
#include "bbsdirectory.h"
#include "sipbackend.h"
#include "sipcontroller.h"
#include "notificationmanager.h"
#include "filetransport.h"
#include "useractivity.h"
#include "trunkmonkey/Profile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#ifndef WAFFLEHOUSE_TERMUX
#include <QDesktopServices>
#endif
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <cwchar>
#include <wchar.h>
#include <stdexcept>

#include <curses.h>

namespace {
constexpr int PairHeader = 1;
constexpr int PairOnline = 2;
constexpr int PairError = 3;
constexpr int PairUnread = 4;
constexpr int PairBorder = 5;
constexpr int PairSecure = 6;
constexpr int PairFooter = 7;

QString timestamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
}

QStringList cliThemeNames()
{
    return {
        QStringLiteral("system"), QStringLiteral("classic"), QStringLiteral("phosphor"),
        QStringLiteral("amber"), QStringLiteral("ice"), QStringLiteral("hacker"),
        QStringLiteral("matrix"), QStringLiteral("midnight"), QStringLiteral("classic-light"),
        QStringLiteral("solarized"), QStringLiteral("solarized-dark"), QStringLiteral("dracula"),
        QStringLiteral("nord"), QStringLiteral("cyberpunk"), QStringLiteral("blood-moon"),
        QStringLiteral("ocean"), QStringLiteral("retro-blue"), QStringLiteral("monochrome"),
        QStringLiteral("blue-box"), QStringLiteral("red-box"), QStringLiteral("beige-box"),
        QStringLiteral("2600"), QStringLiteral("wargames"), QStringLiteral("crt-green"),
        QStringLiteral("vt220"), QStringLiteral("cobalt"), QStringLiteral("vaporwave"),
        QStringLiteral("stealth"), QStringLiteral("synthwave"), QStringLiteral("c64"),
        QStringLiteral("dos"), QStringLiteral("waffle-iron"), QStringLiteral("ghostline"),
        QStringLiteral("hot-dog-stand"), QStringLiteral("neon-miami")
    };
}

int fitDialogWidth(int desired, int preferredMin, int preferredMax)
{
    const int available = std::max(12, COLS - 2);
    const int high = std::min(preferredMax, available);
    const int low = std::min(preferredMin, high);
    return std::clamp(desired, low, high);
}

int fitDialogHeight(int desired, int preferredMin, int preferredMax)
{
    const int available = std::max(6, LINES - 2);
    const int high = std::min(preferredMax, available);
    const int low = std::min(preferredMin, high);
    return std::clamp(desired, low, high);
}

bool isTerminalBackspace(uint codepoint)
{
    // Depending on terminal/termios settings, Backspace can arrive as ASCII
    // BS (0x08), DEL (0x7f), or the terminal's configured erase character.
    const uint configuredErase = static_cast<unsigned char>(erasechar());
    return codepoint == 8 || codepoint == 127 || codepoint == configuredErase;
}


bool isOnlineBuddy(const QSet<QString> &set, const QString &buddy)
{
    return set.contains(buddy.toCaseFolded());
}
} // namespace

TerminalUi::TerminalUi(QObject *parent)
    : QObject(parent)
{
    m_sipController = new SipController(this);
    m_mediaController = new MediaController(this);
    connect(m_mediaController, &MediaController::statusMessage, this,
            [this](const QString &message) { status(QStringLiteral("[media] %1").arg(message)); });
    connect(m_mediaController, &MediaController::errorMessage, this,
            [this](const QString &message) { status(QStringLiteral("[media] [error] %1").arg(message)); });
    connect(m_mediaController, &MediaController::nowPlayingChanged, this,
            [this](const QString &title) {
                if (!title.isEmpty()) status(QStringLiteral("[media] Now playing: %1").arg(title));
            });
    connect(&m_tickTimer, &QTimer::timeout, this, &TerminalUi::tick);
    connect(&m_directTransfers, &CpxDirectTransferManager::progress,
            this, &TerminalUi::handleDirectProgress);
    connect(&m_directTransfers, &CpxDirectTransferManager::incomingFinished,
            this, &TerminalUi::handleDirectIncomingFinished);
    connect(&m_directTransfers, &CpxDirectTransferManager::outgoingFinished,
            this, &TerminalUi::handleDirectOutgoingFinished);
    connect(&m_directTransfers, &CpxDirectTransferManager::failed,
            this, &TerminalUi::handleDirectFailure);
    m_tickTimer.setInterval(35);
}

TerminalUi::~TerminalUi()
{
    m_tickTimer.stop();

    for (ConnectionEntry *entry : m_connections) {
        if (entry && entry->backend) {
            entry->backend->stop();
        }
    }

    shutdownCurses();

    for (Buffer *buffer : m_buffers) {
        delete buffer;
    }
    m_buffers.clear();
    m_bufferByKey.clear();

    for (ConnectionEntry *entry : m_connections) {
        delete entry;
    }
    m_connections.clear();
    m_connectionById.clear();
}

bool TerminalUi::start()
{
    std::setlocale(LC_ALL, "");
    loadOptions();
    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();

    QString secureError;
    m_secureReady = m_secure.initialize(&secureError);
    if (m_secureReady) {
        QString roomError;
        if (!m_secureRooms.initialize(&roomError)) {
            m_secureReady = false;
            secureError = roomError;
        }
    }

    initCurses();
    if (m_options.showSplash) {
        showSplash();
    }

    Buffer *global = ensureBuffer(QStringLiteral("global"), {}, {},
                                  QStringLiteral("Status"), true);
    append(global,
           QStringLiteral("WaffleHouse-Client %1 for Termux started. /help shows commands; /menu shows the compact command map.").arg(appVersionString()),
           false);
    append(global,
           QStringLiteral("Runtime: %1").arg(RuntimeEnvironment::detect().summary()),
           false);
    append(global,
           QStringLiteral("Media engine: %1. Use /media for status; /mstream plays internet radio/streams.")
               .arg(m_mediaController->backendAvailable()
                        ? QStringLiteral("mpv ready")
                        : QStringLiteral("mpv not found")),
           false);

    connect(m_sipController, &SipController::activityLine, this, [this](const QString &line) {
        Buffer *buffer = phoneBuffer(false);
        append(buffer, line, activeBuffer() != buffer);
    });
    connect(m_sipController, &SipController::incomingCall, this, [this](const QString &accountId, int id, const QString &remote) {
        if (ConnectionEntry *entry = sipConnectionByAccountId(accountId)) {
            m_selectedConnectionId = entry->id;
            m_sipController->setSelectedAccountId(accountId);
        }
        Buffer *buffer = phoneBuffer(true);
        append(buffer, QStringLiteral("*** INCOMING SIP CALL %1 on %2 from %3 — /answer %1 or /reject %1 ***")
                           .arg(id)
                           .arg(sipConnectionByAccountId(accountId) ? connectionLabel(sipConnectionByAccountId(accountId)) : accountId)
                           .arg(remote), false);
        beep();
    });
    connect(m_sipController, &SipController::callsChanged, this, [this] {
        if (Buffer *buffer = findBuffer(bufferKey(QStringLiteral("phone"), {}, {}))) {
            if (activeBuffer() == buffer) showPhoneMain(false);
        }
    });
    if (m_secureReady) {
        append(global,
               QStringLiteral("WaffleHouse-CLI secure-DM profile identities ready. Select a connection and use /fingerprint."),
               false);
    } else {
        append(global,
               QStringLiteral("[error] encrypted communications unavailable: %1").arg(secureError),
               false);
    }

    loadConnections();
    m_sipController->initialize();
    append(global,
           QStringLiteral("Integrated multi-account SIP/VoIP softphone ready. Add SIP accounts with /add; use /phone for call controls."),
           false);
    if (m_connections.isEmpty()) {
        append(global,
               QStringLiteral("No saved connections. Use /add to create one."),
               false);
    } else {
        append(global,
               QStringLiteral("Loaded %1 saved connection profile(s). "
                              "Use /connections and /connect.")
                   .arg(m_connections.size()),
               false);
    }

    m_tickTimer.start();
    draw();
    return true;
}

void TerminalUi::initCurses()
{
    if (m_cursesActive) {
        return;
    }

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    meta(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
    }

    m_cursesActive = true;
    applyTheme();
    curs_set(1);
}

void TerminalUi::applyTheme()
{
    if (!m_cursesActive || !has_colors()) {
        return;
    }

    const QString theme = m_options.theme.toCaseFolded();
    const auto color256 = [](short color, short fallback) -> short {
        return (COLORS >= 256 && color >= 0 && color < COLORS) ? color : fallback;
    };
    short header = COLOR_CYAN;
    short online = COLOR_GREEN;
    short unread = COLOR_MAGENTA;
    short border = COLOR_CYAN;
    short secure = COLOR_GREEN;
    short footer = COLOR_MAGENTA;
    short background = -1;

    if (theme == QStringLiteral("phosphor")) {
        header = COLOR_GREEN;
        online = COLOR_GREEN;
        unread = COLOR_CYAN;
        border = COLOR_GREEN;
        secure = COLOR_CYAN;
    } else if (theme == QStringLiteral("amber")) {
        header = COLOR_YELLOW;
        online = COLOR_YELLOW;
        unread = COLOR_MAGENTA;
        border = COLOR_YELLOW;
        secure = COLOR_CYAN;
    } else if (theme == QStringLiteral("ice")) {
        background = color256(17, COLOR_BLACK);
        header = color256(117, COLOR_CYAN);
        online = color256(87, COLOR_CYAN);
        unread = color256(153, COLOR_WHITE);
        border = color256(67, COLOR_BLUE);
        secure = color256(51, COLOR_CYAN);
    } else if (theme == QStringLiteral("hacker")) {
        background = COLOR_BLACK;
        header = COLOR_GREEN;
        online = COLOR_GREEN;
        unread = COLOR_CYAN;
        border = COLOR_GREEN;
        secure = COLOR_CYAN;
    } else if (theme == QStringLiteral("matrix")) {
        background = COLOR_BLACK;
        header = COLOR_GREEN;
        online = COLOR_GREEN;
        unread = COLOR_GREEN;
        border = COLOR_GREEN;
        secure = COLOR_GREEN;
    } else if (theme == QStringLiteral("midnight")) {
        background = COLOR_BLACK;
        header = color256(75, COLOR_CYAN);
        online = color256(114, COLOR_GREEN);
        unread = color256(141, COLOR_MAGENTA);
        border = color256(67, COLOR_BLUE);
        secure = color256(81, COLOR_CYAN);
    } else if (theme == QStringLiteral("classic-light")) {
        background = COLOR_WHITE;
        header = COLOR_BLUE;
        online = COLOR_GREEN;
        unread = COLOR_MAGENTA;
        border = COLOR_BLUE;
        secure = COLOR_CYAN;
    } else if (theme == QStringLiteral("cyberpunk")) {
        background = COLOR_BLACK;
        header = color256(51, COLOR_CYAN);
        online = color256(46, COLOR_GREEN);
        unread = color256(201, COLOR_MAGENTA);
        border = color256(201, COLOR_MAGENTA);
        secure = color256(51, COLOR_CYAN);
    } else if (theme == QStringLiteral("solarized")) {
        background = color256(23, COLOR_BLACK);
        header = color256(136, COLOR_YELLOW);
        online = color256(64, COLOR_GREEN);
        unread = color256(37, COLOR_CYAN);
        border = color256(244, COLOR_WHITE);
        secure = color256(37, COLOR_CYAN);
    } else if (theme == QStringLiteral("nord")) {
        background = color256(236, COLOR_BLACK);
        header = color256(110, COLOR_CYAN);
        online = color256(108, COLOR_GREEN);
        unread = color256(81, COLOR_CYAN);
        border = color256(245, COLOR_WHITE);
        secure = color256(81, COLOR_CYAN);
    } else if (theme == QStringLiteral("ocean")) {
        background = color256(17, COLOR_BLACK);
        header = color256(39, COLOR_CYAN);
        online = color256(48, COLOR_GREEN);
        unread = color256(45, COLOR_CYAN);
        border = color256(24, COLOR_BLUE);
        secure = color256(45, COLOR_CYAN);
    } else if (theme == QStringLiteral("retro-blue")) {
        background = color256(17, COLOR_BLACK);
        header = color256(75, COLOR_BLUE);
        online = color256(81, COLOR_CYAN);
        unread = color256(117, COLOR_CYAN);
        border = color256(60, COLOR_BLUE);
        secure = color256(117, COLOR_CYAN);
    } else if (theme == QStringLiteral("monochrome")) {
        background = COLOR_BLACK;
        header = COLOR_WHITE;
        online = COLOR_WHITE;
        unread = COLOR_WHITE;
        border = color256(245, COLOR_WHITE);
        secure = COLOR_WHITE;
    } else if (theme == QStringLiteral("blue-box")) {
        background = color256(17, COLOR_BLACK);
        header = color256(33, COLOR_BLUE);
        online = color256(51, COLOR_CYAN);
        unread = color256(45, COLOR_CYAN);
        border = color256(24, COLOR_BLUE);
        secure = color256(51, COLOR_CYAN);
    } else if (theme == QStringLiteral("red-box")) {
        background = COLOR_BLACK;
        header = color256(196, COLOR_RED);
        online = color256(208, COLOR_YELLOW);
        unread = color256(203, COLOR_RED);
        border = color256(88, COLOR_RED);
        secure = color256(203, COLOR_RED);
    } else if (theme == QStringLiteral("beige-box")) {
        background = color256(236, COLOR_BLACK);
        header = color256(223, COLOR_YELLOW);
        online = color256(150, COLOR_GREEN);
        unread = color256(180, COLOR_YELLOW);
        border = color256(101, COLOR_YELLOW);
        secure = color256(180, COLOR_YELLOW);
    } else if (theme == QStringLiteral("2600")) {
        background = COLOR_BLACK;
        header = color256(46, COLOR_GREEN);
        online = color256(118, COLOR_GREEN);
        unread = color256(51, COLOR_CYAN);
        border = color256(34, COLOR_GREEN);
        secure = color256(51, COLOR_CYAN);
    } else if (theme == QStringLiteral("wargames")) {
        background = COLOR_BLACK;
        header = color256(40, COLOR_GREEN);
        online = color256(46, COLOR_GREEN);
        unread = color256(82, COLOR_GREEN);
        border = color256(22, COLOR_GREEN);
        secure = color256(82, COLOR_GREEN);
    } else if (theme == QStringLiteral("crt-green")) {
        background = COLOR_BLACK;
        header = color256(118, COLOR_GREEN);
        online = color256(156, COLOR_GREEN);
        unread = color256(120, COLOR_GREEN);
        border = color256(65, COLOR_GREEN);
        secure = color256(120, COLOR_GREEN);
    } else if (theme == QStringLiteral("vt220")) {
        background = COLOR_BLACK;
        header = color256(252, COLOR_WHITE);
        online = color256(255, COLOR_WHITE);
        unread = color256(250, COLOR_WHITE);
        border = color256(244, COLOR_WHITE);
        secure = color256(255, COLOR_WHITE);
    } else if (theme == QStringLiteral("cobalt")) {
        background = color256(17, COLOR_BLACK);
        header = color256(69, COLOR_BLUE);
        online = color256(48, COLOR_GREEN);
        unread = color256(81, COLOR_CYAN);
        border = color256(25, COLOR_BLUE);
        secure = color256(81, COLOR_CYAN);
    } else if (theme == QStringLiteral("stealth")) {
        background = color256(236, COLOR_BLACK);
        header = color256(245, COLOR_WHITE);
        online = color256(108, COLOR_GREEN);
        unread = color256(250, COLOR_WHITE);
        border = color256(238, COLOR_BLACK);
        secure = color256(250, COLOR_WHITE);
    } else if (theme == QStringLiteral("synthwave")) {
        background = color256(17, COLOR_BLACK);
        header = color256(213, COLOR_MAGENTA);
        online = color256(87, COLOR_CYAN);
        unread = color256(207, COLOR_MAGENTA);
        border = color256(99, COLOR_BLUE);
        secure = color256(87, COLOR_CYAN);
    } else if (theme == QStringLiteral("dracula")) {
        background = color256(236, COLOR_BLACK);
        header = color256(141, COLOR_MAGENTA);
        online = color256(84, COLOR_GREEN);
        unread = color256(212, COLOR_MAGENTA);
        border = color256(61, COLOR_BLUE);
        secure = color256(117, COLOR_CYAN);
    } else if (theme == QStringLiteral("vaporwave")) {
        background = color256(236, COLOR_BLACK);
        header = color256(213, COLOR_MAGENTA);
        online = color256(49, COLOR_CYAN);
        unread = color256(207, COLOR_MAGENTA);
        border = color256(135, COLOR_MAGENTA);
        secure = color256(87, COLOR_CYAN);
    } else if (theme == QStringLiteral("blood-moon")) {
        background = COLOR_BLACK;
        header = color256(203, COLOR_RED);
        online = color256(197, COLOR_RED);
        unread = color256(160, COLOR_RED);
        border = color256(124, COLOR_RED);
        secure = color256(210, COLOR_RED);
    } else if (theme == QStringLiteral("c64")) {
        background = color256(55, COLOR_BLUE);
        header = color256(153, COLOR_CYAN);
        online = color256(159, COLOR_CYAN);
        unread = color256(189, COLOR_WHITE);
        border = color256(99, COLOR_MAGENTA);
        secure = color256(153, COLOR_CYAN);
    } else if (theme == QStringLiteral("dos")) {
        background = COLOR_BLUE;
        header = COLOR_YELLOW;
        online = COLOR_CYAN;
        unread = COLOR_WHITE;
        border = COLOR_CYAN;
        secure = COLOR_GREEN;
    } else if (theme == QStringLiteral("solarized-dark")) {
        background = color256(23, COLOR_BLACK);
        header = color256(37, COLOR_CYAN);
        online = color256(64, COLOR_GREEN);
        unread = color256(136, COLOR_YELLOW);
        border = color256(66, COLOR_CYAN);
        secure = color256(37, COLOR_CYAN);
    } else if (theme == QStringLiteral("waffle-iron")) {
        background = color256(52, COLOR_BLACK);
        header = color256(220, COLOR_YELLOW);
        online = color256(214, COLOR_YELLOW);
        unread = color256(180, COLOR_YELLOW);
        border = color256(130, COLOR_YELLOW);
        secure = color256(220, COLOR_YELLOW);
    } else if (theme == QStringLiteral("ghostline")) {
        background = color256(17, COLOR_BLACK);
        header = color256(81, COLOR_CYAN);
        online = color256(117, COLOR_CYAN);
        unread = color256(141, COLOR_MAGENTA);
        border = color256(68, COLOR_BLUE);
        secure = color256(81, COLOR_CYAN);
    } else if (theme == QStringLiteral("hot-dog-stand")) {
        background = COLOR_RED;
        header = COLOR_YELLOW;
        online = COLOR_YELLOW;
        unread = COLOR_WHITE;
        border = COLOR_YELLOW;
        secure = COLOR_YELLOW;
    } else if (theme == QStringLiteral("neon-miami")) {
        background = color256(23, COLOR_BLACK);
        header = color256(50, COLOR_CYAN);
        online = color256(49, COLOR_CYAN);
        unread = color256(205, COLOR_MAGENTA);
        border = color256(204, COLOR_MAGENTA);
        secure = color256(50, COLOR_CYAN);
    }

    init_pair(PairHeader, header, background);
    init_pair(PairOnline, online, background);
    init_pair(PairError, COLOR_RED, background);
    init_pair(PairUnread, unread, background);
    // Footer help uses each theme's own secondary/unread accent so the
    // shortcut rail is distinct without introducing a hard-coded color.
    footer = unread;
    init_pair(PairBorder, border, background);
    init_pair(PairSecure, secure, background);
    init_pair(PairFooter, footer, background);
}

void TerminalUi::showSplash()
{
    if (!m_cursesActive) return;
    erase();
    curs_set(0);

    QStringList logo;
    QString subtitle;
    QString protocols;
    QString hint;
    if (COLS < 72) {
        logo = {
            QStringLiteral("WAFFLEHOUSE-CLIENT"),
            QStringLiteral("3.2-TERMUX"),
        };
        subtitle = QStringLiteral("MULTI-PROTOCOL TERMINAL");
        protocols = QStringLiteral("AIM | IRC | BBS | SIP");
        hint = QStringLiteral("Press a key - /menu for commands");
    } else {
        logo = {
            QStringLiteral("▄     ▄  ▄▄▄▄▄  ▄▄▄▄▄▄ ▄▄▄▄▄▄ ▄      ▄▄▄▄▄▄ ▄     ▄  ▄▄▄▄▄▄ ▄     ▄  ▄▄▄▄▄▄ ▄▄▄▄▄▄"),
            QStringLiteral("█     █ █     █ █      █      █      █      █     ▄ █     ▄ █     █ █       █     "),
            QStringLiteral("█  ▄  ▄ █▄▄▄▄▄▀ █▄▄▄   █▄▄▄   █      █▄▄▄   █▀▀▀▀▀█ █     ▄ █     ▄ █▄▄▄▄▄▄ █▄▄▄  "),
            QStringLiteral("▄  ▀  █ █     ▀ ▄      ▄      █      ▄      █     ▀ █     █ ▄     █       ▀ ▄     "),
            QStringLiteral("▀▄▀ ▀▄▀ █     █ █      █      █▄▄▄▄▄ █▄▄▄▄▄ █     █ ▀▄▄▄▄▄▀ ▀▄▄▄▄▄▀ ▄▄▄▄▄▄▀ █▄▄▄▄▄"),
        };
        subtitle = QStringLiteral("MODERN MULTI-PROTOCOL COMMUNICATIONS TERMINAL");
        protocols = QStringLiteral("AIM/OSCAR  |  IRC  |  TELNET/BBS  |  SIP/VOIP");
        hint = QStringLiteral("Press any key to enter the communications hub");
    }

    const int logoHeight = static_cast<int>(logo.size());
    const int contentHeight = logoHeight + 6;
    const int startY = std::max(0, (LINES - contentHeight) / 2);
    for (int i = 0; i < logoHeight; ++i) {
        const QString &line = logo.at(i);
        const int x = std::max(0, (COLS - static_cast<int>(line.size())) / 2);
        const int attr = A_BOLD | (has_colors() ? COLOR_PAIR(PairHeader) : 0);
        safeAdd(startY + i, x, line, attr, COLS);
    }
    const QString edition = QStringLiteral("VERSION %1").arg(appVersionString().toUpper());
    safeAdd(startY + logoHeight + 1, std::max(0, (COLS - static_cast<int>(edition.size())) / 2), edition, A_BOLD, COLS);
    safeAdd(startY + logoHeight + 2, std::max(0, (COLS - static_cast<int>(subtitle.size())) / 2), subtitle, A_BOLD, COLS);
    safeAdd(startY + logoHeight + 3, std::max(0, (COLS - static_cast<int>(protocols.size())) / 2), protocols, A_DIM, COLS);
    safeAdd(startY + logoHeight + 5, std::max(0, (COLS - static_cast<int>(hint.size())) / 2), hint, A_DIM, COLS);
    refresh();
    wtimeout(stdscr, 75);
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        wint_t ch = 0;
        if (get_wch(&ch) != ERR) break;
    }
    wtimeout(stdscr, 0);
    erase();
    clearok(stdscr, TRUE);
    curs_set(1);
}

void TerminalUi::shutdownCurses()
{
    if (!m_cursesActive) {
        return;
    }
    nodelay(stdscr, FALSE);
    endwin();
    m_cursesActive = false;
}

void TerminalUi::tick()
{
    if (m_quitting) {
        return;
    }
    handleInput();
    updateAutoPresence();
    pumpFileTransfers();
    draw();
}

void TerminalUi::markUserActivity()
{
    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();
    for (ConnectionEntry *entry : m_connections) {
        if (!entry || entry->autoPresenceState.isEmpty() || !entry->connected || !entry->backend) continue;
        if (auto *oscar = qobject_cast<OscarBackend *>(entry->backend)) {
            oscar->setBack();
            entry->autoPresenceState.clear();
        }
    }
}

void TerminalUi::updateAutoPresence()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_nextPresenceCheckMs) return;
    m_nextPresenceCheckMs = now + 1000;
    if (!m_options.autoPresenceEnabled || m_lastUserActivityMs <= 0) return;

    const qint64 inactiveSeconds = UserActivity::idleMilliseconds(m_lastUserActivityMs) / 1000;
    const qint64 idleThreshold = static_cast<qint64>(m_options.autoIdleMinutes) * 60;
    const qint64 awayThreshold = static_cast<qint64>(m_options.autoAwayMinutes) * 60;
    for (ConnectionEntry *entry : m_connections) {
        if (!entry || !entry->connected || !entry->backend
            || entry->settings.protocol != ConnectionSettings::Protocol::Oscar) continue;
        auto *oscar = qobject_cast<OscarBackend *>(entry->backend);
        if (!oscar) continue;

        const bool managed = !entry->autoPresenceState.isEmpty();
        const bool manuallyChanged = !managed
            && (entry->presenceState.compare(QStringLiteral("ONLINE"), Qt::CaseInsensitive) != 0
                || entry->idleSeconds > 0);
        if (manuallyChanged) continue;
        if (managed && inactiveSeconds < idleThreshold) {
            oscar->setBack();
            entry->autoPresenceState.clear();
            continue;
        }

        if (inactiveSeconds >= awayThreshold) {
            if (entry->autoPresenceState != QStringLiteral("AWAY")) {
                oscar->setAwayMessage(QStringLiteral("Auto-away — inactive for %1 minutes")
                                          .arg(m_options.autoAwayMinutes));
                oscar->setIdleSeconds(static_cast<quint32>(std::min<qint64>(inactiveSeconds, 0xffffffffLL)));
                entry->autoPresenceState = QStringLiteral("AWAY");
            }
        } else if (inactiveSeconds >= idleThreshold) {
            if (entry->autoPresenceState.isEmpty()) {
                oscar->setIdleSeconds(static_cast<quint32>(std::min<qint64>(inactiveSeconds, 0xffffffffLL)));
                entry->autoPresenceState = QStringLiteral("IDLE");
            }
        }
    }
}

void TerminalUi::requestClientVersion(ConnectionEntry *entry, QString target)
{
    if (!entry || !entry->connected || !entry->backend) {
        status(QStringLiteral("Select and connect an AIM/OSCAR or IRC connection first."));
        return;
    }
    target = target.trimmed();
    if (target.isEmpty()) target = activeImTarget(entry);
    if (target.isEmpty()) {
        status(QStringLiteral("Usage: /version USER (or run /version from a PM buffer)."));
        return;
    }
    const QString key = entry->id + QChar(0x1f) + target.toCaseFolded();
    m_pendingVersionQueries.insert(key);
    const auto protocol = entry->settings.protocol;
    QTimer::singleShot(3500, this, [this, key, target, protocol] {
        if (!m_pendingVersionQueries.remove(key)) return;
        status(protocol == ConnectionSettings::Protocol::Oscar
            ? QStringLiteral("[version] %1: no 3.2-Termux reply; peer may be an older WaffleHouse/CPX client or another AIM client (exact version unavailable)").arg(target)
            : QStringLiteral("[version] %1: no CTCP VERSION reply received").arg(target));
    });
    if (auto *irc = qobject_cast<IrcBackend *>(entry->backend)) {
        irc->requestClientVersion(target);
        status(QStringLiteral("Version query sent to %1 via IRC CTCP.").arg(target));
        return;
    }
    if (auto *oscar = qobject_cast<OscarBackend *>(entry->backend)) {
        oscar->requestClientVersion(target);
        status(QStringLiteral("WaffleHouse version query sent to %1 via AIM.").arg(target));
        return;
    }
    m_pendingVersionQueries.remove(key);
    status(QStringLiteral("/version is available for AIM/OSCAR and IRC peers."));
}

void TerminalUi::safeAdd(int y, int x, const QString &text, int attr, int maxWidth)
{
    if (!m_cursesActive) {
        return;
    }

    const int h = LINES;
    const int w = COLS;
    if (y < 0 || y >= h || x < 0 || x >= w) {
        return;
    }

    int room = w - x;
    if (maxWidth >= 0) {
        room = std::min(room, maxWidth);
    }
    if (room <= 0) {
        return;
    }

    // ncursesw expects display cells, not UTF-8 bytes.  The old code
    // truncated the UTF-8 byte array at COLS, which could split block glyphs
    // such as ▄/█/▀ in the middle of a multibyte sequence and corrupt the
    // WaffleHouse splash.  Clip a wide-character string by terminal cells.
    const std::wstring wide = text.toStdWString();
    std::wstring clipped;
    clipped.reserve(wide.size());
    int usedCells = 0;
    for (const wchar_t ch : wide) {
        int cells = ::wcwidth(ch);
        if (cells < 0) {
            cells = 1;
        }
        if (usedCells + cells > room) {
            break;
        }
        clipped.push_back(ch);
        usedCells += cells;
    }

    if (clipped.empty()) {
        return;
    }

    attron(attr);
    mvaddnwstr(y, x, clipped.c_str(), static_cast<int>(clipped.size()));
    attroff(attr);
}

QStringList TerminalUi::wrapText(const QString &text, int width)
{
    QStringList output;
    const int safeWidth = std::max(8, width);

    for (const QString &paragraph : text.split(QLatin1Char('\n'))) {
        QString remaining = paragraph;
        if (remaining.isEmpty()) {
            output.push_back(QString());
            continue;
        }

        while (remaining.size() > safeWidth) {
            int split = remaining.lastIndexOf(QLatin1Char(' '), safeWidth);
            if (split < safeWidth / 3) {
                split = safeWidth;
            }
            output.push_back(remaining.left(split).trimmed());
            remaining = remaining.mid(split).trimmed();
        }
        output.push_back(remaining);
    }

    return output;
}

QString TerminalUi::protocolShort(ConnectionSettings::Protocol protocol)
{
    switch (protocol) {
    case ConnectionSettings::Protocol::Oscar:
        return QStringLiteral("AIM");
    case ConnectionSettings::Protocol::Irc:
        return QStringLiteral("IRC");
    case ConnectionSettings::Protocol::Telnet:
        return QStringLiteral("TEL");
    case ConnectionSettings::Protocol::Sip:
        return QStringLiteral("SIP");
    case ConnectionSettings::Protocol::Unknown:
        break;
    }
    return QStringLiteral("?");
}

QString TerminalUi::protocolName(ConnectionSettings::Protocol protocol)
{
    switch (protocol) {
    case ConnectionSettings::Protocol::Oscar:
        return QStringLiteral("AIM/OSCAR");
    case ConnectionSettings::Protocol::Irc:
        return QStringLiteral("IRC");
    case ConnectionSettings::Protocol::Telnet:
        return QStringLiteral("Telnet");
    case ConnectionSettings::Protocol::Sip:
        return QStringLiteral("SIP / VoIP");
    case ConnectionSettings::Protocol::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

QString TerminalUi::connectionLabel(const ConnectionEntry *entry)
{
    if (!entry) {
        return QStringLiteral("none");
    }

    QString who = entry->identity.isEmpty() ? entry->settings.username : entry->identity;
    if (who.isEmpty()) {
        who = protocolShort(entry->settings.protocol);
    }

    QString state;
    if (entry->connecting) {
        state = QStringLiteral("~");
    } else if (entry->connected) {
        state = QStringLiteral("+");
    } else {
        state = QStringLiteral("-");
    }

    return QStringLiteral("%1:%2%3")
        .arg(protocolShort(entry->settings.protocol), who, state);
}

QString TerminalUi::bufferKey(const QString &kind,
                              const QString &connectionId,
                              const QString &target) const
{
    if (kind == QStringLiteral("global")) {
        return QStringLiteral("global");
    }
    if (kind == QStringLiteral("connection")) {
        return QStringLiteral("conn:%1").arg(connectionId);
    }
    return QStringLiteral("%1:%2:%3")
        .arg(kind, connectionId, target.toCaseFolded());
}

TerminalUi::Buffer *TerminalUi::findBuffer(const QString &key) const
{
    return m_bufferByKey.value(key, nullptr);
}

TerminalUi::Buffer *TerminalUi::ensureBuffer(const QString &kind,
                                             const QString &connectionId,
                                             const QString &target,
                                             const QString &displayName,
                                             bool switchTo)
{
    const QString key = bufferKey(kind, connectionId, target);
    Buffer *buffer = findBuffer(key);
    if (!buffer) {
        buffer = new Buffer;
        buffer->number = m_buffers.size() + 1;
        buffer->key = key;
        buffer->kind = kind;
        if (kind == QStringLiteral("terminal"))
            buffer->terminal = std::make_unique<AnsiTerminalModel>(80, 25);
        buffer->connectionId = connectionId;
        buffer->target = target;
        buffer->name = !displayName.isEmpty()
            ? displayName
            : (!target.isEmpty() ? target : QStringLiteral("Status"));
        m_buffers.push_back(buffer);
        m_bufferByKey.insert(key, buffer);
    } else if (!displayName.isEmpty()) {
        buffer->name = displayName;
    }

    if (switchTo) {
        switchToBuffer(buffer);
    }
    return buffer;
}

TerminalUi::Buffer *TerminalUi::activeBuffer() const
{
    if (m_activeBuffer < 0 || m_activeBuffer >= m_buffers.size()) {
        return nullptr;
    }
    return m_buffers.at(m_activeBuffer);
}

void TerminalUi::switchBuffer(int index)
{
    if (m_buffers.isEmpty()) {
        return;
    }
    while (index < 0) {
        index += m_buffers.size();
    }
    index %= m_buffers.size();
    m_activeBuffer = index;
    if (Buffer *buffer = activeBuffer()) {
        buffer->unread = 0;
        buffer->scroll = 0;
        if (!buffer->connectionId.isEmpty()) {
            m_selectedConnectionId = buffer->connectionId;
        }
    }
}

void TerminalUi::switchToBuffer(Buffer *buffer)
{
    const int index = m_buffers.indexOf(buffer);
    if (index >= 0) {
        switchBuffer(index);
    }
}

void TerminalUi::nextBuffer()
{
    switchBuffer(m_activeBuffer + 1);
}

void TerminalUi::previousBuffer()
{
    switchBuffer(m_activeBuffer - 1);
}

void TerminalUi::renumberBuffers()
{
    for (int i = 0; i < m_buffers.size(); ++i) {
        m_buffers[i]->number = i + 1;
    }
}

void TerminalUi::closeActiveBuffer()
{
    Buffer *buffer = activeBuffer();
    if (!buffer || buffer->kind == QStringLiteral("global")) {
        status(QStringLiteral("The global Status buffer cannot be closed."));
        return;
    }

    // A per-connection status buffer is just a view. Hiding it must not
    // disconnect the backend or delete the saved connection profile.
    if (buffer->kind == QStringLiteral("connection")
        && !buffer->connectionId.isEmpty()) {
        m_hiddenConnectionBuffers.insert(buffer->connectionId);
    }

    if (buffer->kind == QStringLiteral("im")) {
        m_secure.closeSession(buffer->connectionId, buffer->target);
    }

    ConnectionEntry *ephemeralTerminalEntry = nullptr;
    if (buffer->kind == QStringLiteral("terminal")) {
        ConnectionEntry *entry = connectionById(buffer->connectionId);
        if (entry && !entry->persistent) {
            ephemeralTerminalEntry = entry;
        }
        if (entry && entry->backend && (entry->connected || entry->connecting)) {
            entry->backend->stop();
        }
    }

    if (buffer->kind == QStringLiteral("chat")) {
        m_secureRooms.closeRoom(buffer->connectionId, buffer->target);
        // Mark this room/channel as explicitly closed *before* asking the
        // backend to leave it. IRC emits PART/member-list events during the
        // leave sequence; without this guard those events can recreate the
        // buffer we just removed. An explicit /join clears this marker.
        m_closedChatBuffers.insert(buffer->key);

        ConnectionEntry *entry = connectionById(buffer->connectionId);
        if (entry && entry->backend && entry->connected) {
            entry->backend->leaveRoom(buffer->target);
        }
    }

    m_bufferByKey.remove(buffer->key);
    const int index = m_buffers.indexOf(buffer);
    if (index >= 0) {
        m_buffers.removeAt(index);
    }
    delete buffer;

    // /telnet quick-connect sessions are intentionally ephemeral.  Their
    // terminal stays available after disconnect, but /close discards the
    // temporary backend/connection object instead of turning it into a saved
    // account.
    if (ephemeralTerminalEntry) {
        m_hiddenConnectionBuffers.remove(ephemeralTerminalEntry->id);
        m_secure.closeConnection(ephemeralTerminalEntry->id);
        m_connectionById.remove(ephemeralTerminalEntry->id);
        m_connections.removeAll(ephemeralTerminalEntry);
        if (ephemeralTerminalEntry->backend) {
            QObject::disconnect(ephemeralTerminalEntry->backend, nullptr, this, nullptr);
            ephemeralTerminalEntry->backend->deleteLater();
        }
        if (m_selectedConnectionId == ephemeralTerminalEntry->id) {
            m_selectedConnectionId = m_connections.isEmpty() ? QString() : m_connections.first()->id;
        }
        delete ephemeralTerminalEntry;
    }

    renumberBuffers();

    if (m_buffers.isEmpty()) {
        ensureBuffer(QStringLiteral("global"), {}, {}, QStringLiteral("Status"), true);
    } else {
        switchBuffer(std::min(index, static_cast<int>(m_buffers.size()) - 1));
    }
}

void TerminalUi::removeConnectionConversationBuffers(const QString &connectionId)
{
    for (int i = m_buffers.size() - 1; i >= 0; --i) {
        Buffer *buffer = m_buffers.at(i);
        if (buffer->connectionId == connectionId
            && buffer->kind != QStringLiteral("connection")) {
            m_bufferByKey.remove(buffer->key);
            m_buffers.removeAt(i);
            delete buffer;
            if (m_activeBuffer >= i && m_activeBuffer > 0) {
                --m_activeBuffer;
            }
        }
    }
    renumberBuffers();
    if (m_activeBuffer >= m_buffers.size()) {
        m_activeBuffer = std::max(0, static_cast<int>(m_buffers.size()) - 1);
    }
}

void TerminalUi::append(Buffer *buffer, const QString &text, bool markUnread)
{
    if (!buffer) {
        return;
    }

    const QString prefix = m_options.showTimestamps
        ? timestamp() + QLatin1Char(' ')
        : QString();
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        buffer->lines.push_back(prefix + line);
    }
    constexpr int MaxLines = 5000;
    while (buffer->lines.size() > MaxLines) {
        buffer->lines.removeFirst();
    }

    if (markUnread && buffer != activeBuffer()) {
        ++buffer->unread;
    } else if (buffer == activeBuffer()) {
        buffer->scroll = 0;
    }
}

void TerminalUi::status(const QString &text)
{
    append(ensureBuffer(QStringLiteral("global")), text);
}

void TerminalUi::connectionStatus(ConnectionEntry *entry, const QString &text)
{
    if (!entry) {
        status(text);
        return;
    }

    // If the user explicitly closed this connection's status view, keep it
    // hidden. Status still remains visible in the global Status buffer so no
    // diagnostics are lost. /conn N (or /buddies /rooms) reopens the view.
    if (m_hiddenConnectionBuffers.contains(entry->id)) {
        status(QStringLiteral("[%1] %2").arg(connectionLabel(entry), text));
        return;
    }

    Buffer *buffer = ensureBuffer(QStringLiteral("connection"), entry->id, {},
                                  connectionLabel(entry));
    buffer->name = connectionLabel(entry);
    append(buffer, text);
}

void TerminalUi::drawHeader(int width)
{
    Buffer *buffer = activeBuffer();
    ConnectionEntry *entry = buffer ? connectionById(buffer->connectionId) : nullptr;

    QString context;
    if (entry) context += QStringLiteral("  •  %1").arg(connectionLabel(entry));
    if (buffer) {
        context += QStringLiteral("  •  %1:%2").arg(buffer->number).arg(buffer->name);
        if (buffer->kind == QStringLiteral("chat")
            && m_secureRooms.hasRoom(buffer->connectionId, buffer->target)) {
            context += QStringLiteral("  •  🔒 ROOM %1")
                .arg(m_secureRooms.keyId(buffer->connectionId, buffer->target).left(8));
        }
    }

    const QString label = QStringLiteral("╭─ WAFFLEHOUSE-CLIENT %1%2 [CLI] ").arg(appVersionString().toUpper(), context);
    QString line = label;
    const int fill = std::max(0, width - static_cast<int>(line.size()) - 1);
    line += QString(fill, QChar(0x2500)); // ─
    if (line.size() < width) line += QChar(0x256e); // ╮
    safeAdd(0, 0, line.left(width), A_BOLD | (has_colors() ? COLOR_PAIR(PairHeader) : 0), width);
}

void TerminalUi::drawConnectionsBar(int row, int width)
{
    QString line = QStringLiteral("│ ");
    int active = 0;
    for (int i = 0; i < m_connections.size(); ++i) {
        ConnectionEntry *entry = m_connections.at(i);
        if (!entry || (!entry->connected && !entry->connecting)) continue;
        ++active;
        const bool selected = entry->id == m_selectedConnectionId;
        const QString stateGlyph = entry->connecting ? QStringLiteral("◌") : QStringLiteral("●");
        const QString token = QStringLiteral("%1 %2:%3")
                                  .arg(stateGlyph)
                                  .arg(i + 1)
                                  .arg(connectionLabel(entry));
        line += selected ? QStringLiteral("[%1]  ").arg(token) : QStringLiteral("%1  ").arg(token);
    }
    if (!active) line += QStringLiteral("none active — /connect PROTOCOL:name or /telnet host:port");
    if (line.size() < width) line = line.leftJustified(width - 1);
    if (line.size() < width) line += QChar(0x2502); // │
    safeAdd(row, 0, line.left(width), A_DIM | (has_colors() ? COLOR_PAIR(PairBorder) : 0), width);
}

void TerminalUi::drawBuddyPane(ConnectionEntry *entry,
                               int top, int bottom, int startX, int width)
{
    if (!entry || width < 16 || bottom < top) {
        return;
    }

    if (entry->settings.protocol == ConnectionSettings::Protocol::Sip) {
        for (int row = top; row <= bottom; ++row) {
            safeAdd(row, startX, QStringLiteral("│"),
                    A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
        }

        const QString accountId = entry->backend ? entry->backend->id() : QString();
        QList<trunkmonkey::CallSnapshot> accountCalls;
        if (m_sipController) {
            for (const auto &call : m_sipController->calls()) {
                if (!call.disconnected
                    && QString::fromStdString(call.accountId) == accountId) {
                    accountCalls.append(call);
                }
            }
        }

        const QString title = QStringLiteral(" CALLS (%1) ").arg(accountCalls.size());
        safeAdd(top, startX, QStringLiteral("╭"),
                A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
        safeAdd(top, startX + 1,
                title + QString(std::max(0, width - static_cast<int>(title.size()) - 1), QChar(0x2500)),
                A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), width - 1);

        int row = top + 1;
        const QString reg = m_sipController
            ? m_sipController->registrationText(accountId)
            : QStringLiteral("Softphone unavailable");
        if (row <= bottom) {
            safeAdd(row++, startX + 2, reg, entry->connected ? A_BOLD : A_DIM, width - 3);
        }
        for (const auto &call : accountCalls) {
            if (row > bottom) break;
            const QString remote = QString::fromStdString(call.remoteUri);
            const QString state = QString::fromStdString(call.state);
            safeAdd(row++, startX + 2,
                    QStringLiteral("#%1 %2 %3").arg(call.id).arg(state).arg(remote),
                    A_BOLD | (has_colors() ? COLOR_PAIR(PairOnline) : 0), width - 3);
        }
        if (row <= bottom) {
            safeAdd(row++, startX + 2, QStringLiteral("/dial <number>"), A_DIM, width - 3);
        }
        if (row <= bottom) {
            safeAdd(row, startX + 2, QStringLiteral("/phone  /calls"), A_DIM, width - 3);
        }
        return;
    }

    for (int row = top; row <= bottom; ++row) {
        safeAdd(row, startX, QStringLiteral("│"), A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
    }

    const QString title = QStringLiteral(" BUDDIES (%1) ").arg(entry->buddies.size());
    safeAdd(top, startX, QStringLiteral("╭"), A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
    safeAdd(top, startX + 1,
            title + QString(std::max(0, width - static_cast<int>(title.size()) - 1), QChar(0x2500)),
            A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), width - 1);

    QStringList buddies = entry->buddies.values();
    std::sort(buddies.begin(), buddies.end(), [entry](const QString &a, const QString &b) {
        const bool aOnline = isOnlineBuddy(entry->onlineBuddies, a);
        const bool bOnline = isOnlineBuddy(entry->onlineBuddies, b);
        if (aOnline != bOnline) {
            return aOnline;
        }
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    int row = top + 1;
    const int available = std::max(0, bottom - top);
    int shown = 0;
    for (const QString &buddy : buddies) {
        if (row > bottom) {
            break;
        }
        const bool online = isOnlineBuddy(entry->onlineBuddies, buddy);
        const QString marker = online ? QStringLiteral("● ") : QStringLiteral("○ ");
        const int attr = online
            ? (A_BOLD | (has_colors() ? COLOR_PAIR(PairOnline) : 0))
            : A_DIM;
        safeAdd(row++, startX + 2, marker + buddy, attr, width - 3);
        ++shown;
    }
    if (buddies.size() > shown && available > 0) {
        safeAdd(bottom, startX + 2,
                QStringLiteral("... +%1 more").arg(buddies.size() - shown),
                A_DIM, width - 3);
    }
}


void TerminalUi::drawMemberPane(Buffer *buffer,
                                int top, int bottom, int startX, int width)
{
    if (!buffer || width < 16 || bottom < top) {
        return;
    }

    for (int row = top; row <= bottom; ++row) {
        safeAdd(row, startX, QStringLiteral("│"), A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
    }

    const QString title = QStringLiteral(" MEMBERS (%1) ").arg(buffer->members.size());
    safeAdd(top, startX, QStringLiteral("╭"), A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
    safeAdd(top, startX + 1,
            title + QString(std::max(0, width - static_cast<int>(title.size()) - 1), QChar(0x2500)),
            A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), width - 1);

    QStringList members = buffer->members.values();
    std::sort(members.begin(), members.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    int row = top + 1;
    const int available = std::max(0, bottom - top);
    int shown = 0;
    for (const QString &member : members) {
        if (row > bottom) {
            break;
        }
        safeAdd(row++, startX + 2, member, 0, width - 3);
        ++shown;
    }
    if (members.size() > shown && available > 0) {
        safeAdd(bottom, startX + 2,
                QStringLiteral("... +%1 more").arg(members.size() - shown),
                A_DIM, width - 3);
    }
}


void TerminalUi::drawScrollBar(int top, int bottom, int x,
                               int totalLines, int firstLine, int visibleLines)
{
    if (bottom < top || x < 0 || totalLines <= visibleLines || visibleLines <= 0) {
        return;
    }

    const int trackHeight = bottom - top + 1;
    if (trackHeight <= 1) {
        return;
    }

    for (int row = top; row <= bottom; ++row) {
        safeAdd(row, x, QStringLiteral("│"), A_DIM | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
    }

    const int thumbHeight = std::max(1,
        static_cast<int>((static_cast<long long>(visibleLines) * trackHeight) / totalLines));
    const int maxFirst = std::max(1, totalLines - visibleLines);
    const int maxPos = std::max(0, trackHeight - thumbHeight);
    const int thumbPos = std::clamp(
        static_cast<int>((static_cast<long long>(firstLine) * maxPos) / maxFirst),
        0, maxPos);

    for (int i = 0; i < thumbHeight && top + thumbPos + i <= bottom; ++i) {
        safeAdd(top + thumbPos + i, x, QStringLiteral("█"), A_BOLD | (has_colors() ? COLOR_PAIR(PairBorder) : 0), 1);
    }
}

void TerminalUi::drawBufferPane(Buffer *buffer,
                                int top, int bottom, int width)
{
    if (!buffer || bottom < top) return;

    if (buffer->kind == QStringLiteral("terminal") && buffer->terminal) {
        const int height = bottom - top + 1;
        const int cols = std::min(80, std::max(1, width - 2));
        for (int r = 0; r < height && r < buffer->terminal->rows(); ++r) {
            int c = 0;
            while (c < cols && c < buffer->terminal->columns()) {
                const auto cell = buffer->terminal->cell(r, c);
                int fg = cell.fg, bg = cell.bg;
                if (cell.inverse) std::swap(fg, bg);
                static const short colors[8] = {COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};
                const int clampedFg = std::clamp(fg, 0, 7);
                const int clampedBg = std::clamp(bg, 0, 7);
                const bool explicitBright = cell.bold && COLORS >= 16;
                const short fgColor = explicitBright
                    ? static_cast<short>(clampedFg + 8)
                    : colors[clampedFg];
                const short bgColor = colors[clampedBg];
                // Keep bright and normal color pairs distinct.  A number of BBSes
                // use SGR 1;30 (bright black) for their ordinary gray labels.  Relying
                // only on A_BOLD + COLOR_BLACK makes those labels disappear on some
                // ncurses/terminal combinations.
                const short pair = static_cast<short>(16 + (explicitBright ? 64 : 0)
                                                     + clampedFg * 8 + clampedBg);
                int attr = (cell.bold && !explicitBright) ? A_BOLD : 0;
                if (has_colors() && pair > 0 && pair < COLOR_PAIRS) {
                    init_pair(pair, fgColor, bgColor);
                    attr |= COLOR_PAIR(pair);
                } else if (cell.bold) {
                    attr |= A_BOLD;
                }
                safeAdd(top + r, 1 + c, QString(cell.ch), attr, 1);
                ++c;
            }
        }
        return;
    }

    ConnectionEntry *entry = connectionById(buffer->connectionId);
    const bool buddyPane = m_options.showSidePanes
        && buffer->kind == QStringLiteral("connection")
        && entry
        && entry->settings.protocol == ConnectionSettings::Protocol::Oscar
        && width >= 78;
    const bool memberPane = m_options.showSidePanes
        && buffer->kind == QStringLiteral("chat") && width >= 78;

    const int sideWidth = (buddyPane || memberPane)
        ? std::clamp(width / 5, 20, 28)
        : 0;
    const int mainWidth = width - sideWidth;
    const int scrollX = std::max(1, mainWidth - 1);
    const int textWidth = std::max(10, mainWidth - 3);

    QStringList wrapped;
    for (const QString &line : buffer->lines) {
        wrapped.append(wrapText(line, textWidth));
    }

    const int height = bottom - top + 1;
    const int maxScroll = std::max(0, static_cast<int>(wrapped.size()) - height);
    buffer->scroll = std::clamp(buffer->scroll, 0, maxScroll);
    const int end = std::max(0, static_cast<int>(wrapped.size()) - buffer->scroll);
    const int begin = std::max(0, end - height);

    int row = top;
    for (int i = begin; i < end && row <= bottom; ++i, ++row) {
        const QString &line = wrapped.at(i);
        int attr = 0;
        if (line.contains(QStringLiteral("[secure]"), Qt::CaseInsensitive)) {
            attr = A_BOLD | (has_colors() ? COLOR_PAIR(PairSecure) : 0);
        } else if (line.contains(QStringLiteral("[error]"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("failed"), Qt::CaseInsensitive)) {
            attr = has_colors() ? COLOR_PAIR(PairError) : A_BOLD;
        } else if (line.contains(QStringLiteral("[online]"), Qt::CaseInsensitive)
                   || line.contains(QStringLiteral("connected"), Qt::CaseInsensitive)) {
            attr = has_colors() ? COLOR_PAIR(PairOnline) : 0;
        }
        safeAdd(row, 1, line, attr, textWidth);
    }

    if (m_options.showScrollbars) {
        drawScrollBar(top, bottom, scrollX,
                      wrapped.size(), begin, height);
    }

    if (sideWidth > 0) {
        const int startX = mainWidth;
        if (buddyPane) {
            drawBuddyPane(entry, top, bottom, startX, sideWidth);
        } else if (memberPane) {
            drawMemberPane(buffer, top, bottom, startX, sideWidth);
        }
    }
}


void TerminalUi::drawShortcutHint(int row, int width)
{
    move(row, 0);
    clrtoeol();

    // Keep the proven keyboard cheat-sheet text intact while presenting it as
    // the muted command rail in the 3.0 terminal shell.
    const QString hint = QStringLiteral(
        " Tab completes /commands | Ctrl-N/P buffers | Alt-1..9/F1..F9 jump | PgUp/PgDn scroll | ");
    safeAdd(row, 0, hint.left(width).leftJustified(width),
            A_DIM | (has_colors() ? COLOR_PAIR(PairFooter) : 0), width);
}

void TerminalUi::drawStatusBar(int row, int width)
{
    move(row, 0);
    clrtoeol();

    Buffer *buffer = activeBuffer();
    // The active screen is authoritative for the status bar. Do not fall back
    // to the merely selected account: doing so produced a redundant unnumbered
    // connection field and could make Status look like it belonged elsewhere.
    ConnectionEntry *entry = buffer ? connectionById(buffer->connectionId) : nullptr;

    const QString clock = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    const QString bufferText = buffer
        ? QStringLiteral("%1:%2").arg(buffer->number).arg(buffer->name)
        : QStringLiteral("-");

    QString stateText = QStringLiteral("IDLE");
    if (entry) {
        if (entry->settings.protocol == ConnectionSettings::Protocol::Sip && m_sipController) {
            const QString accountId = entry->backend ? entry->backend->id() : QString();
            stateText = accountId.isEmpty()
                ? QStringLiteral("OFFLINE")
                : m_sipController->registrationText(accountId);
        } else if (entry->connecting) {
            stateText = QStringLiteral("CONNECTING");
        } else if (entry->connected) {
            if (entry->settings.protocol == ConnectionSettings::Protocol::Oscar) {
                stateText = entry->presenceState.isEmpty() ? QStringLiteral("ONLINE") : entry->presenceState;
                if (entry->idleSeconds > 0) {
                    const quint32 minutes = entry->idleSeconds / 60;
                    stateText += minutes > 0
                        ? QStringLiteral(" · IDLE %1m").arg(minutes)
                        : QStringLiteral(" · IDLE %1s").arg(entry->idleSeconds);
                }
            } else {
                stateText = QStringLiteral("ONLINE");
            }
        } else {
            stateText = QStringLiteral("OFFLINE");
        }
    }

    int unread = 0;
    for (Buffer *candidate : m_buffers) {
        if (candidate) unread += candidate->unread;
    }

    QString text = QStringLiteral(" [%1] [%2] [%3]")
                       .arg(clock)
                       .arg(bufferText)
                       .arg(stateText);
    text.prepend(QStringLiteral(" ◉"));
    if (unread > 0) text += QStringLiteral(" [unread:%1]").arg(unread);

    safeAdd(row, 0, text.left(width).leftJustified(width),
            A_REVERSE | A_BOLD | (has_colors() ? COLOR_PAIR(PairHeader) : 0), width);
}

void TerminalUi::drawInputLine(int row, int width)
{
    Buffer *buffer = activeBuffer();
    QString promptText;
    if (buffer && buffer->kind == QStringLiteral("terminal")) {
        promptText = QStringLiteral("[BBS raw · Ctrl-N/P leaves] ❯ ");
    } else if (!buffer || buffer->kind == QStringLiteral("global")
        || buffer->kind == QStringLiteral("connection")) {
        promptText = QStringLiteral("❯ ");
    } else {
        promptText = QStringLiteral("[%1] ❯ ").arg(buffer->name);
    }

    move(row, 0);
    clrtoeol();
    safeAdd(row, 0, promptText, A_BOLD | (has_colors() ? COLOR_PAIR(PairHeader) : 0), width);

    const int promptWidth = promptText.size();
    const int available = std::max(1, width - promptWidth - 1);
    int viewStart = std::max(0, m_inputCursor - available + 1);
    QString displayInput = m_input;
    if (buffer && buffer->kind == QStringLiteral("terminal") && buffer->sensitiveInput) {
        displayInput = QString(m_input.size(), QLatin1Char('*'));
    }
    const QString visible = displayInput.mid(viewStart, available);
    safeAdd(row, promptWidth, visible, 0, available);

    const int cursorX = std::clamp(promptWidth + (m_inputCursor - viewStart), 0, width - 1);
    move(row, cursorX);
}

void TerminalUi::draw()
{
    if (!m_cursesActive || m_quitting) return;

    erase();
    const int height = LINES;
    const int width = COLS;

    if (height < 8 || width < 30) {
        safeAdd(0, 0, QStringLiteral("WaffleHouse-Client %1: terminal too small").arg(appVersionString()), A_BOLD, width);
        safeAdd(1, 0,
                QStringLiteral("Current size %1x%2; need at least 30x8.").arg(width).arg(height),
                0, width);
        refresh();
        return;
    }

    // The 3.0 shell deliberately keeps the bottom-three-row contract from the
    // proven 2.5.4-r6 TUI so keyboard hints, status, and input never move.
    drawHeader(width);
    drawConnectionsBar(1, width);
    // Keep the navigation cheat-sheet outside the main screen chrome.  The
    // separator is the visual bottom edge of the conversation/dashboard area;
    // shortcut help sits beneath it as a theme-aware footer.
    drawBufferPane(activeBuffer(), 2, height - 5, width);
    safeAdd(height - 4, 0, QString(width, QChar(0x2500)),
            A_DIM | (has_colors() ? COLOR_PAIR(PairBorder) : 0), width);
    drawShortcutHint(height - 3, width);
    drawStatusBar(height - 2, width);
    drawInputLine(height - 1, width);
    refresh();
}

void TerminalUi::handleInput()
{
    wint_t value = 0;
    while (true) {
        const int result = get_wch(&value);
        if (result == ERR) {
            break;
        }
        markUserActivity();
        if (result == KEY_CODE_YES) {
            handleSpecialKey(static_cast<int>(value));
        } else {
            handleCharacter(static_cast<uint>(value));
        }
    }
}

void TerminalUi::handleCharacter(uint codepoint)
{
    if (codepoint == 9 && (!activeBuffer() || activeBuffer()->kind != QStringLiteral("terminal"))) {
        completeCommand();
        return;
    }
    resetCommandCompletion();
    if (codepoint == 3) { // Ctrl-C
        requestQuit();
        return;
    }
    if (codepoint == 14) { // Ctrl-N
        nextBuffer();
        return;
    }
    if (codepoint == 16) { // Ctrl-P
        previousBuffer();
        return;
    }
    if (codepoint == 12) { // Ctrl-L
        clearok(stdscr, TRUE);
        return;
    }
    if (Buffer *buffer = activeBuffer(); buffer && buffer->kind == QStringLiteral("terminal")) {
        ConnectionEntry *entry = connectionById(buffer->connectionId);
        if (entry && entry->backend && entry->connected) {
            QByteArray bytes;
            if (codepoint == 10 || codepoint == 13) {
                bytes = QByteArray("\r");
                // Raw BBS keystrokes are transmitted immediately, but keep a local
                // command-line mirror so the operator can see what they are typing.
                // Password/passphrase prompts are masked by drawInputLine().
                // Enter commits the BBS command and clears the mirror for the next one.
                m_input.clear();
                m_inputCursor = 0;
                buffer->sensitiveInput = false;
            } else if (isTerminalBackspace(codepoint)) {
                bytes = QByteArray(1, '\x08');
                if (m_inputCursor > 0) {
                    m_input.remove(m_inputCursor - 1, 1);
                    --m_inputCursor;
                }
            } else if (codepoint == 9) {
                bytes = QByteArray(1, '\t');
            } else if (codepoint == 27) {
                bytes = QByteArray(1, '\x1b');
            } else if (codepoint >= 32) {
                const char32_t cp = static_cast<char32_t>(codepoint);
                const QString text = QString::fromUcs4(&cp, 1);
                bytes = text.toUtf8();
                m_input.insert(m_inputCursor, text);
                m_inputCursor += text.size();
            }
            if (!bytes.isEmpty()) entry->backend->sendTerminalInput(bytes);
            return;
        }
        // A disconnected BBS screen remains readable until /close.  Once the
        // socket is offline, fall through to normal command-line editing so
        // /close, /active, /telnet, etc. remain usable from that preserved screen.
    }
    if (codepoint == 10 || codepoint == 13) {
        submitInput();
        return;
    }
    if (isTerminalBackspace(codepoint)) {
        if (m_inputCursor > 0) {
            m_input.remove(m_inputCursor - 1, 1);
            --m_inputCursor;
        }
        return;
    }
    if (codepoint == 27) {
        // Alt-number arrives as ESC then digit in most terminals.
        wint_t next = 0;
        wtimeout(stdscr, 25);
        const int result = get_wch(&next);
        wtimeout(stdscr, 0);
        if (result != ERR && result != KEY_CODE_YES
            && next >= L'1' && next <= L'9') {
            const int index = static_cast<int>(next - L'1');
            if (index < m_buffers.size()) {
                switchBuffer(index);
            }
        }
        return;
    }

    if (codepoint >= 32) {
        const char32_t cp = static_cast<char32_t>(codepoint);
        const QString text = QString::fromUcs4(&cp, 1);
        m_input.insert(m_inputCursor, text);
        m_inputCursor += text.size();
    }
}

void TerminalUi::handleSpecialKey(int key)
{
    if (key == KEY_BTAB) {
        completeCommand(-1);
        return;
    }
    resetCommandCompletion();
    if (key >= KEY_F(1) && key <= KEY_F(9)) {
        const int index = key - KEY_F(1);
        if (index < m_buffers.size()) switchBuffer(index);
        return;
    }
    if (Buffer *buffer = activeBuffer(); buffer && buffer->kind == QStringLiteral("terminal")) {
        ConnectionEntry *entry = connectionById(buffer->connectionId);
        if (entry && entry->backend && entry->connected) {
            QByteArray seq;
            switch (key) {
            case KEY_ENTER:
                seq = "\r";
                m_input.clear();
                m_inputCursor = 0;
                buffer->sensitiveInput = false;
                break;
            case KEY_BACKSPACE:
                seq = QByteArray(1, '\x08');
                if (m_inputCursor > 0) {
                    m_input.remove(m_inputCursor - 1, 1);
                    --m_inputCursor;
                }
                break;
            case KEY_UP: seq = "\x1b[A"; break;
            case KEY_DOWN: seq = "\x1b[B"; break;
            case KEY_RIGHT: seq = "\x1b[C"; break;
            case KEY_LEFT: seq = "\x1b[D"; break;
            case KEY_HOME: seq = "\x1b[H"; break;
            case KEY_END: seq = "\x1b[F"; break;
            case KEY_DC: seq = "\x1b[3~"; break;
            case KEY_PPAGE: seq = "\x1b[5~"; break;
            case KEY_NPAGE: seq = "\x1b[6~"; break;
            case KEY_RESIZE:
                entry->backend->setTerminalSize(80, 24);
                break;
            default: break;
            }
            if (!seq.isEmpty()) entry->backend->sendTerminalInput(seq);
            return;
        }
        // Offline preserved BBS buffers use the normal command/navigation path.
    }

    switch (key) {
    case KEY_ENTER:
        submitInput();
        break;
    case KEY_BACKSPACE:
        if (m_inputCursor > 0) {
            m_input.remove(m_inputCursor - 1, 1);
            --m_inputCursor;
        }
        break;
    case KEY_DC:
        if (m_inputCursor < m_input.size()) {
            m_input.remove(m_inputCursor, 1);
        }
        break;
    case KEY_LEFT:
        m_inputCursor = std::max(0, m_inputCursor - 1);
        break;
    case KEY_RIGHT:
        m_inputCursor = std::min(static_cast<int>(m_input.size()), m_inputCursor + 1);
        break;
    case KEY_HOME:
        m_inputCursor = 0;
        break;
    case KEY_END:
        m_inputCursor = m_input.size();
        break;
    case KEY_UP:
        moveHistory(-1);
        break;
    case KEY_DOWN:
        moveHistory(1);
        break;
    case KEY_PPAGE:
        if (Buffer *buffer = activeBuffer()) {
            buffer->scroll += std::max(1, LINES - 8);
        }
        break;
    case KEY_NPAGE:
        if (Buffer *buffer = activeBuffer()) {
            buffer->scroll = std::max(0, buffer->scroll - std::max(1, LINES - 8));
        }
        break;
    case KEY_RESIZE:
        clearok(stdscr, TRUE);
        for (ConnectionEntry *entry : m_connections) {
            if (entry && entry->backend && entry->connected
                && entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
                // BBS ANSI art is normally authored for an 80-column screen.
                // Keep NAWS stable even if the enclosing ncurses window is resized.
                entry->backend->setTerminalSize(80, 24);
            }
        }
        break;
    default:
        break;
    }
}

QStringList TerminalUi::slashCommands()
{
    return {
        QStringLiteral("/active"), QStringLiteral("/accounts"),
        QStringLiteral("/add"), QStringLiteral("/addbuddy"),
        QStringLiteral("/buddies"), QStringLiteral("/buddylist"),
        QStringLiteral("/buffer"), QStringLiteral("/channels"),
        QStringLiteral("/clear"), QStringLiteral("/close"),
        QStringLiteral("/conn"), QStringLiteral("/connect"),
        QStringLiteral("/connections"), QStringLiteral("/delete"),
        QStringLiteral("/delbuddy"), QStringLiteral("/delconn"),
        QStringLiteral("/disconnect"), QStringLiteral("/edit"),
        QStringLiteral("/env"), QStringLiteral("/environment"),
        QStringLiteral("/exit"), QStringLiteral("/fingerprint"),
        QStringLiteral("/sendfile"), QStringLiteral("/transfers"),
        QStringLiteral("/accept"), QStringLiteral("/decline"),
        QStringLiteral("/canceltransfer"), QStringLiteral("/resume"),
        QStringLiteral("/cleartransfer"),
        QStringLiteral("/help"), QStringLiteral("/j"), QStringLiteral("/join"),
        QStringLiteral("/joinprivate"), QStringLiteral("/members"),
        QStringLiteral("/msg"), QStringLiteral("/names"),
        QStringLiteral("/nick"), QStringLiteral("/notice"),
        QStringLiteral("/op"), QStringLiteral("/deop"),
        QStringLiteral("/voice"), QStringLiteral("/devoice"),
        QStringLiteral("/kick"), QStringLiteral("/ban"),
        QStringLiteral("/unban"), QStringLiteral("/topic"),
        QStringLiteral("/mode"), QStringLiteral("/me"),
        QStringLiteral("/invite"), QStringLiteral("/who"),
        QStringLiteral("/whois"), QStringLiteral("/whowas"),
        QStringLiteral("/ison"), QStringLiteral("/list"),
        QStringLiteral("/motd"), QStringLiteral("/quote"),
        QStringLiteral("/options"), QStringLiteral("/menu"),
        QStringLiteral("/notifications"), QStringLiteral("/notify"),
        QStringLiteral("/sound"), QStringLiteral("/soundtest"),
        QStringLiteral("/theme"), QStringLiteral("/themes"),
        QStringLiteral("/passwd"), QStringLiteral("/part"),
        QStringLiteral("/query"), QStringLiteral("/quit"),
        QStringLiteral("/raw"), QStringLiteral("/telnet"), QStringLiteral("/bbsimport"),
        QStringLiteral("/removebuddy"), QStringLiteral("/rooms"),
        QStringLiteral("/say"), QStringLiteral("/secure"),
        QStringLiteral("/away"), QStringLiteral("/afk"), QStringLiteral("/back"),
        QStringLiteral("/idle"), QStringLiteral("/status"), QStringLiteral("/autopresence"),
        QStringLiteral("/version"),
        QStringLiteral("/secureoff"), QStringLiteral("/securestatus"),
        QStringLiteral("/phone"), QStringLiteral("/phoneprofile"),
        QStringLiteral("/phoneconfig"), QStringLiteral("/phonestart"),
        QStringLiteral("/phonestop"), QStringLiteral("/phoneactivity"),
        QStringLiteral("/dial"), QStringLiteral("/dialraw"), QStringLiteral("/dialpreview"),
        QStringLiteral("/prefix"), QStringLiteral("/calls"),
        QStringLiteral("/answer"), QStringLiteral("/reject"),
        QStringLiteral("/hangup"), QStringLiteral("/hold"),
        QStringLiteral("/callresume"), QStringLiteral("/mute"),
        QStringLiteral("/unmute"), QStringLiteral("/dtmf"),
        QStringLiteral("/siplog"), QStringLiteral("/ladder"),
        QStringLiteral("/audio-devices"), QStringLiteral("/audio-use"),
        QStringLiteral("/audio-auto"),
        QStringLiteral("/media"), QStringLiteral("/mstatus"),
        QStringLiteral("/mplay"), QStringLiteral("/mstream"), QStringLiteral("/mshoutcast"),
        QStringLiteral("/menqueue"),
        QStringLiteral("/mplaylist"), QStringLiteral("/mpause"),
        QStringLiteral("/mresume"), QStringLiteral("/mstop"),
        QStringLiteral("/mnext"), QStringLiteral("/mprev"),
        QStringLiteral("/mseek"), QStringLiteral("/mvolume"),
        QStringLiteral("/mmute"), QStringLiteral("/mshuffle"),
        QStringLiteral("/mrepeat"), QStringLiteral("/meq"),
        QStringLiteral("/server"), QStringLiteral("/servers"),
        QStringLiteral("/trust"), QStringLiteral("/untrust"),
        QStringLiteral("/use"), QStringLiteral("/window")
    };
}

void TerminalUi::resetCommandCompletion()
{
    m_commandCompletionMatches.clear();
    m_commandCompletionIndex = -1;
    m_completionMode.clear();
    m_completionStart = -1;
}

void TerminalUi::completeCommand(int direction)
{
    if (m_inputCursor < 0) return;
    const QString beforeCursor = m_input.left(m_inputCursor);

    // Slash-command completion remains available everywhere outside Telnet.
    if (m_inputCursor > 0 && beforeCursor.startsWith(QLatin1Char('/'))
        && !beforeCursor.contains(QLatin1Char(' '))
        && !beforeCursor.contains(QLatin1Char('\t'))) {
        if (m_completionMode == QStringLiteral("slash")
            && !m_commandCompletionMatches.isEmpty()
            && m_commandCompletionIndex >= 0
            && m_commandCompletionIndex < m_commandCompletionMatches.size()
            && beforeCursor.compare(m_commandCompletionMatches[m_commandCompletionIndex],
                                    Qt::CaseInsensitive) == 0) {
            const int count = m_commandCompletionMatches.size();
            m_commandCompletionIndex = (m_commandCompletionIndex + direction + count) % count;
        } else {
            m_commandCompletionMatches.clear();
            for (const QString &command : slashCommands()) {
                if (command.startsWith(beforeCursor, Qt::CaseInsensitive))
                    m_commandCompletionMatches.append(command);
            }
            m_commandCompletionMatches.sort(Qt::CaseInsensitive);
            if (m_commandCompletionMatches.isEmpty()) {
                resetCommandCompletion();
                return;
            }
            m_commandCompletionIndex = direction < 0
                ? m_commandCompletionMatches.size() - 1 : 0;
            m_completionMode = QStringLiteral("slash");
            m_completionStart = 0;
        }
        const QString replacement = m_commandCompletionMatches[m_commandCompletionIndex];
        m_input.replace(0, m_inputCursor, replacement);
        m_inputCursor = replacement.size();
        return;
    }

    // IRC channel nickname completion. At the beginning of the input the
    // completed nickname gets the traditional "nick: " mention suffix.
    Buffer *buffer = activeBuffer();
    ConnectionEntry *entry = buffer ? connectionById(buffer->connectionId) : nullptr;
    if (!buffer || !entry || buffer->kind != QStringLiteral("chat")
        || entry->settings.protocol != ConnectionSettings::Protocol::Irc) {
        resetCommandCompletion();
        return;
    }

    int tokenStart = m_inputCursor;
    while (tokenStart > 0 && !m_input.at(tokenStart - 1).isSpace()) --tokenStart;
    const QString token = m_input.mid(tokenStart, m_inputCursor - tokenStart);
    auto decorated = [tokenStart](const QString &nick) {
        return tokenStart == 0 ? nick + QStringLiteral(": ") : nick + QLatin1Char(' ');
    };

    const bool cycling = m_completionMode == QStringLiteral("member")
        && m_completionStart == tokenStart
        && !m_commandCompletionMatches.isEmpty()
        && m_commandCompletionIndex >= 0
        && m_commandCompletionIndex < m_commandCompletionMatches.size()
        && token.compare(decorated(m_commandCompletionMatches[m_commandCompletionIndex]),
                         Qt::CaseInsensitive) == 0;

    if (cycling) {
        const int count = m_commandCompletionMatches.size();
        m_commandCompletionIndex = (m_commandCompletionIndex + direction + count) % count;
    } else {
        QString seed = token.trimmed();
        if (seed.endsWith(QLatin1Char(':'))) seed.chop(1);
        if (seed.isEmpty()) {
            resetCommandCompletion();
            return;
        }
        m_commandCompletionMatches.clear();
        for (const QString &nick : buffer->members) {
            if (nick.startsWith(seed, Qt::CaseInsensitive))
                m_commandCompletionMatches.append(nick);
        }
        m_commandCompletionMatches.sort(Qt::CaseInsensitive);
        if (m_commandCompletionMatches.isEmpty()) {
            resetCommandCompletion();
            return;
        }
        m_commandCompletionIndex = direction < 0
            ? m_commandCompletionMatches.size() - 1 : 0;
        m_completionMode = QStringLiteral("member");
        m_completionStart = tokenStart;
    }

    const QString replacement = decorated(m_commandCompletionMatches[m_commandCompletionIndex]);
    m_input.replace(tokenStart, m_inputCursor - tokenStart, replacement);
    m_inputCursor = tokenStart + replacement.size();
}

void TerminalUi::moveHistory(int direction)
{
    if (m_history.isEmpty()) {
        return;
    }
    m_historyPos = std::clamp(m_historyPos + direction, 0, static_cast<int>(m_history.size()));
    m_input = m_historyPos == m_history.size()
        ? QString()
        : m_history.at(m_historyPos);
    m_inputCursor = m_input.size();
}

void TerminalUi::submitInput()
{
    const QString line = m_input;
    m_input.clear();
    m_inputCursor = 0;

    if (line.trimmed().isEmpty()) {
        return;
    }

    m_history.push_back(line);
    constexpr int MaxHistory = 500;
    while (m_history.size() > MaxHistory) {
        m_history.removeFirst();
    }
    m_historyPos = m_history.size();

    if (line.startsWith(QLatin1Char('/'))) {
        handleCommand(line);
        return;
    }

    Buffer *buffer = activeBuffer();
    if (!buffer || (buffer->kind != QStringLiteral("im")
                    && buffer->kind != QStringLiteral("chat")
                    && buffer->kind != QStringLiteral("terminal"))) {
        status(QStringLiteral("Open an IM/room/Telnet buffer or enter /help."));
        return;
    }

    ConnectionEntry *entry = connectionById(buffer->connectionId);
    if (!entry || !entry->backend || !entry->connected) {
        status(QStringLiteral("That connection is offline."));
        return;
    }

    if (buffer->kind == QStringLiteral("im")) {
        sendPrivateText(entry, buffer->target, line, buffer);
    } else if (buffer->kind == QStringLiteral("chat")) {
        if (m_secureRooms.hasRoom(entry->id, buffer->target)) {
            QString error;
            const QString frame = m_secureRooms.encrypt(entry->id, buffer->target, line, &error);
            if (frame.isEmpty()) {
                append(buffer, QStringLiteral("[error] [secure-room] %1").arg(error), false);
            } else if (entry->settings.protocol == ConnectionSettings::Protocol::Irc
                       && frame.toUtf8().size() > 400) {
                append(buffer, QStringLiteral("[error] [secure-room] encrypted IRC room message is too long; split it into shorter messages"), false);
            } else {
                entry->backend->sendRoomMessage(buffer->target, frame);
            }
        } else {
            entry->backend->sendRoomMessage(buffer->target, line);
        }
    } else {
        entry->backend->sendTerminalInput(line.toUtf8() + QByteArray("\r"));
    }
}

ChatBackend *TerminalUi::createBackend(const ConnectionSettings &settings)
{
    switch (settings.protocol) {
    case ConnectionSettings::Protocol::Oscar:
        return new OscarBackend(settings, this);
    case ConnectionSettings::Protocol::Irc:
        return new IrcBackend(settings, this);
    case ConnectionSettings::Protocol::Telnet:
        return new TelnetBackend(settings, this);
    case ConnectionSettings::Protocol::Sip:
        return new SipBackend(settings, m_sipController, this);
    case ConnectionSettings::Protocol::Unknown:
        break;
    }
    return nullptr;
}

void TerminalUi::attachBackend(ConnectionEntry *entry, ChatBackend *backend)
{
    if (!entry || !backend) {
        return;
    }

    entry->backend = backend;
    const QString entryId = entry->id;

    connect(backend, &ChatBackend::eventReceived, this,
            [this, entryId, backend](const QString &kind, const QString &target, const QString &text) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onBackendEvent(current, kind, target, text);
                }
            });
    connect(backend, &ChatBackend::membersChanged, this,
            [this, entryId, backend](const QString &room, const QString &action, const QStringList &names) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onMembersChanged(current, room, action, names);
                }
            });
    connect(backend, &ChatBackend::targetNamed, this,
            [this, entryId, backend](const QString &kind, const QString &target, const QString &displayName) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onTargetNamed(current, kind, target, displayName);
                }
            });
    connect(backend, &ChatBackend::roomDiscovered, this,
            [this, entryId, backend](const QString &roomId, const QString &displayName) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onRoomDiscovered(current, roomId, displayName);
                }
            });
    connect(backend, &ChatBackend::buddyListChanged, this,
            [this, entryId, backend](const QStringList &names) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onBuddyListChanged(current, names);
                }
            });
    connect(backend, &ChatBackend::buddyPresenceChanged, this,
            [this, entryId, backend](const QString &name, bool online) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onBuddyPresenceChanged(current, name, online);
                }
            });
    connect(backend, &ChatBackend::connected, this,
            [this, entryId, backend](const QString &identity, const QString &endpoint) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onConnected(current, identity, endpoint);
                }
            });
    connect(backend, &ChatBackend::disconnected, this,
            [this, entryId, backend](const QString &reason) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onDisconnected(current, reason);
                }
            });
    connect(backend, &ChatBackend::backendError, this,
            [this, entryId, backend](const QString &context, const QString &message) {
                if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                    onBackendError(current, context, message);
                }
            });
    if (auto *oscar = qobject_cast<OscarBackend *>(backend)) {
        connect(oscar, &OscarBackend::presenceChanged, this,
                [this, entryId, backend](const QString &state, const QString &message, quint32 idleSeconds) {
                    if (ConnectionEntry *current = connectionById(entryId); current && current->backend == backend) {
                        current->presenceState = state;
                        current->presenceMessage = message;
                        current->idleSeconds = idleSeconds;
                    }
                });
    }
}

TerminalUi::ConnectionEntry *TerminalUi::addConnectionEntry(
    const ConnectionSettings &settings,
    bool secretRequired,
    bool hasSessionSecret,
    bool persist,
    bool autoConnect,
    const QString &profileId)
{
    auto *entry = new ConnectionEntry;
    entry->id = profileId.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : profileId.trimmed();
    entry->settings = settings;
    entry->secretRequired = secretRequired;
    entry->hasSessionSecret = hasSessionSecret;

    // `persist` means a newly created/imported profile should be saved now.
    // Profiles restored from QSettings intentionally call this helper with
    // persist=false so we do not rewrite the settings array while it is being
    // read.  A restored profile always arrives with its stable profileId, so
    // it must still remain a persistent/saved connection in memory.
    const bool restoredPersistentProfile = !profileId.trimmed().isEmpty();
    entry->persistent = persist || restoredPersistentProfile;

    ChatBackend *backend = createBackend(settings);
    if (!backend) {
        delete entry;
        return nullptr;
    }
    attachBackend(entry, backend);

    m_connections.push_back(entry);
    m_connectionById.insert(entry->id, entry);
    if (m_selectedConnectionId.isEmpty()) {
        m_selectedConnectionId = entry->id;
    }

    // SIP is a first-class saved WaffleHouse connection. Load/insert the SIP
    // account into SipController as soon as the CLI connection entry is fully
    // attached to our state model, matching the GUI path. Previously the CLI
    // restored only the connection shell and deferred initializeAccount() until
    // /connect, so /phone incorrectly reported 0 SIP accounts after startup.
    bool backendReady = true;
    if (auto *sip = qobject_cast<SipBackend *>(backend)) {
        QString sipError;
        backendReady = sip->initializeAccount(&sipError);
        if (!backendReady) {
            connectionStatus(entry,
                QStringLiteral("[error] SIP account initialization failed: %1").arg(sipError));
        }
    }

    if (persist) {
        saveConnections();
    }
    if (autoConnect && backendReady) {
        connectConnection(entry);
    }
    return entry;
}

void TerminalUi::replaceBackend(ConnectionEntry *entry,
                                const ConnectionSettings &settings)
{
    if (!entry) {
        return;
    }

    ChatBackend *old = entry->backend;
    entry->backend = nullptr;
    if (old) {
        QObject::disconnect(old, nullptr, this, nullptr);
        old->stop();
        old->deleteLater();
    }

    entry->settings = settings;
    entry->connected = false;
    entry->connecting = false;
    entry->identity.clear();
    entry->endpoint.clear();
    entry->buddies.clear();
    entry->onlineBuddies.clear();
    entry->targetNames.clear();
    entry->discoveredRooms.clear();

    attachBackend(entry, createBackend(settings));
    if (Buffer *buffer = ensureBuffer(QStringLiteral("connection"), entry->id)) {
        buffer->name = connectionLabel(entry);
    }
}

void TerminalUi::deleteConnection(ConnectionEntry *entry)
{
    if (!entry) {
        return;
    }
    if (entry->backend && entry->settings.protocol == ConnectionSettings::Protocol::Sip) {
        for (const auto &call : m_sipController->calls()) {
            if (!call.disconnected && QString::fromStdString(call.accountId) == entry->backend->id()) {
                status(QStringLiteral("Hang up active calls on this SIP account before deleting it."));
                return;
            }
        }
    }

    if (entry->connected || entry->connecting) {
        entry->backend->stop();
    }

    removeConnectionConversationBuffers(entry->id);

    Buffer *connectionBuffer = findBuffer(bufferKey(QStringLiteral("connection"), entry->id, {}));
    if (connectionBuffer) {
        const int index = m_buffers.indexOf(connectionBuffer);
        m_bufferByKey.remove(connectionBuffer->key);
        if (index >= 0) {
            m_buffers.removeAt(index);
            if (m_activeBuffer >= index && m_activeBuffer > 0) {
                --m_activeBuffer;
            }
        }
        delete connectionBuffer;
        renumberBuffers();
    }

    m_hiddenConnectionBuffers.remove(entry->id);
    m_secure.closeConnection(entry->id);
    for (auto it = m_closedChatBuffers.begin(); it != m_closedChatBuffers.end();) {
        if (it->startsWith(QStringLiteral("chat:%1:").arg(entry->id))) {
            it = m_closedChatBuffers.erase(it);
        } else {
            ++it;
        }
    }
    m_connectionById.remove(entry->id);
    const int connIndex = m_connections.indexOf(entry);
    if (connIndex >= 0) {
        m_connections.removeAt(connIndex);
    }

    if (entry->backend) {
        QObject::disconnect(entry->backend, nullptr, this, nullptr);
        entry->backend->deleteLater();
    }

    const bool wasSelected = m_selectedConnectionId == entry->id;
    delete entry;

    if (wasSelected) {
        m_selectedConnectionId = m_connections.isEmpty()
            ? QString()
            : m_connections.first()->id;
    }

    if (m_buffers.isEmpty()) {
        ensureBuffer(QStringLiteral("global"), {}, {}, QStringLiteral("Status"), true);
    } else if (m_activeBuffer >= m_buffers.size()) {
        switchBuffer(m_buffers.size() - 1);
    }

    saveConnections();
}

TerminalUi::ConnectionEntry *TerminalUi::connectionById(const QString &id) const
{
    return m_connectionById.value(id, nullptr);
}

TerminalUi::ConnectionEntry *TerminalUi::selectedConnection() const
{
    if (Buffer *buffer = activeBuffer()) {
        if (!buffer->connectionId.isEmpty()) {
            if (ConnectionEntry *entry = connectionById(buffer->connectionId)) {
                return entry;
            }
        }
    }
    return connectionById(m_selectedConnectionId);
}

TerminalUi::ConnectionEntry *TerminalUi::selectedSipConnection() const
{
    if (ConnectionEntry *entry = selectedConnection();
        entry && entry->settings.protocol == ConnectionSettings::Protocol::Sip) {
        return entry;
    }
    ConnectionEntry *only = nullptr;
    for (ConnectionEntry *entry : m_connections) {
        if (!entry || entry->settings.protocol != ConnectionSettings::Protocol::Sip) continue;
        if (only) return nullptr;
        only = entry;
    }
    return only;
}

TerminalUi::ConnectionEntry *TerminalUi::sipConnectionByAccountId(const QString &accountId) const
{
    for (ConnectionEntry *entry : m_connections) {
        if (entry && entry->backend
            && entry->settings.protocol == ConnectionSettings::Protocol::Sip
            && entry->backend->id() == accountId) {
            return entry;
        }
    }
    return nullptr;
}

TerminalUi::ConnectionEntry *TerminalUi::resolveConnection(const QString &token,
                                                            bool allowEmpty) const
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty() && allowEmpty) {
        return selectedConnection();
    }

    QString protocolToken;
    QString nameToken = trimmed;
    const int separator = trimmed.indexOf(QLatin1Char(':'));
    if (separator > 0) {
        protocolToken = trimmed.left(separator).toCaseFolded();
        nameToken = trimmed.mid(separator + 1);
    }
    auto protocolMatches = [&](ConnectionSettings::Protocol p) {
        if (protocolToken.isEmpty()) return true;
        if (p == ConnectionSettings::Protocol::Oscar) return protocolToken == "aim" || protocolToken == "oscar";
        if (p == ConnectionSettings::Protocol::Irc) return protocolToken == "irc";
        if (p == ConnectionSettings::Protocol::Sip) return protocolToken == "sip";
        if (p == ConnectionSettings::Protocol::Telnet) return protocolToken == "tel" || protocolToken == "telnet" || protocolToken == "bbs";
        return false;
    };

    bool ok = false;
    const int number = nameToken.toInt(&ok);
    if (ok && number >= 1 && number <= m_connections.size()) {
        return m_connections.at(number - 1);
    }

    for (ConnectionEntry *entry : m_connections) {
        if (!entry) {
            continue;
        }
        if (!protocolMatches(entry->settings.protocol)) continue;
        if (entry->id.startsWith(nameToken, Qt::CaseInsensitive)
            || connectionLabel(entry).contains(nameToken, Qt::CaseInsensitive)
            || entry->settings.username.compare(nameToken, Qt::CaseInsensitive) == 0
            || entry->settings.server.compare(nameToken, Qt::CaseInsensitive) == 0) {
            return entry;
        }
    }
    return nullptr;
}

void TerminalUi::selectConnection(ConnectionEntry *entry, bool switchToStatus)
{
    if (!entry) {
        return;
    }
    m_selectedConnectionId = entry->id;
    if (entry->backend && entry->settings.protocol == ConnectionSettings::Protocol::Sip) {
        m_sipController->setSelectedAccountId(entry->backend->id());
    }
    if (switchToStatus) {
        m_hiddenConnectionBuffers.remove(entry->id);
        Buffer *buffer = ensureBuffer(QStringLiteral("connection"), entry->id, {},
                                      connectionLabel(entry));
        switchToBuffer(buffer);
    }
}

void TerminalUi::nextConnection()
{
    if (m_connections.isEmpty()) {
        return;
    }
    ConnectionEntry *current = connectionById(m_selectedConnectionId);
    int index = m_connections.indexOf(current);
    index = (index + 1) % m_connections.size();
    selectConnection(m_connections.at(index));
}

void TerminalUi::previousConnection()
{
    if (m_connections.isEmpty()) {
        return;
    }
    ConnectionEntry *current = connectionById(m_selectedConnectionId);
    int index = m_connections.indexOf(current);
    index = (index <= 0 ? m_connections.size() : index) - 1;
    selectConnection(m_connections.at(index));
}

bool TerminalUi::ensureSecret(ConnectionEntry *entry)
{
    if (!entry || !entry->secretRequired || entry->hasSessionSecret) {
        return true;
    }

    QString label;
    switch (entry->settings.protocol) {
    case ConnectionSettings::Protocol::Oscar:
        label = QStringLiteral("AIM/OSCAR password");
        break;
    case ConnectionSettings::Protocol::Irc:
        label = QStringLiteral("IRC server password");
        break;
    case ConnectionSettings::Protocol::Telnet:
        return true;
    case ConnectionSettings::Protocol::Sip:
        label = QStringLiteral("SIP account password");
        break;
    case ConnectionSettings::Protocol::Unknown:
        return false;
    }

    bool cancelled = false;
    const QString secret = prompt(QStringLiteral("Connection Secret"), label, {}, true, &cancelled);
    if (cancelled) {
        return false;
    }
    if (secret.isEmpty() && entry->settings.protocol != ConnectionSettings::Protocol::Irc) {
        messageBox(QStringLiteral("Connection Secret"),
                   {QStringLiteral("A non-empty secret is required.")});
        return false;
    }

    entry->settings.password = secret;
    // Push the session secret through the complete connection settings object.
    // SIP uses this to update the matching PJSUA2 account before registration;
    // the chat backends simply retain the same settings as before.
    entry->backend->setConnectionSettings(entry->settings);
    entry->settings = entry->backend->settings();
    entry->hasSessionSecret = true;

    if (!secret.isEmpty()) {
        entry->settings.savePassword = confirm(
            QStringLiteral("Connection Secret"),
            QStringLiteral("Save this password on this computer? "
                           "It will be stored locally and is not encrypted at rest."),
            false);
    } else {
        entry->settings.savePassword = false;
    }
    saveConnections();
    return true;
}

void TerminalUi::connectConnection(ConnectionEntry *entry)
{
    if (!entry || !entry->backend) {
        status(QStringLiteral("No connection selected."));
        return;
    }
    if (entry->connected || entry->connecting) {
        connectionStatus(entry, QStringLiteral("Already connected/connecting."));
        return;
    }
    if (!ensureSecret(entry)) {
        connectionStatus(entry, QStringLiteral("Connection cancelled."));
        return;
    }

    entry->connecting = true;
    connectionStatus(entry,
                     QStringLiteral("Connecting to %1:%2…")
                         .arg(entry->settings.server)
                         .arg(entry->settings.port));
    entry->backend->start();
}

void TerminalUi::disconnectConnection(ConnectionEntry *entry)
{
    if (!entry || !entry->backend) {
        return;
    }
    if (!entry->connected && !entry->connecting) {
        connectionStatus(entry, QStringLiteral("Already offline."));
        return;
    }
    connectionStatus(entry, QStringLiteral("Disconnecting…"));
    entry->backend->stop();
}

void TerminalUi::loadConnections()
{
    auto loadFrom = [this](QSettings &settings) -> int {
        const int count = settings.beginReadArray(QStringLiteral("connections"));
        int loaded = 0;

        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);

            ConnectionSettings value;
            const int protocolValue = settings.value(
                QStringLiteral("protocol"),
                static_cast<int>(ConnectionSettings::Protocol::Unknown)).toInt();

            if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Irc)) {
                value.protocol = ConnectionSettings::Protocol::Irc;
            } else if (protocolValue == 2) {
                continue; // Reserved legacy profile value; unsupported in WaffleHouse-CLI.
            } else if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Telnet)) {
                value.protocol = ConnectionSettings::Protocol::Telnet;
            } else if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Sip)) {
                value.protocol = ConnectionSettings::Protocol::Sip;
            } else if (protocolValue == static_cast<int>(ConnectionSettings::Protocol::Oscar)) {
                value.protocol = ConnectionSettings::Protocol::Oscar;
            } else {
                continue;
            }

            value.server = settings.value(QStringLiteral("server")).toString();
            value.port = static_cast<quint16>(settings.value(
                QStringLiteral("port"),
                value.protocol == ConnectionSettings::Protocol::Irc ? 6667
                    : value.protocol == ConnectionSettings::Protocol::Telnet ? 23
                    : value.protocol == ConnectionSettings::Protocol::Sip ? 5060 : 5190)
                .toUInt());
            value.username = settings.value(QStringLiteral("username")).toString();
            value.redirectHost = settings.value(QStringLiteral("redirectHost")).toString();
            value.redirectPort = static_cast<quint16>(
                settings.value(QStringLiteral("redirectPort"), 0).toUInt());
            value.realName = settings.value(
                QStringLiteral("realName"), appDefaultRealName()).toString();
            value.telnetTerminalType = settings.value(
                QStringLiteral("telnetTerminalType"), QStringLiteral("ANSI")).toString();
            value.tls = settings.value(QStringLiteral("tls"), false).toBool();
            value.ircBuddies = settings.value(QStringLiteral("ircBuddies")).toStringList();
            value.sipContacts = settings.value(QStringLiteral("sipContacts")).toStringList();
            value.debug = settings.value(QStringLiteral("debug"), false).toBool();
            value.sipProfileName = settings.value(QStringLiteral("sipProfileName")).toString();
            value.sipDomain = settings.value(QStringLiteral("sipDomain"), value.server).toString();
            value.sipRegistrar = settings.value(QStringLiteral("sipRegistrar")).toString();
            value.sipAuthUsername = settings.value(QStringLiteral("sipAuthUsername")).toString();
            value.sipDisplayName = settings.value(QStringLiteral("sipDisplayName")).toString();
            value.sipOutboundProxy = settings.value(QStringLiteral("sipOutboundProxy")).toString();
            value.sipCallerIdDomain = settings.value(QStringLiteral("sipCallerIdDomain")).toString();
            value.sipDialPrefix = settings.value(QStringLiteral("sipDialPrefix")).toString();
            value.sipStunServer = settings.value(QStringLiteral("sipStunServer")).toString();
            value.sipTransport = settings.value(QStringLiteral("sipTransport"), QStringLiteral("udp")).toString();
            value.sipIdentityMode = settings.value(QStringLiteral("sipIdentityMode"), QStringLiteral("from")).toString();
            value.sipLocalPort = static_cast<quint16>(settings.value(QStringLiteral("sipLocalPort"), value.port ? value.port : 5060).toUInt());
            value.sipRegistrationExpires = settings.value(QStringLiteral("sipRegistrationExpires"), 300).toUInt();
            value.sipUseIce = settings.value(QStringLiteral("sipUseIce"), false).toBool();
            value.sipEnableSrtp = settings.value(QStringLiteral("sipEnableSrtp"), false).toBool();
            if (value.protocol == ConnectionSettings::Protocol::Sip) {
                value.server = value.sipDomain;
                value.port = value.sipLocalPort;
            }
            value.savePassword = settings.value(QStringLiteral("savePassword"), false).toBool();
            value.password = value.savePassword
                ? settings.value(QStringLiteral("password")).toString()
                : QString();

            const bool defaultSecret = value.protocol == ConnectionSettings::Protocol::Oscar
                || value.protocol == ConnectionSettings::Protocol::Sip;
            const bool required = settings.value(
                QStringLiteral("secretRequired"), defaultSecret).toBool();
            QString profileId = settings.value(QStringLiteral("id")).toString().trimmed();
            if (profileId.isEmpty()) {
                // Deterministic one-time migration ID. This avoids two WaffleHouse-CLI
                // processes racing and inventing different profile identities on
                // the first WaffleHouse-CLI migration.
                const QString seed = QStringLiteral("%1|%2|%3|%4|%5")
                    .arg(static_cast<int>(value.protocol))
                    .arg(value.server.toCaseFolded())
                    .arg(value.port)
                    .arg(value.username.toCaseFolded())
                    .arg(i);
                profileId = QStringLiteral("migrated-%1").arg(QString::fromLatin1(
                    QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256)
                        .left(16).toHex()));
            }

            addConnectionEntry(value, required, !value.password.isEmpty(), false, false, profileId);
            ++loaded;
        }

        settings.endArray();
        return loaded;
    };

    // Both GUI and CLI modes of WaffleHouse-Client use the same QSettings
    // namespace, so saved profiles are immediately available in either mode.
    QSettings current;
    int loaded = loadFrom(current);

    if (loaded == 0) {
        QSettings legacyGui(QStringLiteral("WaffleHouseGUI"), QStringLiteral("WaffleHouseGUI"));
        loaded = loadFrom(legacyGui);
        if (loaded > 0) {
            saveConnections();
            status(QStringLiteral("Imported %1 WaffleHouse-GUI profile(s) into WaffleHouse-Client.").arg(loaded));
        }
    }
    if (loaded == 0) {
        QSettings legacyCli(QStringLiteral("WaffleHouse-CLI"), QStringLiteral("WaffleHouse-CLI"));
        loaded = loadFrom(legacyCli);
        if (loaded > 0) {
            saveConnections();
            status(QStringLiteral("Imported %1 WaffleHouse-CLI profile(s) into WaffleHouse-Client.").arg(loaded));
        }
    }
    if (loaded == 0) {
        QSettings crosspoint(QStringLiteral("Crosspoint"), QStringLiteral("Crosspoint"));
        loaded = loadFrom(crosspoint);
        if (loaded > 0) {
            saveConnections();
            status(QStringLiteral("Imported %1 legacy CrossPoint profile(s) into WaffleHouse-Client.").arg(loaded));
        }
    }
    if (loaded == 0) {
        QSettings legacy(QStringLiteral("GhostPulse"), QStringLiteral("GhostPulse"));
        loaded = loadFrom(legacy);
        if (loaded > 0) {
            saveConnections();
            status(QStringLiteral("Imported %1 legacy GhostPulse profile(s) into WaffleHouse-Client.").arg(loaded));
        }
    }

    if (!m_connections.isEmpty()) {
        m_selectedConnectionId = m_connections.first()->id;
        // Persist stable profile UUIDs used by WaffleHouse-CLI so each
        // connection keeps the same secure identity across restarts.
        saveConnections();
    }
}


void TerminalUi::saveConnections() const
{
    QSettings settings;
    settings.remove(QStringLiteral("connections"));
    settings.beginWriteArray(QStringLiteral("connections"));

    int writeIndex = 0;
    for (int i = 0; i < m_connections.size(); ++i) {
        ConnectionEntry *entry = m_connections.at(i);
        if (!entry || !entry->persistent) {
            continue;
        }
        const ConnectionSettings &value = entry->settings;
        settings.setArrayIndex(writeIndex++);
        settings.setValue(QStringLiteral("id"), entry->id);
        settings.setValue(QStringLiteral("protocol"), static_cast<int>(value.protocol));
        settings.setValue(QStringLiteral("server"), value.server);
        settings.setValue(QStringLiteral("port"), value.port);
        settings.setValue(QStringLiteral("username"), value.username);
        settings.setValue(QStringLiteral("redirectHost"), value.redirectHost);
        settings.setValue(QStringLiteral("redirectPort"), value.redirectPort);
        settings.setValue(QStringLiteral("realName"), value.realName);
        settings.setValue(QStringLiteral("tls"), value.tls);
        settings.setValue(QStringLiteral("ircBuddies"), value.ircBuddies);
        settings.setValue(QStringLiteral("sipContacts"), value.sipContacts);
        settings.setValue(QStringLiteral("telnetTerminalType"), value.telnetTerminalType);
        settings.setValue(QStringLiteral("debug"), value.debug);
        settings.setValue(QStringLiteral("sipProfileName"), value.sipProfileName);
        settings.setValue(QStringLiteral("sipDomain"), value.sipDomain);
        settings.setValue(QStringLiteral("sipRegistrar"), value.sipRegistrar);
        settings.setValue(QStringLiteral("sipAuthUsername"), value.sipAuthUsername);
        settings.setValue(QStringLiteral("sipDisplayName"), value.sipDisplayName);
        settings.setValue(QStringLiteral("sipOutboundProxy"), value.sipOutboundProxy);
        settings.setValue(QStringLiteral("sipCallerIdDomain"), value.sipCallerIdDomain);
        settings.setValue(QStringLiteral("sipDialPrefix"), value.sipDialPrefix);
        settings.setValue(QStringLiteral("sipStunServer"), value.sipStunServer);
        settings.setValue(QStringLiteral("sipTransport"), value.sipTransport);
        settings.setValue(QStringLiteral("sipIdentityMode"), value.sipIdentityMode);
        settings.setValue(QStringLiteral("sipLocalPort"), value.sipLocalPort);
        settings.setValue(QStringLiteral("sipRegistrationExpires"), value.sipRegistrationExpires);
        settings.setValue(QStringLiteral("sipUseIce"), value.sipUseIce);
        settings.setValue(QStringLiteral("sipEnableSrtp"), value.sipEnableSrtp);
        settings.setValue(QStringLiteral("secretRequired"), entry->secretRequired);
        settings.setValue(QStringLiteral("savePassword"), value.savePassword);
        if (value.savePassword && !value.password.isEmpty()) {
            settings.setValue(QStringLiteral("password"), value.password);
        }
    }

    settings.endArray();
    settings.sync();
}

void TerminalUi::loadOptions()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("ui"));
    m_options.showSplash = settings.value(QStringLiteral("cliShowSplash"), true).toBool();
    m_options.showTimestamps = settings.value(QStringLiteral("showTimestamps"), true).toBool();
    m_options.showScrollbars = settings.value(QStringLiteral("cliShowScrollbars"), true).toBool();
    m_options.showSidePanes = settings.value(QStringLiteral("showSidePanes"), true).toBool();
    m_options.theme = settings.value(QStringLiteral("cliTheme"), QStringLiteral("phosphor")).toString().toCaseFolded();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("security"));
    m_options.encryptedDmEnabled = settings.value(QStringLiteral("encryptedDmEnabled"), true).toBool();
    m_options.autoReplySecure = settings.value(QStringLiteral("autoReplySecure"), true).toBool();
    m_options.showSecureFingerprints = settings.value(QStringLiteral("showSecureFingerprints"), true).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("presence"));
    m_options.autoPresenceEnabled = settings.value(QStringLiteral("autoEnabled"), true).toBool();
    m_options.autoIdleMinutes = std::clamp(settings.value(QStringLiteral("idleMinutes"), 5).toInt(), 1, 1440);
    m_options.autoAwayMinutes = std::clamp(settings.value(QStringLiteral("awayMinutes"), 15).toInt(), m_options.autoIdleMinutes + 1, 2880);
    settings.endGroup();

    static const QSet<QString> validThemes = {
        QStringLiteral("system"), QStringLiteral("classic"), QStringLiteral("phosphor"),
        QStringLiteral("amber"), QStringLiteral("ice"), QStringLiteral("hacker"),
        QStringLiteral("matrix"), QStringLiteral("midnight"), QStringLiteral("classic-light"),
        QStringLiteral("solarized"), QStringLiteral("solarized-dark"), QStringLiteral("dracula"),
        QStringLiteral("nord"), QStringLiteral("cyberpunk"), QStringLiteral("blood-moon"),
        QStringLiteral("ocean"), QStringLiteral("retro-blue"), QStringLiteral("monochrome"),
        QStringLiteral("blue-box"), QStringLiteral("red-box"), QStringLiteral("beige-box"),
        QStringLiteral("2600"), QStringLiteral("wargames"), QStringLiteral("crt-green"),
        QStringLiteral("vt220"), QStringLiteral("cobalt"), QStringLiteral("vaporwave"),
        QStringLiteral("stealth"), QStringLiteral("synthwave"), QStringLiteral("c64"),
        QStringLiteral("dos"), QStringLiteral("waffle-iron"), QStringLiteral("ghostline"),
        QStringLiteral("hot-dog-stand"), QStringLiteral("neon-miami")
    };
    if (!validThemes.contains(m_options.theme)) {
        m_options.theme = QStringLiteral("phosphor");
    }
}

void TerminalUi::saveOptions() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("ui"));
    settings.setValue(QStringLiteral("cliShowSplash"), m_options.showSplash);
    settings.setValue(QStringLiteral("showTimestamps"), m_options.showTimestamps);
    settings.setValue(QStringLiteral("cliShowScrollbars"), m_options.showScrollbars);
    settings.setValue(QStringLiteral("showSidePanes"), m_options.showSidePanes);
    settings.setValue(QStringLiteral("cliTheme"), m_options.theme);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("security"));
    settings.setValue(QStringLiteral("encryptedDmEnabled"), m_options.encryptedDmEnabled);
    settings.setValue(QStringLiteral("autoReplySecure"), m_options.autoReplySecure);
    settings.setValue(QStringLiteral("showSecureFingerprints"), m_options.showSecureFingerprints);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("presence"));
    settings.setValue(QStringLiteral("autoEnabled"), m_options.autoPresenceEnabled);
    settings.setValue(QStringLiteral("idleMinutes"), m_options.autoIdleMinutes);
    settings.setValue(QStringLiteral("awayMinutes"), m_options.autoAwayMinutes);
    settings.endGroup();
    settings.sync();
}

void TerminalUi::showOptions()
{
    UiOptions pending = m_options;
    const QStringList themes = {
        QStringLiteral("system"), QStringLiteral("classic"), QStringLiteral("phosphor"),
        QStringLiteral("amber"), QStringLiteral("ice"), QStringLiteral("hacker"),
        QStringLiteral("matrix"), QStringLiteral("midnight"), QStringLiteral("classic-light"),
        QStringLiteral("solarized"), QStringLiteral("solarized-dark"), QStringLiteral("dracula"),
        QStringLiteral("nord"), QStringLiteral("cyberpunk"), QStringLiteral("blood-moon"),
        QStringLiteral("ocean"), QStringLiteral("retro-blue"), QStringLiteral("monochrome"),
        QStringLiteral("blue-box"), QStringLiteral("red-box"), QStringLiteral("beige-box"),
        QStringLiteral("2600"), QStringLiteral("wargames"), QStringLiteral("crt-green"),
        QStringLiteral("vt220"), QStringLiteral("cobalt"), QStringLiteral("vaporwave"),
        QStringLiteral("stealth"), QStringLiteral("synthwave"), QStringLiteral("c64"),
        QStringLiteral("dos"), QStringLiteral("waffle-iron"), QStringLiteral("ghostline"),
        QStringLiteral("hot-dog-stand"), QStringLiteral("neon-miami")
    };

    int selected = 0;
    constexpr int itemCount = 8;
    wtimeout(stdscr, 50);
    curs_set(0);

    auto cycleTheme = [&](int direction) {
        int index = themes.indexOf(pending.theme);
        if (index < 0) index = 0;
        index = (index + direction + themes.size()) % themes.size();
        pending.theme = themes.at(index);
    };

    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        const int boxWidth = fitDialogWidth(COLS - 12, 54, 76);
        const int boxHeight = 15;
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);
        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) break;
        keypad(box, TRUE);
        meta(box, TRUE);
        wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);
        const QByteArray title = QByteArray(" WaffleHouse-Client Options ");
        mvwaddnstr(box, 0, 2, title.constData(), boxWidth - 4);

        struct Item { QString label; bool *value; };
        Item items[] = {
            {QStringLiteral("Show startup logo"), &pending.showSplash},
            {QStringLiteral("Show timestamps"), &pending.showTimestamps},
            {QStringLiteral("Show scrollbars"), &pending.showScrollbars},
            {QStringLiteral("Show buddy/member side panes"), &pending.showSidePanes},
            {QStringLiteral("Enable encrypted communications (DMs + rooms)"), &pending.encryptedDmEnabled},
            {QStringLiteral("Auto-reply to secure handshakes"), &pending.autoReplySecure},
            {QStringLiteral("Show secure fingerprint notices"), &pending.showSecureFingerprints},
        };

        for (int i = 0; i < 7; ++i) {
            const QString rowText = QStringLiteral("[%1] %2")
                .arg(*items[i].value ? QLatin1Char('x') : QLatin1Char(' '))
                .arg(items[i].label);
            if (i == selected) wattron(box, A_REVERSE);
            const QByteArray bytes = rowText.toUtf8();
            mvwaddnstr(box, 2 + i, 3, bytes.constData(), boxWidth - 6);
            if (i == selected) wattroff(box, A_REVERSE);
        }

        const QString themeText = QStringLiteral("Theme: < %1 >")
            .arg(pending.theme.toUpper());
        if (selected == 7) wattron(box, A_REVERSE);
        const QByteArray themeBytes = themeText.toUtf8();
        mvwaddnstr(box, 9, 3, themeBytes.constData(), boxWidth - 6);
        if (selected == 7) wattroff(box, A_REVERSE);

        ConnectionEntry *fpEntry = selectedConnection();
        const QString fp = (m_secureReady && fpEntry)
            ? QStringLiteral("Secure fingerprint (%1): %2")
                  .arg(connectionLabel(fpEntry), m_secure.localFingerprint(fpEntry->id))
            : (m_secureReady
                   ? QStringLiteral("Secure fingerprint: select a connection")
                   : QStringLiteral("Secure DMs: unavailable"));
        const QByteArray fpBytes = fp.toUtf8();
        mvwaddnstr(box, 11, 3, fpBytes.constData(), boxWidth - 6);

        const QByteArray hint = QByteArray("Up/Down select | Space toggle | Left/Right theme | F10/Ctrl-S save | Esc cancel");
        mvwaddnstr(box, 13, 2, hint.constData(), boxWidth - 4);
        wrefresh(box);

        wint_t ch = 0;
        const int result = wget_wch(box, &ch);
        delwin(box);
        if (result == ERR) continue;

        bool save = false;
        if (result == KEY_CODE_YES) {
            const int key = static_cast<int>(ch);
            if (key == KEY_EXIT) break;
            if (key == KEY_UP) { selected = (selected + itemCount - 1) % itemCount; continue; }
            if (key == KEY_DOWN) { selected = (selected + 1) % itemCount; continue; }
            if (key == KEY_LEFT && selected == 7) { cycleTheme(-1); continue; }
            if (key == KEY_RIGHT && selected == 7) { cycleTheme(+1); continue; }
            if (key == KEY_ENTER) {
                if (selected < 7) {
                    bool *value = items[selected].value;
                    *value = !*value;
                } else {
                    cycleTheme(+1);
                }
                continue;
            }
            if (key == KEY_F(10)) save = true;
            else continue;
        } else {
            const uint cp = static_cast<uint>(ch);
            if (cp == 27 || cp == 'q' || cp == 'Q') {
                break;
            }
            if (cp == 19) save = true; // Ctrl-S
            else if (cp == ' ' || cp == 10 || cp == 13) {
                if (selected < 7) {
                    bool *value = items[selected].value;
                    *value = !*value;
                } else {
                    cycleTheme(+1);
                }
                continue;
            } else {
                continue;
            }
        }

        if (save) {
            m_options = pending;
            saveOptions();
            applyTheme();
            clearok(stdscr, TRUE);
            break;
        }
    }

    curs_set(1);
    wtimeout(stdscr, 0);
    clearok(stdscr, TRUE);
}

QString TerminalUi::imPayload(const QString &text) const
{
    if (text.startsWith(QLatin1Char('<'))) {
        const int end = text.indexOf(QStringLiteral("> "));
        if (end >= 0) {
            return text.mid(end + 2);
        }
    }
    return text;
}

QString TerminalUi::imSpeakerPrefix(const QString &text) const
{
    if (text.startsWith(QLatin1Char('<'))) {
        const int end = text.indexOf(QStringLiteral("> "));
        if (end >= 0) {
            return text.left(end + 2);
        }
    }
    return QString();
}

QString TerminalUi::secureTrustKey(ConnectionEntry *entry, const QString &target) const
{
    if (!entry) return {};
    const QString profile = QStringLiteral("%1|%2|%3|%4|%5")
        .arg(static_cast<int>(entry->settings.protocol))
        .arg(entry->settings.server.toCaseFolded())
        .arg(entry->settings.port)
        .arg(entry->settings.username.toCaseFolded())
        .arg(target.toCaseFolded());
    const QByteArray digest = QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("security/trustedPeers/%1").arg(QString::fromLatin1(digest));
}

QString TerminalUi::trustedFingerprint(ConnectionEntry *entry, const QString &target) const
{
    if (!entry) return {};
    QSettings settings;
    return settings.value(secureTrustKey(entry, target)).toString();
}

void TerminalUi::setTrustedFingerprint(ConnectionEntry *entry,
                                       const QString &target,
                                       const QString &fingerprint)
{
    if (!entry || fingerprint.isEmpty()) return;
    QSettings settings;
    settings.setValue(secureTrustKey(entry, target), fingerprint);
    settings.sync();
}

void TerminalUi::clearTrustedFingerprint(ConnectionEntry *entry, const QString &target)
{
    if (!entry) return;
    QSettings settings;
    settings.remove(secureTrustKey(entry, target));
    settings.sync();
}

QString TerminalUi::activeImTarget(ConnectionEntry *entry) const
{
    Buffer *buffer = activeBuffer();
    if (buffer && buffer->kind == QStringLiteral("im") && entry
        && buffer->connectionId == entry->id) {
        return buffer->target;
    }
    return {};
}

bool TerminalUi::secureTarget(ConnectionEntry *entry, QString target, bool switchTo)
{
    if (!entry || !entry->connected || !entry->backend) {
        status(QStringLiteral("Select an online connection first."));
        return false;
    }
    if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
        status(QStringLiteral("WaffleHouse-CLI encrypted DMs are for AIM and IRC private messages."));
        return false;
    }
    if (!m_options.encryptedDmEnabled || !m_secureReady) {
        status(QStringLiteral("Encrypted communications are disabled or unavailable. Use /options."));
        return false;
    }
    if (target.trimmed().isEmpty()) {
        target = activeImTarget(entry);
    }
    target = target.trimmed();
    if (target.isEmpty()) {
        status(QStringLiteral("usage: /secure USER (or run it from a PM buffer)"));
        return false;
    }

    Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target, switchTo);
    QString notice;
    const QString frame = m_secure.beginHandshake(entry->id, target, &notice);
    if (frame.isEmpty()) {
        append(buffer, QStringLiteral("[secure] unable to start handshake"), false);
        return false;
    }
    m_outgoingSecureFrames.insert(entry->id + QChar(0x1f) + target.toCaseFolded() + QChar(0x1f) + frame);
    entry->backend->sendPrivateMessage(target, frame);
    append(buffer, QStringLiteral("[secure] %1").arg(notice), false);
    return true;
}

bool TerminalUi::startSecureRoom(ConnectionEntry *entry, Buffer *buffer)
{
    if (!entry || !buffer || buffer->kind != QStringLiteral("chat")
        || buffer->connectionId != entry->id || !entry->connected || !entry->backend) {
        status(QStringLiteral("Run /secure from an active AIM/IRC room or channel buffer."));
        return false;
    }
    if (entry->settings.protocol != ConnectionSettings::Protocol::Oscar
        && entry->settings.protocol != ConnectionSettings::Protocol::Irc) {
        status(QStringLiteral("Secure rooms are available only for AIM/OSCAR and IRC."));
        return false;
    }
    if (!m_options.encryptedDmEnabled || !m_secureReady) {
        status(QStringLiteral("Secure communications are disabled or unavailable. Use /options."));
        return false;
    }

    QString error;
    if (!m_secureRooms.createOrRotate(entry->id, buffer->target, &error)) {
        append(buffer, QStringLiteral("[error] [secure-room] %1").arg(error), false);
        return false;
    }
    append(buffer,
           QStringLiteral("[secure-room] New shared key %1 created. Public room traffic will carry ciphertext; the key is sent only through encrypted CPX PM sessions.")
               .arg(m_secureRooms.keyId(entry->id, buffer->target)),
           false);
    distributeSecureRoomKeyToMembers(entry, buffer);
    return true;
}

void TerminalUi::showSecureRoomStatus(ConnectionEntry *entry, Buffer *buffer)
{
    if (!entry || !buffer || buffer->kind != QStringLiteral("chat")) {
        status(QStringLiteral("Run /securestatus from a room/channel buffer."));
        return;
    }
    const bool active = m_secureRooms.hasRoom(entry->id, buffer->target);
    if (!active) {
        append(buffer, QStringLiteral("[secure-room] no shared room key is active; use /secure"), false);
        return;
    }
    append(buffer,
           QStringLiteral("[secure-room] active | key=%1 | role=%2 | members=%3")
               .arg(m_secureRooms.keyId(entry->id, buffer->target),
                    m_secureRooms.locallyOwned(entry->id, buffer->target)
                        ? QStringLiteral("owner/distributor") : QStringLiteral("participant"))
               .arg(buffer->members.size()),
           false);
    append(buffer,
           QStringLiteral("[secure-room] XChaCha20-Poly1305 room messages; key delivery uses CPX encrypted PMs. Plaintext received while active is marked [plaintext]."),
           false);
}

void TerminalUi::closeSecureRoom(ConnectionEntry *entry, Buffer *buffer)
{
    if (!entry || !buffer || buffer->kind != QStringLiteral("chat")) {
        status(QStringLiteral("Run /secureoff from a room/channel buffer."));
        return;
    }
    m_secureRooms.closeRoom(entry->id, buffer->target);
    append(buffer, QStringLiteral("[secure-room] closed locally; outgoing room messages are plaintext again"), false);
}

void TerminalUi::distributeSecureRoomKey(ConnectionEntry *entry,
                                         Buffer *buffer,
                                         const QString &peer)
{
    if (!entry || !entry->backend || !buffer || peer.trimmed().isEmpty()) return;
    const QString cleanPeer = peer.trimmed();
    const QString own = entry->identity.isEmpty() ? entry->settings.username : entry->identity;
    if (!own.isEmpty() && cleanPeer.compare(own, Qt::CaseInsensitive) == 0) return;

    const QString pendingKey = entry->id + QChar(0x1f) + cleanPeer.toCaseFolded();
    if (!m_secure.hasSession(entry->id, cleanPeer)) {
        m_pendingSecureRoomKeys[pendingKey].insert(buffer->target);
        append(buffer,
               QStringLiteral("[secure-room] %1 not included yet: establish /secure %1 in a PM first; the room key will be sent automatically afterward.")
                   .arg(cleanPeer),
               false);
        return;
    }

    const QStringList caps = m_secure.peerCapabilities(entry->id, cleanPeer);
    if (!m_secure.peerSupports(entry->id, cleanPeer, QStringLiteral("secure-room-v1"))) {
        if (caps.isEmpty()) {
            m_pendingSecureRoomKeys[pendingKey].insert(buffer->target);
            append(buffer, QStringLiteral("[secure-room] waiting for %1 secure-room capability advertisement").arg(cleanPeer), false);
        } else {
            m_pendingSecureRoomKeys[pendingKey].remove(buffer->target);
            append(buffer, QStringLiteral("[secure-room] %1 does not advertise secure-room-v1; not included").arg(cleanPeer), false);
        }
        return;
    }

    QString error;
    const QString offer = m_secureRooms.keyOffer(entry->id, buffer->target, &error);
    const QString encrypted = offer.isEmpty() ? QString() : m_secure.encrypt(entry->id, cleanPeer, offer, &error);
    if (encrypted.isEmpty()) {
        append(buffer, QStringLiteral("[error] [secure-room] key delivery to %1 failed: %2").arg(cleanPeer, error), false);
        return;
    }

    m_outgoingSecureFrames.insert(entry->id + QChar(0x1f)
        + cleanPeer.toCaseFolded() + QChar(0x1f) + encrypted);
    entry->backend->sendPrivateMessage(cleanPeer, encrypted);
    m_pendingSecureRoomKeys[pendingKey].remove(buffer->target);
    if (m_pendingSecureRoomKeys[pendingKey].isEmpty()) m_pendingSecureRoomKeys.remove(pendingKey);
    append(buffer,
           QStringLiteral("[secure-room] key %1 sent privately to %2 over CPX encryption")
               .arg(m_secureRooms.keyId(entry->id, buffer->target), cleanPeer),
           false);
}

void TerminalUi::distributeSecureRoomKeyToMembers(ConnectionEntry *entry, Buffer *buffer)
{
    if (!entry || !buffer) return;
    QStringList members = buffer->members.values();
    members.sort(Qt::CaseInsensitive);
    int peers = 0;
    for (const QString &member : members) {
        const QString own = entry->identity.isEmpty() ? entry->settings.username : entry->identity;
        if (!own.isEmpty() && member.compare(own, Qt::CaseInsensitive) == 0) continue;
        ++peers;
        distributeSecureRoomKey(entry, buffer, member);
    }
    if (peers == 0) append(buffer, QStringLiteral("[secure-room] no other members are known yet"), false);
}

void TerminalUi::flushPendingSecureRoomKeys(ConnectionEntry *entry, const QString &peer)
{
    if (!entry || peer.trimmed().isEmpty()) return;
    const QString key = entry->id + QChar(0x1f) + peer.trimmed().toCaseFolded();
    const QSet<QString> rooms = m_pendingSecureRoomKeys.value(key);
    for (const QString &room : rooms) {
        Buffer *buffer = findBuffer(bufferKey(QStringLiteral("chat"), entry->id, room));
        if (buffer && m_secureRooms.hasRoom(entry->id, room)) {
            distributeSecureRoomKey(entry, buffer, peer);
        }
    }
}

bool TerminalUi::handleSecureRoomKeyOffer(ConnectionEntry *entry,
                                          const QString &peer,
                                          const QString &plaintext)
{
    if (!entry || !SecureRoomManager::looksLikeKeyOffer(plaintext)) return false;
    QString room, id, error;
    if (!m_secureRooms.installKeyOffer(entry->id, plaintext, &room, &id, &error)) {
        status(QStringLiteral("[error] [secure-room] key offer from %1 rejected: %2").arg(peer, error));
        return true;
    }
    Buffer *buffer = ensureBuffer(QStringLiteral("chat"), entry->id, room, room, false);
    append(buffer,
           QStringLiteral("[secure-room] installed shared room key %1 received privately from %2; room ciphertext can now be decrypted")
               .arg(id, peer),
           activeBuffer() != buffer);
    return true;
}

bool TerminalUi::sendSecureControlPayload(ConnectionEntry *entry,
                                               const QString &target,
                                               const QString &plaintext,
                                               Buffer *buffer)
{
    if (!entry || !entry->backend || !entry->connected) return false;

    const QString transferId = WaffleFileTransport::transferId(plaintext);
    const bool secureTransfer = transferId.isEmpty()
        ? true : m_fileTransferSecure.value(transferId, true);

    if (!secureTransfer) {
        const QString frame = WaffleFileTransport::wrapUnsecured(plaintext);
        if (entry->settings.protocol == ConnectionSettings::Protocol::Irc
            && frame.toUtf8().size() > 400) {
            append(buffer, QStringLiteral("[file] unsecured IRC transfer frame is too large; transfer stopped."), false);
            return false;
        }
        m_outgoingUnsecuredFileFrames.insert(entry->id + QChar(0x1f) + target.toCaseFolded()
                                             + QChar(0x1f) + frame);
        entry->backend->sendPrivateMessage(target, frame);
        return true;
    }

    if (!m_secureReady || !m_secure.hasSession(entry->id, target)) return false;
    QString error;
    const QString frame = m_secure.encrypt(entry->id, target, plaintext, &error);
    if (frame.isEmpty()) {
        append(buffer, QStringLiteral("[file] secure transport error: %1").arg(error), false);
        return false;
    }
    if (entry->settings.protocol == ConnectionSettings::Protocol::Irc
        && frame.toUtf8().size() > 400) {
        append(buffer, QStringLiteral("[file] encrypted IRC transfer frame is too large; transfer stopped."), false);
        return false;
    }
    m_outgoingSecureFrames.insert(entry->id + QChar(0x1f) + target.toCaseFolded()
                                  + QChar(0x1f) + frame);
    entry->backend->sendPrivateMessage(target, frame);
    return true;
}

void TerminalUi::appendTransferProgress(Buffer *buffer,
                                        const CpxFileTransferManager::Event &event,
                                        const QString &direction)
{
    if (!buffer || event.id.isEmpty()) return;
    const int bucket = event.percent < 0 ? -1 : (event.percent / 10) * 10;
    if (bucket >= 0 && bucket < 100 && m_fileTransferProgressShown.value(event.id, -10) == bucket) {
        return;
    }
    if (bucket >= 0) m_fileTransferProgressShown.insert(event.id, bucket);
    append(buffer,
           QStringLiteral("[file] %1 %2: %3% (%4/%5 bytes) [%6]")
               .arg(direction, event.fileName)
               .arg(event.percent)
               .arg(event.transferred)
               .arg(event.total)
               .arg(event.id),
           false);
}

bool TerminalUi::handleFileTransferPayload(ConnectionEntry *entry,
                                           const QString &target,
                                           const QString &plaintext,
                                           Buffer *buffer,
                                           bool secureTransport)
{
    if (!CpxFileTransferManager::looksLikeMessage(plaintext)) return false;
    const auto event = m_fileTransfers.processIncoming(target, plaintext);
    if (!event.id.isEmpty()) {
        m_fileTransferProfiles.insert(event.id, entry->id);
        m_fileTransferSecure.insert(event.id, secureTransport);
    }
    if (!event.replyPayload.isEmpty()) {
        sendSecureControlPayload(entry, target, event.replyPayload, buffer);
    }

    using Kind = CpxFileTransferManager::EventKind;
    switch (event.kind) {
    case Kind::OfferReceived:
        append(buffer,
               QStringLiteral("[file] %1 OFFER from %2: %3 (%4 bytes) [%5]")
                   .arg(secureTransport ? QStringLiteral("SECURE") : QStringLiteral("UNSECURED"),
                        target, event.fileName).arg(event.total).arg(event.id), false);
        append(buffer,
               QStringLiteral("[file] Accept with /accept %1 [PATH] or decline with /decline %1 [reason].")
                   .arg(event.id), false);
        break;
    case Kind::Accepted:
        if (event.direct && m_fileTransferSecure.value(event.id, true)) {
            append(buffer,
                   QStringLiteral("[file] %1 accepted %2; establishing encrypted direct transport [%3]")
                       .arg(target, event.fileName, event.id), false);
            startDirectOutgoing(event, entry, buffer);
        } else {
            append(buffer,
                   QStringLiteral("[file] %1 accepted %2; %3 starting at byte %4 [%5]")
                       .arg(target, event.fileName,
                            m_fileTransferSecure.value(event.id, true) ? QStringLiteral("secure relay")
                                                                       : QStringLiteral("UNSECURED relay"))
                       .arg(event.transferred).arg(event.id), false);
        }
        break;
    case Kind::Fallback:
        m_directTransfers.cancel(event.id);
        append(buffer,
               QStringLiteral("[file] direct transport fallback; secure relay resumes at byte %1 [%2]")
                   .arg(event.transferred).arg(event.id), false);
        break;
    case Kind::Declined:
        append(buffer,
               QStringLiteral("[file] transfer %1 declined: %2").arg(event.id, event.reason), false);
        break;
    case Kind::ResumeRequested:
        append(buffer,
               QStringLiteral("[file] %1 requested resume of %2 [%3]")
                   .arg(target, event.fileName, event.id), false);
        resumeIncomingFileTransfer(event.id, entry, buffer);
        break;
    case Kind::Progress:
        appendTransferProgress(buffer, event, event.outgoing
            ? QStringLiteral("sending") : QStringLiteral("receiving"));
        break;
    case Kind::Completed:
        if (event.outgoing) {
            append(buffer,
                   QStringLiteral("[file] receiver confirmed SHA-256 verification; transfer complete: %1 [%2]")
                       .arg(event.fileName, event.id), false);
        } else {
            append(buffer,
                   QStringLiteral("[file] received and SHA-256 verified: %1 -> %2 [%3]")
                       .arg(event.fileName, event.path, event.id), false);
        }
        m_fileTransferProgressShown.remove(event.id);
        break;
    case Kind::Cancelled:
        m_directTransfers.cancel(event.id);
        append(buffer,
               QStringLiteral("[file] transfer %1 cancelled: %2").arg(event.id, event.reason), false);
        m_fileTransferProgressShown.remove(event.id);
        break;
    case Kind::Error:
        append(buffer,
               QStringLiteral("[error] [file] %1").arg(event.reason), false);
        break;
    case Kind::None:
        break;
    }
    return true;
}

bool TerminalUi::resumeIncomingFileTransfer(const QString &transferId,
                                                ConnectionEntry *entry,
                                                Buffer *buffer)
{
    const auto info = m_fileTransfers.transfer(transferId);
    if (info.id.isEmpty() || info.outgoing || !entry || !entry->connected
        || !m_fileTransfers.canResume(transferId)) {
        append(buffer, QStringLiteral("[file] transfer %1 is not resumable").arg(transferId), false);
        return false;
    }
    const bool secureTransfer = m_fileTransferSecure.value(transferId, true);
    if (secureTransfer && !m_secure.hasSession(entry->id, info.target)) {
        append(buffer, QStringLiteral("[file] re-establish the secure session before resuming %1").arg(transferId), false);
        return false;
    }

    m_directTransfers.cancel(transferId);
    QString error;
    QString payload;
    bool directReady = false;
    if (secureTransfer
        && m_secure.peerSupports(entry->id, info.target, QStringLiteral("file-direct-v1"))
        && m_secure.peerSupports(entry->id, info.target, QStringLiteral("file-ack"))) {
        QString keyError;
        const QByteArray transferKey = m_secure.fileTransferKey(entry->id, info.target, transferId, &keyError);
        const QString partPath = info.path + QStringLiteral(".cpxpart");
        qint64 resumeOffset = 0;
        const QFileInfo partInfo(partPath);
        if (partInfo.exists() && partInfo.isFile() && partInfo.size() <= info.total) {
            resumeOffset = partInfo.size();
        }
        CpxDirectTransferManager::ListenResult listener;
        if (!transferKey.isEmpty()
            && m_directTransfers.prepareIncoming(
                transferId, partPath, info.total, resumeOffset,
                transferKey, listener, &error)) {
            payload = m_fileTransfers.resumeIncoming(transferId, &error, listener.port, listener.hosts);
            directReady = !payload.isEmpty();
            if (!directReady) m_directTransfers.cancel(transferId);
        } else if (!keyError.isEmpty()) {
            error = keyError;
        }
    }
    if (!directReady) {
        error.clear();
        payload = m_fileTransfers.resumeIncoming(transferId, &error);
    }
    if (payload.isEmpty()) {
        append(buffer, QStringLiteral("[file] resume failed: %1").arg(error), false);
        return false;
    }
    if (!sendSecureControlPayload(entry, info.target, payload, buffer)) {
        if (directReady) m_directTransfers.cancel(transferId);
        append(buffer, QStringLiteral("[file] could not send resume acceptance for %1").arg(transferId), false);
        return false;
    }
    const auto resumed = m_fileTransfers.transfer(transferId);
    append(buffer,
           QStringLiteral("[file] resuming %1 at byte %2 [%3] %4")
               .arg(resumed.fileName).arg(resumed.transferred).arg(transferId)
               .arg(directReady ? QStringLiteral("[direct encrypted transport]")
                                : (secureTransfer ? QStringLiteral("[secure relay]")
                                                  : QStringLiteral("[UNSECURED relay]"))), false);
    m_fileTransferProgressShown.insert(transferId, -10);
    return true;
}

void TerminalUi::startDirectOutgoing(const CpxFileTransferManager::Event &event,
                                     ConnectionEntry *entry,
                                     Buffer *buffer)
{
    if (!entry || event.id.isEmpty()) return;
    const auto info = m_fileTransfers.transfer(event.id);
    QString error;
    const QByteArray key = m_secure.fileTransferKey(entry->id, info.target, event.id, &error);
    if (key.isEmpty()
        || !m_directTransfers.startOutgoing(event.id, info.path, info.total,
                                            info.transferred, event.directHosts,
                                            event.directPort, key, &error)) {
        handleDirectFailure(event.id,
                            error.isEmpty() ? QStringLiteral("direct transfer could not be started") : error,
                            true);
        return;
    }
    append(buffer, QStringLiteral("[file] encrypted direct data connection starting [%1]").arg(event.id), false);
}

void TerminalUi::handleDirectProgress(const QString &transferId,
                                      qint64 transferred,
                                      qint64 total,
                                      bool outgoing)
{
    m_fileTransfers.updateDirectProgress(transferId, transferred, outgoing);
    const auto info = m_fileTransfers.transfer(transferId);
    ConnectionEntry *entry = connectionById(m_fileTransferProfiles.value(transferId));
    if (!entry) return;
    Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, info.target, info.target);
    CpxFileTransferManager::Event event;
    event.id = transferId;
    event.fileName = info.fileName;
    event.transferred = transferred;
    event.total = total;
    event.percent = total > 0 ? static_cast<int>((transferred * 100) / total) : 100;
    event.outgoing = outgoing;
    appendTransferProgress(buffer, event,
                           outgoing ? QStringLiteral("sending direct")
                                    : QStringLiteral("receiving direct"));
}

void TerminalUi::handleDirectIncomingFinished(const QString &transferId)
{
    const auto before = m_fileTransfers.transfer(transferId);
    ConnectionEntry *entry = connectionById(m_fileTransferProfiles.value(transferId));
    if (before.id.isEmpty() || !entry) return;
    Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, before.target, before.target);
    QString error;
    if (!m_fileTransfers.finalizeIncomingDirect(transferId, &error)) {
        append(buffer, QStringLiteral("[error] [file] direct transfer verification failed: %1 [%2]")
                           .arg(error, transferId), false);
        const QString cancelPayload = m_fileTransfers.cancel(transferId, error);
        sendSecureControlPayload(entry, before.target, cancelPayload, buffer);
        return;
    }
    sendSecureControlPayload(entry, before.target,
                             m_fileTransfers.completionPayload(transferId), buffer);
    const auto info = m_fileTransfers.transfer(transferId);
    append(buffer, QStringLiteral("[file] direct download complete and SHA-256 verified: %1 [%2]")
                       .arg(info.path, transferId), false);
    m_fileTransferProgressShown.remove(transferId);
}

void TerminalUi::handleDirectOutgoingFinished(const QString &transferId)
{
    m_fileTransfers.markOutgoingDirectSent(transferId);
    const auto info = m_fileTransfers.transfer(transferId);
    ConnectionEntry *entry = connectionById(m_fileTransferProfiles.value(transferId));
    if (!entry) return;
    Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, info.target, info.target);
    append(buffer, QStringLiteral("[file] direct upload transmitted; waiting for receiver SHA-256 confirmation: %1 [%2]")
                       .arg(info.fileName, transferId), false);
}

void TerminalUi::handleDirectFailure(const QString &transferId,
                                     const QString &reason,
                                     bool outgoing)
{
    const auto info = m_fileTransfers.transfer(transferId);
    ConnectionEntry *entry = connectionById(m_fileTransferProfiles.value(transferId));
    if (info.id.isEmpty() || !entry || !entry->connected) return;
    m_directTransfers.cancel(transferId);
    Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, info.target, info.target);
    QString payload;
    if (outgoing) {
        payload = m_fileTransfers.requestOutgoingRelayFallback(transferId, reason);
        append(buffer, QStringLiteral("[file] direct transport unavailable (%1); requesting secure relay resume [%2]")
                           .arg(reason, transferId), false);
    } else {
        payload = m_fileTransfers.fallbackIncomingToRelay(transferId);
        append(buffer, QStringLiteral("[file] direct transport interrupted (%1); resuming through secure relay at byte %2 [%3]")
                           .arg(reason).arg(info.transferred).arg(transferId), false);
    }
    if (!payload.isEmpty()) sendSecureControlPayload(entry, info.target, payload, buffer);
}

void TerminalUi::pumpFileTransfers()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_nextFilePumpMs) return;
    m_nextFilePumpMs = now + 100;

    const QStringList ids = m_fileTransfers.activeOutgoingIds();
    for (const QString &id : ids) {
        const QString profileId = m_fileTransferProfiles.value(id);
        ConnectionEntry *entry = connectionById(profileId);
        const auto info = m_fileTransfers.transfer(id);
        if (!entry || !entry->backend || !entry->connected || info.target.isEmpty()) continue;
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, info.target, info.target);
        QString error;
        bool finished = false;
        const bool secureTransfer = m_fileTransferSecure.value(id, true);
        if (secureTransfer && !m_secure.hasSession(entry->id, info.target)) continue;
        const bool irc = entry->settings.protocol == ConnectionSettings::Protocol::Irc;
        const int chunkBytes = irc ? (secureTransfer ? 120 : 96) : 768;
        const int minimumSendIntervalMs = irc ? 1000 : 500;
        const QString payload = m_fileTransfers.nextOutgoingPayload(
            id, chunkBytes, &finished, &error, minimumSendIntervalMs);
        if (!error.isEmpty()) {
            append(buffer, QStringLiteral("[error] [file] %1").arg(error), false);
            continue;
        }
        if (!payload.isEmpty() && !sendSecureControlPayload(entry, info.target, payload, buffer)) {
            const QString cancelPayload = m_fileTransfers.cancel(id, QStringLiteral("file transport failed"));
            Q_UNUSED(cancelPayload);
            continue;
        }
        const auto updated = m_fileTransfers.transfer(id);
        if (updated.total > 0 && !finished) {
            CpxFileTransferManager::Event progress;
            progress.id = updated.id;
            progress.fileName = updated.fileName;
            progress.transferred = updated.transferred;
            progress.total = updated.total;
            progress.percent = static_cast<int>((updated.transferred * 100) / updated.total);
            appendTransferProgress(buffer, progress, QStringLiteral("sending"));
        }
        if (finished) {
            append(buffer,
                   QStringLiteral("[file] finished sending %1; receiver will verify SHA-256 [%2]")
                       .arg(updated.fileName, id), false);
            m_fileTransferProgressShown.remove(id);
        }
    }
}

bool TerminalUi::sendPrivateText(ConnectionEntry *entry,
                                 const QString &target,
                                 const QString &text,
                                 Buffer *buffer)
{
    if (!entry || !entry->backend || !entry->connected) {
        return false;
    }

    if (m_options.encryptedDmEnabled && m_secureReady
        && m_secure.hasSession(entry->id, target)) {
        QString error;
        const QString frame = m_secure.encrypt(entry->id, target, text, &error);
        if (frame.isEmpty()) {
            status(QStringLiteral("[secure] %1").arg(error));
            return false;
        }
        if (entry->settings.protocol == ConnectionSettings::Protocol::Irc
            && frame.toUtf8().size() > 400) {
            status(QStringLiteral("[secure] IRC encrypted message is too long; keep secure IRC messages shorter."));
            return false;
        }
        m_outgoingSecureFrames.insert(entry->id + QChar(0x1f) + target.toCaseFolded() + QChar(0x1f) + frame);
        entry->backend->sendPrivateMessage(target, frame);
        if (!buffer) {
            buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target);
        }
        const QString me = entry->identity.isEmpty()
            ? (entry->settings.username.isEmpty() ? QStringLiteral("me") : entry->settings.username)
            : entry->identity;
        append(buffer, QStringLiteral("<%1> [secure] %2").arg(me, text), false);
        return true;
    }

    entry->backend->sendPrivateMessage(target, text);
    return true;
}

void TerminalUi::onBackendEvent(ConnectionEntry *entry,
                                const QString &kind,
                                const QString &target,
                                const QString &text)
{
    if (!entry) {
        return;
    }

    if (kind == QStringLiteral("version-request")) {
        if (auto *irc = qobject_cast<IrcBackend *>(entry->backend); irc && !target.isEmpty()) {
            const QString ctcp = QString(QChar(0x01))
                + QStringLiteral("VERSION WaffleHouse-Client %1").arg(appVersionString())
                + QChar(0x01);
            irc->sendRaw(QStringLiteral("NOTICE %1 :%2").arg(target, ctcp));
        }
        return;
    }
    if (kind == QStringLiteral("version")) {
        m_pendingVersionQueries.remove(entry->id + QChar(0x1f) + target.toCaseFolded());
        QString report = text.trimmed();
        if (entry->settings.protocol == ConnectionSettings::Protocol::Irc
            && !report.contains(QStringLiteral("WaffleHouse"), Qt::CaseInsensitive)) {
            report = QStringLiteral("IRC client reports: %1 (not identified as WaffleHouse-Client)").arg(report);
        }
        const QString line = QStringLiteral("[version] %1: %2").arg(target, report);
        if (Buffer *buffer = findBuffer(bufferKey(QStringLiteral("im"), entry->id, target))) append(buffer, line, false);
        status(line);
        return;
    }

    if (kind == QStringLiteral("status")) {
        connectionStatus(entry, text);
        return;
    }

    // Ignore late traffic after teardown so closed conversation buffers do not reappear.
    if (!entry->connected) {
        return;
    }

    // A room/channel explicitly closed by the user stays closed while PART
    // acknowledgements and queued member/message events drain from the backend.
    if (kind == QStringLiteral("chat")
        && m_closedChatBuffers.contains(bufferKey(kind, entry->id, target))) {
        return;
    }

    if (kind == QStringLiteral("im")) {
        const QString payload = imPayload(text);
        const QString outgoingToken = entry->id + QChar(0x1f) + target.toCaseFolded()
            + QChar(0x1f) + payload;
        if (m_outgoingUnsecuredFileFrames.remove(outgoingToken)) return;
        QString filePayload;
        if (WaffleFileTransport::unwrapUnsecured(payload, filePayload)) {
            const QString displayName = entry->targetNames.value(
                QStringLiteral("im|%1").arg(target), target);
            Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, displayName);
            handleFileTransferPayload(entry, target, filePayload, buffer, false);
            return;
        }
    }

    if (kind == QStringLiteral("im") && m_secureReady) {
        const QString payload = imPayload(text);
        const QString outgoingToken = entry->id + QChar(0x1f) + target.toCaseFolded()
            + QChar(0x1f) + payload;
        if (m_outgoingSecureFrames.remove(outgoingToken)) {
            return;
        }

        if (SecureChannelManager::looksLikeFrame(payload)) {
            const QString displayName = entry->targetNames.value(
                QStringLiteral("im|%1").arg(target), target);
            Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, displayName);

            if (!m_options.encryptedDmEnabled) {
                append(buffer, QStringLiteral("[secure] WaffleHouse-CLI encrypted DM frame ignored because encryption is disabled."));
                return;
            }

            const auto result = m_secure.processIncoming(
                entry->id, target, payload, m_options.autoReplySecure);

            if (!result.replyFrame.isEmpty()) {
                m_outgoingSecureFrames.insert(entry->id + QChar(0x1f) + target.toCaseFolded()
                                              + QChar(0x1f) + result.replyFrame);
                entry->backend->sendPrivateMessage(target, result.replyFrame);
            }

            const QString capabilities = m_secure.capabilitiesFrame(entry->id, target);
            if (!capabilities.isEmpty()) {
                m_outgoingSecureFrames.insert(entry->id + QChar(0x1f) + target.toCaseFolded()
                                              + QChar(0x1f) + capabilities);
                entry->backend->sendPrivateMessage(target, capabilities);
            }

            if (result.kind == SecureChannelManager::IncomingKind::Decrypted) {
                if (handleSecureRoomKeyOffer(entry, target, result.plaintext)) {
                    return;
                }
                if (handleFileTransferPayload(entry, target, result.plaintext, buffer, true)) {
                    return;
                }
                QString prefix = imSpeakerPrefix(text);
                if (prefix.isEmpty()) {
                    prefix = QStringLiteral("<%1> ").arg(displayName);
                }
                append(buffer, prefix + QStringLiteral("[secure] ") + result.plaintext);
                if (const auto event = NotificationManager::classifyIncoming(
                        entry->settings, entry->identity, kind, text)) {
                    NotificationManager::play(*event, true);
                }
                return;
            }

            if (result.kind == SecureChannelManager::IncomingKind::Error) {
                append(buffer, QStringLiteral("[error] [secure] %1").arg(result.notice));
                return;
            }

            if (result.kind == SecureChannelManager::IncomingKind::Control) {
                QString notice = result.notice;
                if (!result.peerFingerprint.isEmpty()) {
                    const QString trusted = trustedFingerprint(entry, target);
                    if (!trusted.isEmpty() && trusted != result.peerFingerprint) {
                        notice = QStringLiteral("TRUST WARNING: peer fingerprint changed. Trusted %1, received %2")
                                     .arg(trusted, result.peerFingerprint);
                        append(buffer, QStringLiteral("[error] [secure] %1").arg(notice));
                        m_secure.closeSession(entry->id, target);
                        return;
                    }
                    if (trusted == result.peerFingerprint) {
                        notice += QStringLiteral(" [trusted]");
                    } else {
                        notice += QStringLiteral(" [UNVERIFIED — compare fingerprints, then /trust]");
                    }
                }
                if (!notice.isEmpty()) {
                    if (m_options.showSecureFingerprints || result.peerFingerprint.isEmpty()) {
                        append(buffer, QStringLiteral("[secure] %1").arg(notice));
                    } else {
                        append(buffer, result.peerFingerprint.isEmpty()
                            ? QStringLiteral("[secure] WaffleHouse-CLI secure session established.")
                            : QStringLiteral("[secure] WaffleHouse-CLI secure session established [UNVERIFIED — use /securestatus, compare, then /trust]."));
                    }
                }
                flushPendingSecureRoomKeys(entry, target);
                return;
            }
        }
    }

    if (kind == QStringLiteral("chat") && m_secureReady) {
        const QString payload = imPayload(text);
        if (SecureRoomManager::looksLikeFrame(payload)) {
            const QString displayName = entry->targetNames.value(QStringLiteral("chat|%1").arg(target), target);
            Buffer *buffer = ensureBuffer(QStringLiteral("chat"), entry->id, target, displayName);
            const auto result = m_secureRooms.processIncoming(entry->id, target, payload);
            if (result.kind == SecureRoomManager::IncomingKind::Decrypted) {
                QString prefix = imSpeakerPrefix(text);
                if (prefix.isEmpty()) prefix = QStringLiteral("<room> ");
                append(buffer, prefix + QStringLiteral("[secure-room] ") + result.plaintext);
                if (const auto event = NotificationManager::classifyIncoming(
                        entry->settings, entry->identity, kind, text)) {
                    NotificationManager::play(*event, true);
                }
                return;
            }
            if (result.kind == SecureRoomManager::IncomingKind::Error) {
                append(buffer, QStringLiteral("[error] [secure-room] %1").arg(result.notice));
                return;
            }
        }
        if (m_secureRooms.hasRoom(entry->id, target)) {
            const QString prefix = imSpeakerPrefix(text);
            if (!prefix.isEmpty()) {
                const QString displayName = entry->targetNames.value(QStringLiteral("chat|%1").arg(target), target);
                Buffer *buffer = ensureBuffer(QStringLiteral("chat"), entry->id, target, displayName);
                append(buffer, prefix + QStringLiteral("[plaintext] ") + payload);
                return;
            }
        }
    }

    const QString displayName = entry->targetNames.value(
        QStringLiteral("%1|%2").arg(kind, target), target);
    Buffer *buffer = ensureBuffer(kind, entry->id, target, displayName);
    if (kind == QStringLiteral("terminal") && buffer->terminal) {
        buffer->terminal->feed(text);

        // Classic BBS software usually suppresses password echo at the
        // application layer without renegotiating Telnet ECHO.  Detect the
        // prompt nearest the terminal cursor and mask WaffleHouse's local input
        // mirror so credentials never appear in the bottom command field.
        QString promptLine;
        for (int row = buffer->terminal->cursorRow(); row >= 0 && row >= buffer->terminal->cursorRow() - 2; --row) {
            const QString candidate = buffer->terminal->plainLine(row).trimmed();
            if (!candidate.isEmpty()) { promptLine = candidate; break; }
        }
        static const QRegularExpression sensitivePrompt(
            QStringLiteral("(?i)(?:password|passphrase|passwd|pin|access\\s*code|login\\s*key|secret)\\s*[:>?\]]?\\s*$"));
        buffer->sensitiveInput = sensitivePrompt.match(promptLine).hasMatch();

        buffer->unread += (buffer != activeBuffer());
        return;
    }
    append(buffer, text);
    if (const auto event = NotificationManager::classifyIncoming(
            entry->settings, entry->identity, kind, text)) {
        NotificationManager::play(*event, true);
    }
}

void TerminalUi::onMembersChanged(ConnectionEntry *entry,
                                  const QString &room,
                                  const QString &action,
                                  const QStringList &names)
{
    if (!entry || !entry->connected) {
        return;
    }

    if (m_closedChatBuffers.contains(
            bufferKey(QStringLiteral("chat"), entry->id, room))) {
        return;
    }

    const QString displayName = entry->targetNames.value(
        QStringLiteral("chat|%1").arg(room), room);
    Buffer *buffer = ensureBuffer(QStringLiteral("chat"), entry->id, room, displayName);

    if (action == QStringLiteral("replace")) {
        buffer->members.clear();
        for (const QString &name : names) {
            buffer->members.insert(name);
        }
    } else if (action == QStringLiteral("add")) {
        for (const QString &name : names) {
            buffer->members.insert(name);
        }
    } else if (action == QStringLiteral("remove")) {
        for (const QString &name : names) {
            const QString folded = name.toCaseFolded();
            for (auto it = buffer->members.begin(); it != buffer->members.end();) {
                if (it->toCaseFolded() == folded) {
                    it = buffer->members.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    if ((action == QStringLiteral("add") || action == QStringLiteral("remove"))
        && m_secureRooms.hasRoom(entry->id, room)
        && m_secureRooms.locallyOwned(entry->id, room)) {
        QString error;
        if (m_secureRooms.createOrRotate(entry->id, room, &error)) {
            append(buffer, QStringLiteral("[secure-room] membership changed; rotated key to %1 and redistributing")
                               .arg(m_secureRooms.keyId(entry->id, room)), false);
            distributeSecureRoomKeyToMembers(entry, buffer);
        } else {
            append(buffer, QStringLiteral("[error] [secure-room] key rotation failed: %1").arg(error), false);
        }
    }
}

void TerminalUi::onTargetNamed(ConnectionEntry *entry,
                               const QString &kind,
                               const QString &target,
                               const QString &displayName)
{
    if (!entry || displayName.isEmpty()) {
        return;
    }
    entry->targetNames.insert(QStringLiteral("%1|%2").arg(kind, target), displayName);
    if (Buffer *buffer = findBuffer(bufferKey(kind, entry->id, target))) {
        buffer->name = displayName;
    }
}

void TerminalUi::onRoomDiscovered(ConnectionEntry *entry,
                                  const QString &roomId,
                                  const QString &displayName)
{
    if (!entry) {
        return;
    }
    entry->discoveredRooms.insert(roomId, displayName.isEmpty() ? roomId : displayName);
}

void TerminalUi::onBuddyListChanged(ConnectionEntry *entry, const QStringList &names)
{
    if (!entry) {
        return;
    }
    entry->buddies.clear();
    for (const QString &name : names) {
        entry->buddies.insert(name);
    }
    if (entry->settings.protocol == ConnectionSettings::Protocol::Irc) {
        entry->settings.ircBuddies = names;
        saveConnections();
    }
}

void TerminalUi::onBuddyPresenceChanged(ConnectionEntry *entry,
                                        const QString &name,
                                        bool online)
{
    if (!entry) {
        return;
    }
    entry->buddies.insert(name);
    const QString folded = name.toCaseFolded();
    if (online) {
        entry->onlineBuddies.insert(folded);
    } else {
        entry->onlineBuddies.remove(folded);
    }
}

void TerminalUi::onConnected(ConnectionEntry *entry,
                             const QString &identity,
                             const QString &endpoint)
{
    if (!entry) {
        return;
    }
    entry->connecting = false;
    entry->connected = true;
    entry->identity = identity;
    entry->endpoint = endpoint;

    connectionStatus(
        entry,
        QStringLiteral("[online] %1 connected as %2 via %3")
            .arg(protocolName(entry->settings.protocol), identity, endpoint));
    selectConnection(entry, false);

    if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
        entry->backend->setTerminalSize(80, 24);
        const QString display = entry->settings.username.trimmed().isEmpty()
            ? entry->settings.server
            : entry->settings.username.trimmed();
        ensureBuffer(QStringLiteral("terminal"), entry->id,
                     entry->settings.server, display, true);
    }
}

void TerminalUi::onDisconnected(ConnectionEntry *entry, const QString &reason)
{
    if (!entry) {
        return;
    }
    entry->connecting = false;
    entry->connected = false;
    entry->identity.clear();
    entry->endpoint.clear();
    entry->onlineBuddies.clear();
    entry->presenceState = QStringLiteral("ONLINE");
    entry->presenceMessage.clear();
    entry->idleSeconds = 0;
    entry->autoPresenceState.clear();
    m_secure.closeConnection(entry->id);
    m_secureRooms.closeConnection(entry->id);

    // Preserve a Telnet/BBS terminal exactly as it looked at disconnect.
    // The user can review/copy it and explicitly remove it with /close.
    // Other protocols retain their historical teardown behavior.
    if (entry->settings.protocol != ConnectionSettings::Protocol::Telnet) {
        removeConnectionConversationBuffers(entry->id);
    }
    connectionStatus(
        entry,
        reason.isEmpty()
            ? QStringLiteral("[offline] disconnected")
            : QStringLiteral("[offline] disconnected: %1").arg(reason));

    // ncurses tracks what it believes is already painted on the physical
    // terminal.  If a library ever writes directly to stderr/stdout while
    // curses owns the screen, that cache becomes stale and ordinary refresh()
    // may leave garbage visible.  A disconnect is a natural synchronization
    // point, so force the next draw to repaint every cell.
    if (m_cursesActive) {
        clearok(stdscr, TRUE);
        touchwin(stdscr);
    }
}

void TerminalUi::onBackendError(ConnectionEntry *entry,
                                const QString &context,
                                const QString &message)
{
    if (!entry) {
        return;
    }
    connectionStatus(entry,
                     QStringLiteral("[error] %1: %2").arg(context, message));

    if (entry->connecting && entry->secretRequired) {
        entry->hasSessionSecret = false;
        if (!entry->settings.savePassword) {
            entry->settings.password.clear();
        }
        if (entry->backend) {
            if (auto *sip = qobject_cast<SipBackend *>(entry->backend)) {
                // Never re-enter SIP account creation/reconfiguration from an
                // error callback. The old path recursively retried a failed
                // initializeAccount() until the CLI stack overflowed.
                sip->clearSessionPassword();
            } else {
                ConnectionSettings cleared = entry->backend->settings();
                cleared.password.clear();
                entry->backend->setConnectionSettings(cleared);
            }
            entry->settings = entry->backend->settings();
        }
    }
}

QString TerminalUi::takeArgument(QString &rest)
{
    rest = rest.trimmed();
    if (rest.isEmpty()) {
        return {};
    }

    if (rest.startsWith(QLatin1Char('"')) || rest.startsWith(QLatin1Char('\''))) {
        const QChar quote = rest.front();
        QString value;
        bool escaped = false;
        int i = 1;
        for (; i < rest.size(); ++i) {
            const QChar ch = rest.at(i);
            if (escaped) {
                value += ch;
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == quote) {
                ++i;
                break;
            } else {
                value += ch;
            }
        }
        rest = rest.mid(i).trimmed();
        return value;
    }

    const int split = rest.indexOf(QLatin1Char(' '));
    if (split < 0) {
        const QString value = rest;
        rest.clear();
        return value;
    }

    const QString value = rest.left(split);
    rest = rest.mid(split + 1).trimmed();
    return value;
}

bool TerminalUi::parseYesNo(const QString &value, bool defaultValue)
{
    const QString v = value.trimmed().toCaseFolded();
    if (v.isEmpty()) {
        return defaultValue;
    }
    return v == QStringLiteral("y") || v == QStringLiteral("yes")
        || v == QStringLiteral("1") || v == QStringLiteral("true")
        || v == QStringLiteral("on");
}

quint16 TerminalUi::parsePort(const QString &value, quint16 fallback)
{
    bool ok = false;
    const int port = value.trimmed().toInt(&ok);
    if (!ok || port < 1 || port > 65535) {
        return fallback;
    }
    return static_cast<quint16>(port);
}


TerminalUi::Buffer *TerminalUi::phoneBuffer(bool switchTo)
{
    return ensureBuffer(QStringLiteral("phone"), {}, {}, QStringLiteral("Softphone"), switchTo);
}

void TerminalUi::showPhoneMain(bool switchTo)
{
    Buffer *buffer = phoneBuffer(switchTo);
    buffer->lines.clear();
    append(buffer, QStringLiteral("WAFFLEHOUSE INTEGRATED SIP SOFTPHONE"), false);
    append(buffer, m_sipController->audioSummary(), false);
    append(buffer, QString(), false);
    append(buffer, QStringLiteral("SIP ACCOUNTS"), false);
    const auto accounts = m_sipController->accounts();
    if (accounts.isEmpty()) {
        append(buffer, QStringLiteral("No SIP accounts. Use /add and choose SIP."), false);
    } else {
        for (const auto &account : accounts) {
            ConnectionEntry *entry = sipConnectionByAccountId(account.id);
            const bool selected = entry && selectedConnection() == entry;
            append(buffer, QStringLiteral("%1 %2  %3  %4")
                               .arg(selected ? QStringLiteral(">") : QStringLiteral(" "))
                               .arg(account.name.left(18), -18)
                               .arg(account.identity.left(30), -30)
                               .arg(account.registrationText), false);
        }
    }
    const auto calls = m_sipController->calls();
    int active = 0;
    for (const auto &call : calls) if (!call.disconnected) ++active;
    append(buffer, QString(), false);
    append(buffer, QStringLiteral("Calls: %1 active / %2 total this session").arg(active).arg(calls.size()), false);
    if (ConnectionEntry *entry=selectedSipConnection()) {
        append(buffer, QStringLiteral("Current PBX dial prefix: %1")
                           .arg(m_sipController->dialPrefix(entry->backend->id()).isEmpty()?QStringLiteral("<none>"):m_sipController->dialPrefix(entry->backend->id())), false);
    }
    append(buffer, QStringLiteral("Select a SIP connection with /select, then /connect and /dial."), false);
    append(buffer, QStringLiteral("Commands: /prefix [VALUE|off] | /dial DEST [CID] | /dialraw DEST [CID] | /calls"), false);
    append(buffer, QStringLiteral("          /dialpreview DEST | /answer ID | /reject ID | /hangup ID"), false);
    append(buffer, QStringLiteral("          /hold ID | /callresume ID | /mute ID | /unmute ID | /dtmf ID DIGITS"), false);
    append(buffer, QStringLiteral("          /siplog [ID] | /ladder ID | /phoneprofile | /phoneconfig | /phoneactivity"), false);
    append(buffer, QStringLiteral("          /audio-devices | /audio-use CAPTURE PLAYBACK | /audio-auto on|off"), false);
}

void TerminalUi::showPhoneCalls(bool switchTo)
{
    Buffer *buffer = ensureBuffer(QStringLiteral("phone-calls"), {}, {}, QStringLiteral("Phone Calls"), switchTo);
    buffer->lines.clear();
    append(buffer, QStringLiteral("ID  ACCOUNT            DIR  STATE              REMOTE                         CODEC        MEDIA  MUTED  FG"), false);
    const auto calls = m_sipController->calls();
    if (calls.empty()) {
        append(buffer, QStringLiteral("No SIP calls in this session."), false);
        return;
    }
    for (const auto &c : calls) {
        QString account = QString::fromStdString(c.accountName);
        if (account.isEmpty()) account = QString::fromStdString(c.accountId);
        append(buffer, QStringLiteral("%1  %2  %3   %4  %5  %6  %7  %8  %9")
                           .arg(c.id, 2)
                           .arg(account.left(18), -18)
                           .arg(c.direction == trunkmonkey::CallDirection::Incoming ? QStringLiteral("IN ") : QStringLiteral("OUT"))
                           .arg(QString::fromStdString(c.state), -18)
                           .arg(QString::fromStdString(c.remoteUri), -30)
                           .arg(QString::fromStdString(c.codecName), -12)
                           .arg(c.mediaActive ? QStringLiteral("yes") : QStringLiteral("no "))
                           .arg(c.microphoneMuted ? QStringLiteral("yes") : QStringLiteral("no "))
                           .arg(c.foreground ? QStringLiteral("yes") : QStringLiteral("no")), false);
    }
}

void TerminalUi::showPhoneProfile(bool switchTo)
{
    Buffer *buffer = ensureBuffer(QStringLiteral("phone-profile"), {}, {}, QStringLiteral("Phone Profile"), switchTo);
    buffer->lines.clear();
    ConnectionEntry *entry = selectedSipConnection();
    if (!entry || !entry->backend) {
        append(buffer, QStringLiteral("No unique SIP account is selected."), false);
        append(buffer, QStringLiteral("Use /connections and /select <number|name>, or /add to create a SIP account."), false);
        return;
    }
    m_sipController->setSelectedAccountId(entry->backend->id());
    bool ok = false;
    const auto p = m_sipController->accountProfile(entry->backend->id(), &ok);
    if (!ok) {
        append(buffer, QStringLiteral("The selected WaffleHouse connection has no active SIP profile."), false);
        return;
    }
    const QStringList lines = {
        QStringLiteral("SIP ACCOUNT — %1").arg(connectionLabel(entry)),
        QStringLiteral("Registration: %1").arg(m_sipController->registrationText(entry->backend->id())),
        QStringLiteral("Name: %1").arg(QString::fromStdString(p.name)),
        QStringLiteral("SIP domain: %1").arg(QString::fromStdString(p.sipDomain)),
        QStringLiteral("Registrar: %1").arg(QString::fromStdString(p.registrar)),
        QStringLiteral("Username: %1").arg(QString::fromStdString(p.username)),
        QStringLiteral("Auth username: %1").arg(QString::fromStdString(p.authUsername)),
        QStringLiteral("Display name: %1").arg(QString::fromStdString(p.displayName)),
        QStringLiteral("Outbound proxy: %1").arg(QString::fromStdString(p.outboundProxy)),
        QStringLiteral("Caller-ID domain: %1").arg(QString::fromStdString(p.callerIdDomain)),
        QStringLiteral("Profile startup prefix: %1").arg(QString::fromStdString(p.dialPrefix).isEmpty()?QStringLiteral("<none>"):QString::fromStdString(p.dialPrefix)),
        QStringLiteral("Current runtime prefix: %1").arg(m_sipController->dialPrefix(entry->backend->id()).isEmpty()?QStringLiteral("<none>"):m_sipController->dialPrefix(entry->backend->id())),
        QStringLiteral("STUN: %1").arg(QString::fromStdString(p.stunServer)),
        QStringLiteral("Transport: %1").arg(QString::fromStdString(trunkmonkey::toString(p.transport))),
        QStringLiteral("Identity mode: %1").arg(QString::fromStdString(trunkmonkey::toString(p.identityMode))),
        QStringLiteral("Local SIP port: %1").arg(p.localSipPort),
        QStringLiteral("Registration expiry: %1").arg(p.registrationExpires),
        QStringLiteral("ICE: %1 | SRTP: %2 | saved password: %3")
            .arg(p.useIce ? QStringLiteral("on") : QStringLiteral("off"),
                 p.enableSrtp ? QStringLiteral("on") : QStringLiteral("off"),
                 entry->settings.savePassword ? QStringLiteral("yes") : QStringLiteral("no")),
        QString(),
        QStringLiteral("Use /phoneconfig (or /edit) to edit this same saved WaffleHouse SIP connection."),
    };
    for (const QString &line : lines) append(buffer, line, false);
}

void TerminalUi::showPhoneSipLog(int callId, bool switchTo)
{
    Buffer *buffer = ensureBuffer(QStringLiteral("phone-siplog"), {}, {}, QStringLiteral("SIP Log"), switchTo);
    buffer->lines.clear();
    append(buffer, QStringLiteral("SIP LOG%1").arg(callId >= 0 ? QStringLiteral(" — CALL %1").arg(callId) : QString()), false);
    const QStringList lines = m_sipController->sipLogText(callId).split('\n');
    for (const QString &line : lines) append(buffer, line, false);
}

void TerminalUi::showPhoneLadder(int callId, bool switchTo)
{
    Buffer *buffer = ensureBuffer(QStringLiteral("phone-ladder"), {}, {}, QStringLiteral("SIP Ladder"), switchTo);
    buffer->lines.clear();
    const QStringList lines = m_sipController->ladderText(callId).split('\n');
    for (const QString &line : lines) append(buffer, line, false);
}

void TerminalUi::showPhoneActivity(bool switchTo)
{
    Buffer *buffer = ensureBuffer(QStringLiteral("phone-activity"), {}, {}, QStringLiteral("Phone Activity"), switchTo);
    buffer->lines.clear();
    const QStringList lines = m_sipController->activityText().split('\n');
    if (lines.size() == 1 && lines.first().isEmpty()) append(buffer, QStringLiteral("No softphone activity yet."), false);
    else for (const QString &line : lines) append(buffer, line, false);
}

void TerminalUi::configurePhoneProfile()
{
    ConnectionEntry *entry = selectedSipConnection();
    if (!entry) {
        status(QStringLiteral("Select a SIP connection first with /select, or create one with /add."));
        return;
    }
    editConnectionWizard(entry);
    if (entry->backend) m_sipController->setSelectedAccountId(entry->backend->id());
    showPhoneProfile(true);
}

void TerminalUi::handleCommand(const QString &line)
{
    QString rest = line.mid(1).trimmed();
    const QString command = takeArgument(rest).toCaseFolded();

    if (command == QStringLiteral("quit") || command == QStringLiteral("exit")) {
        requestQuit();
        return;
    }
    if (command == QStringLiteral("help")) {
        showHelp();
        return;
    }
    if (command == QStringLiteral("options")) {
        showOptions();
        return;
    }
    if (command == QStringLiteral("version")) {
        requestClientVersion(selectedConnection(), rest.trimmed());
        return;
    }
    if (command == QStringLiteral("autopresence")) {
        QString args = rest.trimmed();
        QString action = takeArgument(args).toCaseFolded();
        if (action.isEmpty()) {
            status(QStringLiteral("Auto OSCAR presence: %1 | idle after %2 min | away after %3 min")
                       .arg(m_options.autoPresenceEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
                       .arg(m_options.autoIdleMinutes).arg(m_options.autoAwayMinutes));
            return;
        }
        if (action == QStringLiteral("on") || action == QStringLiteral("off")) {
            m_options.autoPresenceEnabled = action == QStringLiteral("on");
            if (!m_options.autoPresenceEnabled) markUserActivity();
            saveOptions();
            status(QStringLiteral("Automatic OSCAR idle/away %1.").arg(action.toUpper()));
            return;
        }
        bool ok = false;
        const int minutes = takeArgument(args).toInt(&ok);
        if (!ok || minutes < 1) {
            status(QStringLiteral("Usage: /autopresence [on|off|idle MINUTES|away MINUTES]"));
            return;
        }
        if (action == QStringLiteral("idle")) {
            if (minutes >= m_options.autoAwayMinutes) {
                status(QStringLiteral("Auto-idle must be less than auto-away (%1 minutes).").arg(m_options.autoAwayMinutes));
                return;
            }
            m_options.autoIdleMinutes = minutes;
        } else if (action == QStringLiteral("away")) {
            if (minutes <= m_options.autoIdleMinutes) {
                status(QStringLiteral("Auto-away must be greater than auto-idle (%1 minutes).").arg(m_options.autoIdleMinutes));
                return;
            }
            m_options.autoAwayMinutes = minutes;
        } else {
            status(QStringLiteral("Usage: /autopresence [on|off|idle MINUTES|away MINUTES]"));
            return;
        }
        saveOptions();
        status(QStringLiteral("Auto OSCAR presence: idle after %1 min, away after %2 min.")
                   .arg(m_options.autoIdleMinutes).arg(m_options.autoAwayMinutes));
        return;
    }
    if (command == QStringLiteral("notifications")) {
        QStringList lines;
        lines << QStringLiteral("Notification sounds: %1").arg(NotificationManager::globalEnabled() ? QStringLiteral("ON") : QStringLiteral("OFF"));
        lines << QStringLiteral("Event keys: irc-mention, irc-pm, aim-im, aim-chat");
        lines << QStringLiteral("");
        for (const auto event : NotificationManager::configurableEvents()) {
            const auto cfg = NotificationManager::setting(event);
            QString source = cfg.soundSpec;
            if (NotificationManager::isBuiltinSpec(source)) source = QStringLiteral("built-in");
            else if (NotificationManager::isCustomSpec(source)) source = NotificationManager::customPath(source);
            lines << QStringLiteral("%1 [%2] — %3")
                         .arg(NotificationManager::key(event), cfg.enabled ? QStringLiteral("on") : QStringLiteral("off"), source);
        }
        lines << QStringLiteral("");
        lines << QStringLiteral("/notify on|off");
        lines << QStringLiteral("/sound EVENT builtin|off|PATH");
        lines << QStringLiteral("/soundtest EVENT");
        messageBox(QStringLiteral("Notification Sounds"), lines);
        return;
    }
    if (command == QStringLiteral("notify")) {
        const QString mode = rest.trimmed().toCaseFolded();
        if (mode != QStringLiteral("on") && mode != QStringLiteral("off")) {
            status(QStringLiteral("usage: /notify on|off"));
            return;
        }
        NotificationManager::setGlobalEnabled(mode == QStringLiteral("on"));
        status(QStringLiteral("Notification sounds %1.").arg(mode.toUpper()));
        return;
    }
    if (command == QStringLiteral("sound") || command == QStringLiteral("soundtest")) {
        QString args = rest;
        const QString eventName = takeArgument(args);
        const auto event = NotificationManager::eventFromKey(eventName);
        if (!event) {
            status(QStringLiteral("Unknown event. Use: irc-mention, irc-pm, aim-im, aim-chat"));
            return;
        }
        if (command == QStringLiteral("soundtest")) {
            NotificationManager::playSpec(NotificationManager::setting(*event).soundSpec, true);
            status(QStringLiteral("Tested %1 sound.").arg(NotificationManager::displayName(*event)));
            return;
        }
        QString value = args.trimmed();
        if (value.isEmpty()) {
            status(QStringLiteral("usage: /sound EVENT builtin|off|PATH"));
            return;
        }
        NotificationManager::Setting cfg = NotificationManager::setting(*event);
        if (value.compare(QStringLiteral("builtin"), Qt::CaseInsensitive) == 0) {
            cfg.enabled = true;
            cfg.soundSpec = NotificationManager::builtinSpec(*event);
        } else if (value.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0
                   || value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
            cfg.enabled = false;
            cfg.soundSpec = QStringLiteral("none");
        } else {
            if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
                value = value.mid(1, value.size() - 2);
            }
            if (!QFileInfo::exists(value)) {
                status(QStringLiteral("Sound file does not exist: %1").arg(value));
                return;
            }
            cfg.enabled = true;
            cfg.soundSpec = NotificationManager::customSpec(QFileInfo(value).absoluteFilePath());
        }
        NotificationManager::setSetting(*event, cfg);
        status(QStringLiteral("%1 sound updated. Use /soundtest %2 to preview it.")
                   .arg(NotificationManager::displayName(*event), NotificationManager::key(*event)));
        return;
    }
    if (command == QStringLiteral("themes")) {
        messageBox(QStringLiteral("CLI Themes"), cliThemeNames());
        return;
    }
    if (command == QStringLiteral("theme")) {
        QString name = takeArgument(rest).trimmed().toCaseFolded();
        name.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (name.isEmpty()) {
            status(QStringLiteral("Current CLI theme: %1. Usage: /theme NAME; use /themes to list choices.")
                       .arg(m_options.theme));
            return;
        }
        const QStringList themes = cliThemeNames();
        if (!themes.contains(name)) {
            status(QStringLiteral("Unknown theme '%1'. Use /themes to list choices.").arg(name));
            return;
        }
        m_options.theme = name;
        saveOptions();
        applyTheme();
        clearok(stdscr, TRUE);
        status(QStringLiteral("CLI theme changed to %1.").arg(name));
        return;
    }
    if (command == QStringLiteral("menu")) {
        messageBox(QStringLiteral("Termux Command Map"), {
            QStringLiteral("Connections: /connections /add /edit /connect /disconnect /use"),
            QStringLiteral("Chat: /msg /query /join /j /part /buddies /rooms /members"),
            QStringLiteral("IRC: /nick /notice /me /topic /mode /who /whois /list /motd /quote"),
            QStringLiteral("Secure: /secure /secureoff /fingerprint /trust /untrust /securestatus"),
            QStringLiteral("Files: /sendfile /transfers /accept /decline /resume /canceltransfer"),
            QStringLiteral("SIP: /phone /dial /calls /answer /reject /hangup /hold /dtmf /siplog /ladder"),
            QStringLiteral("Audio: /audio-devices /audio-use /audio-auto"),
            QStringLiteral("Media: /media /mplay /mstream /mshoutcast /menqueue /mplaylist /mstop /meq"),
            QStringLiteral("UI: /theme /themes /options /notifications /help"),
            QStringLiteral("Tip: Termux extra-keys can provide TAB, CTRL, ALT, PGUP and PGDN.")
        });
        return;
    }
    if (command == QStringLiteral("env") || command == QStringLiteral("environment")) {
        const RuntimeEnvironment info = RuntimeEnvironment::detect();
        messageBox(QStringLiteral("Runtime Environment"), {
            QStringLiteral("OS: %1").arg(info.osName),
            QStringLiteral("Mode: %1").arg(info.mode),
            QStringLiteral("Session: %1").arg(info.sessionType),
            QStringLiteral("Desktop/WM: %1").arg(info.desktop.isEmpty() ? QStringLiteral("not detected") : info.desktop),
            QStringLiteral("Terminal: %1").arg(info.terminal.isEmpty() ? QStringLiteral("not detected") : info.terminal),
        });
        return;
    }
    if (command == QStringLiteral("media") || command == QStringLiteral("mstatus")) {
        messageBox(QStringLiteral("WaffleHouse Media"), m_mediaController->statusLines());
        return;
    }
    if (command == QStringLiteral("mplay") || command == QStringLiteral("mstream")) {
        const QString source = takeArgument(rest);
        if (source.isEmpty()) {
            status(command == QStringLiteral("mstream")
                ? QStringLiteral("Usage: /mstream URL (SHOUTcast/Icecast/HTTP/HLS)")
                : QStringLiteral("Usage: /mplay FILE|URL"));
            return;
        }
        m_mediaController->play(source);
        return;
    }
    if (command == QStringLiteral("mshoutcast")) {
        const QString query = takeArgument(rest);
        if (query.isEmpty()) { status(QStringLiteral("Usage: /mshoutcast SEARCH-TERMS")); return; }
        const QByteArray encoded = QByteArrayLiteral("https://directory.shoutcast.com/Search?query=")
            + QUrl::toPercentEncoding(query);
        const QUrl url = QUrl::fromEncoded(encoded);
#ifdef WAFFLEHOUSE_TERMUX
        const QString opener = QStandardPaths::findExecutable(QStringLiteral("termux-open-url"));
        if (!opener.isEmpty() && QProcess::startDetached(opener, {url.toString()})) {
            status(QStringLiteral("Opened SHOUTcast directory search in Android: %1").arg(query));
        } else {
            status(QStringLiteral("SHOUTcast search URL: %1 (install termux-api for browser handoff)").arg(url.toString()));
        }
#else
        if (QDesktopServices::openUrl(url)) {
            status(QStringLiteral("Opened SHOUTcast directory search: %1").arg(query));
        } else {
            status(QStringLiteral("SHOUTcast search URL: %1").arg(url.toString()));
        }
#endif
        return;
    }
    if (command == QStringLiteral("menqueue")) {
        const QString source = takeArgument(rest);
        if (source.isEmpty()) { status(QStringLiteral("Usage: /menqueue FILE|URL")); return; }
        m_mediaController->enqueue(source);
        return;
    }
    if (command == QStringLiteral("mplaylist")) {
        const QString source = takeArgument(rest);
        if (source.isEmpty()) {
            status(QStringLiteral("Usage: /mplaylist PLAYLIST-PATH-OR-URL (use /mstream for HLS .m3u8)"));
            return;
        }
        m_mediaController->loadPlaylist(source, true);
        return;
    }
    if (command == QStringLiteral("mpause")) { m_mediaController->pause(); return; }
    if (command == QStringLiteral("mresume")) { m_mediaController->resume(); return; }
    if (command == QStringLiteral("mstop")) { m_mediaController->stop(); return; }
    if (command == QStringLiteral("mnext")) { m_mediaController->next(); return; }
    if (command == QStringLiteral("mprev")) { m_mediaController->previous(); return; }
    if (command == QStringLiteral("mseek")) {
        bool ok = false;
        const double seconds = takeArgument(rest).toDouble(&ok);
        if (!ok) { status(QStringLiteral("Usage: /mseek SECONDS (negative seeks backward)")); return; }
        m_mediaController->seekRelative(seconds);
        return;
    }
    if (command == QStringLiteral("mvolume")) {
        bool ok = false;
        const int volume = takeArgument(rest).toInt(&ok);
        if (!ok || volume < 0 || volume > 150) { status(QStringLiteral("Usage: /mvolume 0..150")); return; }
        m_mediaController->setVolume(volume);
        return;
    }
    if (command == QStringLiteral("mmute")) {
        const QString value = takeArgument(rest).toCaseFolded();
        if (value == QStringLiteral("on")) m_mediaController->setMuted(true);
        else if (value == QStringLiteral("off")) m_mediaController->setMuted(false);
        else if (value == QStringLiteral("toggle") || value.isEmpty()) m_mediaController->toggleMuted();
        else status(QStringLiteral("Usage: /mmute on|off|toggle"));
        return;
    }
    if (command == QStringLiteral("mshuffle")) {
        const QString value = takeArgument(rest).toCaseFolded();
        if (value != QStringLiteral("on") && value != QStringLiteral("off")) { status(QStringLiteral("Usage: /mshuffle on|off")); return; }
        m_mediaController->setShuffle(value == QStringLiteral("on"));
        return;
    }
    if (command == QStringLiteral("mrepeat")) {
        const QString value = takeArgument(rest).toCaseFolded();
        if (value != QStringLiteral("off") && value != QStringLiteral("one") && value != QStringLiteral("all")) {
            status(QStringLiteral("Usage: /mrepeat off|one|all")); return;
        }
        m_mediaController->setRepeatMode(value);
        return;
    }
    if (command == QStringLiteral("meq")) {
        const QString bandText = takeArgument(rest).toCaseFolded();
        if (bandText == QStringLiteral("flat") || bandText == QStringLiteral("reset")) {
            m_mediaController->resetEqualizer(); return;
        }
        bool bandOk = false, gainOk = false;
        const int band = bandText.toInt(&bandOk);
        const double gain = takeArgument(rest).toDouble(&gainOk);
        if (!bandOk || !gainOk || band < 0 || band > 9 || gain < -12.0 || gain > 12.0) {
            status(QStringLiteral("Usage: /meq BAND(0..9) GAIN(-12..12) or /meq flat")); return;
        }
        m_mediaController->setEqualizerBand(band, gain);
        return;
    }

    if (command == QStringLiteral("phone")) {
        showPhoneMain(true);
        return;
    }
    if (command == QStringLiteral("phoneprofile")) {
        showPhoneProfile(true);
        return;
    }
    if (command == QStringLiteral("phoneconfig")) {
        configurePhoneProfile();
        return;
    }
    if (command == QStringLiteral("phonestart")) {
        ConnectionEntry *entry = selectedSipConnection();
        if (!entry) { status(QStringLiteral("Select a SIP connection first with /select.")); return; }
        connectConnection(entry);
        showPhoneMain(true);
        return;
    }
    if (command == QStringLiteral("phonestop")) {
        ConnectionEntry *entry = selectedSipConnection();
        if (!entry) { status(QStringLiteral("Select a SIP connection first with /select.")); return; }
        disconnectConnection(entry);
        showPhoneMain(true);
        return;
    }
    if (command == QStringLiteral("phoneactivity")) {
        showPhoneActivity(true);
        return;
    }
    if (command == QStringLiteral("prefix")) {
        ConnectionEntry *entry=selectedSipConnection();
        if(!entry || !entry->backend){status(QStringLiteral("Select a SIP connection with /select first."));return;}
        QString value=takeArgument(rest).trimmed();
        if(value.isEmpty()){
            const QString current=m_sipController->dialPrefix(entry->backend->id());
            status(QStringLiteral("Current PBX dial prefix: %1 (profile startup default: %2)")
                       .arg(current.isEmpty()?QStringLiteral("<none>"):current,
                            entry->settings.sipDialPrefix.isEmpty()?QStringLiteral("<none>"):entry->settings.sipDialPrefix));
            return;
        }
        if(value.compare(QStringLiteral("off"),Qt::CaseInsensitive)==0 ||
           value.compare(QStringLiteral("none"),Qt::CaseInsensitive)==0 ||
           value==QStringLiteral("-")) value.clear();
        QString error;
        if(!m_sipController->setDialPrefix(entry->backend->id(),value,&error)) status(QStringLiteral("Dial prefix failed: %1").arg(error));
        else { status(QStringLiteral("Current PBX dial prefix: %1").arg(value.isEmpty()?QStringLiteral("<none>"):value)); showPhoneMain(false); }
        return;
    }
    if (command == QStringLiteral("dialpreview")) {
        ConnectionEntry *entry=selectedSipConnection();
        if(!entry || !entry->backend){status(QStringLiteral("Select a SIP connection with /select first."));return;}
        const QString destination=takeArgument(rest);
        if(destination.isEmpty()){status(QStringLiteral("Usage: /dialpreview DESTINATION"));return;}
        QString error;
        const QString target=m_sipController->dialPreview(entry->backend->id(),destination,true,&error);
        if(target.isEmpty() && !error.isEmpty()) status(QStringLiteral("Dial preview failed: %1").arg(error));
        else messageBox(QStringLiteral("SIP Dial Preview"),{
            QStringLiteral("Account: %1").arg(connectionLabel(entry)),
            QStringLiteral("Entered destination: %1").arg(destination),
            QStringLiteral("Current runtime prefix: %1").arg(m_sipController->dialPrefix(entry->backend->id()).isEmpty()?QStringLiteral("<none>"):m_sipController->dialPrefix(entry->backend->id())),
            QStringLiteral("Profile startup default: %1").arg(entry->settings.sipDialPrefix.isEmpty()?QStringLiteral("<none>"):entry->settings.sipDialPrefix),
            QStringLiteral("SIP Request-URI target: %1").arg(target)
        });
        return;
    }
    if (command == QStringLiteral("dial") || command == QStringLiteral("dialraw")) {
        ConnectionEntry *entry = selectedSipConnection();
        if (!entry || !entry->backend) {
            status(QStringLiteral("Select a SIP connection with /select before dialing."));
            return;
        }
        const QString destination = takeArgument(rest);
        const QString callerId = takeArgument(rest);
        const bool applyPrefix=command==QStringLiteral("dial");
        if (destination.isEmpty()) { status(QStringLiteral("Usage: /%1 DESTINATION [CALLER-ID]").arg(command)); return; }
        m_sipController->setSelectedAccountId(entry->backend->id());
        QString error;
        const QString wireTarget=m_sipController->dialPreview(entry->backend->id(),destination,applyPrefix,&error);
        if(wireTarget.isEmpty() && !error.isEmpty()){status(QStringLiteral("Dial failed: %1").arg(error));return;}
        const int id = m_sipController->dial(entry->backend->id(), destination, callerId, &error, applyPrefix);
        if (id < 0) status(QStringLiteral("Dial failed: %1").arg(error));
        else { showPhoneCalls(true); status(QStringLiteral("Dialing call %1 via %2 -> %3%4").arg(id).arg(connectionLabel(entry), wireTarget, applyPrefix?QString():QStringLiteral(" [prefix bypassed]"))); }
        return;
    }
    if (command == QStringLiteral("calls")) {
        showPhoneCalls(true);
        return;
    }
    auto parseCallId = [this](QString &args, const QString &usage) -> int {
        bool ok = false;
        const int id = takeArgument(args).toInt(&ok);
        if (!ok || id < 0) status(usage);
        return ok && id >= 0 ? id : -1;
    };
    if (command == QStringLiteral("answer")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /answer CALL-ID")); if (id < 0) return;
        QString error; if (!m_sipController->answer(id, &error)) status(QStringLiteral("Answer failed: %1").arg(error));
        showPhoneCalls(true); return;
    }
    if (command == QStringLiteral("reject")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /reject CALL-ID")); if (id < 0) return;
        QString error; if (!m_sipController->reject(id, &error)) status(QStringLiteral("Reject failed: %1").arg(error));
        showPhoneCalls(true); return;
    }
    if (command == QStringLiteral("hangup")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /hangup CALL-ID")); if (id < 0) return;
        QString error; if (!m_sipController->hangup(id, &error)) status(QStringLiteral("Hangup failed: %1").arg(error));
        showPhoneCalls(true); return;
    }
    if (command == QStringLiteral("hold")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /hold CALL-ID")); if (id < 0) return;
        QString error; if (!m_sipController->hold(id, &error)) status(QStringLiteral("Hold failed: %1").arg(error));
        showPhoneCalls(true); return;
    }
    if (command == QStringLiteral("callresume")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /callresume CALL-ID")); if (id < 0) return;
        QString error; if (!m_sipController->resume(id, &error)) status(QStringLiteral("Resume failed: %1").arg(error));
        showPhoneCalls(true); return;
    }
    if (command == QStringLiteral("mute") || command == QStringLiteral("unmute")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /%1 CALL-ID").arg(command)); if (id < 0) return;
        QString error; if (!m_sipController->setMuted(id, command == QStringLiteral("mute"), &error)) status(QStringLiteral("Mute change failed: %1").arg(error));
        showPhoneCalls(true); return;
    }
    if (command == QStringLiteral("dtmf")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /dtmf CALL-ID DIGITS")); if (id < 0) return;
        const QString digits = takeArgument(rest); if (digits.isEmpty()) { status(QStringLiteral("Usage: /dtmf CALL-ID DIGITS")); return; }
        QString error; if (!m_sipController->sendDtmf(id, digits, &error)) status(QStringLiteral("DTMF failed: %1").arg(error));
        return;
    }
    if (command == QStringLiteral("siplog")) {
        int id = -1;
        if (!rest.trimmed().isEmpty()) { id = parseCallId(rest, QStringLiteral("Usage: /siplog [CALL-ID]")); if (id < 0) return; }
        showPhoneSipLog(id, true); return;
    }
    if (command == QStringLiteral("ladder")) {
        const int id = parseCallId(rest, QStringLiteral("Usage: /ladder CALL-ID")); if (id < 0) return;
        showPhoneLadder(id, true); return;
    }
    if (command == QStringLiteral("audio-devices")) {
        Buffer *buffer = ensureBuffer(QStringLiteral("phone-audio"), {}, {}, QStringLiteral("Phone Audio"), true);
        buffer->lines.clear();
        for (const QString &line : m_sipController->audioDevicesText().split('\n')) append(buffer, line, false);
        append(buffer, QString(), false);
        append(buffer, m_sipController->audioSummary(), false);
        return;
    }
    if (command == QStringLiteral("audio-use")) {
        bool ok1 = false, ok2 = false;
        const int capture = takeArgument(rest).toInt(&ok1);
        const int playback = takeArgument(rest).toInt(&ok2);
        if (!ok1 || !ok2) { status(QStringLiteral("Usage: /audio-use CAPTURE-ID PLAYBACK-ID")); return; }
        QString error; if (!m_sipController->setAudioDevices(capture, playback, &error)) status(QStringLiteral("Audio selection failed: %1").arg(error));
        else status(m_sipController->audioSummary());
        return;
    }
    if (command == QStringLiteral("audio-auto")) {
        const QString value = takeArgument(rest).toCaseFolded();
        if (value != QStringLiteral("on") && value != QStringLiteral("off")) { status(QStringLiteral("Usage: /audio-auto on|off")); return; }
        m_sipController->setAudioAutoSwitch(value == QStringLiteral("on"));
        status(m_sipController->audioSummary());
        return;
    }

    if (command == QStringLiteral("bbsimport")) {
        const QString path = takeArgument(rest);
        if (path.isEmpty()) status(QStringLiteral("Usage: /bbsimport /path/to/bbs-list.csv|json|txt"));
        else importBbsList(path);
        return;
    }
    if (command == QStringLiteral("telnet")) {
        const QString spec = takeArgument(rest);
        const QString port = takeArgument(rest);
        if (spec.isEmpty()) status(QStringLiteral("Usage: /telnet HOST [PORT] or /telnet HOST:PORT"));
        else openAdHocTelnet(spec, port);
        return;
    }
    if (command == QStringLiteral("fingerprint")) {
        if (!m_secureReady) {
            status(QStringLiteral("Encrypted DMs are unavailable."));
        } else {
            ConnectionEntry *entry = selectedConnection();
            if (!entry) {
                status(QStringLiteral("Select a connection first; each saved profile has its own secure fingerprint."));
            } else {
                status(QStringLiteral("%1 secure fingerprint: %2")
                           .arg(connectionLabel(entry), m_secure.localFingerprint(entry->id)));
            }
        }
        return;
    }
    if (command == QStringLiteral("add")) {
        addConnectionWizard();
        return;
    }
    if (command == QStringLiteral("connections") || command == QStringLiteral("accounts")
        || command == QStringLiteral("servers")) {
        listConnections();
        return;
    }
    if (command == QStringLiteral("active")) {
        listActiveConnections();
        return;
    }
    if (command == QStringLiteral("conn") || command == QStringLiteral("server")) {
        const QString token = takeArgument(rest);
        if (token == QStringLiteral("next")) {
            nextConnection();
            return;
        }
        if (token == QStringLiteral("prev") || token == QStringLiteral("previous")) {
            previousConnection();
            return;
        }
        ConnectionEntry *entry = resolveConnection(token);
        if (!entry) {
            status(QStringLiteral("Connection not found. /connections lists them."));
            return;
        }
        selectConnection(entry);
        return;
    }
    if (command == QStringLiteral("connect")) {
        ConnectionEntry *entry = resolveConnection(takeArgument(rest));
        if (!entry) {
            status(QStringLiteral("Connection not found."));
            return;
        }
        selectConnection(entry, true);
        connectConnection(entry);
        return;
    }
    if (command == QStringLiteral("disconnect")) {
        ConnectionEntry *entry = resolveConnection(takeArgument(rest));
        if (!entry) {
            status(QStringLiteral("Connection not found."));
            return;
        }
        disconnectConnection(entry);
        return;
    }
    if (command == QStringLiteral("delete") || command == QStringLiteral("delconn")) {
        ConnectionEntry *entry = resolveConnection(takeArgument(rest));
        if (!entry) {
            status(QStringLiteral("Connection not found."));
            return;
        }
        if (confirm(QStringLiteral("Delete Connection"),
                    QStringLiteral("Delete %1 from saved connections?")
                        .arg(connectionLabel(entry)), false)) {
            deleteConnection(entry);
            status(QStringLiteral("Connection deleted."));
        }
        return;
    }
    if (command == QStringLiteral("edit")) {
        ConnectionEntry *entry = resolveConnection(takeArgument(rest));
        if (!entry) {
            status(QStringLiteral("Connection not found."));
            return;
        }
        if (entry->connected || entry->connecting) {
            status(QStringLiteral("Disconnect the connection before editing it."));
            return;
        }
        editConnectionWizard(entry);
        return;
    }

    if (command == QStringLiteral("window") || command == QStringLiteral("buffer")
        || command == QStringLiteral("use")) {
        const QString token = rest.trimmed();
        if (token.isEmpty()) {
            status(QStringLiteral("usage: /window N|next|prev|NAME"));
            return;
        }
        if (token.compare(QStringLiteral("next"), Qt::CaseInsensitive) == 0) {
            nextBuffer();
            return;
        }
        if (token.compare(QStringLiteral("prev"), Qt::CaseInsensitive) == 0
            || token.compare(QStringLiteral("previous"), Qt::CaseInsensitive) == 0) {
            previousBuffer();
            return;
        }
        bool ok = false;
        const int number = token.toInt(&ok);
        if (ok) {
            if (number >= 1 && number <= m_buffers.size()) {
                switchBuffer(number - 1);
            } else {
                status(QStringLiteral("No buffer %1.").arg(number));
            }
            return;
        }
        for (Buffer *buffer : m_buffers) {
            if (buffer->name.compare(token, Qt::CaseInsensitive) == 0) {
                switchToBuffer(buffer);
                return;
            }
        }
        status(QStringLiteral("No buffer named %1.").arg(token));
        return;
    }

    if (command == QStringLiteral("close")) {
        closeActiveBuffer();
        return;
    }
    if (command == QStringLiteral("clear")) {
        if (Buffer *buffer = activeBuffer()) {
            buffer->lines.clear();
            buffer->scroll = 0;
        }
        return;
    }

    ConnectionEntry *entry = selectedConnection();

    if (command == QStringLiteral("away") || command == QStringLiteral("afk")
        || command == QStringLiteral("idle") || command == QStringLiteral("back")
        || command == QStringLiteral("status")) {
        if (!entry || !entry->connected
            || entry->settings.protocol != ConnectionSettings::Protocol::Oscar) {
            status(QStringLiteral("This command requires an online AIM/OSCAR connection."));
            return;
        }
        auto *oscar = qobject_cast<OscarBackend *>(entry->backend);
        if (!oscar) {
            status(QStringLiteral("The selected AIM backend does not expose presence controls."));
            return;
        }
        if (command != QStringLiteral("status")) entry->autoPresenceState.clear();
        if (command == QStringLiteral("away")) {
            oscar->setAwayMessage(rest.trimmed());
            return;
        }
        if (command == QStringLiteral("afk")) {
            oscar->setAfkMessage(rest.trimmed());
            return;
        }
        if (command == QStringLiteral("back")) {
            oscar->setBack();
            return;
        }
        if (command == QStringLiteral("idle")) {
            const QString value = rest.trimmed().toCaseFolded();
            if (value == QStringLiteral("off") || value == QStringLiteral("0")) {
                oscar->setIdleSeconds(0);
                return;
            }
            quint32 seconds = 1;
            if (!value.isEmpty()) {
                bool ok = false;
                const quint64 parsed = value.toULongLong(&ok);
                if (!ok || parsed > 0xffffffffULL) {
                    status(QStringLiteral("Usage: /idle [SECONDS|off]"));
                    return;
                }
                seconds = static_cast<quint32>(parsed);
            }
            oscar->setIdleSeconds(seconds);
            return;
        }
        QString summary = entry->presenceState.isEmpty() ? QStringLiteral("ONLINE") : entry->presenceState;
        if (entry->idleSeconds > 0) summary += QStringLiteral(" + IDLE %1s").arg(entry->idleSeconds);
        if (!entry->presenceMessage.isEmpty()) summary += QStringLiteral(" — %1").arg(entry->presenceMessage);
        status(QStringLiteral("AIM presence: %1").arg(summary));
        return;
    }

    if (command == QStringLiteral("secure")) {
        Buffer *buffer = activeBuffer();
        if (buffer && buffer->kind == QStringLiteral("chat") && rest.trimmed().isEmpty()) {
            startSecureRoom(entry, buffer);
        } else {
            secureTarget(entry, rest.trimmed(), true);
        }
        return;
    }

    if (command == QStringLiteral("securestatus")) {
        Buffer *active = activeBuffer();
        if (active && active->kind == QStringLiteral("chat") && rest.trimmed().isEmpty()) {
            showSecureRoomStatus(entry, active);
            return;
        }
        if (!entry) {
            status(QStringLiteral("Select a connection first."));
            return;
        }
        QString target = rest.trimmed();
        if (target.isEmpty()) target = activeImTarget(entry);
        if (target.isEmpty()) {
            status(QStringLiteral("usage: /securestatus USER (or run from a PM buffer)"));
            return;
        }
        const QString peer = m_secure.peerFingerprint(entry->id, target);
        if (peer.isEmpty()) {
            status(QStringLiteral("No secure session with %1.").arg(target));
            return;
        }
        const QString trusted = trustedFingerprint(entry, target);
        QString trustState = QStringLiteral("UNVERIFIED");
        if (!trusted.isEmpty() && trusted == peer) trustState = QStringLiteral("trusted");
        else if (!trusted.isEmpty()) trustState = QStringLiteral("TRUST MISMATCH");
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target, true);
        append(buffer, QStringLiteral("[secure] peer fingerprint: %1 [%2]").arg(peer, trustState), false);
        append(buffer, QStringLiteral("[secure] local fingerprint: %1")
                           .arg(m_secure.localFingerprint(entry->id)), false);
        const QStringList caps = m_secure.peerCapabilities(entry->id, target);
        append(buffer, QStringLiteral("[secure] peer capabilities: %1")
                           .arg(caps.isEmpty() ? QStringLiteral("legacy/unknown") : caps.join(QStringLiteral(", "))), false);
        return;
    }

    if (command == QStringLiteral("secureoff")) {
        Buffer *active = activeBuffer();
        if (active && active->kind == QStringLiteral("chat") && rest.trimmed().isEmpty()) {
            closeSecureRoom(entry, active);
            return;
        }
        if (!entry) {
            status(QStringLiteral("Select a connection first."));
            return;
        }
        QString target = rest.trimmed();
        if (target.isEmpty()) target = activeImTarget(entry);
        if (target.isEmpty()) {
            status(QStringLiteral("usage: /secureoff USER (or run from a PM buffer)"));
            return;
        }
        m_secure.closeSession(entry->id, target);
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target, true);
        append(buffer, QStringLiteral("[secure] secure session closed; messages are plaintext until /secure is used again."), false);
        return;
    }

    if (command == QStringLiteral("trust")) {
        if (!entry) {
            status(QStringLiteral("Select a connection first."));
            return;
        }
        QString target = rest.trimmed();
        if (target.isEmpty()) target = activeImTarget(entry);
        if (target.isEmpty()) {
            status(QStringLiteral("usage: /trust USER (or run from a PM buffer)"));
            return;
        }
        const QString peer = m_secure.peerFingerprint(entry->id, target);
        if (peer.isEmpty()) {
            status(QStringLiteral("No secure peer fingerprint for %1; use /secure first.").arg(target));
            return;
        }
        setTrustedFingerprint(entry, target, peer);
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target, true);
        append(buffer, QStringLiteral("[secure] trusted peer fingerprint %1").arg(peer), false);
        return;
    }

    if (command == QStringLiteral("untrust")) {
        if (!entry) {
            status(QStringLiteral("Select a connection first."));
            return;
        }
        QString target = rest.trimmed();
        if (target.isEmpty()) target = activeImTarget(entry);
        if (target.isEmpty()) {
            status(QStringLiteral("usage: /untrust USER (or run from a PM buffer)"));
            return;
        }
        clearTrustedFingerprint(entry, target);
        status(QStringLiteral("Cleared trusted fingerprint for %1.").arg(target));
        return;
    }

    if (command == QStringLiteral("sendfile")) {
        if (!entry || !entry->connected || !entry->backend) {
            status(QStringLiteral("Select an online AIM or IRC connection first."));
            return;
        }
        if (entry->settings.protocol != ConnectionSettings::Protocol::Oscar
            && entry->settings.protocol != ConnectionSettings::Protocol::Irc) {
            status(QStringLiteral("/sendfile is available on AIM and IRC private-message connections."));
            return;
        }

        QString target = activeImTarget(entry);
        QString path;
        bool secureTransfer = true;
        // Legacy arguments are accepted only as initial values; 3.0r1 always
        // opens the guided transfer form so mode and path are visible before send.
        QString legacy = rest;
        if (!legacy.trimmed().isEmpty()) {
            if (target.isEmpty()) target = takeArgument(legacy);
            path = takeArgument(legacy);
        }
        if (!promptFileTransfer(entry, target, path, secureTransfer)) return;

        Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target, true);
        if (secureTransfer) {
            if (!m_secureReady || !m_secure.hasSession(entry->id, target)) {
                messageBox(QStringLiteral("Secure File Transfer — Setup Required"), {
                    QStringLiteral("Secure transfer needs a CPX secure DM with %1.").arg(target),
                    QStringLiteral("1. Open the PM: /query %1").arg(target),
                    QStringLiteral("2. Start CPX: /secure %1").arg(target),
                    QStringLiteral("3. Compare /fingerprint and /secure-status fingerprints."),
                    QStringLiteral("4. Trust the verified peer, then run /sendfile again."),
                    QStringLiteral("Nothing was sent.")});
                return;
            }
            if (!m_secure.peerSupports(entry->id, target, QStringLiteral("file-transfer"))) {
                status(QStringLiteral("Peer %1 does not advertise CPX file-transfer support.").arg(target));
                return;
            }
        }

        QString transferId, offer, error;
        const bool reliableTransfer = secureTransfer
            ? m_secure.peerSupports(entry->id, target, QStringLiteral("file-ack"))
            : true;
        const bool directPreferred = secureTransfer && reliableTransfer && m_secure.peerSupports(
            entry->id, target, QStringLiteral("file-direct-v1"));
        if (!m_fileTransfers.createOffer(target, path, transferId, offer, &error,
                                         reliableTransfer)) {
            status(QStringLiteral("[file] %1").arg(error));
            return;
        }
        m_fileTransferProfiles.insert(transferId, entry->id);
        m_fileTransferSecure.insert(transferId, secureTransfer);
        if (!sendSecureControlPayload(entry, target, offer, buffer)) {
            m_fileTransfers.cancel(transferId, QStringLiteral("file transport failed"));
            return;
        }
        const auto info = m_fileTransfers.transfer(transferId);
        append(buffer, QStringLiteral("[file] offered %1 (%2 bytes) to %3 [%4] %5")
                           .arg(info.fileName).arg(info.total).arg(target).arg(transferId)
                           .arg(secureTransfer
                                ? (directPreferred ? QStringLiteral("[SECURE CPX / direct preferred]")
                                                   : QStringLiteral("[SECURE CPX relay]"))
                                : QStringLiteral("[UNSECURED AIM/IRC relay / SHA-256 verified]")), false);
        return;
    }

    if (command == QStringLiteral("transfers")) {
        const auto transfers = m_fileTransfers.transfers();
        if (transfers.isEmpty()) {
            status(QStringLiteral("No file transfers in this session."));
            return;
        }
        Buffer *buffer = activeBuffer();
        if (!buffer) buffer = ensureBuffer(QStringLiteral("global"));
        append(buffer, QStringLiteral("[file] transfers:"), false);
        for (const auto &info : transfers) {
            const QString mode = m_fileTransferSecure.value(info.id, true)
                ? QStringLiteral("secure") : QStringLiteral("unsecured");
            append(buffer,
                   QStringLiteral("  %1 %2 %3 [%4] %5/%6 bytes — %7")
                       .arg(info.id,
                            info.outgoing ? QStringLiteral("->") : QStringLiteral("<-"),
                            info.fileName,
                            mode)
                       .arg(info.transferred).arg(info.total).arg(info.status), false);
        }
        return;
    }

    if (command == QStringLiteral("accept")) {
        QString args = rest;
        const QString id = takeArgument(args);
        if (id.isEmpty()) {
            status(QStringLiteral("usage: /accept TRANSFER_ID [PATH]"));
            return;
        }
        const auto info = m_fileTransfers.transfer(id);
        if (info.id.isEmpty() || info.outgoing) {
            status(QStringLiteral("Unknown incoming transfer ID %1.").arg(id));
            return;
        }
        const QString profileId = m_fileTransferProfiles.value(id);
        ConnectionEntry *owner = connectionById(profileId);
        if (!owner || !owner->connected) {
            status(QStringLiteral("The connection for transfer %1 is not online.").arg(id));
            return;
        }
        QString path = takeArgument(args);
        if (path.isEmpty()) {
            QString dir;
#ifdef WAFFLEHOUSE_TERMUX
            const QString sharedDownloads = QDir::home().filePath(QStringLiteral("storage/downloads"));
            if (QFileInfo(sharedDownloads).isDir()) dir = sharedDownloads;
#endif
            if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            if (dir.isEmpty()) dir = QDir::homePath();
            path = QDir(dir).filePath(info.fileName);
        }
        QString error;
        QString payload = m_fileTransfers.acceptIncoming(id, path, &error);
        if (payload.isEmpty()) {
            status(QStringLiteral("[file] %1").arg(error));
            return;
        }
        const QString relayPayload = payload;
        bool directReady = false;
        const bool secureTransfer = m_fileTransferSecure.value(id, true);
        if (secureTransfer
            && m_secure.peerSupports(owner->id, info.target, QStringLiteral("file-direct-v1"))
            && m_secure.peerSupports(owner->id, info.target, QStringLiteral("file-ack"))) {
            QString keyError;
            const QByteArray transferKey = m_secure.fileTransferKey(owner->id, info.target, id, &keyError);
            const auto acceptedInfo = m_fileTransfers.transfer(id);
            CpxDirectTransferManager::ListenResult listener;
            if (!transferKey.isEmpty()
                && m_directTransfers.prepareIncoming(
                    id, acceptedInfo.path + QStringLiteral(".cpxpart"), acceptedInfo.total,
                    acceptedInfo.transferred, transferKey, listener, &error)) {
                const QString directPayload = m_fileTransfers.acceptIncoming(
                    id, path, &error, listener.port, listener.hosts);
                directReady = !directPayload.isEmpty();
                if (directReady) {
                    payload = directPayload;
                } else {
                    payload = relayPayload;
                    m_directTransfers.cancel(id);
                }
            } else if (!keyError.isEmpty()) {
                error = keyError;
            }
        }
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), owner->id, info.target, info.target, true);
        bool sent = sendSecureControlPayload(owner, info.target, payload, buffer);
        if (!sent && directReady) {
            m_directTransfers.cancel(id);
            directReady = false;
            payload = m_fileTransfers.fallbackIncomingToRelay(id);
            sent = !payload.isEmpty() && sendSecureControlPayload(owner, info.target, payload, buffer);
        }
        if (sent) {
            append(buffer,
                   directReady
                       ? QStringLiteral("[file] accepting %1 -> %2 [%3] [encrypted direct transport]")
                             .arg(info.fileName, path, id)
                       : QStringLiteral("[file] accepting %1 -> %2 [%3] %4")
                             .arg(info.fileName, path, id,
                                  secureTransfer ? QStringLiteral("[secure relay]")
                                                 : QStringLiteral("[UNSECURED relay]")),
                   false);
        }
        return;
    }

    if (command == QStringLiteral("decline")) {
        QString args = rest;
        const QString id = takeArgument(args);
        if (id.isEmpty()) { status(QStringLiteral("usage: /decline TRANSFER_ID [reason]")); return; }
        const auto info = m_fileTransfers.transfer(id);
        ConnectionEntry *owner = connectionById(m_fileTransferProfiles.value(id));
        if (info.id.isEmpty() || info.outgoing || !owner) { status(QStringLiteral("Unknown incoming transfer ID %1.").arg(id)); return; }
        const QString reason = args.trimmed().isEmpty() ? QStringLiteral("declined") : args.trimmed();
        const QString payload = m_fileTransfers.declineIncoming(id, reason);
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), owner->id, info.target, info.target, true);
        sendSecureControlPayload(owner, info.target, payload, buffer);
        append(buffer, QStringLiteral("[file] declined %1 [%2]").arg(info.fileName, id), false);
        return;
    }

    if (command == QStringLiteral("canceltransfer")) {
        QString args = rest;
        const QString id = takeArgument(args);
        if (id.isEmpty()) { status(QStringLiteral("usage: /canceltransfer TRANSFER_ID [reason]")); return; }
        const auto info = m_fileTransfers.transfer(id);
        ConnectionEntry *owner = connectionById(m_fileTransferProfiles.value(id));
        if (info.id.isEmpty() || !owner) { status(QStringLiteral("Unknown transfer ID %1.").arg(id)); return; }
        const QString reason = args.trimmed().isEmpty() ? QStringLiteral("cancelled by user") : args.trimmed();
        m_directTransfers.cancel(id);
        const QString payload = m_fileTransfers.cancel(id, reason);
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), owner->id, info.target, info.target, true);
        sendSecureControlPayload(owner, info.target, payload, buffer);
        append(buffer, QStringLiteral("[file] cancelled transfer %1 [%2]").arg(info.fileName, id), false);
        return;
    }

    if (command == QStringLiteral("resume")) {
        QString args = rest;
        const QString id = takeArgument(args);
        if (id.isEmpty()) { status(QStringLiteral("usage: /resume TRANSFER_ID")); return; }
        const auto info = m_fileTransfers.transfer(id);
        ConnectionEntry *owner = connectionById(m_fileTransferProfiles.value(id));
        if (info.id.isEmpty() || !owner || !m_fileTransfers.canResume(id)) {
            status(QStringLiteral("Transfer %1 is not resumable.").arg(id));
            return;
        }
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), owner->id, info.target, info.target, true);
        if (!info.outgoing) {
            resumeIncomingFileTransfer(id, owner, buffer);
            return;
        }
        const bool secureTransfer = m_fileTransferSecure.value(id, true);
        if (secureTransfer && !m_secure.hasSession(owner->id, info.target)) {
            append(buffer, QStringLiteral("[file] re-establish the secure session before resuming %1").arg(id), false);
            return;
        }
        QString error;
        const QString payload = m_fileTransfers.requestResume(id, &error);
        if (payload.isEmpty() || !sendSecureControlPayload(owner, info.target, payload, buffer)) {
            m_fileTransfers.cancel(id, QStringLiteral("resume request could not be sent"));
            append(buffer, QStringLiteral("[file] resume request failed: %1").arg(error), false);
            return;
        }
        append(buffer, QStringLiteral("[file] requested resume of %1 [%2]").arg(info.fileName, id), false);
        return;
    }

    if (command == QStringLiteral("cleartransfer")) {
        QString args = rest;
        const QString id = takeArgument(args);
        if (id.isEmpty()) { status(QStringLiteral("usage: /cleartransfer TRANSFER_ID")); return; }
        const auto info = m_fileTransfers.transfer(id);
        if (info.id.isEmpty()) { status(QStringLiteral("Unknown transfer ID %1.").arg(id)); return; }
        if (!info.complete) { status(QStringLiteral("Cancel transfer %1 before clearing it.").arg(id)); return; }
        m_directTransfers.cancel(id);
        QString error;
        if (!m_fileTransfers.clearTransfer(id, &error)) {
            status(QStringLiteral("[file] %1").arg(error));
            return;
        }
        m_fileTransferProfiles.remove(id);
        m_fileTransferSecure.remove(id);
        m_fileTransferProgressShown.remove(id);
        status(QStringLiteral("Cleared transfer %1.").arg(id));
        return;
    }

    if (command == QStringLiteral("query")) {
        if (!entry || !entry->connected) {
            status(QStringLiteral("Select an online connection first."));
            return;
        }
        if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
            status(QStringLiteral("Telnet uses its session buffer; /query is not applicable."));
            return;
        }
        const QString target = rest.trimmed();
        if (target.isEmpty()) {
            status(QStringLiteral("usage: /query USER"));
            return;
        }
        ensureBuffer(QStringLiteral("im"), entry->id, target, target, true);
        return;
    }

    if (command == QStringLiteral("msg")) {
        if (!entry || !entry->connected) {
            status(QStringLiteral("Select an online connection first."));
            return;
        }
        if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
            status(QStringLiteral("Telnet uses its session buffer; type normally there."));
            return;
        }
        QString args = rest;
        const QString target = takeArgument(args);
        if (target.isEmpty() || args.isEmpty()) {
            status(QStringLiteral("usage: /msg USER MESSAGE (quote USER if it contains spaces)"));
            return;
        }
        Buffer *buffer = ensureBuffer(QStringLiteral("im"), entry->id, target, target, true);
        sendPrivateText(entry, target, args, buffer);
        return;
    }

    if (command == QStringLiteral("join") || command == QStringLiteral("j")) {
        if (!entry || !entry->connected) {
            status(QStringLiteral("Select an online connection first."));
            return;
        }
        if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
            status(QStringLiteral("Telnet sessions do not use /join."));
            return;
        }
        QString room = rest.trimmed();
        if (room.isEmpty()) {
            status(QStringLiteral("usage: /join ROOM"));
            return;
        }
        if (entry->settings.protocol == ConnectionSettings::Protocol::Irc
            && !QStringLiteral("#&+!").contains(room.front())) {
            room.prepend(QLatin1Char('#'));
        }
        m_closedChatBuffers.remove(
            bufferKey(QStringLiteral("chat"), entry->id, room));
        Buffer *buffer = ensureBuffer(QStringLiteral("chat"), entry->id, room, room, true);
        const bool privateRoom = entry->settings.protocol == ConnectionSettings::Protocol::Oscar;
        append(buffer,
               privateRoom
                   ? QStringLiteral("*** joining private room %1…").arg(room)
                   : QStringLiteral("*** joining %1…").arg(room),
               false);
        entry->backend->joinRoom(room, privateRoom);
        return;
    }

    if (command == QStringLiteral("joinprivate")) {
        if (!entry || entry->settings.protocol != ConnectionSettings::Protocol::Oscar
            || !entry->connected) {
            status(QStringLiteral("/joinprivate requires an online AIM/OSCAR connection."));
            return;
        }
        const QString room = rest.trimmed();
        if (room.isEmpty()) {
            status(QStringLiteral("usage: /joinprivate ROOM"));
            return;
        }
        m_closedChatBuffers.remove(
            bufferKey(QStringLiteral("chat"), entry->id, room));
        Buffer *buffer = ensureBuffer(QStringLiteral("chat"), entry->id, room, room, true);
        append(buffer, QStringLiteral("*** joining private room %1…").arg(room), false);
        entry->backend->joinRoom(room, true);
        return;
    }

    if (command == QStringLiteral("say")) {
        Buffer *buffer = activeBuffer();
        if (!buffer || buffer->kind != QStringLiteral("chat") || rest.isEmpty()) {
            status(QStringLiteral("usage: /say MESSAGE from an active room/channel buffer"));
            return;
        }
        ConnectionEntry *roomEntry = connectionById(buffer->connectionId);
        if (!roomEntry || !roomEntry->connected) {
            status(QStringLiteral("That connection is offline."));
            return;
        }
        if (m_secureRooms.hasRoom(roomEntry->id, buffer->target)) {
            QString error;
            const QString frame = m_secureRooms.encrypt(roomEntry->id, buffer->target, rest, &error);
            if (frame.isEmpty()) append(buffer, QStringLiteral("[error] [secure-room] %1").arg(error), false);
            else if (roomEntry->settings.protocol == ConnectionSettings::Protocol::Irc && frame.toUtf8().size() > 400)
                append(buffer, QStringLiteral("[error] [secure-room] encrypted IRC room message is too long"), false);
            else roomEntry->backend->sendRoomMessage(buffer->target, frame);
        } else {
            roomEntry->backend->sendRoomMessage(buffer->target, rest);
        }
        return;
    }

    if (command == QStringLiteral("rooms") || command == QStringLiteral("channels")) {
        listRooms(entry);
        return;
    }
    if (command == QStringLiteral("members") || command == QStringLiteral("names")) {
        listMembers(activeBuffer());
        return;
    }
    if (command == QStringLiteral("buddies") || command == QStringLiteral("buddylist")) {
        listBuddies(entry);
        return;
    }

    if (command == QStringLiteral("addbuddy")) {
        if (!entry || (entry->settings.protocol != ConnectionSettings::Protocol::Oscar
                       && entry->settings.protocol != ConnectionSettings::Protocol::Irc)
            || !entry->connected) {
            status(QStringLiteral("/addbuddy requires an online AIM/OSCAR or IRC connection."));
            return;
        }
        const QString buddy = rest.trimmed();
        if (buddy.isEmpty()) {
            status(QStringLiteral("usage: /addbuddy NAME (AIM buddy or IRC nickname watch)"));
            return;
        }
        entry->backend->addBuddy(buddy);
        return;
    }

    if (command == QStringLiteral("delbuddy") || command == QStringLiteral("removebuddy")) {
        if (!entry || (entry->settings.protocol != ConnectionSettings::Protocol::Oscar
                       && entry->settings.protocol != ConnectionSettings::Protocol::Irc)
            || !entry->connected) {
            status(QStringLiteral("/delbuddy requires an online AIM/OSCAR or IRC connection."));
            return;
        }
        const QString buddy = rest.trimmed();
        if (buddy.isEmpty()) {
            status(QStringLiteral("usage: /delbuddy NAME (AIM buddy or IRC nickname watch)"));
            return;
        }
        entry->backend->removeBuddy(buddy);
        return;
    }

    if (command == QStringLiteral("passwd")) {
        if (!entry || entry->settings.protocol != ConnectionSettings::Protocol::Oscar
            || !entry->connected) {
            status(QStringLiteral("/passwd requires an online AIM/OSCAR connection."));
            return;
        }
        bool cancelled = false;
        const QString current = prompt(QStringLiteral("Change AIM Password"),
                                       QStringLiteral("Current password"), {}, true, &cancelled);
        if (cancelled) return;
        const QString next = prompt(QStringLiteral("Change AIM Password"),
                                    QStringLiteral("New password"), {}, true, &cancelled);
        if (cancelled) return;
        const QString confirmValue = prompt(QStringLiteral("Change AIM Password"),
                                            QStringLiteral("Confirm new password"), {}, true, &cancelled);
        if (cancelled) return;
        if (next.isEmpty() || next != confirmValue) {
            messageBox(QStringLiteral("Change AIM Password"),
                       {QStringLiteral("Passwords are empty or do not match.")});
            return;
        }
        entry->backend->changePassword(current, next);
        return;
    }

    if (command == QStringLiteral("nick")) {
        if (!entry || entry->settings.protocol != ConnectionSettings::Protocol::Irc
            || !entry->connected || rest.trimmed().isEmpty()) {
            status(QStringLiteral("usage: /nick NEWNICK on an online IRC connection"));
            return;
        }
        entry->backend->changeNickname(rest.trimmed());
        return;
    }

    if (command == QStringLiteral("part") || command == QStringLiteral("leave")) {
        if (!entry || entry->settings.protocol != ConnectionSettings::Protocol::Irc
            || !entry->connected) {
            status(QStringLiteral("/part requires an online IRC connection."));
            return;
        }

        QString args = rest.trimmed();
        Buffer *current = activeBuffer();
        if (args.isEmpty()) {
            if (!current || current->kind != QStringLiteral("chat")
                || current->connectionId != entry->id) {
                status(QStringLiteral("usage: /part [#channel] (or run /part in the active IRC channel)"));
                return;
            }
            // closeActiveBuffer() already sends PART through the backend and
            // removes the channel buffer without disconnecting IRC.
            closeActiveBuffer();
            return;
        }

        QString room = takeArgument(args);
        if (!room.isEmpty() && !QStringLiteral("#&+!").contains(room.front())) {
            room.prepend(QLatin1Char('#'));
        }
        if (room.isEmpty()) {
            status(QStringLiteral("usage: /part [#channel]"));
            return;
        }

        // If the requested room is the active channel, use the normal close
        // path so the local buffer disappears as expected.
        if (current && current->kind == QStringLiteral("chat")
            && current->connectionId == entry->id
            && current->target.compare(room, Qt::CaseInsensitive) == 0
            && args.trimmed().isEmpty()) {
            closeActiveBuffer();
            return;
        }

        // Explicit channel/reason form.  Raw IRC is used here so an optional
        // PART reason is preserved.  A leading ':' is added when needed.
        QString rawLine = QStringLiteral("PART %1").arg(room);
        const QString reason = args.trimmed();
        if (!reason.isEmpty()) {
            rawLine += QLatin1Char(' ');
            rawLine += reason.startsWith(QLatin1Char(':'))
                ? reason : QStringLiteral(":%1").arg(reason);
        }
        entry->backend->sendRaw(rawLine);
        status(QStringLiteral("IRC PART sent for %1.").arg(room));
        return;
    }

    if (command == QStringLiteral("raw")) {
        if (!entry || !entry->connected) {
            status(QStringLiteral("Select an online connection first."));
            return;
        }
        if (entry->settings.protocol == ConnectionSettings::Protocol::Oscar) {
            QString args = rest;
            const QString family = takeArgument(args);
            const QString subtype = takeArgument(args);
            const QString body = takeArgument(args);
            if (family.isEmpty() || subtype.isEmpty()) {
                status(QStringLiteral("usage: /raw FAMILY SUBTYPE [HEX_BODY]"));
                return;
            }
            entry->backend->sendRaw(family, subtype, body);
        } else {
            if (rest.trimmed().isEmpty()) {
                if (entry->settings.protocol == ConnectionSettings::Protocol::Irc) {
                    status(QStringLiteral("usage: /raw IRC COMMAND..."));
                } else if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
                    status(QStringLiteral("usage: /raw TEXT"));
                } else {
                    status(QStringLiteral("usage: /raw JSON_GATEWAY_PAYLOAD"));
                }
                return;
            }

            if (entry->settings.protocol == ConnectionSettings::Protocol::Irc) {
                QString rawLine = rest.trimmed();
                QString inspect = rawLine;
                if (inspect.startsWith(QLatin1Char('/'))) {
                    inspect.remove(0, 1);
                    inspect = inspect.trimmed();
                }
                QString inspectArgs = inspect;
                const QString ircCommand = takeArgument(inspectArgs).toCaseFolded();

                // Convenience behavior for the common IRC-client spelling
                // "/raw /part": a bare PART means the active IRC channel.
                if (ircCommand == QStringLiteral("part")
                    && inspectArgs.trimmed().isEmpty()) {
                    Buffer *current = activeBuffer();
                    if (!current || current->kind != QStringLiteral("chat")
                        || current->connectionId != entry->id) {
                        status(QStringLiteral("/raw /part needs an active IRC channel, or specify one: /raw /part #channel"));
                        return;
                    }
                    closeActiveBuffer();
                    return;
                }

                // IrcBackend::sendRaw() strips one optional leading slash and
                // normalizes only the command token (e.g. /part -> PART).
                entry->backend->sendRaw(rawLine);
            } else {
                entry->backend->sendRaw(rest.trimmed());
            }
        }
        return;
    }

    if (entry && entry->backend && entry->connected
        && entry->settings.protocol == ConnectionSettings::Protocol::Irc) {
        if (auto *irc = qobject_cast<IrcBackend *>(entry->backend)) {
            Buffer *buffer = activeBuffer();
            const QString roomContext = (buffer && buffer->connectionId == entry->id
                                         && buffer->kind == QStringLiteral("chat"))
                ? buffer->target : QString();
            if (irc->handleSlashCommand(roomContext, line)) return;

            // Unknown slash-prefixed input remains ordinary IRC conversation
            // text. Preserve both CPX secure-DM and secure-room behavior.
            if (buffer && buffer->connectionId == entry->id) {
                if (buffer->kind == QStringLiteral("im")) {
                    sendPrivateText(entry, buffer->target, line, buffer);
                    return;
                }
                if (buffer->kind == QStringLiteral("chat")) {
                    if (m_secureRooms.hasRoom(entry->id, buffer->target)) {
                        QString error;
                        const QString frame = m_secureRooms.encrypt(entry->id, buffer->target, line, &error);
                        if (frame.isEmpty()) {
                            append(buffer, QStringLiteral("[error] [secure-room] %1").arg(error), false);
                        } else if (frame.toUtf8().size() > 400) {
                            append(buffer, QStringLiteral("[error] [secure-room] encrypted IRC room message is too long; split it into shorter messages"), false);
                        } else {
                            entry->backend->sendRoomMessage(buffer->target, frame);
                        }
                    } else {
                        entry->backend->sendRoomMessage(buffer->target, line);
                    }
                    return;
                }
            }
        }
    }

    status(QStringLiteral("Unknown command /%1. Use /help.").arg(command));
}

void TerminalUi::showHelp()
{
    const QStringList lines = {
        QStringLiteral("WAFFLEHOUSE-CLIENT COMMAND REFERENCE"),
        QStringLiteral(""),
        QStringLiteral("OPTIONS / APPEARANCE"),
        QStringLiteral("  Tab                          complete/cycle matching slash commands"),
        QStringLiteral("  /options                     open check-box options/theme dialog"),
        QStringLiteral("  /notifications               show notification sound settings"),
        QStringLiteral("  /notify on|off               globally enable/disable notification sounds"),
        QStringLiteral("  /sound EVENT builtin|off|PATH set a built-in/custom sound (quote paths with spaces)"),
        QStringLiteral("  /soundtest EVENT             preview irc-mention, irc-pm, aim-im, or aim-chat"),
        QStringLiteral("      Themes: System/Classic plus the full WaffleHouse + S.I.P.H.E.R. palette,"),
        QStringLiteral("              Cyberpunk, Synthwave, Dracula, Vaporwave, Blood Moon, C64, DOS,"),
        QStringLiteral("              Solarized Dark, Waffle Iron, Ghostline, Hot Dog Stand, Neon Miami"),
        QStringLiteral("  /env                         show OS / GUI-session / terminal environment"),
        QStringLiteral("  /fingerprint                 show selected connection profile's secure fingerprint"),
        QStringLiteral("  /version [USER]              query an AIM/IRC peer's client/WaffleHouse version"),
        QStringLiteral("  /autopresence                show automatic AIM Idle/Away settings"),
        QStringLiteral("  /autopresence on|off         enable/disable automatic OSCAR presence"),
        QStringLiteral("  /autopresence idle MINUTES   set automatic Idle threshold"),
        QStringLiteral("  /autopresence away MINUTES   set automatic Away threshold"),
        QStringLiteral(""),
        QStringLiteral("CONNECTIONS"),
        QStringLiteral("  /add                         add AIM/IRC/Telnet/SIP connection; saves only"),
        QStringLiteral("  /connections                 list saved profiles"),
        QStringLiteral("  /accounts                    alias of /connections"),
        QStringLiteral("  /active                      list live connections and screen numbers"),
        QStringLiteral("  /conn N|next|prev            select/reopen a connection status buffer"),
        QStringLiteral("  /connect N|PROTO:name        connect saved profile and open its status page"),
        QStringLiteral("  /disconnect [N]              disconnect selected/profile N"),
        QStringLiteral("  /edit [N]                    edit an offline saved profile"),
        QStringLiteral("  /delete [N]                  delete a saved profile"),
        QStringLiteral(""),
        QStringLiteral("WAFFLEHOUSE MEDIA / INTERNET RADIO"),
        QStringLiteral("  /media | /mstatus            show media backend and now-playing state"),
        QStringLiteral("  /mplay FILE|URL              play local audio/video or a direct URL"),
        QStringLiteral("  /mstream URL                 play SHOUTcast/Icecast/HTTP(S)/HLS stream"),
        QStringLiteral("  /mshoutcast TERMS            search directory.shoutcast.com in your browser"),
        QStringLiteral("  /menqueue FILE|URL           append media to the playback queue"),
        QStringLiteral("  /mplaylist PATH|URL          load M3U/M3U8/PLS; use /mstream for HLS .m3u8"),
        QStringLiteral("  /mpause | /mresume | /mstop  playback controls"),
        QStringLiteral("  /mnext | /mprev              move through the real mpv playlist"),
        QStringLiteral("  /mseek SECONDS               relative seek; negative values seek backward"),
        QStringLiteral("  /mvolume 0..150              set media volume"),
        QStringLiteral("  /mmute on|off|toggle         media mute"),
        QStringLiteral("  /mshuffle on|off             queue shuffle"),
        QStringLiteral("  /mrepeat off|one|all         repeat mode"),
        QStringLiteral("  /meq BAND GAIN | /meq flat  10-band EQ; bands 0..9, gain -12..12 dB"),
        QStringLiteral(""),
        QStringLiteral("SIP / VOIP SOFTPHONE"),
        QStringLiteral("  /phone                       open softphone Main view/status"),
        QStringLiteral("  /phoneprofile                show selected WaffleHouse SIP account profile"),
        QStringLiteral("  /phoneconfig                 edit selected SIP connection (same data as /edit)"),
        QStringLiteral("  /phonestart | /phonestop     register/disconnect selected SIP account"),
        QStringLiteral("  /prefix [VALUE|off]          show/change selected account's current PBX dial prefix"),
        QStringLiteral("  /dial DEST [CID]             call using current runtime prefix"),
        QStringLiteral("  /dialraw DEST [CID]          call while bypassing the runtime prefix"),
        QStringLiteral("  /dialpreview DEST            show exact SIP Request-URI before dialing"),
        QStringLiteral("  /calls                       show Active Call view"),
        QStringLiteral("  /answer ID | /reject ID      answer/reject incoming call"),
        QStringLiteral("  /hangup ID                   end call"),
        QStringLiteral("  /hold ID | /callresume ID    hold/resume call"),
        QStringLiteral("  /mute ID | /unmute ID        microphone mute control"),
        QStringLiteral("  /dtmf ID DIGITS              send DTMF"),
        QStringLiteral("  /siplog [ID]                 show SIP Log for all calls or one call"),
        QStringLiteral("  /ladder ID                   show SIP Ladder for a call"),
        QStringLiteral("  /phoneactivity               show softphone Activity view"),
        QStringLiteral("  /audio-devices               list PJSIP audio devices"),
        QStringLiteral("  /audio-use CAP PLAY          select capture/playback devices"),
        QStringLiteral("  /audio-auto on|off           enable/disable live system headset following"),
        QStringLiteral("  Multiple SIP accounts may be registered at once; /select chooses the outbound identity."),
        QStringLiteral("  Incoming calls identify/select the account they arrived on and appear in the Softphone buffer."),
        QStringLiteral(""),
        QStringLiteral("ENCRYPTED DIRECT MESSAGES + SECURE ROOMS"),
        QStringLiteral("  WaffleHouse can encrypt private messages when both users run a CPX3-compatible WaffleHouse client."),
        QStringLiteral("  Each saved connection profile has its own stable secure identity/fingerprint."),
        QStringLiteral("  Supported: AIM IMs, IRC PMs, AIM chat rooms, and IRC channels. Telnet stays plaintext."),
        QStringLiteral("  In a room/channel, /secure creates a shared XChaCha20-Poly1305 room key."),
        QStringLiteral("  Room keys are sent only to members who already have an established CPX secure PM."),
        QStringLiteral("  Public room traffic carries CPXROOM ciphertext; WaffleHouse peers show decrypted text as [secure-room]."),
        QStringLiteral("  Ordinary room traffic received while secure-room mode is active is visibly marked [plaintext]."),
        QStringLiteral(""),
        QStringLiteral("  QUICK START"),
        QStringLiteral("    1. Open a PM with the other WaffleHouse/CPX3-compatible user: /query USER"),
        QStringLiteral("    2. Start negotiation: /secure USER   (or just /secure inside that PM)"),
        QStringLiteral("    3. Check the session: /securestatus USER"),
        QStringLiteral("    4. Compare the displayed peer fingerprint with that user OUT OF BAND."),
        QStringLiteral("       Example: compare by phone/voice or another trusted channel."),
        QStringLiteral("    5. If it matches, save it: /trust USER"),
        QStringLiteral("    6. Type normally. Secure PMs are encrypted automatically and marked [secure]."),
        QStringLiteral(""),
        QStringLiteral("  SECURITY COMMANDS"),
        QStringLiteral("  /fingerprint                 show selected connection profile's fingerprint"),
        QStringLiteral("  /secure [USER]               secure a DM; with no USER in a room, start/rotate secure-room mode"),
        QStringLiteral("  /securestatus [USER]         DM status; with no USER in a room, show secure-room key/status"),
        QStringLiteral("  /trust [USER]                trust the current peer fingerprint after comparing it"),
        QStringLiteral("  /untrust [USER]              forget the saved trusted fingerprint"),
        QStringLiteral("  /secureoff [USER]            close DM; with no USER in a room, disable secure-room mode locally"),
        QStringLiteral(""),
        QStringLiteral("FILE TRANSFER"),
        QStringLiteral("  /sendfile                    open guided recipient/file/security dialog (F2 browses files)"),
        QStringLiteral("  /transfers                   list transfer IDs, security mode, and status"),
        QStringLiteral("  /accept ID [PATH]            accept/resume incoming transfer (Downloads by default)"),
        QStringLiteral("  /decline ID [reason]         decline an offered transfer"),
        QStringLiteral("  /canceltransfer ID [reason]  cancel either direction"),
        QStringLiteral("  /resume ID                   resume a cancelled/interrupted transfer"),
        QStringLiteral("  /cleartransfer ID            clear a finished/cancelled transfer"),
        QStringLiteral("  Secure mode uses CPX encryption/authentication and requires an established verified secure DM."),
        QStringLiteral("  Unsecured mode uses ordinary AIM/IRC PM transport; it is NOT CPX encrypted/authenticated."),
        QStringLiteral("  Both modes remain chunked/resumable and SHA-256 verified before finalizing."),
        QStringLiteral(""),
        QStringLiteral("  IMPORTANT"),
        QStringLiteral("  A secure session is NOT verified until you compare fingerprints and /trust the peer."),
        QStringLiteral("  If a trusted peer's key changes, WaffleHouse-CLI rejects that secure session."),
        QStringLiteral("  Non-CPX3 clients may display encoded [[CPX3:...]] control/ciphertext frames."),
        QStringLiteral("  Encryption protects DM contents, not routing metadata, timing, or message length."),
        QStringLiteral(""),
        QStringLiteral("CONVERSATIONS"),
        QStringLiteral("  /msg USER MESSAGE            private message (encrypted automatically if secure)"),
        QStringLiteral("  /query USER                  open a PM buffer"),
        QStringLiteral("AIM / OSCAR PRESENCE"),
        QStringLiteral("  /away [MESSAGE]              set classic AIM Away + optional message"),
        QStringLiteral("  /afk [MESSAGE]               custom AFK state carried as an AIM away message"),
        QStringLiteral("  /idle [SECONDS|off]          advertise OSCAR idle duration (default: 1 second)"),
        QStringLiteral("  /back                        clear Away/AFK and Idle; return Online"),
        QStringLiteral("  /status                      show your current AIM presence"),
        QStringLiteral(""),
        QStringLiteral("  /join ROOM                   IRC channel; AIM private chatroom"),
        QStringLiteral("  /j ROOM                      alias of /join"),
        QStringLiteral("  /joinprivate ROOM            AIM private exchange room"),
        QStringLiteral("  /say MESSAGE                 send to active room"),
        QStringLiteral("  /window N|next|prev|NAME     switch buffers (/buffer and /use also work)"),
        QStringLiteral("  /close                       close PM / leave room / hide connection status"),
        QStringLiteral("  /rooms                       list known/open rooms"),
        QStringLiteral("  /members                     list active-room members"),
        QStringLiteral("  /clear                       clear active buffer"),
        QStringLiteral(""),
        QStringLiteral("AIM"),
        QStringLiteral("  /buddies                     show buddy list"),
        QStringLiteral("  /addbuddy NAME               add buddy"),
        QStringLiteral("  /delbuddy NAME               remove buddy"),
        QStringLiteral("  /passwd                      change AIM password securely"),
        QStringLiteral("  /raw FAMILY SUBTYPE [HEX]    raw OSCAR SNAC"),
        QStringLiteral(""),
        QStringLiteral("IRC"),
        QStringLiteral("  /nick NEWNICK                change nickname"),
        QStringLiteral("  /part [#channel] [reason]    leave current/specified IRC channel"),
        QStringLiteral("  /op NICK... | /deop NICK...  grant/remove channel operator"),
        QStringLiteral("  /voice NICK... | /devoice   grant/remove voice"),
        QStringLiteral("  /kick [#chan] NICK [reason]  kick a channel member"),
        QStringLiteral("  /ban NICK|MASK | /unban ... set/remove +b (nick becomes NICK!*@*)"),
        QStringLiteral("  /topic [#chan] [topic]       query/set topic"),
        QStringLiteral("  /mode [#chan] [modes args]   query/set channel modes"),
        QStringLiteral("  /me ACTION                   send IRC ACTION to active channel"),
        QStringLiteral("  /notice TARGET MESSAGE       send NOTICE"),
        QStringLiteral("  /invite NICK [#channel]      invite a user"),
        QStringLiteral("  /who, /whois, /whowas, /ison, /list, /motd supported"),
        QStringLiteral("  /raw or /quote COMMAND...    raw IRC line; optional leading / is accepted"),
        QStringLiteral("  Unknown /text in IRC chat is sent literally as a normal message."),
        QStringLiteral(""),
        QStringLiteral(""),
        QStringLiteral("TELNET / MUD / BBS"),
        QStringLiteral("  /telnet HOST [PORT]          quick-connect ANSI/BBS session; not saved"),
        QStringLiteral("  /telnet HOST:PORT            same as above"),
        QStringLiteral("  /bbsimport FILE              import many BBS entries from CSV/TSV/JSON/text"),
        QStringLiteral("  Active BBS buffers use raw-key mode; Ctrl-N/P leaves the BBS buffer."),
        QStringLiteral("  Disconnected BBS screens stay visible until you /close them."),
        QStringLiteral("  /raw TEXT                    send a literal line to the Telnet session"),
        QStringLiteral(""),
        QStringLiteral("KEYBOARD"),
        QStringLiteral("  Ctrl-N / Ctrl-P              next / previous buffer"),
        QStringLiteral("  F1-F9 or Alt-1..9            jump to buffers 1-9"),
        QStringLiteral("  PageUp / PageDown            scroll current buffer"),
        QStringLiteral("  Up / Down                    input history"),
        QStringLiteral("  Ctrl-L                       redraw"),
        QStringLiteral("  Ctrl-C                       quit"),
        QStringLiteral(""),
        QStringLiteral("Help popup: Up/Down, PgUp/PgDn, Home/End scroll; Esc/q closes."),
    };
    scrollablePopup(QStringLiteral("WaffleHouse-Client Help"), lines);
}


void TerminalUi::listConnections()
{
    Buffer *buffer = ensureBuffer(QStringLiteral("global"));
    append(buffer, QStringLiteral("Saved connections/accounts:"), false);
    int shown = 0;
    for (int i = 0; i < m_connections.size(); ++i) {
        ConnectionEntry *entry = m_connections.at(i);
        if (!entry || !entry->persistent) continue;
        ++shown;
        const QString state = entry->connecting
            ? QStringLiteral("connecting")
            : entry->connected ? QStringLiteral("online") : QStringLiteral("offline");
        append(buffer,
               QStringLiteral("  %1) %2 %3  %4:%5  [%6]")
                   .arg(shown)
                   .arg(protocolName(entry->settings.protocol), -10)
                   .arg(entry->settings.username)
                   .arg(entry->settings.server)
                   .arg(entry->settings.port)
                   .arg(state),
               false);
    }
    if (!shown) append(buffer, QStringLiteral("  (none)"), false);
    switchToBuffer(buffer);
}

void TerminalUi::listActiveConnections()
{
    Buffer *buffer = ensureBuffer(QStringLiteral("global"));
    append(buffer, QStringLiteral("Active connections:"), false);
    int shown = 0;
    for (ConnectionEntry *entry : m_connections) {
        if (!entry || (!entry->connected && !entry->connecting)) continue;
        ++shown;

        Buffer *screen = nullptr;
        if (entry->settings.protocol == ConnectionSettings::Protocol::Telnet) {
            screen = findBuffer(bufferKey(QStringLiteral("terminal"), entry->id, entry->settings.server));
        }
        if (!screen) {
            screen = findBuffer(bufferKey(QStringLiteral("connection"), entry->id, {}));
        }

        const QString state = entry->connecting ? QStringLiteral("connecting") : QStringLiteral("online");
        const QString screenText = screen ? QString::number(screen->number) : QStringLiteral("-");
        QString label = connectionLabel(entry);
        if (!entry->persistent) label += QStringLiteral(" [quick]");
        append(buffer,
               QStringLiteral("  screen %1  %2  %3:%4  [%5]")
                   .arg(screenText, -3)
                   .arg(label, -24)
                   .arg(entry->settings.server)
                   .arg(entry->settings.port)
                   .arg(state),
               false);
    }
    if (!shown) append(buffer, QStringLiteral("  (none)"), false);
    switchToBuffer(buffer);
}

void TerminalUi::listBuddies(ConnectionEntry *entry)
{
    if (!entry || (entry->settings.protocol != ConnectionSettings::Protocol::Oscar
                   && entry->settings.protocol != ConnectionSettings::Protocol::Irc)) {
        status(QStringLiteral("Select an AIM/OSCAR or IRC connection first."));
        return;
    }
    m_hiddenConnectionBuffers.remove(entry->id);
    Buffer *buffer = ensureBuffer(QStringLiteral("connection"), entry->id, {},
                                  connectionLabel(entry));
    QStringList names = entry->buddies.values();
    std::sort(names.begin(), names.end(), [entry](const QString &a, const QString &b) {
        const bool ao = isOnlineBuddy(entry->onlineBuddies, a);
        const bool bo = isOnlineBuddy(entry->onlineBuddies, b);
        if (ao != bo) return ao;
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    append(buffer, entry->settings.protocol == ConnectionSettings::Protocol::Irc
        ? QStringLiteral("IRC local buddy/watch list (%1):").arg(names.size())
        : QStringLiteral("AIM buddy list (%1):").arg(names.size()), false);
    for (const QString &name : names) {
        append(buffer,
               QStringLiteral("  %1 %2")
                   .arg(isOnlineBuddy(entry->onlineBuddies, name) ? QStringLiteral("+")
                                                                 : QStringLiteral("-"),
                        name),
               false);
    }
    switchToBuffer(buffer);
}

void TerminalUi::listRooms(ConnectionEntry *entry)
{
    if (!entry) {
        status(QStringLiteral("Select a connection first."));
        return;
    }
    m_hiddenConnectionBuffers.remove(entry->id);
    Buffer *buffer = ensureBuffer(QStringLiteral("connection"), entry->id, {},
                                  connectionLabel(entry));
    QSet<QString> rooms;
    for (Buffer *candidate : m_buffers) {
        if (candidate->connectionId == entry->id && candidate->kind == QStringLiteral("chat")) {
            rooms.insert(candidate->name);
        }
    }
    for (const QString &room : entry->discoveredRooms) {
        rooms.insert(room);
    }
    QStringList names = rooms.values();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    append(buffer, QStringLiteral("Rooms/channels known to this connection (%1):")
                       .arg(names.size()), false);
    for (const QString &name : names) {
        append(buffer, QStringLiteral("  %1").arg(name), false);
    }
    switchToBuffer(buffer);
}

void TerminalUi::listMembers(Buffer *buffer)
{
    if (!buffer || buffer->kind != QStringLiteral("chat")) {
        status(QStringLiteral("/members requires an active room/channel buffer."));
        return;
    }
    QStringList members = buffer->members.values();
    std::sort(members.begin(), members.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    append(buffer,
           QStringLiteral("*** Members (%1): %2")
               .arg(members.size())
               .arg(members.join(QStringLiteral(", "))),
           false);
}

QString TerminalUi::prompt(const QString &title,
                           const QString &label,
                           const QString &initial,
                           bool secret,
                           bool *cancelled)
{
    if (cancelled) {
        *cancelled = false;
    }

    QString value = initial;
    int cursor = value.size();
    wtimeout(stdscr, 50);

    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        const int boxWidth = fitDialogWidth(COLS - 8, 36, 72);
        const int boxHeight = 7;
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);

        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) {
            wtimeout(stdscr, 0);
            return {};
        }
        keypad(box, TRUE);
        meta(box, TRUE);
        wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);

        const QByteArray titleBytes = QStringLiteral(" %1 ").arg(title).toUtf8();
        mvwaddnstr(box, 0, 2, titleBytes.constData(), boxWidth - 4);

        const QByteArray labelBytes = label.toUtf8();
        mvwaddnstr(box, 2, 2, labelBytes.constData(), boxWidth - 4);

        QString shown = secret ? QString(value.size(), QLatin1Char('*')) : value;
        const int inputWidth = std::max(8, boxWidth - 6);
        int viewStart = std::max(0, cursor - inputWidth + 1);
        shown = shown.mid(viewStart, inputWidth);

        wattron(box, A_REVERSE);
        const QByteArray shownBytes = shown.leftJustified(inputWidth).toUtf8();
        mvwaddnstr(box, 3, 2, shownBytes.constData(), inputWidth);
        wattroff(box, A_REVERSE);

        const QByteArray hint = QByteArray(" Enter=OK   Esc=Cancel");
        mvwaddnstr(box, 5, 2, hint.constData(), boxWidth - 4);

        const int cursorX = std::clamp(2 + cursor - viewStart, 2, boxWidth - 3);
        wmove(box, 3, cursorX);
        wrefresh(box);

        wint_t ch = 0;
        const int result = get_wch(&ch);
        delwin(box);

        if (result == ERR) {
            continue;
        }

        if (result == KEY_CODE_YES) {
            switch (static_cast<int>(ch)) {
            case KEY_ENTER:
                wtimeout(stdscr, 0);
                clearok(stdscr, TRUE);
                return value;
            case KEY_EXIT:
                if (cancelled) {
                    *cancelled = true;
                }
                wtimeout(stdscr, 0);
                clearok(stdscr, TRUE);
                return {};
            case KEY_LEFT:
                cursor = std::max(0, cursor - 1);
                break;
            case KEY_RIGHT:
                cursor = std::min(static_cast<int>(value.size()), cursor + 1);
                break;
            case KEY_HOME:
                cursor = 0;
                break;
            case KEY_END:
                cursor = value.size();
                break;
            case KEY_BACKSPACE:
                if (cursor > 0) {
                    value.remove(cursor - 1, 1);
                    --cursor;
                }
                break;
            case KEY_DC:
                if (cursor < value.size()) {
                    value.remove(cursor, 1);
                }
                break;
            default:
                break;
            }
            continue;
        }

        const uint cp = static_cast<uint>(ch);
        if (cp == 27) {
            if (cancelled) {
                *cancelled = true;
            }
            wtimeout(stdscr, 0);
            clearok(stdscr, TRUE);
            return {};
        }
        if (cp == 10 || cp == 13) {
            wtimeout(stdscr, 0);
            clearok(stdscr, TRUE);
            return value;
        }
        if (isTerminalBackspace(cp)) {
            if (cursor > 0) {
                value.remove(cursor - 1, 1);
                --cursor;
            }
            continue;
        }
        if (cp >= 32) {
            const char32_t cp32 = static_cast<char32_t>(cp);
            const QString piece = QString::fromUcs4(&cp32, 1);
            value.insert(cursor, piece);
            cursor += piece.size();
        }
    }
}

bool TerminalUi::confirm(const QString &title,
                         const QString &question,
                         bool defaultYes)
{
    bool cancelled = false;
    const QString suffix = defaultYes ? QStringLiteral(" [Y/n]") : QStringLiteral(" [y/N]");
    const QString result = prompt(title, question + suffix, {}, false, &cancelled);
    if (cancelled) {
        return false;
    }
    return parseYesNo(result, defaultYes);
}

void TerminalUi::messageBox(const QString &title, const QStringList &lines)
{
    wtimeout(stdscr, 50);
    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        int contentWidth = title.size() + 6;
        for (const QString &line : lines) {
            contentWidth = std::max(contentWidth, static_cast<int>(line.size()) + 4);
        }
        const int boxWidth = fitDialogWidth(contentWidth, 36, std::max(36, COLS - 6));
        const int boxHeight = fitDialogHeight(static_cast<int>(lines.size()) + 5, 6, std::max(6, LINES - 4));
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);

        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) {
            break;
        }
        keypad(box, TRUE);
        meta(box, TRUE);
        wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);
        const QByteArray titleBytes = QStringLiteral(" %1 ").arg(title).toUtf8();
        mvwaddnstr(box, 0, 2, titleBytes.constData(), boxWidth - 4);

        int row = 2;
        for (const QString &line : lines) {
            if (row >= boxHeight - 2) break;
            const QByteArray bytes = line.toUtf8();
            mvwaddnstr(box, row++, 2, bytes.constData(), boxWidth - 4);
        }
        const QByteArray hint = QByteArray("Press Enter or Esc");
        mvwaddnstr(box, boxHeight - 2, 2, hint.constData(), boxWidth - 4);
        wrefresh(box);

        wint_t ch = 0;
        const int result = get_wch(&ch);
        delwin(box);
        if (result == ERR) {
            continue;
        }
        if (result == KEY_CODE_YES
            && (static_cast<int>(ch) == KEY_ENTER || static_cast<int>(ch) == KEY_EXIT)) {
            break;
        }
        if (result != KEY_CODE_YES && (ch == 10 || ch == 13 || ch == 27)) {
            break;
        }
    }
    wtimeout(stdscr, 0);
    clearok(stdscr, TRUE);
}

void TerminalUi::scrollablePopup(const QString &title, const QStringList &sourceLines)
{
    QStringList lines;
    const int desiredWidth = fitDialogWidth(COLS - 8, 44, 88);
    const int textWidth = std::max(20, desiredWidth - 6);
    for (const QString &line : sourceLines) {
        lines.append(wrapText(line, textWidth));
    }

    int offset = 0;
    wtimeout(stdscr, 50);
    curs_set(0);

    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        const int boxWidth = fitDialogWidth(COLS - 6, 44, 92);
        const int boxHeight = fitDialogHeight(LINES - 4, 10, 28);
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);
        const int visible = std::max(1, boxHeight - 4);
        const int maxOffset = std::max(0, static_cast<int>(lines.size()) - visible);
        offset = std::clamp(offset, 0, maxOffset);

        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) {
            break;
        }
        keypad(box, TRUE);
        meta(box, TRUE);
        wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);
        const QByteArray titleBytes = QStringLiteral(" %1 ").arg(title).toUtf8();
        mvwaddnstr(box, 0, 2, titleBytes.constData(), boxWidth - 4);

        for (int i = 0; i < visible; ++i) {
            const int index = offset + i;
            if (index >= lines.size()) {
                break;
            }
            const QByteArray bytes = lines.at(index).toUtf8();
            mvwaddnstr(box, 1 + i, 2, bytes.constData(), boxWidth - 5);
        }

        // DOS-style scrollbar.
        const int barX = boxWidth - 2;
        for (int row = 1; row <= visible; ++row) {
            mvwaddch(box, row, barX, ':');
        }
        if (lines.size() > visible) {
            const int thumbHeight = std::max(1,
                static_cast<int>((static_cast<long long>(visible) * visible) / lines.size()));
            const int maxPos = std::max(0, visible - thumbHeight);
            const int thumbPos = maxOffset > 0
                ? static_cast<int>((static_cast<long long>(offset) * maxPos) / maxOffset)
                : 0;
            for (int i = 0; i < thumbHeight && thumbPos + i < visible; ++i) {
                mvwaddch(box, 1 + thumbPos + i, barX, '#');
            }
        }

        const QByteArray hint = QByteArray("Up/Down PgUp/PgDn Home/End | Esc/q close");
        mvwaddnstr(box, boxHeight - 2, 2, hint.constData(), boxWidth - 4);
        wrefresh(box);

        wint_t ch = 0;
        const int result = wget_wch(box, &ch);
        delwin(box);
        if (result == ERR) {
            continue;
        }
        if (result == KEY_CODE_YES) {
            const int key = static_cast<int>(ch);
            if (key == KEY_EXIT || key == KEY_ENTER) {
                break;
            }
            switch (key) {
            case KEY_UP: offset = std::max(0, offset - 1); break;
            case KEY_DOWN: offset = std::min(maxOffset, offset + 1); break;
            case KEY_PPAGE: offset = std::max(0, offset - visible); break;
            case KEY_NPAGE: offset = std::min(maxOffset, offset + visible); break;
            case KEY_HOME: offset = 0; break;
            case KEY_END: offset = maxOffset; break;
            default: break;
            }
            continue;
        }
        if (ch == 27 || ch == 'q' || ch == 'Q' || ch == 10 || ch == 13) {
            break;
        }
    }

    curs_set(1);
    wtimeout(stdscr, 0);
    clearok(stdscr, TRUE);
}



QString TerminalUi::browseFile(const QString &initialPath)
{
    QFileInfo initialInfo(initialPath);
    QDir dir(initialInfo.exists() && initialInfo.isDir()
                 ? initialInfo.absoluteFilePath()
                 : (initialInfo.exists() ? initialInfo.absolutePath() : QDir::homePath()));
    int selected = 0;
    int firstVisible = 0;
    wtimeout(stdscr, 50);
    curs_set(0);

    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        const int count = entries.size() + 1; // synthetic parent entry
        selected = std::clamp(selected, 0, std::max(0, count - 1));

        const int boxWidth = fitDialogWidth(COLS - 8, 52, 96);
        const int boxHeight = fitDialogHeight(LINES - 6, 12, 28);
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);
        const int visible = std::max(4, boxHeight - 5);
        if (selected < firstVisible) firstVisible = selected;
        if (selected >= firstVisible + visible) firstVisible = selected - visible + 1;
        firstVisible = std::clamp(firstVisible, 0, std::max(0, count - visible));

        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) break;
        keypad(box, TRUE); meta(box, TRUE); wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);
        mvwaddnstr(box, 0, 2, " File Browser ", boxWidth - 4);
        const QByteArray where = QStringLiteral(" %1").arg(dir.absolutePath()).toUtf8();
        mvwaddnstr(box, 1, 2, where.constData(), boxWidth - 4);

        for (int row = 0; row < visible; ++row) {
            const int index = firstVisible + row;
            if (index >= count) break;
            QString label;
            if (index == 0) {
                label = QStringLiteral("[..]  Parent directory");
            } else {
                const QFileInfo &info = entries.at(index - 1);
                label = info.isDir()
                    ? QStringLiteral("[DIR] %1/").arg(info.fileName())
                    : QStringLiteral("      %1  (%2 bytes)").arg(info.fileName()).arg(info.size());
            }
            if (index == selected) wattron(box, A_REVERSE);
            const QByteArray bytes = label.toUtf8();
            mvwaddnstr(box, 2 + row, 2, bytes.constData(), boxWidth - 4);
            if (index == selected) wattroff(box, A_REVERSE);
        }
        const QByteArray hint("Enter=open/select | Backspace=up | Esc=cancel");
        mvwaddnstr(box, boxHeight - 2, 2, hint.constData(), boxWidth - 4);
        wrefresh(box);

        wint_t ch = 0;
        const int result = wget_wch(box, &ch);
        delwin(box);
        if (result == ERR) continue;
        const int key = static_cast<int>(ch);
        if (result == KEY_CODE_YES) {
            if (key == KEY_UP) { selected = std::max(0, selected - 1); continue; }
            if (key == KEY_DOWN) { selected = std::min(count - 1, selected + 1); continue; }
            if (key == KEY_PPAGE) { selected = std::max(0, selected - visible); continue; }
            if (key == KEY_NPAGE) { selected = std::min(count - 1, selected + visible); continue; }
            if (key == KEY_HOME) { selected = 0; continue; }
            if (key == KEY_END) { selected = count - 1; continue; }
            if (key == KEY_BACKSPACE) { dir.cdUp(); selected = firstVisible = 0; continue; }
            if (key == KEY_EXIT) break;
            if (key != KEY_ENTER) continue;
        } else {
            const uint cp = static_cast<uint>(ch);
            if (cp == 27) break;
            if (isTerminalBackspace(cp)) { dir.cdUp(); selected = firstVisible = 0; continue; }
            if (cp != 10 && cp != 13) continue;
        }
        if (selected == 0) {
            dir.cdUp(); selected = firstVisible = 0; continue;
        }
        const QFileInfo choice = entries.at(selected - 1);
        if (choice.isDir()) {
            dir.setPath(choice.absoluteFilePath()); selected = firstVisible = 0; continue;
        }
        wtimeout(stdscr, 0); curs_set(1); clearok(stdscr, TRUE);
        return choice.absoluteFilePath();
    }

    wtimeout(stdscr, 0); curs_set(1); clearok(stdscr, TRUE);
    return {};
}

bool TerminalUi::promptFileTransfer(ConnectionEntry *entry,
                                    QString &target,
                                    QString &path,
                                    bool &secureTransfer)
{
    if (!entry) return false;
    int active = 0; // 0 recipient, 1 file, 2 secure toggle
    int cursor = target.size();
    wtimeout(stdscr, 50);
    curs_set(1);

    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const int boxWidth = fitDialogWidth(COLS - 8, 58, 92);
        const int boxHeight = 13;
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);
        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) { wtimeout(stdscr, 0); return false; }
        keypad(box, TRUE); meta(box, TRUE); wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);
        mvwaddnstr(box, 0, 2, " Send File ", boxWidth - 4);
        const QByteArray subtitle("AIM / IRC guided transfer — F2 opens file browser");
        mvwaddnstr(box, 1, 2, subtitle.constData(), boxWidth - 4);

        const QString labels[] = {QStringLiteral("Recipient"), QStringLiteral("File"), QStringLiteral("Secure transfer")};
        QString values[] = {target, path, secureTransfer ? QStringLiteral("[x] yes") : QStringLiteral("[ ] no")};
        for (int i = 0; i < 3; ++i) {
            const int y = 3 + i;
            const bool selected = i == active;
            QString label = (selected ? QStringLiteral("> ") : QStringLiteral("  ")) + labels[i];
            const QByteArray lb = label.leftJustified(19).toUtf8();
            if (selected) wattron(box, A_BOLD);
            mvwaddnstr(box, y, 2, lb.constData(), 19);
            if (selected) wattroff(box, A_BOLD);
            const int valueX = 22;
            const int valueWidth = std::max(12, boxWidth - valueX - 3);
            QString shown = values[i];
            int localCursor = (i == 0 ? target.size() : i == 1 ? path.size() : 0);
            if (selected && i < 2) localCursor = cursor;
            const int viewStart = i < 2 ? std::max(0, localCursor - valueWidth + 1) : 0;
            shown = shown.mid(viewStart, valueWidth).leftJustified(valueWidth);
            if (selected) wattron(box, A_REVERSE);
            const QByteArray vb = shown.toUtf8();
            mvwaddnstr(box, y, valueX, vb.constData(), valueWidth);
            if (selected) wattroff(box, A_REVERSE);
            if (selected && i < 2) wmove(box, y, std::clamp(valueX + cursor - viewStart, valueX, valueX + valueWidth - 1));
        }

        const QString securityNote = secureTransfer
            ? QStringLiteral("SECURE: requires /secure with the peer + fingerprint verification; CPX encrypts/authenticates the transfer.")
            : QStringLiteral("UNSECURED: sends via ordinary AIM/IRC PM traffic; no CPX encryption/authentication. SHA-256 verification remains.");
        const QByteArray note = securityNote.toUtf8();
        mvwaddnstr(box, 7, 2, note.constData(), boxWidth - 4);
        const QByteArray hint("Tab/Up/Down move | Space toggles secure | F2 browse | F10/Ctrl-S send | Esc cancel");
        mvwaddnstr(box, boxHeight - 2, 2, hint.constData(), boxWidth - 4);
        wrefresh(box);

        wint_t ch = 0;
        const int result = wget_wch(box, &ch);
        delwin(box);
        if (result == ERR) continue;
        int key = static_cast<int>(ch);
        if (result == KEY_CODE_YES) {
            if (key == KEY_EXIT) { wtimeout(stdscr, 0); clearok(stdscr, TRUE); return false; }
            if (key == KEY_UP || key == KEY_BTAB) { active = (active + 2) % 3; cursor = active == 0 ? target.size() : active == 1 ? path.size() : 0; continue; }
            if (key == KEY_DOWN || key == KEY_ENTER) { active = (active + 1) % 3; cursor = active == 0 ? target.size() : active == 1 ? path.size() : 0; continue; }
            if (key == KEY_F(2)) {
                const QString chosen = browseFile(path);
                if (!chosen.isEmpty()) path = chosen;
                active = 1; cursor = path.size(); continue;
            }
            if (key == KEY_LEFT && active < 2) { cursor = std::max(0, cursor - 1); continue; }
            if (key == KEY_RIGHT && active < 2) { const int n = active == 0 ? target.size() : path.size(); cursor = std::min(n, cursor + 1); continue; }
            if (key == KEY_HOME && active < 2) { cursor = 0; continue; }
            if (key == KEY_END && active < 2) { cursor = active == 0 ? target.size() : path.size(); continue; }
            if (key == KEY_BACKSPACE && active < 2 && cursor > 0) { QString &v = active == 0 ? target : path; v.remove(cursor - 1, 1); --cursor; continue; }
            if (key == KEY_DC && active < 2) { QString &v = active == 0 ? target : path; if (cursor < v.size()) v.remove(cursor, 1); continue; }
            if (key != KEY_F(10)) continue;
        } else {
            const uint cp = static_cast<uint>(ch);
            if (cp == 27) { wtimeout(stdscr, 0); clearok(stdscr, TRUE); return false; }
            if (cp == 19) key = KEY_F(10); // Ctrl-S
            else if (cp == 9 || cp == 10 || cp == 13) { active = (active + 1) % 3; cursor = active == 0 ? target.size() : active == 1 ? path.size() : 0; continue; }
            else if (cp == ' ' && active == 2) { secureTransfer = !secureTransfer; continue; }
            else if (isTerminalBackspace(cp) && active < 2) { if (cursor > 0) { QString &v = active == 0 ? target : path; v.remove(cursor - 1, 1); --cursor; } continue; }
            else if (cp >= 32 && active < 2) { QString &v = active == 0 ? target : path; const char32_t cp32 = static_cast<char32_t>(cp); const QString piece = QString::fromUcs4(&cp32, 1); v.insert(cursor, piece); cursor += piece.size(); continue; }
            else continue;
        }

        if (key == KEY_F(10)) {
            target = target.trimmed();
            path = path.trimmed();
            if (target.isEmpty()) { messageBox(QStringLiteral("Send File"), {QStringLiteral("Enter the AIM screen name or IRC nickname to receive the file.")}); active = 0; cursor = target.size(); continue; }
            const QFileInfo file(path);
            if (path.isEmpty() || !file.exists() || !file.isFile()) { messageBox(QStringLiteral("Send File"), {QStringLiteral("Choose a readable file. Press F2 to browse.")}); active = 1; cursor = path.size(); continue; }
            wtimeout(stdscr, 0); clearok(stdscr, TRUE); curs_set(1);
            return true;
        }
    }
}

bool TerminalUi::promptConnectionSettings(ConnectionSettings &settings,
                                          bool &secretRequired,
                                          QString &sessionSecret,
                                          bool editing)
{
    enum class FieldType { Text, Secret, Toggle };
    struct FormField {
        QString key;
        QString label;
        QString value;
        FieldType type = FieldType::Text;
    };

    auto protocolText = [](ConnectionSettings::Protocol protocol) -> QString {
        switch (protocol) {
        case ConnectionSettings::Protocol::Oscar: return QStringLiteral("aim");
        case ConnectionSettings::Protocol::Irc: return QStringLiteral("irc");
        case ConnectionSettings::Protocol::Telnet: return QStringLiteral("telnet");
        case ConnectionSettings::Protocol::Sip: return QStringLiteral("sip");
        case ConnectionSettings::Protocol::Unknown: break;
        }
        return {};
    };

    auto parseProtocol = [](const QString &value) -> ConnectionSettings::Protocol {
        const QString p = value.trimmed().toCaseFolded();
        if (p == QStringLiteral("aim") || p == QStringLiteral("oscar")) {
            return ConnectionSettings::Protocol::Oscar;
        }
        if (p == QStringLiteral("irc")) {
            return ConnectionSettings::Protocol::Irc;
        }
        if (p == QStringLiteral("telnet") || p == QStringLiteral("tn")) {
            return ConnectionSettings::Protocol::Telnet;
        }
        if (p == QStringLiteral("sip") || p == QStringLiteral("voip") || p == QStringLiteral("phone")) {
            return ConnectionSettings::Protocol::Sip;
        }
        return ConnectionSettings::Protocol::Unknown;
    };

    QList<FormField> fields = {
        {QStringLiteral("protocol"), QStringLiteral("Protocol"),
         editing ? protocolText(settings.protocol) : QString(), FieldType::Text},
        {QStringLiteral("server"), QStringLiteral("Server"), settings.server, FieldType::Text},
        {QStringLiteral("port"), QStringLiteral("Port"),
         settings.port ? QString::number(settings.port) : QString(), FieldType::Text},
        {QStringLiteral("account"), QStringLiteral("Account / Nick / Label"),
         settings.username, FieldType::Text},
        {QStringLiteral("secret"), QStringLiteral("Password / Token"),
         QString(), FieldType::Secret},
        {QStringLiteral("savepass"), QStringLiteral("Save password"),
         settings.savePassword ? QStringLiteral("yes") : QStringLiteral("no"), FieldType::Toggle},
        {QStringLiteral("siplabel"), QStringLiteral("SIP account label"), settings.sipProfileName, FieldType::Text},
        {QStringLiteral("sipregistrar"), QStringLiteral("SIP registrar"), settings.sipRegistrar, FieldType::Text},
        {QStringLiteral("sipauth"), QStringLiteral("SIP auth username"), settings.sipAuthUsername, FieldType::Text},
        {QStringLiteral("sipdisplay"), QStringLiteral("SIP display name"), settings.sipDisplayName, FieldType::Text},
        {QStringLiteral("sipproxy"), QStringLiteral("SIP outbound proxy"), settings.sipOutboundProxy, FieldType::Text},
        {QStringLiteral("sipciddomain"), QStringLiteral("Caller-ID domain"), settings.sipCallerIdDomain, FieldType::Text},
        {QStringLiteral("sipprefix"), QStringLiteral("Dial prefix"), settings.sipDialPrefix, FieldType::Text},
        {QStringLiteral("sipstun"), QStringLiteral("STUN server"), settings.sipStunServer, FieldType::Text},
        {QStringLiteral("siptransport"), QStringLiteral("SIP transport"), settings.sipTransport.isEmpty() ? QStringLiteral("udp") : settings.sipTransport, FieldType::Text},
        {QStringLiteral("sipidentity"), QStringLiteral("Caller-ID mode"), settings.sipIdentityMode.isEmpty() ? QStringLiteral("from") : settings.sipIdentityMode, FieldType::Text},
        {QStringLiteral("sipexpires"), QStringLiteral("Registration expiry"), QString::number(settings.sipRegistrationExpires ? settings.sipRegistrationExpires : 300), FieldType::Text},
        {QStringLiteral("sipice"), QStringLiteral("SIP ICE"), settings.sipUseIce ? QStringLiteral("yes") : QStringLiteral("no"), FieldType::Toggle},
        {QStringLiteral("sipsrtp"), QStringLiteral("SIP SRTP"), settings.sipEnableSrtp ? QStringLiteral("yes") : QStringLiteral("no"), FieldType::Toggle},
        {QStringLiteral("tls"), QStringLiteral("IRC TLS"),
         settings.tls ? QStringLiteral("yes") : QStringLiteral("no"), FieldType::Toggle},
        {QStringLiteral("realname"), QStringLiteral("IRC Real name"),
         settings.realName.isEmpty() ? appDefaultRealName() : settings.realName,
         FieldType::Text},
        {QStringLiteral("ircpass"), QStringLiteral("IRC PASS required"),
         secretRequired && settings.protocol == ConnectionSettings::Protocol::Irc
             ? QStringLiteral("yes") : QStringLiteral("no"), FieldType::Toggle},
        {QStringLiteral("redirecthost"), QStringLiteral("AIM Redirect host"),
         settings.redirectHost, FieldType::Text},
        {QStringLiteral("redirectport"), QStringLiteral("AIM Redirect port"),
         QString::number(settings.redirectPort), FieldType::Text},
        {QStringLiteral("terminaltype"), QStringLiteral("Telnet terminal type"),
         settings.telnetTerminalType.isEmpty() ? QStringLiteral("ANSI")
                                               : settings.telnetTerminalType,
         FieldType::Text},
        {QStringLiteral("debug"), QStringLiteral("Protocol debug"),
         settings.debug ? QStringLiteral("yes") : QStringLiteral("no"), FieldType::Toggle},
    };

    auto field = [&fields](const QString &key) -> FormField & {
        for (FormField &item : fields) {
            if (item.key == key) return item;
        }
        throw std::runtime_error("WaffleHouse-CLI form field missing");
    };

    ConnectionSettings::Protocol currentProtocol = parseProtocol(field(QStringLiteral("protocol")).value);
    ConnectionSettings::Protocol lastProtocol = editing ? settings.protocol
                                                        : ConnectionSettings::Protocol::Unknown;

    auto enabled = [](const QString &key, ConnectionSettings::Protocol p) {
        if (key == QStringLiteral("protocol")) return true;
        if (p == ConnectionSettings::Protocol::Unknown) return false;
        if (key == QStringLiteral("server") || key == QStringLiteral("port")
            || key == QStringLiteral("account") || key == QStringLiteral("debug")) return true;
        if (key == QStringLiteral("secret") || key == QStringLiteral("savepass"))
            return p != ConnectionSettings::Protocol::Telnet;
        if (key == QStringLiteral("tls") || key == QStringLiteral("realname")
            || key == QStringLiteral("ircpass")) return p == ConnectionSettings::Protocol::Irc;
        if (key == QStringLiteral("redirecthost") || key == QStringLiteral("redirectport"))
            return p == ConnectionSettings::Protocol::Oscar;
        if (key == QStringLiteral("terminaltype")) return p == ConnectionSettings::Protocol::Telnet;
        if (key.startsWith(QStringLiteral("sip"))) return p == ConnectionSettings::Protocol::Sip;
        return false;
    };

    auto applyDefaults = [&](ConnectionSettings::Protocol p) {
        if (p == ConnectionSettings::Protocol::Unknown || p == lastProtocol) return;
        const bool changingProtocol = !editing || p != settings.protocol;
        if (changingProtocol) {
            field(QStringLiteral("server")).value.clear();
            field(QStringLiteral("port")).value = QString::number(
                p == ConnectionSettings::Protocol::Oscar ? 5190
                : p == ConnectionSettings::Protocol::Irc ? 6667
                : p == ConnectionSettings::Protocol::Sip ? 5060
                : 23);
            field(QStringLiteral("account")).value.clear();
            field(QStringLiteral("secret")).value.clear();
            field(QStringLiteral("tls")).value = QStringLiteral("no");
            field(QStringLiteral("realname")).value = appDefaultRealName();
            field(QStringLiteral("ircpass")).value = QStringLiteral("no");
            field(QStringLiteral("redirecthost")).value.clear();
            field(QStringLiteral("redirectport")).value = QStringLiteral("0");
            // Discord/MESSAGE_CONTENT was removed from this branch; no form field exists here.
            field(QStringLiteral("terminaltype")).value = QStringLiteral("ANSI");
            field(QStringLiteral("siplabel")).value.clear();
            field(QStringLiteral("sipregistrar")).value.clear();
            field(QStringLiteral("sipauth")).value.clear();
            field(QStringLiteral("sipdisplay")).value.clear();
            field(QStringLiteral("sipproxy")).value.clear();
            field(QStringLiteral("sipciddomain")).value.clear();
            field(QStringLiteral("sipprefix")).value.clear();
            field(QStringLiteral("sipstun")).value.clear();
            field(QStringLiteral("siptransport")).value = QStringLiteral("udp");
            field(QStringLiteral("sipidentity")).value = QStringLiteral("from");
            field(QStringLiteral("sipexpires")).value = QStringLiteral("300");
            field(QStringLiteral("sipice")).value = QStringLiteral("no");
            field(QStringLiteral("sipsrtp")).value = QStringLiteral("no");
            field(QStringLiteral("debug")).value = QStringLiteral("no");
        }
        lastProtocol = p;
    };

    auto dynamicLabel = [](const FormField &f, ConnectionSettings::Protocol p) -> QString {
        if (f.key == QStringLiteral("server") && p == ConnectionSettings::Protocol::Sip) return QStringLiteral("SIP domain");
        if (f.key == QStringLiteral("port") && p == ConnectionSettings::Protocol::Sip) return QStringLiteral("Local SIP port");
        if (f.key == QStringLiteral("account")) {
            if (p == ConnectionSettings::Protocol::Oscar) return QStringLiteral("AIM screen name");
            if (p == ConnectionSettings::Protocol::Irc) return QStringLiteral("IRC nickname");
            if (p == ConnectionSettings::Protocol::Telnet) return QStringLiteral("Session label");
            if (p == ConnectionSettings::Protocol::Sip) return QStringLiteral("SIP username/ext");
        }
        if (f.key == QStringLiteral("secret")) {
            if (p == ConnectionSettings::Protocol::Oscar) return QStringLiteral("AIM password");
            if (p == ConnectionSettings::Protocol::Irc) return QStringLiteral("IRC PASS password");
            if (p == ConnectionSettings::Protocol::Sip) return QStringLiteral("SIP password");
        }
        return f.label;
    };

    int active = 0;
    int cursor = fields.at(active).value.size();
    int firstVisible = 0;
    curs_set(1);
    wtimeout(stdscr, 50);

    auto nextEnabled = [&](int from, int direction) {
        int index = from;
        for (int tries = 0; tries < fields.size(); ++tries) {
            index = (index + direction + fields.size()) % fields.size();
            if (enabled(fields.at(index).key, currentProtocol)) return index;
        }
        return from;
    };

    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        currentProtocol = parseProtocol(field(QStringLiteral("protocol")).value);
        applyDefaults(currentProtocol);

        if (!enabled(fields.at(active).key, currentProtocol)) {
            active = 0;
            cursor = fields.at(active).value.size();
        }

        const int boxWidth = fitDialogWidth(COLS - 6, 58, 86);
        const int boxHeight = fitDialogHeight(LINES - 3, 14, 22);
        const int startY = std::max(0, (LINES - boxHeight) / 2);
        const int startX = std::max(0, (COLS - boxWidth) / 2);
        const int fieldRows = std::max(4, boxHeight - 5);

        if (active < firstVisible) firstVisible = active;
        if (active >= firstVisible + fieldRows) firstVisible = active - fieldRows + 1;
        firstVisible = std::clamp(firstVisible, 0,
                                  std::max(0, static_cast<int>(fields.size()) - fieldRows));

        WINDOW *box = newwin(boxHeight, boxWidth, startY, startX);
        if (!box) {
            wtimeout(stdscr, 0);
            return false;
        }
        keypad(box, TRUE);
        meta(box, TRUE);
        wtimeout(box, 50);
        wborder(box, 0, 0, 0, 0, 0, 0, 0, 0);
        const QString title = editing ? QStringLiteral(" Edit Connection ")
                                      : QStringLiteral(" Add Connection ");
        const QByteArray titleBytes = title.toUtf8();
        mvwaddnstr(box, 0, 2, titleBytes.constData(), boxWidth - 4);
        const QByteArray choices = QByteArray("Protocol: AIM | IRC | Telnet | SIP  (no default)");
        mvwaddnstr(box, 1, 2, choices.constData(), boxWidth - 4);

        const int labelWidth = std::min(24, boxWidth / 3);
        for (int row = 0; row < fieldRows; ++row) {
            const int index = firstVisible + row;
            if (index >= fields.size()) break;
            const FormField &f = fields.at(index);
            const bool isEnabled = enabled(f.key, currentProtocol);
            const bool selected = index == active;
            const int y = 2 + row;

            QString label = dynamicLabel(f, currentProtocol);
            label = label.left(labelWidth - 1).leftJustified(labelWidth - 1);
            if (selected) label.prepend(QLatin1Char('>'));
            else label.prepend(QLatin1Char(' '));
            const QByteArray labelBytes = label.toUtf8();
            if (!isEnabled) wattron(box, A_DIM);
            else if (selected) wattron(box, A_BOLD);
            mvwaddnstr(box, y, 2, labelBytes.constData(), labelWidth);
            if (!isEnabled) wattroff(box, A_DIM);
            else if (selected) wattroff(box, A_BOLD);

            const int valueX = 3 + labelWidth;
            const int valueWidth = std::max(8, boxWidth - valueX - 3);
            QString shown = f.type == FieldType::Secret
                ? QString(f.value.size(), QLatin1Char('*')) : f.value;
            if (!isEnabled) shown = QStringLiteral("--");
            const int localCursor = selected ? cursor : shown.size();
            const int viewStart = std::max(0, localCursor - valueWidth + 1);
            shown = shown.mid(viewStart, valueWidth).leftJustified(valueWidth);
            if (selected && isEnabled) wattron(box, A_REVERSE);
            else if (!isEnabled) wattron(box, A_DIM);
            const QByteArray valueBytes = shown.toUtf8();
            mvwaddnstr(box, y, valueX, valueBytes.constData(), valueWidth);
            if (selected && isEnabled) wattroff(box, A_REVERSE);
            else if (!isEnabled) wattroff(box, A_DIM);

            if (selected && isEnabled) {
                const int cx = std::clamp(valueX + cursor - viewStart,
                                          valueX, valueX + valueWidth - 1);
                wmove(box, y, cx);
            }
        }

        // Form scrollbar if terminal is short.
        if (fields.size() > fieldRows) {
            const int x = boxWidth - 2;
            for (int row = 0; row < fieldRows; ++row) mvwaddch(box, 2 + row, x, ':');
            const int maxFirst = fields.size() - fieldRows;
            const int thumb = maxFirst > 0
                ? static_cast<int>((static_cast<long long>(firstVisible) * (fieldRows - 1)) / maxFirst)
                : 0;
            mvwaddch(box, 2 + thumb, x, '#');
        }

        const QByteArray hint = QByteArray("Tab/Up/Down move | Space toggles | F10/Ctrl-S save | Esc cancel");
        mvwaddnstr(box, boxHeight - 2, 2, hint.constData(), boxWidth - 4);
        wrefresh(box);

        wint_t ch = 0;
        const int result = wget_wch(box, &ch);
        delwin(box);
        if (result == ERR) continue;

        if (result == KEY_CODE_YES) {
            const int cancelKey = static_cast<int>(ch);
            if (cancelKey == KEY_EXIT || cancelKey == 27) {
                wtimeout(stdscr, 0);
                clearok(stdscr, TRUE);
                return false;
            }
            const int key = static_cast<int>(ch);
            if (key == KEY_UP || key == KEY_BTAB) {
                active = nextEnabled(active, -1);
                cursor = fields.at(active).value.size();
                continue;
            }
            if (key == KEY_DOWN || key == KEY_ENTER) {
                active = nextEnabled(active, +1);
                cursor = fields.at(active).value.size();
                continue;
            }
            if (key == KEY_F(10)) {
                // handled below as save
            } else if (key == KEY_LEFT) {
                if (fields.at(active).type == FieldType::Toggle) {
                    fields[active].value = parseYesNo(fields.at(active).value, false)
                        ? QStringLiteral("no") : QStringLiteral("yes");
                    cursor = fields.at(active).value.size();
                } else {
                    cursor = std::max(0, cursor - 1);
                }
                continue;
            } else if (key == KEY_RIGHT) {
                if (fields.at(active).type == FieldType::Toggle) {
                    fields[active].value = parseYesNo(fields.at(active).value, false)
                        ? QStringLiteral("no") : QStringLiteral("yes");
                    cursor = fields.at(active).value.size();
                } else {
                    cursor = std::min(static_cast<int>(fields.at(active).value.size()), cursor + 1);
                }
                continue;
            } else if (key == KEY_HOME) {
                cursor = 0; continue;
            } else if (key == KEY_END) {
                cursor = fields.at(active).value.size(); continue;
            } else if (key == KEY_BACKSPACE) {
                if (cursor > 0 && fields.at(active).type != FieldType::Toggle) {
                    fields[active].value.remove(cursor - 1, 1); --cursor;
                }
                continue;
            } else if (key == KEY_DC) {
                if (cursor < fields.at(active).value.size()
                    && fields.at(active).type != FieldType::Toggle) {
                    fields[active].value.remove(cursor, 1);
                }
                continue;
            } else {
                continue;
            }
        }

        const uint cp = static_cast<uint>(ch);
        if (cp == 27) {
            wtimeout(stdscr, 0);
            clearok(stdscr, TRUE);
            return false;
        }
        // Terminals do not all report Backspace the same way. ncurses may
        // return KEY_BACKSPACE, while many xterm-compatible terminals send
        // ASCII BS (0x08) or DEL (0x7f). Treat all three as Backspace so the
        // form never inserts a literal ^? into the field.
        if (isTerminalBackspace(cp)) {
            if (cursor > 0 && fields.at(active).type != FieldType::Toggle) {
                fields[active].value.remove(cursor - 1, 1);
                --cursor;
            }
            continue;
        }
        if (cp == 9 || cp == 10 || cp == 13) {
            active = nextEnabled(active, +1);
            cursor = fields.at(active).value.size();
            continue;
        }
        if (cp == 19) { // Ctrl-S
            ch = KEY_F(10);
        }
        if (cp == ' ' && fields.at(active).type == FieldType::Toggle) {
            fields[active].value = parseYesNo(fields.at(active).value, false)
                ? QStringLiteral("no") : QStringLiteral("yes");
            cursor = fields.at(active).value.size();
            continue;
        }

        bool saveRequested = result == KEY_CODE_YES && static_cast<int>(ch) == KEY_F(10);
        if (cp == 19) saveRequested = true;
        if (!saveRequested && cp >= 32 && fields.at(active).type != FieldType::Toggle) {
            const char32_t cp32 = static_cast<char32_t>(cp);
            const QString piece = QString::fromUcs4(&cp32, 1);
            fields[active].value.insert(cursor, piece);
            cursor += piece.size();
            continue;
        }
        if (!saveRequested) continue;

        currentProtocol = parseProtocol(field(QStringLiteral("protocol")).value);
        if (currentProtocol == ConnectionSettings::Protocol::Unknown) {
            messageBox(QStringLiteral("Add Connection"),
                       {QStringLiteral("Choose a protocol: AIM, IRC, Telnet, or SIP.")});
            active = 0; cursor = fields.at(0).value.size();
            continue;
        }

        const QString server = field(QStringLiteral("server")).value.trimmed();
        const quint16 fallbackPort = currentProtocol == ConnectionSettings::Protocol::Oscar ? 5190
            : currentProtocol == ConnectionSettings::Protocol::Irc ? 6667
            : currentProtocol == ConnectionSettings::Protocol::Sip ? 5060
            : 23;
        const quint16 port = parsePort(field(QStringLiteral("port")).value, 0);
        const QString account = field(QStringLiteral("account")).value.trimmed();
        const QString secret = field(QStringLiteral("secret")).value;
        const bool ircPass = parseYesNo(field(QStringLiteral("ircpass")).value, false);

        if (server.isEmpty() || port == 0) {
            messageBox(QStringLiteral("Connection"),
                       {QStringLiteral("Server and a valid port are required."),
                        QStringLiteral("Default port for this protocol is %1.").arg(fallbackPort)});
            continue;
        }
        if ((currentProtocol == ConnectionSettings::Protocol::Oscar
             || currentProtocol == ConnectionSettings::Protocol::Irc
             || currentProtocol == ConnectionSettings::Protocol::Sip)
            && account.isEmpty()) {
            messageBox(QStringLiteral("Connection"),
                       {QStringLiteral("An AIM screen name, IRC nickname, or SIP username is required.")});
            continue;
        }
        if (!editing
            && (currentProtocol == ConnectionSettings::Protocol::Oscar
                || currentProtocol == ConnectionSettings::Protocol::Sip
                || (currentProtocol == ConnectionSettings::Protocol::Irc && ircPass))
            && secret.isEmpty()) {
            messageBox(QStringLiteral("Connection"),
                       {QStringLiteral("This connection requires a password/token before it can auto-connect.")});
            continue;
        }

        settings.protocol = currentProtocol;
        settings.server = server;
        settings.port = port;
        settings.username = account;
        settings.redirectHost = field(QStringLiteral("redirecthost")).value.trimmed();
        bool rpOk = false;
        const int rp = field(QStringLiteral("redirectport")).value.toInt(&rpOk);
        settings.redirectPort = rpOk && rp >= 0 && rp <= 65535 ? static_cast<quint16>(rp) : 0;
        settings.realName = field(QStringLiteral("realname")).value;
        settings.tls = parseYesNo(field(QStringLiteral("tls")).value, false);
        settings.telnetTerminalType = field(QStringLiteral("terminaltype")).value.trimmed();
        if (settings.telnetTerminalType.isEmpty()) {
            settings.telnetTerminalType = QStringLiteral("ANSI");
        }
        settings.sipProfileName = field(QStringLiteral("siplabel")).value.trimmed();
        settings.sipDomain = currentProtocol == ConnectionSettings::Protocol::Sip ? server : settings.sipDomain;
        settings.sipRegistrar = field(QStringLiteral("sipregistrar")).value.trimmed();
        settings.sipAuthUsername = field(QStringLiteral("sipauth")).value.trimmed();
        settings.sipDisplayName = field(QStringLiteral("sipdisplay")).value.trimmed();
        settings.sipOutboundProxy = field(QStringLiteral("sipproxy")).value.trimmed();
        settings.sipCallerIdDomain = field(QStringLiteral("sipciddomain")).value.trimmed();
        settings.sipDialPrefix = field(QStringLiteral("sipprefix")).value.trimmed();
        settings.sipStunServer = field(QStringLiteral("sipstun")).value.trimmed();
        settings.sipTransport = field(QStringLiteral("siptransport")).value.trimmed().toCaseFolded();
        settings.sipIdentityMode = field(QStringLiteral("sipidentity")).value.trimmed().toCaseFolded();
        if (currentProtocol == ConnectionSettings::Protocol::Sip) {
            try {
                (void)trunkmonkey::transportFromString(settings.sipTransport.toStdString());
            } catch (const std::exception &) {
                messageBox(QStringLiteral("SIP Account"),
                           {QStringLiteral("SIP transport must be udp, tcp, or tls.")});
                continue;
            }
            try {
                (void)trunkmonkey::identityModeFromString(settings.sipIdentityMode.toStdString());
            } catch (const std::exception &) {
                messageBox(QStringLiteral("SIP Account"),
                           {QStringLiteral("SIP identity mode must be from, pai, rpid, or from+pai.")});
                continue;
            }
        }
        bool expOk = false;
        const uint exp = field(QStringLiteral("sipexpires")).value.toUInt(&expOk);
        settings.sipRegistrationExpires = expOk && exp > 0 ? exp : 300;
        settings.sipUseIce = parseYesNo(field(QStringLiteral("sipice")).value, false);
        settings.sipEnableSrtp = parseYesNo(field(QStringLiteral("sipsrtp")).value, false);
        if (currentProtocol == ConnectionSettings::Protocol::Sip) {
            settings.sipLocalPort = port;
            if (settings.sipProfileName.isEmpty()) settings.sipProfileName = account;
        }
        settings.debug = parseYesNo(field(QStringLiteral("debug")).value, false);
        settings.savePassword = parseYesNo(
            field(QStringLiteral("savepass")).value, false);

        secretRequired = currentProtocol == ConnectionSettings::Protocol::Oscar
            || currentProtocol == ConnectionSettings::Protocol::Sip
            || (currentProtocol == ConnectionSettings::Protocol::Irc && ircPass);
        sessionSecret = secret;
        settings.password = sessionSecret;

        wtimeout(stdscr, 0);
        clearok(stdscr, TRUE);
        return true;
    }
}


void TerminalUi::importBbsList(const QString &path)
{
    QString error;
    const auto entries = BbsDirectory::loadFile(path, &error);
    if (entries.isEmpty()) {
        status(error.isEmpty() ? QStringLiteral("No BBS entries found in %1").arg(path)
                               : QStringLiteral("BBS import failed: %1").arg(error));
        return;
    }
    int added = 0, skipped = 0;
    for (const auto &bbs : entries) {
        bool duplicate = false;
        for (ConnectionEntry *existing : m_connections) {
            if (existing && existing->settings.protocol == ConnectionSettings::Protocol::Telnet
                && existing->settings.server.compare(bbs.host, Qt::CaseInsensitive) == 0
                && existing->settings.port == bbs.port) { duplicate = true; break; }
        }
        if (duplicate) { ++skipped; continue; }
        ConnectionSettings cfg;
        cfg.protocol = ConnectionSettings::Protocol::Telnet;
        cfg.server = bbs.host; cfg.port = bbs.port; cfg.username = bbs.name;
        cfg.telnetTerminalType = bbs.terminalType.isEmpty() ? QStringLiteral("ANSI") : bbs.terminalType;
        if (addConnectionEntry(cfg, false, false, true, false)) ++added;
    }
    status(QStringLiteral("BBS import complete: %1 added, %2 duplicates skipped. Imported entries are saved offline.")
               .arg(added).arg(skipped));
}

void TerminalUi::openAdHocTelnet(const QString &spec, const QString &portText)
{
    QString host = spec.trimmed();
    quint16 port = 23;
    if (!portText.trimmed().isEmpty()) port = parsePort(portText, 23);
    else {
        const int colon = host.lastIndexOf(QLatin1Char(':'));
        if (colon > 0 && !host.contains(QLatin1Char(']'))) {
            port = parsePort(host.mid(colon + 1), 23);
            host = host.left(colon);
        }
    }
    if (host.isEmpty()) { status(QStringLiteral("Invalid Telnet host.")); return; }
    ConnectionSettings cfg;
    cfg.protocol = ConnectionSettings::Protocol::Telnet;
    cfg.server = host; cfg.port = port; cfg.username = QStringLiteral("%1:%2").arg(host).arg(port);
    cfg.telnetTerminalType = QStringLiteral("ANSI");
    ConnectionEntry *entry = addConnectionEntry(cfg, false, false, false, false);
    if (!entry) { status(QStringLiteral("Could not create Telnet session.")); return; }
    // Quick-connect only: do not add this host to saved accounts, and keep
    // transient connection-status chatter in the global Status buffer.  The
    // actual BBS terminal screen appears once connected.
    m_hiddenConnectionBuffers.insert(entry->id);
    selectConnection(entry, false);
    connectConnection(entry);
}

void TerminalUi::addConnectionWizard()
{
    ConnectionSettings settings;
    settings.protocol = ConnectionSettings::Protocol::Unknown;
    settings.port = 0;
    settings.realName = appDefaultRealName();
    settings.telnetTerminalType = QStringLiteral("ANSI");

    bool secretRequired = false;
    QString secret;
    if (!promptConnectionSettings(settings, secretRequired, secret, false)) {
        status(QStringLiteral("Add connection cancelled."));
        return;
    }

    settings.password = secret;
    settings.savePassword = settings.savePassword && !settings.password.isEmpty();
    ConnectionEntry *entry = addConnectionEntry(
        settings, secretRequired, !secret.isEmpty(), true, false);
    if (entry) {
        status(QStringLiteral("Saved %1 offline. Use /connect %2 to connect it.")
                   .arg(protocolName(settings.protocol), connectionLabel(entry)));
    }
}


void TerminalUi::editConnectionWizard(ConnectionEntry *entry)
{
    if (!entry) {
        return;
    }
    if (entry->connected || entry->connecting) {
        status(QStringLiteral("Disconnect this connection before editing it."));
        return;
    }
    if (entry->settings.protocol == ConnectionSettings::Protocol::Sip && entry->backend) {
        for (const auto &call : m_sipController->calls()) {
            if (!call.disconnected
                && QString::fromStdString(call.accountId) == entry->backend->id()) {
                status(QStringLiteral("Hang up active calls on this SIP account before editing it."));
                return;
            }
        }
    }

    ConnectionSettings settings = entry->settings;
    settings.password.clear();
    bool secretRequired = entry->secretRequired;
    QString newSecret;

    if (!promptConnectionSettings(settings, secretRequired, newSecret, true)) {
        status(QStringLiteral("Edit connection cancelled."));
        return;
    }

    const bool protocolChanged = settings.protocol != entry->settings.protocol;
    const bool shouldReplaceSecret = !newSecret.isEmpty();

    if (!shouldReplaceSecret && entry->hasSessionSecret && !protocolChanged) {
        settings.password = entry->settings.password;
    } else {
        settings.password = newSecret;
    }
    settings.savePassword = settings.savePassword && !settings.password.isEmpty();

    replaceBackend(entry, settings);
    entry->secretRequired = secretRequired;
    entry->hasSessionSecret = !settings.password.isEmpty();
    saveConnections();
    connectionStatus(entry, QStringLiteral("Saved connection updated."));
}

void TerminalUi::requestQuit()
{
    if (m_quitting) {
        return;
    }
    m_quitting = true;
    m_tickTimer.stop();

    // Tell every backend to stop, but do not synchronously join worker
    // threads here.  Some Qt socket/DNS/authentication calls can remain
    // blocked long enough that backend destructors make an explicit /quit
    // appear to hang after ncurses has already disappeared.
    for (ConnectionEntry *entry : m_connections) {
        if (entry && entry->backend) {
            entry->backend->requestStop();
            QObject::disconnect(entry->backend, nullptr, this, nullptr);
        }
    }

    saveConnections();
    shutdownCurses();
    std::fflush(nullptr);

    // /quit is the final process shutdown path.  State is already saved and
    // the terminal has been restored, so bypass QObject/backend destructors
    // that could otherwise wait indefinitely on network worker threads.
    // The operating system closes remaining sockets and threads on process
    // exit, guaranteeing that control returns to the invoking shell.
    std::_Exit(EXIT_SUCCESS);
}

