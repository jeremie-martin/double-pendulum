#pragma once

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct MusicTrack {
    std::string id;
    std::string title;
    fs::path filepath;
    int drop_time_ms;  // Time of the "drop" in milliseconds

    // Convert drop time to seconds
    double dropTimeSeconds() const { return drop_time_ms / 1000.0; }
};

class MusicManager {
public:
    // Load music database from directory containing database.json
    bool load(fs::path const& music_dir);

    // Get all available tracks
    std::vector<MusicTrack> const& tracks() const { return tracks_; }

    // Get track by ID
    std::optional<MusicTrack> getTrack(std::string const& id) const;

    // Get a random track
    MusicTrack const& randomTrack();

    // Check if any tracks are loaded
    bool hasTracts() const { return !tracks_.empty(); }
    size_t trackCount() const { return tracks_.size(); }

    // Mux video with audio, aligning boom frame with music drop
    // Returns true on success
    static bool muxWithAudio(
        fs::path const& video_path,
        fs::path const& audio_path,
        fs::path const& output_path,
        int boom_frame,
        int drop_time_ms,
        int video_fps
    );

private:
    std::vector<MusicTrack> tracks_;
    fs::path music_dir_;
    std::mt19937 rng_{std::random_device{}()};
};
