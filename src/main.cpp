#include "appbranding.h"
#include "appicon.h"
#include "backend.h"
#include "mainwindow.h"
#include "mediawindow.h"
#include "platforminfo.h"
#include "terminalui.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QFont>
#include <QHash>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QPair>
#include <QPixmap>
#include <QSet>
#include <QLabel>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <cstdio>

namespace {
enum class FrontendMode { Auto, Gui, Cli };

FrontendMode requestedMode(int argc, char *argv[])
{
    FrontendMode mode = FrontendMode::Auto;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]).trimmed().toCaseFolded();
        if (arg == QStringLiteral("--gui")) {
            if (mode == FrontendMode::Cli) return FrontendMode::Auto;
            mode = FrontendMode::Gui;
        } else if (arg == QStringLiteral("--cli")) {
            if (mode == FrontendMode::Gui) return FrontendMode::Auto;
            mode = FrontendMode::Cli;
        }
    }
    return mode;
}

bool bothOverridesPresent(int argc, char *argv[])
{
    bool gui = false;
    bool cli = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]).trimmed().toCaseFolded();
        gui = gui || arg == QStringLiteral("--gui");
        cli = cli || arg == QStringLiteral("--cli");
    }
    return gui && cli;
}

void configureApplicationIdentity()
{
    QCoreApplication::setApplicationName(appId());
    QCoreApplication::setApplicationVersion(appVersionString());
    QCoreApplication::setOrganizationName(appId());
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    QCoreApplication::setOrganizationDomain(QStringLiteral("local.wafflehouse-client"));
#endif
}

struct StoredProfile {
    QHash<QString, QVariant> values;
    QString dedupeKey;
};

QList<StoredProfile> readProfiles(QSettings &settings)
{
    static const QStringList keys = {
        QStringLiteral("id"), QStringLiteral("protocol"), QStringLiteral("server"),
        QStringLiteral("port"), QStringLiteral("username"), QStringLiteral("redirectHost"),
        QStringLiteral("redirectPort"), QStringLiteral("realName"), QStringLiteral("tls"),
        QStringLiteral("ircBuddies"), QStringLiteral("sipContacts"),
        QStringLiteral("telnetTerminalType"), QStringLiteral("sipProfileName"),
        QStringLiteral("sipDomain"), QStringLiteral("sipRegistrar"),
        QStringLiteral("sipAuthUsername"), QStringLiteral("sipDisplayName"),
        QStringLiteral("sipOutboundProxy"), QStringLiteral("sipCallerIdDomain"),
        QStringLiteral("sipDialPrefix"), QStringLiteral("sipStunServer"),
        QStringLiteral("sipTransport"), QStringLiteral("sipIdentityMode"),
        QStringLiteral("sipLocalPort"), QStringLiteral("sipRegistrationExpires"),
        QStringLiteral("sipUseIce"), QStringLiteral("sipEnableSrtp"),
        QStringLiteral("debug"), QStringLiteral("secretRequired"),
        QStringLiteral("savePassword"), QStringLiteral("password")
    };

    QList<StoredProfile> out;
    const int count = settings.beginReadArray(QStringLiteral("connections"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        StoredProfile profile;
        for (const QString &key : keys) {
            if (settings.contains(key)) profile.values.insert(key, settings.value(key));
        }
        const QString protocol = profile.values.value(QStringLiteral("protocol")).toString();
        const QString server = profile.values.value(QStringLiteral("server")).toString().trimmed().toCaseFolded();
        const QString port = profile.values.value(QStringLiteral("port")).toString();
        const QString username = profile.values.value(QStringLiteral("username")).toString().trimmed().toCaseFolded();
        profile.dedupeKey = protocol + QLatin1Char('|') + server + QLatin1Char('|') + port + QLatin1Char('|') + username;
        if (!server.isEmpty() || !username.isEmpty()) out.append(profile);
    }
    settings.endArray();
    return out;
}

void migrateLegacyWaffleHouseProfiles()
{
    QSettings current;
    if (current.value(QStringLiteral("connections/size"), 0).toInt() > 0) return;

    QList<StoredProfile> merged;
    QSet<QString> seen;
    const QList<QPair<QString, QString>> stores = {
        {QStringLiteral("WaffleHouseGUI"), QStringLiteral("WaffleHouseGUI")},
        {QStringLiteral("WaffleHouse-CLI"), QStringLiteral("WaffleHouse-CLI")}
    };

    for (const auto &store : stores) {
        QSettings legacy(store.first, store.second);
        const QList<StoredProfile> profiles = readProfiles(legacy);
        for (StoredProfile profile : profiles) {
            if (seen.contains(profile.dedupeKey)) continue;
            seen.insert(profile.dedupeKey);
            if (profile.values.value(QStringLiteral("id")).toString().trimmed().isEmpty()) {
                profile.values.insert(
                    QStringLiteral("id"),
                    QStringLiteral("migrated-%1").arg(QString::fromLatin1(
                        QCryptographicHash::hash(profile.dedupeKey.toUtf8(), QCryptographicHash::Sha256)
                            .left(16).toHex())));
            }
            merged.append(profile);
        }
    }

    if (merged.isEmpty()) return;
    current.remove(QStringLiteral("connections"));
    current.beginWriteArray(QStringLiteral("connections"));
    for (int i = 0; i < merged.size(); ++i) {
        current.setArrayIndex(i);
        for (auto it = merged.at(i).values.constBegin(); it != merged.at(i).values.constEnd(); ++it) {
            current.setValue(it.key(), it.value());
        }
    }
    current.endArray();
    current.setValue(QStringLiteral("migration/legacyWaffleHouseProfiles"), true);
    current.sync();
}

void addModeOptions(QCommandLineParser &parser)
{
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("gui")},
                                       QStringLiteral("Force the Qt GUI frontend.")));
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("cli")},
                                       QStringLiteral("Force the ncurses CLI frontend.")));
}

int runCli(int argc, char *argv[], const RuntimeEnvironment &runtime)
{
    QCoreApplication app(argc, argv);
    configureApplicationIdentity();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("WaffleHouse-CLI %1 WaffleHouse media — communications, secure rooms, media, radio, and streaming")
            .arg(appVersionString()));
    parser.addHelpOption();
    parser.addVersionOption();
    addModeOptions(parser);
    parser.process(app);

    migrateLegacyWaffleHouseProfiles();

    TerminalUi ui;
    QObject::connect(&ui, &TerminalUi::finished, &app, &QCoreApplication::quit);
    if (!ui.start()) {
        fprintf(stderr, "WaffleHouse-CLI %s: terminal frontend could not start.\nDetected: %s\n",
                appVersionString().toUtf8().constData(),
                runtime.summary().toUtf8().constData());
        return 1;
    }
    return app.exec();
}

int runGui(int argc, char *argv[], const RuntimeEnvironment &runtime)
{
    QApplication app(argc, argv);
    configureApplicationIdentity();
    QApplication::setApplicationDisplayName(appEditionDisplayName());
    QApplication::setWindowIcon(appIcon());
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("%1 %2 — AIM/OSCAR, IRC, Telnet/BBS, SIP/VoIP, secure rooms, media, radio, and streaming client")
            .arg(appEditionDisplayName(), appVersionString()));
    parser.addHelpOption();
    parser.addVersionOption();
    addModeOptions(parser);

    QCommandLineOption protocolOpt(
        QStringList{QStringLiteral("protocol")},
        QStringLiteral("Prefill connection protocol: aim/oscar, irc, or telnet."),
        QStringLiteral("protocol"));
    QCommandLineOption serverOpt(
        QStringList{QStringLiteral("s"), QStringLiteral("server")},
        QStringLiteral("Prefill server hostname/IP."), QStringLiteral("server"));
    QCommandLineOption portOpt(
        QStringList{QStringLiteral("p"), QStringLiteral("port")},
        QStringLiteral("Prefill server port."), QStringLiteral("port"));
    QCommandLineOption userOpt(
        QStringList{QStringLiteral("u"), QStringLiteral("username")},
        QStringLiteral("Prefill AIM screen name / IRC nickname / Telnet session label / SIP username."),
        QStringLiteral("username"));
    QCommandLineOption redirectHostOpt(
        QStringList{QStringLiteral("redirect-host")},
        QStringLiteral("Override hostnames advertised by OSCAR service redirects."),
        QStringLiteral("host"));
    QCommandLineOption redirectPortOpt(
        QStringList{QStringLiteral("redirect-port")},
        QStringLiteral("Override ports advertised by OSCAR service redirects."),
        QStringLiteral("port"));
    QCommandLineOption realNameOpt(
        QStringList{QStringLiteral("realname")}, QStringLiteral("Prefill IRC real-name field."),
        QStringLiteral("name"), appDefaultRealName());
    QCommandLineOption tlsOpt(QStringList{QStringLiteral("tls")},
                              QStringLiteral("Enable IRC TLS by default."));
    QCommandLineOption terminalTypeOpt(
        QStringList{QStringLiteral("terminal-type")}, QStringLiteral("Prefill Telnet terminal type."),
        QStringLiteral("terminal"), QStringLiteral("ANSI"));
    QCommandLineOption debugOpt(QStringList{QStringLiteral("debug")},
                                QStringLiteral("Enable protocol debug logging."));

    parser.addOption(protocolOpt);
    parser.addOption(serverOpt);
    parser.addOption(portOpt);
    parser.addOption(userOpt);
    parser.addOption(redirectHostOpt);
    parser.addOption(redirectPortOpt);
    parser.addOption(realNameOpt);
    parser.addOption(tlsOpt);
    parser.addOption(terminalTypeOpt);
    parser.addOption(debugOpt);
    parser.process(app);

    migrateLegacyWaffleHouseProfiles();

    ConnectionSettings defaults;
    const QString protocol = parser.value(protocolOpt).trimmed().toCaseFolded();
    if (protocol == QStringLiteral("oscar") || protocol == QStringLiteral("aim"))
        defaults.protocol = ConnectionSettings::Protocol::Oscar;
    else if (protocol == QStringLiteral("irc"))
        defaults.protocol = ConnectionSettings::Protocol::Irc;
    else if (protocol == QStringLiteral("telnet"))
        defaults.protocol = ConnectionSettings::Protocol::Telnet;
    else
        defaults.protocol = ConnectionSettings::Protocol::Unknown;

    defaults.server = parser.value(serverOpt);
    defaults.username = parser.value(userOpt);
    defaults.redirectHost = parser.value(redirectHostOpt);
    defaults.realName = parser.value(realNameOpt);
    defaults.tls = parser.isSet(tlsOpt);
    defaults.telnetTerminalType = parser.value(terminalTypeOpt);
    defaults.debug = parser.isSet(debugOpt);

    bool ok = false;
    const int suppliedPort = parser.value(portOpt).toInt(&ok);
    if (ok && suppliedPort > 0 && suppliedPort <= 65535)
        defaults.port = static_cast<quint16>(suppliedPort);
    else if (defaults.protocol == ConnectionSettings::Protocol::Irc)
        defaults.port = static_cast<quint16>(defaults.tls ? 6697 : 6667);
    else if (defaults.protocol == ConnectionSettings::Protocol::Telnet)
        defaults.port = 23;
    else
        defaults.port = 5190;

    const int redirectPort = parser.value(redirectPortOpt).toInt(&ok);
    if (ok && redirectPort > 0 && redirectPort <= 65535)
        defaults.redirectPort = static_cast<quint16>(redirectPort);

    QDialog splash;
    splash.setWindowTitle(QStringLiteral("%1 — Version %2").arg(appEditionDisplayName(), appVersionString()));
    splash.setModal(true);
    splash.setWindowFlag(Qt::FramelessWindowHint, true);
    auto *layout = new QVBoxLayout(&splash);
    auto *logo = new QLabel(&splash);
    const QPixmap logoPixmap = appLogoPixmap();
    if (!logoPixmap.isNull()) {
        logo->setPixmap(logoPixmap.scaled(220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        // The embedded image should always be present. Keep the terminal-era
        // text logo as a last-resort GUI fallback for damaged resource builds.
        logo->setTextFormat(Qt::PlainText);
        QFont mono(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        logo->setFont(mono);
        logo->setText(appAsciiLogo());
    }
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);
    auto *edition = new QLabel(QStringLiteral("WAFFLEHOUSE-CLIENT — VERSION %1").arg(appVersionString().toUpper()), &splash);
    QFont editionFont = edition->font();
    editionFont.setBold(true);
    edition->setFont(editionFont);
    edition->setAlignment(Qt::AlignCenter);
    layout->addWidget(edition);
    auto *radioEdition = new QLabel(QStringLiteral("\"WAFFLEHOUSE MEDIA\""), &splash);
    QFont radioFont = radioEdition->font();
    radioFont.setBold(true);
    radioEdition->setFont(radioFont);
    radioEdition->setAlignment(Qt::AlignCenter);
    layout->addWidget(radioEdition);
    auto *mode = new QLabel(QStringLiteral("AUTO FRONTEND — GUI MODE"), &splash);
    mode->setAlignment(Qt::AlignCenter);
    layout->addWidget(mode);
    auto *protocols = new QLabel(QStringLiteral("AIM/OSCAR  |  IRC  |  TELNET/BBS  |  SIP/VOIP  |  SECURE ROOMS  |  MEDIA/RADIO"), &splash);
    protocols->setAlignment(Qt::AlignCenter);
    layout->addWidget(protocols);
    QTimer::singleShot(900, &splash, &QDialog::accept);
    splash.exec();

    MainWindow window(defaults);

    auto *mediaWindow = new MediaWindow(&window);
    auto *mediaMenu = window.menuBar()->addMenu(QStringLiteral("&Media"));
    mediaMenu->addAction(QStringLiteral("Open Media Center"), mediaWindow, &MediaWindow::showAndRaise);
    mediaMenu->addSeparator();
    mediaMenu->addAction(QStringLiteral("Open Media Files…"), mediaWindow, &MediaWindow::openMediaFiles);
    mediaMenu->addAction(QStringLiteral("Open Stream / Radio URL…"), mediaWindow, &MediaWindow::openStreamDialog);
    mediaMenu->addAction(QStringLiteral("Search SHOUTcast Directory…"), mediaWindow, &MediaWindow::searchShoutcastDirectory);
    mediaMenu->addAction(QStringLiteral("Open Internet Playlist URL…"), mediaWindow, &MediaWindow::openInternetPlaylistDialog);
    mediaMenu->addAction(QStringLiteral("Load Local Playlist…"), mediaWindow, &MediaWindow::openPlaylistDialog);

    window.show();
    window.statusBar()->showMessage(
        QStringLiteral("Auto frontend selected GUI | Runtime: %1").arg(runtime.summary()), 12000);
    return app.exec();
}
} // namespace

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]).trimmed().toCaseFolded();
        if (arg == QStringLiteral("--version") || arg == QStringLiteral("-v")) {
            fprintf(stdout, "%s %s\n",
                    appEditionDisplayName().toUtf8().constData(),
                    appVersionString().toUtf8().constData());
            return 0;
        }
    }

    if (bothOverridesPresent(argc, argv)) {
        fprintf(stderr, "WaffleHouse-Client: --gui and --cli cannot be used together.\n");
        return 2;
    }

    const RuntimeEnvironment runtime = RuntimeEnvironment::detect();
    FrontendMode mode = requestedMode(argc, argv);
    if (mode == FrontendMode::Auto) {
        // A controlling terminal means the user intentionally launched the program
        // from a shell, so prefer the ncurses frontend. Desktop/menu launches have
        // no attached TTY and therefore default to the GUI.
        mode = runtime.ttyAttached ? FrontendMode::Cli : FrontendMode::Gui;
    }

    if (mode == FrontendMode::Gui) {
        if (!runtime.graphicalSession && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            fprintf(stderr,
                    "WaffleHouse-Client 3.2-Termux: GUI mode requested but no X11/Wayland session was detected.\n"
                    "Detected: %s\nUse --cli from an interactive terminal.\n",
                    runtime.summary().toUtf8().constData());
            return 2;
        }
        return runGui(argc, argv, runtime);
    }

    if (!runtime.ttyAttached) {
        fprintf(stderr,
                "WaffleHouse-CLI 3.2-Termux: CLI mode requires an interactive terminal.\n"
                "Detected: %s\nUse --gui inside a graphical session.\n",
                runtime.summary().toUtf8().constData());
        return 2;
    }
    return runCli(argc, argv, runtime);
}
