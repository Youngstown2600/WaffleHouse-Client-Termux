#include "ansiterminal.h"

#include <algorithm>
#include <utility>

AnsiTerminalModel::AnsiTerminalModel(int columns, int rows)
{
    resize(columns, rows);
}

void AnsiTerminalModel::resize(int columns, int rows)
{
    columns = std::clamp(columns, 20, 240);
    rows = std::clamp(rows, 5, 100);
    QVector<Cell> next(columns * rows);
    const int copyRows = std::min(rows, m_rows);
    const int copyCols = std::min(columns, m_columns);
    for (int r = 0; r < copyRows; ++r) {
        for (int c = 0; c < copyCols; ++c) {
            if (!m_cells.isEmpty()) next[r * columns + c] = m_cells[r * m_columns + c];
        }
    }
    m_columns = columns;
    m_rows = rows;
    m_cells = std::move(next);
    m_row = std::clamp(m_row, 0, m_rows - 1);
    m_col = std::clamp(m_col, 0, m_columns - 1);
}

void AnsiTerminalModel::reset()
{
    m_row = m_col = m_savedRow = m_savedCol = 0;
    m_fg = 7; m_bg = 0; m_bold = m_blink = m_inverse = false;
    m_state = ParseState::Text;
    m_csi.clear();
    for (Cell &cell : m_cells) clearCell(cell);
}

const AnsiTerminalModel::Cell &AnsiTerminalModel::cell(int row, int col) const
{
    static const Cell blank{};
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns) return blank;
    return m_cells[row * m_columns + col];
}

QString AnsiTerminalModel::plainLine(int row) const
{
    QString out;
    out.reserve(m_columns);
    for (int c = 0; c < m_columns; ++c) out += cell(row, c).ch;
    while (out.endsWith(QLatin1Char(' '))) out.chop(1);
    return out;
}

QStringList AnsiTerminalModel::plainLines() const
{
    QStringList lines;
    for (int r = 0; r < m_rows; ++r) lines << plainLine(r);
    return lines;
}

void AnsiTerminalModel::clearCell(Cell &cell)
{
    cell = Cell{};
    cell.fg = m_fg;
    cell.bg = m_bg;
}

void AnsiTerminalModel::put(QChar ch)
{
    if (m_col >= m_columns) {
        if (m_wrap) { m_col = 0; lineFeed(); }
        else m_col = m_columns - 1;
    }
    Cell &dst = m_cells[m_row * m_columns + m_col];
    dst.ch = ch;
    dst.fg = m_fg; dst.bg = m_bg;
    dst.bold = m_bold; dst.blink = m_blink; dst.inverse = m_inverse;
    ++m_col;
}

void AnsiTerminalModel::scrollUp()
{
    if (m_rows <= 1) return;
    for (int r = 1; r < m_rows; ++r)
        for (int c = 0; c < m_columns; ++c)
            m_cells[(r - 1) * m_columns + c] = m_cells[r * m_columns + c];
    for (int c = 0; c < m_columns; ++c) clearCell(m_cells[(m_rows - 1) * m_columns + c]);
}

void AnsiTerminalModel::lineFeed()
{
    ++m_row;
    if (m_row >= m_rows) { scrollUp(); m_row = m_rows - 1; }
}

void AnsiTerminalModel::carriageReturn() { m_col = 0; }
void AnsiTerminalModel::backspace() { if (m_col > 0) --m_col; }
void AnsiTerminalModel::tab() { m_col = std::min(m_columns - 1, ((m_col / 8) + 1) * 8); }

void AnsiTerminalModel::setCursor(int row, int col)
{
    m_row = std::clamp(row, 0, m_rows - 1);
    m_col = std::clamp(col, 0, m_columns - 1);
}

QVector<int> AnsiTerminalModel::parseParams(const QString &params, int defaultValue) const
{
    QString p = params;
    if (p.startsWith(QLatin1Char('?'))) p.remove(0, 1);
    QVector<int> values;
    if (p.isEmpty()) { values << defaultValue; return values; }
    for (const QString &part : p.split(QLatin1Char(';'))) {
        bool ok = false;
        int v = part.toInt(&ok);
        values << (ok ? v : defaultValue);
    }
    return values;
}

void AnsiTerminalModel::eraseDisplay(int mode)
{
    if (mode == 2 || mode == 3) {
        // ANSI ED clears the display but does not move the cursor.  BBS ANSI often
        // sends ED and CUP separately; homing here causes later screen fragments to
        // overwrite earlier rows when a server intentionally keeps the cursor.
        for (Cell &c : m_cells) clearCell(c);
        return;
    }
    if (mode == 0) {
        for (int r = m_row; r < m_rows; ++r) {
            int start = r == m_row ? m_col : 0;
            for (int c = start; c < m_columns; ++c) clearCell(m_cells[r * m_columns + c]);
        }
    } else if (mode == 1) {
        for (int r = 0; r <= m_row; ++r) {
            int end = r == m_row ? m_col : m_columns - 1;
            for (int c = 0; c <= end; ++c) clearCell(m_cells[r * m_columns + c]);
        }
    }
}

void AnsiTerminalModel::eraseLine(int mode)
{
    int start = 0, end = m_columns - 1;
    if (mode == 0) start = m_col;
    else if (mode == 1) end = m_col;
    for (int c = start; c <= end; ++c) clearCell(m_cells[m_row * m_columns + c]);
}

void AnsiTerminalModel::handleCsi(QChar finalByte, const QString &params)
{
    const QVector<int> p = parseParams(params, 0);
    const int n = p.isEmpty() || p[0] == 0 ? 1 : p[0];
    switch (finalByte.unicode()) {
    case 'A': setCursor(m_row - n, m_col); break;
    case 'B': setCursor(m_row + n, m_col); break;
    case 'C': setCursor(m_row, m_col + n); break;
    case 'D': setCursor(m_row, m_col - n); break;
    case 'E': setCursor(m_row + n, 0); break;
    case 'F': setCursor(m_row - n, 0); break;
    case 'G': setCursor(m_row, std::max(1, n) - 1); break;
    case '`': setCursor(m_row, std::max(1, n) - 1); break; // HPA
    case 'a': setCursor(m_row, m_col + n); break;          // HPR
    case 'd': setCursor(std::max(1, n) - 1, m_col); break; // VPA
    case 'e': setCursor(m_row + n, m_col); break;          // VPR
    case 'H': case 'f': {
        const int r = p.size() > 0 && p[0] > 0 ? p[0] : 1;
        const int c = p.size() > 1 && p[1] > 0 ? p[1] : 1;
        setCursor(r - 1, c - 1); break;
    }
    case 'J': eraseDisplay(p.isEmpty() ? 0 : p[0]); break;
    case 'K': eraseLine(p.isEmpty() ? 0 : p[0]); break;
    case 'X': { // ECH - erase characters without moving the cursor
        const int count = std::max(1, n);
        for (int c = m_col; c < std::min(m_columns, m_col + count); ++c)
            clearCell(m_cells[m_row * m_columns + c]);
        break;
    }
    case '@': { // ICH - insert blank characters
        const int count = std::min(std::max(1, n), m_columns - m_col);
        for (int c = m_columns - 1; c >= m_col + count; --c)
            m_cells[m_row * m_columns + c] = m_cells[m_row * m_columns + c - count];
        for (int c = m_col; c < m_col + count; ++c) clearCell(m_cells[m_row * m_columns + c]);
        break;
    }
    case 'P': { // DCH - delete characters
        const int count = std::min(std::max(1, n), m_columns - m_col);
        for (int c = m_col; c + count < m_columns; ++c)
            m_cells[m_row * m_columns + c] = m_cells[m_row * m_columns + c + count];
        for (int c = m_columns - count; c < m_columns; ++c) clearCell(m_cells[m_row * m_columns + c]);
        break;
    }
    case 's': m_savedRow = m_row; m_savedCol = m_col; break;
    case 'u': setCursor(m_savedRow, m_savedCol); break;
    case 'm': {
        QVector<int> codes = p;
        if (params.isEmpty()) codes = {0};
        for (int code : codes) {
            if (code == 0) { m_fg = 7; m_bg = 0; m_bold = m_blink = m_inverse = false; }
            else if (code == 1) m_bold = true;
            else if (code == 5) m_blink = true;
            else if (code == 7) m_inverse = true;
            else if (code == 22) m_bold = false;
            else if (code == 25) m_blink = false;
            else if (code == 27) m_inverse = false;
            else if (code >= 30 && code <= 37) m_fg = code - 30;
            else if (code >= 40 && code <= 47) m_bg = code - 40;
            else if (code >= 90 && code <= 97) { m_fg = code - 90; m_bold = true; }
            else if (code >= 100 && code <= 107) m_bg = code - 100;
            else if (code == 39) m_fg = 7;
            else if (code == 49) m_bg = 0;
        }
        break;
    }
    case 'h': if (params == QStringLiteral("?7")) m_wrap = true; break;
    case 'l': if (params == QStringLiteral("?7")) m_wrap = false; break;
    default: break;
    }
}

void AnsiTerminalModel::feed(const QString &text)
{
    for (const QChar ch : text) {
        switch (m_state) {
        case ParseState::Text:
            if (ch.unicode() == 0x1b) m_state = ParseState::Escape;
            else if (ch == QLatin1Char('\r')) carriageReturn();
            else if (ch == QLatin1Char('\n')) lineFeed();
            else if (ch == QLatin1Char('\b')) backspace();
            else if (ch == QLatin1Char('\t')) tab();
            else if (ch.unicode() >= 0x20 && ch.unicode() != 0x7f) put(ch);
            break;
        case ParseState::Escape:
            if (ch == QLatin1Char('[')) { m_csi.clear(); m_state = ParseState::Csi; }
            else if (ch == QLatin1Char('7')) { m_savedRow = m_row; m_savedCol = m_col; m_state = ParseState::Text; }
            else if (ch == QLatin1Char('8')) { setCursor(m_savedRow, m_savedCol); m_state = ParseState::Text; }
            else if (ch == QLatin1Char('D')) { lineFeed(); m_state = ParseState::Text; } // IND
            else if (ch == QLatin1Char('E')) { lineFeed(); carriageReturn(); m_state = ParseState::Text; } // NEL
            else if (ch == QLatin1Char('M')) { setCursor(m_row - 1, m_col); m_state = ParseState::Text; } // RI (bounded)
            else if (ch == QLatin1Char('c')) { reset(); }
            else m_state = ParseState::Text;
            break;
        case ParseState::Csi:
            if (ch.unicode() >= 0x40 && ch.unicode() <= 0x7e) {
                handleCsi(ch, m_csi);
                m_csi.clear();
                m_state = ParseState::Text;
            } else if (m_csi.size() < 64) {
                m_csi += ch;
            } else {
                m_csi.clear();
                m_state = ParseState::Text;
            }
            break;
        }
    }
}

QColor AnsiTerminalModel::ansiColor(int index, bool bright)
{
    static const int normal[8][3] = {
        {0,0,0},{170,0,0},{0,170,0},{170,85,0},{0,0,170},{170,0,170},{0,170,170},{170,170,170}
    };
    static const int high[8][3] = {
        {85,85,85},{255,85,85},{85,255,85},{255,255,85},{85,85,255},{255,85,255},{85,255,255},{255,255,255}
    };
    index = std::clamp(index, 0, 7);
    const int (*table)[3] = bright ? high : normal;
    return QColor(table[index][0], table[index][1], table[index][2]);
}
