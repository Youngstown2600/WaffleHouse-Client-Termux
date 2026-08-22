#include "transferwindow.h"
#include "appbranding.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QFont>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <algorithm>

TransferWindow::TransferWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("File Transfers — %1").arg(appDisplayName()));
    resize(600, 380);
    setMinimumSize(480, 300);
    setAttribute(Qt::WA_QuitOnClose, false);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(7);

    auto *transferLabel = new QLabel(QStringLiteral("Transfers"), central);
    QFont heading = transferLabel->font();
    heading.setBold(true);
    transferLabel->setFont(heading);
    outer->addWidget(transferLabel);

    m_table = new QTableWidget(0, 6, central);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("Direction"),
        QStringLiteral("Peer"),
        QStringLiteral("File"),
        QStringLiteral("Progress"),
        QStringLiteral("Status"),
        QStringLiteral("Action")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    outer->addWidget(m_table, 2);

    auto *logLabel = new QLabel(QStringLiteral("Transfer Log"), central);
    logLabel->setFont(heading);
    outer->addWidget(logLabel);

    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(3000);
    outer->addWidget(m_log, 1);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *clearLog = new QPushButton(QStringLiteral("Clear Log"), central);
    auto *hideButton = new QPushButton(QStringLiteral("Hide"), central);
    buttons->addWidget(clearLog);
    buttons->addWidget(hideButton);
    outer->addLayout(buttons);

    connect(clearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    connect(hideButton, &QPushButton::clicked, this, &QWidget::hide);

    setCentralWidget(central);
}

int TransferWindow::ensureRow(const QString &id)
{
    auto existing = m_rows.constFind(id);
    if (existing != m_rows.constEnd()) return existing.value();

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    for (int column = 0; column < m_table->columnCount(); ++column) {
        if (column != 3 && column != 5) m_table->setItem(row, column, new QTableWidgetItem);
    }

    auto *bar = new QProgressBar(m_table);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setFormat(QStringLiteral("0%"));
    m_table->setCellWidget(row, 3, bar);

    auto *actions = new QWidget(m_table);
    auto *actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(4);

    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), actions);
    cancelButton->setToolTip(QStringLiteral("Cancel this transfer without disconnecting the chat session"));
    connect(cancelButton, &QPushButton::clicked, this, [this, id]() {
        emit cancelRequested(id);
    });
    auto *resumeButton = new QPushButton(QStringLiteral("Resume"), actions);
    resumeButton->setToolTip(QStringLiteral("Resume this transfer from the saved partial offset"));
    resumeButton->hide();
    connect(resumeButton, &QPushButton::clicked, this, [this, id]() {
        emit resumeRequested(id);
    });
    auto *clearButton = new QPushButton(QStringLiteral("Clear"), actions);
    clearButton->setToolTip(QStringLiteral("Clear this transfer entry; partial download data is discarded"));
    clearButton->hide();
    connect(clearButton, &QPushButton::clicked, this, [this, id]() {
        emit clearRequested(id);
    });
    actionLayout->addWidget(cancelButton);
    actionLayout->addWidget(resumeButton);
    actionLayout->addWidget(clearButton);
    m_table->setCellWidget(row, 5, actions);

    m_rows.insert(id, row);
    m_progressBars.insert(id, bar);
    m_cancelButtons.insert(id, cancelButton);
    m_resumeButtons.insert(id, resumeButton);
    m_clearButtons.insert(id, clearButton);
    return row;
}

QString TransferWindow::humanBytes(qint64 bytes)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = bytes < 0 ? 0.0 : static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = unit == 0 ? 0 : (value < 10.0 ? 2 : 1);
    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals)
        .arg(QString::fromLatin1(units[unit]));
}

void TransferWindow::updateTransfer(const QString &id,
                                    const QString &direction,
                                    const QString &peer,
                                    const QString &fileName,
                                    qint64 transferred,
                                    qint64 total,
                                    const QString &status,
                                    bool resumable)
{
    if (id.isEmpty()) return;
    const int row = ensureRow(id);
    m_table->item(row, 0)->setText(direction);
    m_table->item(row, 1)->setText(peer);
    m_table->item(row, 2)->setText(fileName);
    m_table->item(row, 4)->setText(status);
    const QString state = status.toCaseFolded();
    const bool terminal = state.contains(QStringLiteral("complete"))
        || state.contains(QStringLiteral("cancel"))
        || state.contains(QStringLiteral("declin"))
        || state.contains(QStringLiteral("error"))
        || state.contains(QStringLiteral("failed"));
    if (QPushButton *cancelButton = m_cancelButtons.value(id, nullptr)) {
        cancelButton->setVisible(!terminal);
        cancelButton->setEnabled(!terminal);
    }
    if (QPushButton *resumeButton = m_resumeButtons.value(id, nullptr)) {
        resumeButton->setVisible(terminal && resumable);
        resumeButton->setEnabled(terminal && resumable);
    }
    if (QPushButton *clearButton = m_clearButtons.value(id, nullptr)) {
        clearButton->setVisible(terminal);
        clearButton->setEnabled(terminal);
    }

    QProgressBar *bar = m_progressBars.value(id, nullptr);
    if (bar) {
        const qint64 rawPercent = total > 0
            ? (transferred * qint64{100}) / total
            : qint64{0};
        const int percent = static_cast<int>(
            std::clamp(rawPercent, qint64{0}, qint64{100}));
        bar->setValue(percent);
        bar->setFormat(QStringLiteral("%1% — %2 / %3")
                           .arg(percent)
                           .arg(humanBytes(transferred))
                           .arg(humanBytes(total)));
    }
}

void TransferWindow::removeTransfer(const QString &id)
{
    const auto it = m_rows.constFind(id);
    if (it == m_rows.constEnd()) return;
    const int removedRow = it.value();

    m_rows.remove(id);
    m_progressBars.remove(id);
    m_cancelButtons.remove(id);
    m_resumeButtons.remove(id);
    m_clearButtons.remove(id);
    m_table->removeRow(removedRow);

    for (auto rowIt = m_rows.begin(); rowIt != m_rows.end(); ++rowIt) {
        if (rowIt.value() > removedRow) --rowIt.value();
    }
}

void TransferWindow::appendLog(const QString &message)
{
    if (!m_log || message.trimmed().isEmpty()) return;
    m_log->appendPlainText(QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
}

void TransferWindow::showAndRaise()
{
    show();
    raise();
    activateWindow();
}

void TransferWindow::closeEvent(QCloseEvent *event)
{
    hide();
    event->ignore();
}
