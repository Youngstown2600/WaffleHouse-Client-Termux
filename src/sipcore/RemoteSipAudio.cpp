#include "trunkmonkey/RemoteSipAudio.h"
#include "trunkmonkey/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace trunkmonkey {
namespace {
constexpr std::size_t kPlaybackQueueLimit = RemoteSipAudioBridge::kClockRate * 2U * 2U; // ~2 s mono PCM16
constexpr std::size_t kCaptureQueueLimit = RemoteSipAudioBridge::kClockRate * 2U * 2U;

int connectUnixSocket(const std::string& path)
{
    if (path.empty() || path.size() >= sizeof(((sockaddr_un*)nullptr)->sun_path)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
#ifdef __FreeBSD__
    addr.sun_len = static_cast<unsigned char>(offsetof(sockaddr_un, sun_path) + path.size() + 1U);
#endif
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1U);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data + offset, size - offset, 0);
#endif
        if (n > 0) { offset += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void closeSocket(int& fd)
{
    if (fd >= 0) {
        (void)::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        fd = -1;
    }
}
}

RemoteSipAudioBridge::PlaybackPort::PlaybackPort(Logger& logger, std::string socketPath)
    : logger_(logger), socketPath_(std::move(socketPath)) {}

RemoteSipAudioBridge::PlaybackPort::~PlaybackPort() { stopPort(); }

void RemoteSipAudioBridge::PlaybackPort::startPort()
{
    pj::MediaFormatAudio fmt;
    fmt.init(PJMEDIA_FORMAT_PCM, kClockRate, kChannels, kFrameUsec, kBitsPerSample);
    createPort("WaffleHouse SSH SIP playback", fmt);
    running_ = true;
    worker_ = std::thread(&PlaybackPort::workerMain, this);
}

void RemoteSipAudioBridge::PlaybackPort::stopPort()
{
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void RemoteSipAudioBridge::PlaybackPort::onFrameReceived(pj::MediaFrame& frame)
{
    if (!running_ || frame.type != PJMEDIA_FRAME_TYPE_AUDIO || frame.buf.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t incoming = std::min<std::size_t>(frame.size, frame.buf.size());
    if (incoming >= kPlaybackQueueLimit) {
        queue_.clear();
        const auto start = frame.buf.end() - static_cast<std::ptrdiff_t>(kPlaybackQueueLimit);
        queue_.insert(queue_.end(), start, frame.buf.end());
    } else {
        while (queue_.size() + incoming > kPlaybackQueueLimit && !queue_.empty()) queue_.pop_front();
        queue_.insert(queue_.end(), frame.buf.begin(), frame.buf.begin() + static_cast<std::ptrdiff_t>(incoming));
    }
    cv_.notify_one();
}

void RemoteSipAudioBridge::PlaybackPort::workerMain()
{
    int fd = -1;
    bool announced = false;
    std::vector<std::uint8_t> chunk;
    chunk.reserve(4096);
    while (running_) {
        if (fd < 0) {
            fd = connectUnixSocket(socketPath_);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (!announced) {
                logger_.info("SSH SIP playback bridge connected: " + socketPath_);
                announced = true;
            }
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]{ return !running_ || !queue_.empty(); });
            if (!running_) break;
            if (queue_.empty()) continue;
            const std::size_t count = std::min<std::size_t>(queue_.size(), 4096);
            chunk.clear();
            chunk.reserve(count);
            for (std::size_t i = 0; i < count; ++i) { chunk.push_back(queue_.front()); queue_.pop_front(); }
        }
        if (!writeAll(fd, chunk.data(), chunk.size())) {
            closeSocket(fd);
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
    }
    closeSocket(fd);
}

RemoteSipAudioBridge::CapturePort::CapturePort(Logger& logger, std::string socketPath)
    : logger_(logger), socketPath_(std::move(socketPath)) {}

RemoteSipAudioBridge::CapturePort::~CapturePort() { stopPort(); }

void RemoteSipAudioBridge::CapturePort::startPort()
{
    pj::MediaFormatAudio fmt;
    fmt.init(PJMEDIA_FORMAT_PCM, kClockRate, kChannels, kFrameUsec, kBitsPerSample);
    createPort("WaffleHouse SSH SIP capture", fmt);
    running_ = true;
    worker_ = std::thread(&CapturePort::workerMain, this);
}

void RemoteSipAudioBridge::CapturePort::stopPort()
{
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void RemoteSipAudioBridge::CapturePort::onFrameRequested(pj::MediaFrame& frame)
{
    const std::size_t requested = frame.size;
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame.buf.assign(requested, 0);
    if (!running_ || requested == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t available = std::min<std::size_t>(requested, queue_.size());
    for (std::size_t i = 0; i < available; ++i) { frame.buf[i] = queue_.front(); queue_.pop_front(); }
    frame.size = static_cast<unsigned>(requested);
}

void RemoteSipAudioBridge::CapturePort::workerMain()
{
    int fd = -1;
    bool announced = false;
    std::uint8_t buffer[4096];
    while (running_) {
        if (fd < 0) {
            fd = connectUnixSocket(socketPath_);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (!announced) {
                logger_.info("SSH SIP capture bridge connected: " + socketPath_);
                announced = true;
            }
        }
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t incoming = static_cast<std::size_t>(n);
            while (queue_.size() + incoming > kCaptureQueueLimit && !queue_.empty()) queue_.pop_front();
            queue_.insert(queue_.end(), buffer, buffer + incoming);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        closeSocket(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    closeSocket(fd);
}

RemoteSipAudioBridge::RemoteSipAudioBridge(Logger& logger) : logger_(logger) {}
RemoteSipAudioBridge::~RemoteSipAudioBridge() { stop(); }

void RemoteSipAudioBridge::start(const std::string& playbackSocket, const std::string& captureSocket)
{
    if (active_) return;
    if (playbackSocket.empty() || captureSocket.empty())
        throw std::runtime_error("Remote SIP audio requires both playback and capture SSH sockets");
    playback_ = std::make_unique<PlaybackPort>(logger_, playbackSocket);
    capture_ = std::make_unique<CapturePort>(logger_, captureSocket);
    try {
        playback_->startPort();
        capture_->startPort();
        active_ = true;
        logger_.info("WaffleHouse SSH full-duplex SIP audio bridge enabled (PCM16/16k/mono)");
    } catch (...) {
        stop();
        throw;
    }
}

void RemoteSipAudioBridge::stop()
{
    active_ = false;
    if (capture_) capture_->stopPort();
    if (playback_) playback_->stopPort();
    capture_.reset();
    playback_.reset();
}

pj::AudioMedia& RemoteSipAudioBridge::playbackSink()
{
    if (!playback_) throw std::runtime_error("Remote SIP playback bridge is not active");
    return *playback_;
}

pj::AudioMedia& RemoteSipAudioBridge::captureSource()
{
    if (!capture_) throw std::runtime_error("Remote SIP capture bridge is not active");
    return *capture_;
}

} // namespace trunkmonkey
