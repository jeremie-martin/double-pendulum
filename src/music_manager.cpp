#include "music_manager.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <json.hpp>
#include <sstream>

using json = nlohmann::json;

bool MusicManager::load(fs::path const& music_dir) {
    music_dir_ = music_dir;
    tracks_.clear();

    fs::path db_path = music_dir / "database.json";
    if (!fs::exists(db_path)) {
        std::cerr << "Music database not found: " << db_path << "\n";
        return false;
    }

    std::ifstream file(db_path);
    if (!file) {
        std::cerr << "Failed to open music database: " << db_path << "\n";
        return false;
    }

    try {
        json data = json::parse(file);

        // Handle both array and object with "tracks" key
        json tracks_array;
        if (data.is_array()) {
            tracks_array = data;
        } else if (data.contains("tracks") && data["tracks"].is_array()) {
            tracks_array = data["tracks"];
        } else {
            std::cerr << "Invalid music database format\n";
            return false;
        }

        for (const auto& track_json : tracks_array) {
            MusicTrack track;

            if (track_json.contains("id") && track_json["id"].is_string()) {
                track.id = track_json["id"].get<std::string>();
            }
            if (track_json.contains("title") && track_json["title"].is_string()) {
                track.title = track_json["title"].get<std::string>();
            }
            if (track_json.contains("filepath") && track_json["filepath"].is_string()) {
                track.filepath = music_dir / track_json["filepath"].get<std::string>();
            }
            if (track_json.contains("drop_time_ms") && track_json["drop_time_ms"].is_number()) {
                track.drop_time_ms = track_json["drop_time_ms"].get<int>();
            }

            // Validate track has required fields
            if (!track.id.empty() && !track.filepath.empty() && track.drop_time_ms > 0) {
                tracks_.push_back(track);
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "Error parsing music database: " << e.what() << "\n";
        return false;
    }

    std::cout << "Loaded " << tracks_.size() << " music tracks\n";
    return !tracks_.empty();
}

std::optional<MusicTrack> MusicManager::getTrack(std::string const& id) const {
    for (auto const& track : tracks_) {
        if (track.id == id) {
            return track;
        }
    }
    return std::nullopt;
}

MusicTrack const& MusicManager::randomTrack() {
    if (tracks_.empty()) {
        throw std::runtime_error("No tracks loaded");
    }
    std::uniform_int_distribution<size_t> dist(0, tracks_.size() - 1);
    return tracks_[dist(rng_)];
}

bool MusicManager::muxWithAudio(fs::path const& video_path, fs::path const& audio_path,
                                fs::path const& output_path, int boom_frame, int drop_time_ms,
                                int video_fps) {
    if (!fs::exists(video_path)) {
        std::cerr << "Video file not found: " << video_path << "\n";
        return false;
    }
    if (!fs::exists(audio_path)) {
        std::cerr << "Audio file not found: " << audio_path << "\n";
        return false;
    }

    // Calculate audio offset to align boom with drop
    // boom_time_video = boom_frame / video_fps (seconds)
    // drop_time_audio = drop_time_ms / 1000 (seconds)
    // We want: audio_position = video_position + offset
    // At boom: audio should be at drop_time
    // So: drop_time = boom_time + offset
    // offset = drop_time - boom_time

    double boom_time = static_cast<double>(boom_frame) / video_fps;
    double drop_time = static_cast<double>(drop_time_ms) / 1000.0;
    double audio_offset = drop_time - boom_time;

    std::cout << "Muxing video with audio:\n"
              << "  Video: " << video_path << "\n"
              << "  Audio: " << audio_path << "\n"
              << "  Boom frame: " << boom_frame << " (" << boom_time << "s)\n"
              << "  Drop time: " << drop_time << "s\n"
              << "  Audio offset: " << audio_offset << "s\n";

    // Build FFmpeg command
    std::ostringstream cmd;
    cmd << "ffmpeg -y ";

    if (audio_offset >= 0) {
        // Audio starts after video starts - seek into audio
        cmd << "-i \"" << video_path.string() << "\" "
            << "-ss " << audio_offset << " "
            << "-i \"" << audio_path.string() << "\" ";
    } else {
        // Audio starts before video - delay video or pad audio
        // For simplicity, we'll start audio at beginning and accept slight misalignment
        // A more sophisticated approach would pad the video with black frames
        cmd << "-i \"" << video_path.string() << "\" "
            << "-i \"" << audio_path.string() << "\" ";
        std::cerr << "Warning: Audio drop comes before boom frame. "
                  << "Audio will start at beginning of video.\n";
    }

    cmd << "-c:v copy -c:a aac -map 0:v:0 -map 1:a:0 -shortest "
        << "\"" << output_path.string() << "\" 2>&1";

    std::cout << "Running: " << cmd.str() << "\n";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        std::cerr << "FFmpeg muxing failed with code " << result << "\n";
        return false;
    }

    std::cout << "Output: " << output_path << "\n";
    return true;
}
