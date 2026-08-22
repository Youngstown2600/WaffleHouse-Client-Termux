#pragma once

#include <QHash>
#include <QMainWindow>
#include <QString>

class QPlainTextEdit;
class QProgressBar;
class QTableWidget;
class QPushButton;

class TransferWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit TransferWindow(QWidget *parent = nullptr);

    void updateTransfer(const QString &id,
                        const QString &direction,
                        const QString &peer,
                        const QString &fileName,
                        qint64 transferred,
                        qint64 total,
                        const QString &status,
                        bool resumable = false);
    void appendLog(const QString &message);
    void removeTransfer(const QString &id);
    void showAndRaise();

signals:
    void cancelRequested(const QString &transferId);
    void resumeRequested(const QString &transferId);
    void clearRequested(const QString &transferId);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    int ensureRow(const QString &id);
    static QString humanBytes(qint64 bytes);

    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QHash<QString, int> m_rows;
    QHash<QString, QProgressBar *> m_progressBars;
    QHash<QString, QPushButton *> m_cancelButtons;
    QHash<QString, QPushButton *> m_resumeButtons;
    QHash<QString, QPushButton *> m_clearButtons;
};
