# WaffleHouse-Client 5.0r13 — Media Center

The WaffleHouse-Client Media Center is the integrated mpv-backed media and internet-radio player for Linux and FreeBSD.

The media engine controls an external `mpv` process through its JSON IPC interface. On Linux and FreeBSD it connects directly to mpv's filesystem Unix-domain socket with native `AF_UNIX`/`SOCK_STREAM`; `QSocketNotifier` feeds readable IPC data into the Qt event loop. `ffmpeg` provides broad codec/demuxer support through mpv.

Supported workflows include local audio, local video, direct HTTP/HTTPS media, SHOUTcast/Icecast radio, HLS streams, and M3U/M3U8/PLS playlists. For HLS `.m3u8`, use **Stream URL** or `/mstream`; use **Playlist URL**/`/mplaylist` for ordinary station/media playlists.

## YouTube removal

The dedicated YouTube streaming/audio feature is intentionally absent from WaffleHouse-Client 5.0r13. The GUI button and Media-menu action are removed, `/myoutube` is absent from the CLI, yt-dlp/Deno resolver dependencies are removed, and mpv starts with `--ytdl=no`.

## Media Center

Use **Media → Open Media Center**. The window provides transport controls, seek, volume/mute, queue management, shuffle/repeat, local/internet playlist loading, direct stream/radio URLs, SHOUTcast directory search, and a 10-band EQ. Video is displayed by mpv in its own native video window.

CLI media commands include `/media`, `/mstatus`, `/mplay`, `/mstream`, `/mshoutcast`, `/menqueue`, `/mplaylist`, `/mpause`, `/mresume`, `/mstop`, `/mnext`, `/mprev`, `/mseek`, `/mvolume`, `/mmute`, `/mshuffle`, `/mrepeat`, and `/meq`.

Stop preserves the current queue. If playback is stopped, **Play/Resume starts item 1 of the preserved queue automatically**.
