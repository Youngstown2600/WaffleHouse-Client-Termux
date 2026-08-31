#pragma once

#include <pjsua2.hpp>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trunkmonkey {

class Logger;

// Bidirectional PCM bridge used by WaffleHouse shell-server sessions.
// The SSH companion creates two reverse-forwarded Unix-domain sockets on the
// server. PJSUA2's conference bridge feeds/consumes 16 kHz mono signed PCM16
// through these custom AudioMediaPort objects. Network/socket I/O happens only
// on worker threads; PJSIP's real-time media callbacks never block on SSH I/O.
class RemoteSipAudioBridge final {
public:
    explicit RemoteSipAudioBridge(Logger& logger);
    ~RemoteSipAudioBridge();

    RemoteSipAudioBridge(const RemoteSipAudioBridge&) = delete;
    RemoteSipAudioBridge& operator=(const RemoteSipAudioBridge&) = delete;

    void start(const std::string& playbackSocket, const std::string& captureSocket);
    void stop();
    bool active() const noexcept { return active_.load(); }

    // Call audio transmits to playbackSink(); microphone audio is sourced from
    // captureSource() and transmitted into the call.
    pj::AudioMedia& playbackSink();
    pj::AudioMedia& captureSource();

    static constexpr unsigned kClockRate = 16000;
    static constexpr unsigned kChannels = 1;
    static constexpr unsigned kFrameUsec = 20000;
    static constexpr unsigned kBitsPerSample = 16;

private:
    class PlaybackPort final : public pj::AudioMediaPort {
    public:
        PlaybackPort(Logger& logger, std::string socketPath);
        ~PlaybackPort() override;
        void startPort();
        void stopPort();
        void onFrameReceived(pj::MediaFrame& frame) override;
    private:
        void workerMain();
        Logger& logger_;
        std::string socketPath_;
        std::atomic<bool> running_{false};
        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::uint8_t> queue_;
    };

    class CapturePort final : public pj::AudioMediaPort {
    public:
        CapturePort(Logger& logger, std::string socketPath);
        ~CapturePort() override;
        void startPort();
        void stopPort();
        void onFrameRequested(pj::MediaFrame& frame) override;
    private:
        void workerMain();
        Logger& logger_;
        std::string socketPath_;
        std::atomic<bool> running_{false};
        std::thread worker_;
        std::mutex mutex_;
        std::deque<std::uint8_t> queue_;
    };

    Logger& logger_;
    std::unique_ptr<PlaybackPort> playback_;
    std::unique_ptr<CapturePort> capture_;
    std::atomic<bool> active_{false};
};

} // namespace trunkmonkey
