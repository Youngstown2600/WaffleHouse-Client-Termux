#include "modernstyle.h"

namespace ModernStyle {

QString normalizeThemeKey(const QString &key)
{
    QString out = key.trimmed().toCaseFolded();
    if (out.isEmpty() || out == QStringLiteral("classic")) return QStringLiteral("system");
    return out;
}

Palette paletteFor(const QString &raw)
{
    const QString key = normalizeThemeKey(raw);
    Palette p{
        QStringLiteral("#0b0f14"), QStringLiteral("#0d131b"), QStringLiteral("#111923"),
        QStringLiteral("#16212e"), QStringLiteral("#e8eef6"), QStringLiteral("#8e9baa"),
        QStringLiteral("#4ea1ff"), QStringLiteral("#7dc4ff"), QStringLiteral("#273444"),
        QStringLiteral("#55d187"), QStringLiteral("#ff6b78"), false};

    if (key == QStringLiteral("classic-light")) {
        p = {"#f4f6f8", "#eef1f5", "#ffffff", "#f8fafc", "#18212c", "#647182",
             "#2563eb", "#0ea5e9", "#d8dee8", "#15803d", "#dc2626", true};
    } else if (key == QStringLiteral("hacker") || key == QStringLiteral("2600")) {
        p = {"#030703", "#050b06", "#071008", "#0a170c", "#bfffc6", "#6da975",
             "#35ff5a", "#82ff96", "#174d20", "#35ff5a", "#ff5a67", false};
    } else if (key == QStringLiteral("matrix") || key == QStringLiteral("wargames") || key == QStringLiteral("crt-green")) {
        p = {"#010401", "#030803", "#050d05", "#091609", "#8cff8c", "#4d8752",
             "#33ff33", "#9bff9b", "#155f1b", "#33ff33", "#ff5c5c", false};
    } else if (key == QStringLiteral("phosphor")) {
        p = {"#061006", "#081708", "#0b1d0b", "#102710", "#b9ffb9", "#68a568",
             "#68ff68", "#a1ffa1", "#245c24", "#68ff68", "#ff7474", false};
    } else if (key == QStringLiteral("amber") || key == QStringLiteral("waffle-iron") || key == QStringLiteral("beige-box")) {
        p = {"#120c03", "#191005", "#211606", "#2b1d08", "#ffe0a0", "#b78c4d",
             "#ffb52f", "#ffd274", "#6d4b18", "#a8d45b", "#ff6f62", false};
    } else if (key == QStringLiteral("ice") || key == QStringLiteral("ocean") || key == QStringLiteral("blue-box")) {
        p = {"#041019", "#061722", "#08202e", "#0c2a3c", "#d8f2ff", "#77a8bf",
             "#45c8ff", "#8ee6ff", "#194e68", "#59e6b5", "#ff7182", false};
    } else if (key == QStringLiteral("midnight") || key == QStringLiteral("cobalt") || key == QStringLiteral("retro-blue")) {
        p = {"#07111f", "#09182a", "#0c2137", "#112d49", "#e5efff", "#8298b8",
             "#5aa7ff", "#7bd6ff", "#234b74", "#68d391", "#ff7182", false};
    } else if (key == QStringLiteral("cyberpunk") || key == QStringLiteral("neon-miami")) {
        p = {"#070611", "#0d091b", "#120d25", "#1b1234", "#f0f9ff", "#9b87b5",
             "#25e6ff", "#ff4ed8", "#4a2c66", "#61ffa6", "#ff557f", false};
    } else if (key == QStringLiteral("synthwave") || key == QStringLiteral("vaporwave")) {
        p = {"#130a27", "#1a0d35", "#241246", "#30185b", "#ffe8ff", "#b296c9",
             "#ff5fd1", "#42e9ff", "#633279", "#60f5b1", "#ff687f", false};
    } else if (key == QStringLiteral("dracula")) {
        p = {"#21222c", "#252631", "#282a36", "#343746", "#f8f8f2", "#9aa0bd",
             "#bd93f9", "#8be9fd", "#4d5270", "#50fa7b", "#ff5555", false};
    } else if (key == QStringLiteral("blood-moon") || key == QStringLiteral("red-box")) {
        p = {"#100205", "#190408", "#22060b", "#310a12", "#ffe4e6", "#b98a91",
             "#ff4d68", "#ff8a9c", "#6f1927", "#68d391", "#ff334f", false};
    } else if (key == QStringLiteral("solarized") || key == QStringLiteral("solarized-dark")) {
        p = {"#002b36", "#073642", "#0b3c46", "#104752", "#93a1a1", "#73878b",
             "#2aa198", "#268bd2", "#355d64", "#859900", "#dc322f", false};
    } else if (key == QStringLiteral("nord")) {
        p = {"#2e3440", "#303744", "#3b4252", "#434c5e", "#eceff4", "#a7b1c2",
             "#88c0d0", "#81a1c1", "#596477", "#a3be8c", "#bf616a", false};
    } else if (key == QStringLiteral("monochrome") || key == QStringLiteral("vt220") || key == QStringLiteral("stealth")) {
        p = {"#0b0b0b", "#111111", "#171717", "#202020", "#ededed", "#9b9b9b",
             "#d0d0d0", "#ffffff", "#3a3a3a", "#c8f7c5", "#ff7b7b", false};
    } else if (key == QStringLiteral("c64")) {
        p = {"#352879", "#40318d", "#4a3898", "#5643aa", "#d3d9ff", "#a5afea",
             "#9ca8ff", "#c4cbff", "#7869c4", "#8ff0b5", "#ff8c9a", false};
    } else if (key == QStringLiteral("dos")) {
        p = {"#000050", "#000064", "#000080", "#00009c", "#ffffff", "#a8c3ff",
             "#00ffff", "#ffff55", "#006caa", "#55ff55", "#ff5555", false};
    } else if (key == QStringLiteral("ghostline")) {
        p = {"#06121a", "#081923", "#0b2230", "#102f41", "#d7f4ff", "#85a8b8",
             "#62dcff", "#9b7bff", "#315a74", "#72e0ae", "#ff7182", false};
    } else if (key == QStringLiteral("hot-dog-stand")) {
        p = {"#b80000", "#d40000", "#ef1717", "#ff2a2a", "#fff96a", "#ffd76a",
             "#fff000", "#ffffff", "#7d0000", "#a8ff60", "#ffffff", false};
    }
    return p;
}

QString styleSheet(const QString &key)
{
    const Palette p = paletteFor(key);
    return QStringLiteral(R"QSS(
* { outline: none; }
QWidget { color: %5; font-size: 10pt; }
QMainWindow, QDialog, QWidget#ModernRoot { background: %1; }
QToolTip { background: %4; color: %5; border: 1px solid %9; padding: 6px; border-radius: 6px; }
QFrame#Sidebar { background: %2; border-right: 1px solid %9; }
QFrame#TopBar { background: transparent; border: none; }
QFrame#Card, QFrame#SoftphoneCard, QFrame#ActionCard, QFrame#ConnectionCard {
    background: %3; border: 1px solid %9; border-radius: 14px;
}
QLabel#BrandTitle { color: %5; font-size: 17pt; font-weight: 800; letter-spacing: 1px; }
QLabel#BrandVersion { color: %6; font-size: 9pt; }
QLabel#PageTitle { color: %5; font-size: 20pt; font-weight: 750; }
QLabel#PageSubtitle, QLabel#Muted { color: %6; }
QLabel#CardTitle { color: %5; font-size: 11.5pt; font-weight: 700; }
QLabel#StatusPill { background: %10; color: %1; border-radius: 10px; padding: 4px 10px; font-weight: 700; }
QPushButton { background: %4; color: %5; border: 1px solid %9; border-radius: 8px; padding: 6px 10px; min-height: 18px; }
QPushButton:hover { background: %2; border-color: %7; }
QPushButton:pressed { background: %3; }
QPushButton:disabled { color: %6; border-color: %9; }
QPushButton[role="primary"] { background: %7; color: %1; border-color: %7; font-weight: 700; }
QPushButton[role="primary"]:hover { background: %8; border-color: %8; }
QPushButton[role="danger"] { color: %11; }
QPushButton[dialKey="true"] {
    background: %2; color: %5; border: 1px solid %9; border-radius: 32px;
    padding: 0; font-weight: 800;
}
QPushButton[dialKey="true"]:hover { background: %4; border: 2px solid %7; }
QPushButton[dialKey="true"]:pressed { background: %7; color: %1; }
QPushButton[phoneUtility="true"] { background: transparent; border-color: %9; color: %6; }
QPushButton[phoneUtility="true"]:hover { color: %5; border-color: %7; background: %2; }
QPushButton[phoneAction="call"] { background: %10; color: %1; border-color: %10; font-weight: 800; letter-spacing: 1px; }
QPushButton[phoneAction="call"]:hover { border-color: %7; }
QPushButton[phoneAction="hangup"] { background: transparent; color: %11; border: 1px solid %11; font-weight: 800; letter-spacing: 1px; }
QPushButton[phoneAction="hangup"]:hover { background: %11; color: %1; }
QPushButton[nav="true"] { text-align: left; background: transparent; border: 1px solid transparent; padding: 7px 10px; }
QPushButton[nav="true"]:hover { background: %3; border-color: %9; }
QPushButton[nav="true"]:checked { background: %4; border-color: %7; color: %8; font-weight: 700; }
QLineEdit, QPlainTextEdit, QTextEdit, QTextBrowser, QListWidget, QTreeWidget, QTableWidget, QComboBox, QSpinBox {
    background: %4; color: %5; border: 1px solid %9; border-radius: 8px; padding: 5px 7px;
    selection-background-color: %7; selection-color: %1;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QListWidget:focus, QTreeWidget:focus, QComboBox:focus, QSpinBox:focus {
    border: 1px solid %7;
}
QTreeWidget { padding: 4px; }
QTreeWidget::item, QListWidget::item { padding: 6px 7px; border-radius: 5px; }
QTreeWidget::item:hover, QListWidget::item:hover { background: %4; }
QTreeWidget::item:selected, QListWidget::item:selected { background: %7; color: %1; }
QHeaderView::section { background: %3; color: %6; border: none; border-bottom: 1px solid %9; padding: 7px 8px; font-weight: 650; }
QGroupBox { background: %3; border: 1px solid %9; border-radius: 10px; margin-top: 12px; padding-top: 10px; font-weight: 650; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: %6; }
QTabWidget::pane { background: %3; border: 1px solid %9; border-radius: 10px; top: -1px; }
QTabBar::tab { background: transparent; color: %6; border: none; border-bottom: 2px solid transparent; padding: 9px 14px; }
QTabBar::tab:hover { color: %5; }
QTabBar::tab:selected { color: %8; border-bottom-color: %7; font-weight: 700; }
QMenuBar { background: %2; color: %5; border-bottom: 1px solid %9; padding: 2px; }
QMenuBar::item { background: transparent; padding: 6px 10px; border-radius: 5px; }
QMenuBar::item:selected { background: %3; }
QMenu { background: %3; color: %5; border: 1px solid %9; padding: 6px; }
QMenu::item { padding: 7px 24px 7px 10px; border-radius: 5px; }
QMenu::item:selected { background: %7; color: %1; }
QStatusBar { background: %2; color: %6; border-top: 1px solid %9; }
QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle { background: %9; border-radius: 5px; min-height: 28px; min-width: 28px; }
QScrollBar::handle:hover { background: %7; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QCheckBox { spacing: 8px; }
QDialogButtonBox QPushButton { min-width: 86px; }
)QSS")
        .arg(p.background)
        .arg(p.sidebar)
        .arg(p.surface)
        .arg(p.surfaceRaised)
        .arg(p.text)
        .arg(p.muted)
        .arg(p.accent)
        .arg(p.accentAlt)
        .arg(p.border)
        .arg(p.success)
        .arg(p.danger);
}

} // namespace ModernStyle
