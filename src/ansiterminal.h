#pragma once

#include <QByteArray>
#ifndef WAFFLEHOUSE_TERMUX
#include <QColor>
#endif
#include <QString>
#include <QStringList>
#include <QVector>

class AnsiTerminalModel {
public:
    struct Cell {
        QChar ch = QLatin1Char(' ');
        int fg = 7;
        int bg = 0;
        bool bold = false;
        bool blink = false;
        bool inverse = false;
    };

    explicit AnsiTerminalModel(int columns = 80, int rows = 25);

    void resize(int columns, int rows);
    void reset();
    void feed(const QString &text);

    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    int cursorRow() const { return m_row; }
    int cursorColumn() const { return m_col; }
    const Cell &cell(int row, int col) const;
    QString plainLine(int row) const;
    QStringList plainLines() const;

#ifndef WAFFLEHOUSE_TERMUX
    static QColor ansiColor(int index, bool bright = false);
#endif

private:
    void put(QChar ch);
    void lineFeed();
    void carriageReturn();
    void backspace();
    void tab();
    void scrollUp();
    void eraseDisplay(int mode);
    void eraseLine(int mode);
    void handleCsi(QChar finalByte, const QString &params);
    QVector<int> parseParams(const QString &params, int defaultValue = 0) const;
    void setCursor(int row, int col);
    void clearCell(Cell &cell);

    int m_columns = 80;
    int m_rows = 25;
    int m_row = 0;
    int m_col = 0;
    int m_savedRow = 0;
    int m_savedCol = 0;
    int m_fg = 7;
    int m_bg = 0;
    bool m_bold = false;
    bool m_blink = false;
    bool m_inverse = false;
    bool m_wrap = true;
    QVector<Cell> m_cells;

    enum class ParseState { Text, Escape, Csi };
    ParseState m_state = ParseState::Text;
    QString m_csi;
};
