#include "audio_player.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void number(std::ofstream& output, unsigned value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) output.put(static_cast<char>((value >> (8 * i)) & 255));
}

void fixture(const std::string& path) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "Cannot create audio fixture");
    constexpr unsigned rate = 8000;
    constexpr unsigned samples = rate * 4;
    output.write("RIFF", 4); number(output, 36 + samples * 2, 4);
    output.write("WAVEfmt ", 8); number(output, 16, 4); number(output, 1, 2); number(output, 1, 2);
    number(output, rate, 4); number(output, rate * 2, 4); number(output, 2, 2); number(output, 16, 2);
    output.write("data", 4); number(output, samples * 2, 4);
    for (unsigned i = 0; i < samples; ++i) {
        const auto value = static_cast<int16_t>(2000 * std::sin(i * 2.0 * 3.141592653589793 * 220 / rate));
        number(output, static_cast<uint16_t>(value), 2);
    }
    require(static_cast<bool>(output), "Cannot write audio fixture");
}

cartoon::PlaybackUpdate wait(cartoon::AudioPlayer& player,
    const std::function<bool(const cartoon::PlaybackUpdate&)>& condition, int timeout = 20) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto update = player.take_update()) {
            if (update->state == cartoon::PlaybackState::Error) throw std::runtime_error(update->error);
            if (condition(*update)) return *update;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("Timed out waiting for real player state");
}

void playback(const std::string& source) {
    using namespace cartoon;
    AudioPlayer player;
    player.play(source, 5);
    const auto playing = wait(player, [](const PlaybackUpdate& update) {
        return update.state == PlaybackState::Playing && update.position > 0.2 && update.duration > 3.9;
    });
    player.set_paused(true);
    const auto paused = wait(player, [](const PlaybackUpdate& update) { return update.state == PlaybackState::Paused; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (auto update = player.take_update()) {
        require(update->state == PlaybackState::Paused && update->position <= paused.position + 0.15,
                "Paused audio must not advance");
    }
    player.set_volume(10);
    player.set_paused(false);
    wait(player, [paused](const PlaybackUpdate& update) {
        return update.state == PlaybackState::Playing && update.position > paused.position + 0.25;
    });
    player.play(source, 5);
    const auto replay = wait(player, [](const PlaybackUpdate& update) {
        return update.state == PlaybackState::Playing && update.duration > 0;
    });
    require(replay.position < 1, "Replay must reset the actual player position");
    wait(player, [](const PlaybackUpdate& update) { return update.state == PlaybackState::Finished; }, 8);
    player.stop();
    require(!player.take_update(), "Stopping must discard stale player events");
    std::printf("PASS: actual audio load, duration %.2f, pause/resume, replay, EOF and cleanup.\n", playing.duration);
}

} // namespace

int main(int argc, char** argv) {
    const auto path = argc > 1 && std::string(argv[1]).front() == '/' ? std::string(argv[1]) :
        (std::filesystem::temp_directory_path() / ("cartoon-audio-test-" + std::to_string(getpid()) + ".wav")).string();
    try {
        fixture(path);
        if (argc > 2 && std::string(argv[2]) == "--fixture-only") {
            std::puts("Fixture generated.");
            return 0;
        }
        playback(argc > 1 && std::string(argv[1]).front() != '/' ? argv[1] : path);
        cartoon::AudioPlayer missing;
        missing.play("/does-not-exist/cartoon.wav", 5);
        bool failed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto update = missing.take_update()) {
                failed = update->state == cartoon::PlaybackState::Error && !update->error.empty();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(failed, "Missing audio must report an explicit error");
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::filesystem::remove(path);
        return 1;
    }
}
