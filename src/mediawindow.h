#pragma once

#include <QMainWindow>
#include <QList>
#include <QStringList>

class MediaController;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QListWidget;
class QSlider;

class MediaWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MediaWindow(QWidget *parent = nullptr);

public slots:
    void showAndRaise();
    void openMediaFiles();
    void openStreamDialog();
    void openInternetPlaylistDialog();
    void searchShoutcastDirectory();
    void openPlaylistDialog();
    void savePlaylistDialog();

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void addSources(const QStringList &sources, bool playFirst);
    void playSelected();
    void removeSelected();
    void clearPlaylist();
    void syncPlaylist(const QStringList &sources,
                      const QStringList &titles,
                      int currentIndex);
    void updateTime();
    QString timeText(double seconds) const;

    MediaController *m_media = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_source = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_backend = nullptr;
    QListWidget *m_playlist = nullptr;
    QSlider *m_seek = nullptr;
    QSlider *m_volume = nullptr;
    QCheckBox *m_shuffle = nullptr;
    QComboBox *m_repeat = nullptr;
    QList<QSlider *> m_eqSliders;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_draggingSeek = false;
    bool m_syncingPlaylist = false;
};
