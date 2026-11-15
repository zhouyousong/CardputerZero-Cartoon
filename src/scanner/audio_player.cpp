#include "audio_player.h"
#include "audio_source.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cartoon {
namespace {

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

class PlayerProcess {
public:
    PlayerProcess() {
        char pattern[] = "/tmp/cartoon-player-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        if (!directory) {
            throw std::runtime_error(u8"无法创建播放器通信目录");
        }
        directory_ = directory;
        socket_path = directory_ + "/ipc";
    }

    ~PlayerProcess() {
        if (fd >= 0) {
            close(fd);
        }
        if (pid > 0) {
            kill(pid, SIGTERM);
            bool reaped = false;
            for (int attempt = 0; attempt < 50; ++attempt) {
                const auto status = waitpid(pid, nullptr, WNOHANG);
                if (status == pid || (status < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!reaped) {
                kill(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
        if (unlink(socket_path.c_str()) < 0 && errno != ENOENT) {
            std::fprintf(stderr, "Cartoon: cannot remove player socket: %s\n", strerror(errno));
        }
        if (rmdir(directory_.c_str()) < 0) {
            std::fprintf(stderr, "Cartoon: cannot remove player directory: %s\n", strerror(errno));
        }
    }

    void launch() {
        posix_spawn_file_actions_t actions;
        int status = posix_spawn_file_actions_init(&actions);
        if (status != 0) {
            throw std::runtime_error(u8"无法初始化播放器");
        }
        for (int target = 0; target < 3 && status == 0; ++target) {
            status = posix_spawn_file_actions_addopen(&actions, target, "/dev/null",
                                                     target == 0 ? O_RDONLY : O_WRONLY, 0);
        }
        const auto ipc = "--input-ipc-server=" + socket_path;
        const char* args[] = {
            "/usr/bin/mpv", "--no-config", "--idle=yes", "--keep-open=no",
            "--vid=no", "--audio-display=no", "--ao=pulse", "--volume=0", "--no-terminal",
            "--load-scripts=no", "--ytdl=no", "--access-references=no",
            "--network-timeout=15", "--input-default-bindings=no",
            ipc.c_str(), nullptr
        };
        if (status == 0) {
            status = posix_spawn(&pid, args[0], &actions, nullptr,
                                 const_cast<char* const*>(args), environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        if (status != 0) {
            throw std::runtime_error(u8"无法启动 mpv 播放器");
        }
    }

    bool connect_ipc() {
        const int candidate = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (candidate < 0) {
            throw std::runtime_error(u8"无法创建播放器通信连接");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, socket_path.c_str());
        if (connect(candidate, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            const int error = errno;
            close(candidate);
            if (error == ENOENT || error == ECONNREFUSED) {
                return false;
            }
            throw std::runtime_error(u8"无法连接播放器");
        }
        fd = candidate;
        timeval timeout{1, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            throw std::runtime_error(u8"无法设置播放器通信超时");
        }
        return true;
    }

    void send(Json command) {
        if (!command) {
            throw std::runtime_error(u8"播放器命令内存不足");
        }
        std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(command.get()), cJSON_free);
        if (!text) {
            throw std::runtime_error(u8"无法生成播放器命令");
        }
        const std::string line = std::string(text.get()) + "\n";
        size_t sent = 0;
        while (sent < line.size()) {
            const auto count = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error(u8"播放器连接中断");
            }
            sent += static_cast<size_t>(count);
        }
    }

    pid_t pid{-1};
    int fd{-1};
    std::string socket_path;

private:
    std::string directory_;
};

Json command(const char* name, int id = 0) {
    Json json(cJSON_CreateObject(), cJSON_Delete);
    auto* args = cJSON_AddArrayToObject(json.get(), "command");
    if (!json || !args || !cJSON_AddItemToArray(args, cJSON_CreateString(name)) ||
        !cJSON_AddNumberToObject(json.get(), "request_id", id)) {
        throw std::runtime_error(u8"播放器命令内存不足");
    }
    return json;
}

void add_string(Json& json, const char* value) {
    if (!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(json.get(), "command"),
                             cJSON_CreateString(value))) {
        throw std::runtime_error(u8"播放器命令内存不足");
    }
}

void add_number(Json& json, double value) {
    if (!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(json.get(), "command"),
                             cJSON_CreateNumber(value))) {
        throw std::runtime_error(u8"播放器命令内存不足");
    }
}

const char* string_value(const cJSON* json, const char* name) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

} // namespace

AudioPlayer::~AudioPlayer() { stop(); }

void AudioPlayer::play(const std::string& path, int volume) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = false;
        volume_ = std::clamp(volume, 0, 100);
    }
    stopping_ = false;
    worker_ = std::thread(&AudioPlayer::run, this, path);
}

void AudioPlayer::stop() {
    stopping_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    update_.reset();
}

void AudioPlayer::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

void AudioPlayer::set_volume(int volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = std::clamp(volume, 0, 100);
}

std::optional<PlaybackUpdate> AudioPlayer::take_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(update_);
    update_.reset();
    return result;
}

void AudioPlayer::publish(const PlaybackUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);
    update_ = update;
}

void AudioPlayer::run(std::string path) {
    PlaybackUpdate snapshot;
    try {
        AudioSource source;
        std::string source_error;
        if (!parse_audio_source(path, source, source_error)) {
            throw std::runtime_error(source_error);
        }
        path = source.path;
        if (path.front() == '/') {
            struct stat info{};
            if (stat(path.c_str(), &info) < 0 || !S_ISREG(info.st_mode) || access(path.c_str(), R_OK) < 0) {
                throw std::runtime_error(u8"音频文件不存在或无法读取");
            }
        }
        PlayerProcess process;
        process.launch();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stopping_ && !process.connect_ipc()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(u8"播放器启动超时");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (stopping_) {
            return;
        }
        int sent_volume = -1;
        bool sent_pause = false;
        auto load = command("loadfile", 1);
        add_string(load, path.c_str());
        process.send(std::move(load));
        int observer_id = 10;
        for (const auto* property : {"time-pos", "duration", "pause", "paused-for-cache"}) {
            auto observe = command("observe_property");
            add_number(observe, observer_id++);
            add_string(observe, property);
            process.send(std::move(observe));
        }
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        bool loaded = false;
        bool buffering = false;
        bool actual_pause = false;
        std::string buffer;
        while (!stopping_) {
            int volume;
            bool pause;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                volume = volume_;
                pause = paused_;
            }
            if (volume != sent_volume) {
                auto set = command("set_property");
                add_string(set, "volume");
                add_number(set, volume);
                process.send(std::move(set));
                sent_volume = volume;
            }
            if (pause != sent_pause) {
                auto set = command("set_property");
                add_string(set, "pause");
                if (!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(set.get(), "command"),
                                         cJSON_CreateBool(pause))) {
                    throw std::runtime_error(u8"播放器命令内存不足");
                }
                process.send(std::move(set));
                sent_pause = pause;
            }
            if ((!loaded || buffering) && std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(u8"音频加载超时，请检查网络或路径");
            }
            pollfd descriptor{process.fd, POLLIN, 0};
            const auto ready = poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready < 0) {
                throw std::runtime_error(u8"播放器通信失败");
            }
            if (ready == 0) {
                continue;
            }
            char chunk[4096];
            const auto count = read(process.fd, chunk, sizeof(chunk));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error(u8"播放器意外退出");
            }
            buffer.append(chunk, static_cast<size_t>(count));
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                const auto line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                Json message(cJSON_Parse(line.c_str()), cJSON_Delete);
                if (!message) {
                    throw std::runtime_error(u8"播放器响应格式错误");
                }
                const auto* id = cJSON_GetObjectItemCaseSensitive(message.get(), "request_id");
                if (cJSON_IsNumber(id) &&
                    std::strcmp(string_value(message.get(), "error"), "success") != 0) {
                    throw std::runtime_error(u8"播放器命令失败，请重试或检查音频");
                }
                const std::string event = string_value(message.get(), "event");
                if (event == "file-loaded") {
                    auto query = command("get_property", 2);
                    add_string(query, "track-list");
                    process.send(std::move(query));
                }
                if (cJSON_IsNumber(id) && id->valueint == 2) {
                    const auto* data = cJSON_GetObjectItemCaseSensitive(message.get(), "data");
                    bool audio = false;
                    const cJSON* track = nullptr;
                    cJSON_ArrayForEach(track, data) {
                        if (std::strcmp(string_value(track, "type"), "audio") == 0) {
                            audio = true;
                        }
                    }
                    if (!audio) {
                        throw std::runtime_error(u8"文件没有可播放音轨（请使用音频直链）");
                    }
                    loaded = true;
                    std::fprintf(stderr, "Cartoon: audio loaded.\n");
                }
                if (event == "end-file") {
                    const std::string reason = string_value(message.get(), "reason");
                    if (reason != "eof") {
                        throw std::runtime_error(u8"无法播放：网络、音频格式或输出设备错误");
                    }
                    snapshot.state = PlaybackState::Finished;
                    publish(snapshot);
                    std::fprintf(stderr, "Cartoon: audio finished.\n");
                    return;
                }
                if (event == "property-change") {
                    const std::string name = string_value(message.get(), "name");
                    const auto* data = cJSON_GetObjectItemCaseSensitive(message.get(), "data");
                    if (cJSON_IsNumber(data) && std::isfinite(data->valuedouble)) {
                        if (name == "time-pos") snapshot.position = std::max(0.0, data->valuedouble);
                        if (name == "duration") snapshot.duration = std::max(0.0, data->valuedouble);
                    }
                    if (name == "pause" && cJSON_IsBool(data)) {
                        actual_pause = cJSON_IsTrue(data);
                    }
                    if (name == "paused-for-cache" && cJSON_IsBool(data)) {
                        const bool next = cJSON_IsTrue(data);
                        if (next && !buffering) {
                            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
                        }
                        buffering = next;
                    }
                }
                snapshot.state = !loaded || buffering ? PlaybackState::Loading :
                    actual_pause ? PlaybackState::Paused : PlaybackState::Playing;
                publish(snapshot);
            }
            if (buffer.size() > 65536) {
                throw std::runtime_error(u8"播放器响应过长");
            }
        }
    } catch (const std::exception& error) {
        if (!stopping_) {
            snapshot.state = PlaybackState::Error;
            snapshot.error = error.what();
            std::fprintf(stderr, "Cartoon audio: %s\n", error.what());
            publish(snapshot);
        }
    }
}

} // namespace cartoon
