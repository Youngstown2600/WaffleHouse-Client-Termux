#pragma once

#include "ansiterminal.h"

#include <QWidget>
#include <QFont>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

class AnsiTerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit AnsiTerminalWidget(QWidget *parent = nullptr);
    void feed(const QString &text);
    void clearScreen();
    int terminalColumns() const { return m_model.columns(); }
    int terminalRows() const { return m_model.rows(); }

signals:
    void terminalBytes(const QByteArray &bytes);
    void terminalSizeChanged(int columns, int rows);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void updateGeometryFromPixels();
    QByteArray keySequence(QKeyEvent *event) const;

    AnsiTerminalModel m_model{80, 25};
    QFont m_font;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
};
