#include "ansiterminalwidget.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>

AnsiTerminalWidget::AnsiTerminalWidget(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setStyleHint(QFont::Monospace);
    setFont(m_font);
    QFontMetrics fm(m_font);
    m_cellWidth = std::max(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = std::max(1, fm.height());
    setMinimumSize(m_cellWidth * 40, m_cellHeight * 12);
}

void AnsiTerminalWidget::feed(const QString &text)
{
    m_model.feed(text);
    update();
}

void AnsiTerminalWidget::clearScreen()
{
    m_model.reset();
    update();
}

void AnsiTerminalWidget::updateGeometryFromPixels()
{
    const int cols = 80;
    const int rows = std::max(5, std::min(50, height() / m_cellHeight));
    if (cols != m_model.columns() || rows != m_model.rows()) {
        m_model.resize(cols, rows);
        emit terminalSizeChanged(cols, rows);
    }
}

void AnsiTerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateGeometryFromPixels();
}

void AnsiTerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.setFont(m_font);
    QFontMetrics fm(m_font);
    const int baselineOffset = fm.ascent();

    for (int r = 0; r < m_model.rows(); ++r) {
        int c = 0;
        while (c < m_model.columns()) {
            const auto first = m_model.cell(r, c);
            int end = c + 1;
            while (end < m_model.columns()) {
                const auto next = m_model.cell(r, end);
                if (next.fg != first.fg || next.bg != first.bg || next.bold != first.bold
                    || next.inverse != first.inverse) break;
                ++end;
            }
            int fg = first.fg, bg = first.bg;
            if (first.inverse) std::swap(fg, bg);
            const QColor bgColor = AnsiTerminalModel::ansiColor(bg, false);
            p.fillRect(c * m_cellWidth, r * m_cellHeight,
                       (end - c) * m_cellWidth, m_cellHeight, bgColor);
            p.setPen(AnsiTerminalModel::ansiColor(fg, first.bold));
            QString run;
            for (int x = c; x < end; ++x) run += m_model.cell(r, x).ch;
            p.drawText(c * m_cellWidth, r * m_cellHeight + baselineOffset, run);
            c = end;
        }
    }

    if (hasFocus()) {
        const int x = m_model.cursorColumn() * m_cellWidth;
        const int y = m_model.cursorRow() * m_cellHeight;
        p.fillRect(x, y + m_cellHeight - 2, m_cellWidth, 2, Qt::white);
    }
}

QByteArray AnsiTerminalWidget::keySequence(QKeyEvent *event) const
{
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    QByteArray out;
    switch (event->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: return QByteArray("\r");
    case Qt::Key_Backspace: return QByteArray(1, '\x08');
    case Qt::Key_Tab: return QByteArray(1, '\t');
    case Qt::Key_Escape: return QByteArray(1, '\x1b');
    case Qt::Key_Up: return QByteArray("\x1b[A");
    case Qt::Key_Down: return QByteArray("\x1b[B");
    case Qt::Key_Right: return QByteArray("\x1b[C");
    case Qt::Key_Left: return QByteArray("\x1b[D");
    case Qt::Key_Home: return QByteArray("\x1b[H");
    case Qt::Key_End: return QByteArray("\x1b[F");
    case Qt::Key_Delete: return QByteArray("\x1b[3~");
    case Qt::Key_PageUp: return QByteArray("\x1b[5~");
    case Qt::Key_PageDown: return QByteArray("\x1b[6~");
    case Qt::Key_F1: return QByteArray("\x1bOP");
    case Qt::Key_F2: return QByteArray("\x1bOQ");
    case Qt::Key_F3: return QByteArray("\x1bOR");
    case Qt::Key_F4: return QByteArray("\x1bOS");
    default: break;
    }
    const QString text = event->text();
    if (ctrl && !text.isEmpty()) {
        const QChar ch = text.at(0).toUpper();
        if (ch.unicode() >= '@' && ch.unicode() <= '_') out.append(char(ch.unicode() - '@'));
    } else if (!text.isEmpty()) {
        out = text.toUtf8();
    }
    if (alt && !out.isEmpty()) out.prepend('\x1b');
    return out;
}

void AnsiTerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const QByteArray bytes = keySequence(event);
    if (!bytes.isEmpty()) {
        emit terminalBytes(bytes);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AnsiTerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    QWidget::mousePressEvent(event);
}
