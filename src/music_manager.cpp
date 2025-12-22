#include "music_manager.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

// Simple JSON parsing for our specific format
namespace {

std::string trim(std::string const& s) {
    auto start = s.find_first_not_of(" \t\n\r\"");
    if (start == std::string::npos)
        return "";
    auto end = s.find_last_not_of(" \t\n\r\"");
    return s.substr(start, end - start + 1);
}

std::string extractValue(std::string const& line, std::string const& key) {
    auto pos = line.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return "";

    auto colon = line.find(':', pos);
    if (colon == std::string::npos)
        return "";

    auto value_start = line.find_first_not_of(" \t", colon + 1);
    if (value_start == std::string::npos)
        return "";

    // Check if it's a number or string
    if (line[value_start] == '"') {
        auto value_end = line.find('"', value_start + 1);
        if (value_end == std::string::npos)
            return "";
        return line.substr(value_start + 1, value_end - value_start - 1);
    } else {
        // Number - find end (comma or end of line)
        auto value_end = line.find_first_of(",\n\r}", value_start);
        if (value_end == std::string::npos)
            value_end = line.length();
        return trim(line.substr(value_start, value_end - value_start));
    }
}

} // namespace

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

    // Read entire file
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Parse tracks array (simple parsing for our specific format)
    size_t pos = 0;
    while ((pos = content.find('{', pos)) != std::string::npos) {
        auto end = content.find('}', pos);
        if (end == std::string::npos)
            break;

        std::string track_block = content.substr(pos, end - pos + 1);

        MusicTrack track;
        track.id = extractValue(track_block, "id");
        track.title = extractValue(track_block, "title");

        std::string filepath = extractValue(track_block, "filepath");
        if (!filepath.empty()) {
            track.filepath = music_dir / filepath;
        }

        std::string drop_time = extractValue(track_block, "drop_time_ms");
        if (!drop_time.empty()) {
            track.drop_time_ms = std::stoi(drop_time);
        }

        // Validate track has required fields
        if (!track.id.empty() && !track.filepath.empty() && track.drop_time_ms > 0) {
            tracks_.push_back(track);
        }

        pos = end + 1;
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
