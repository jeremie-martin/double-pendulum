#include "simulation_data.h"

#include <zstd.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace simulation_data {

// Header implementation

Header::Header() {
    std::memset(this, 0, sizeof(Header));
    std::memcpy(magic, MAGIC, 8);
    format_version = FORMAT_VERSION;
    floats_per_pendulum = 6;
}

void Header::initFromConfig(Config const& config, uint32_t total_frames) {
    std::memcpy(magic, MAGIC, 8);
    format_version = FORMAT_VERSION;

    pendulum_count = static_cast<uint32_t>(config.simulation.pendulum_count);
    frame_count = total_frames;
    duration_seconds = config.simulation.duration_seconds;
    max_dt = config.simulation.max_dt;

    gravity = config.physics.gravity;
    length1 = config.physics.length1;
    length2 = config.physics.length2;
    mass1 = config.physics.mass1;
    mass2 = config.physics.mass2;
    initial_angle1 = config.physics.initial_angle1;
    initial_angle2 = config.physics.initial_angle2;
    initial_velocity1 = config.physics.initial_velocity1;
    initial_velocity2 = config.physics.initial_velocity2;
    angle_variation = config.simulation.angle_variation;

    floats_per_pendulum = 6;
    uncompressed_size = static_cast<uint64_t>(pendulum_count) * frame_count *
                        floats_per_pendulum * sizeof(float);
    compressed_size = 0;  // Set after compression
}

bool Header::validate() const {
    if (std::memcmp(magic, MAGIC, 4) != 0) {
        return false;  // Magic mismatch
    }
    if (format_version > FORMAT_VERSION) {
        return false;  // Unsupported version
    }
    if (pendulum_count == 0 || frame_count == 0) {
        return false;
    }
    if (floats_per_pendulum != 6) {
        return false;
    }
    return true;
}

// PackedState implementation

PackedState::PackedState(PendulumState const& state)
    : x1(static_cast<float>(state.x1))
    , y1(static_cast<float>(state.y1))
    , x2(static_cast<float>(state.x2))
    , y2(static_cast<float>(state.y2))
    , th1(static_cast<float>(state.th1))
    , th2(static_cast<float>(state.th2)) {}

PendulumState PackedState::toPendulumState() const {
    PendulumState state;
    state.x1 = static_cast<double>(x1);
    state.y1 = static_cast<double>(y1);
    state.x2 = static_cast<double>(x2);
    state.y2 = static_cast<double>(y2);
    state.th1 = static_cast<double>(th1);
    state.th2 = static_cast<double>(th2);
    return state;
}

// Writer implementation

Writer::Writer() = default;
Writer::~Writer() {
    if (is_open_) {
        close();
    }
}

bool Writer::open(std::filesystem::path const& path, Config const& config,
                  uint32_t expected_frames) {
    if (is_open_) {
        return false;
    }

    path_ = path;
    header_.initFromConfig(config, expected_frames);

    // Pre-allocate buffer for all frame data
    size_t total_floats = static_cast<size_t>(header_.pendulum_count) *
                          expected_frames * header_.floats_per_pendulum;
    buffer_.reserve(total_floats);

    frames_written_ = 0;
    is_open_ = true;
    return true;
}

void Writer::writeFrame(std::vector<PendulumState> const& states) {
    if (!is_open_) {
        return;
    }

    // Append packed state data to buffer
    for (auto const& state : states) {
        buffer_.push_back(static_cast<float>(state.x1));
        buffer_.push_back(static_cast<float>(state.y1));
        buffer_.push_back(static_cast<float>(state.x2));
        buffer_.push_back(static_cast<float>(state.y2));
        buffer_.push_back(static_cast<float>(state.th1));
        buffer_.push_back(static_cast<float>(state.th2));
    }

    frames_written_++;
}

bool Writer::close() {
    if (!is_open_) {
        return false;
    }

    is_open_ = false;

    // Update header with actual frame count
    header_.frame_count = frames_written_;
    header_.uncompressed_size = buffer_.size() * sizeof(float);

    // Compress data with ZSTD
    size_t const src_size = buffer_.size() * sizeof(float);
    size_t const max_dst_size = ZSTD_compressBound(src_size);
    std::vector<char> compressed(max_dst_size);

    size_t const compressed_size = ZSTD_compress(
        compressed.data(), max_dst_size, buffer_.data(), src_size,
        3  // Compression level (1-22, 3 is default)
    );

    if (ZSTD_isError(compressed_size)) {
        std::cerr << "ZSTD compression error: "
                  << ZSTD_getErrorName(compressed_size) << "\n";
        return false;
    }

    header_.compressed_size = compressed_size;

    // Write to file
    std::ofstream file(path_, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing: " << path_ << "\n";
        return false;
    }

    // Write header
    file.write(reinterpret_cast<char const*>(&header_), sizeof(Header));

    // Write compressed data
    file.write(compressed.data(), static_cast<std::streamsize>(compressed_size));

    if (!file.good()) {
        std::cerr << "Error writing simulation data file\n";
        return false;
    }

    // Print stats
    double compression_ratio =
        static_cast<double>(src_size) / static_cast<double>(compressed_size);
    std::cout << "Simulation data saved: " << path_ << "\n"
              << "  Frames: " << frames_written_ << "\n"
              << "  Pendulums: " << header_.pendulum_count << "\n"
              << "  Uncompressed: " << (src_size / (1024 * 1024)) << " MB\n"
              << "  Compressed: " << (compressed_size / (1024 * 1024)) << " MB\n"
              << "  Ratio: " << std::fixed << std::setprecision(2)
              << compression_ratio << "x\n";

    buffer_.clear();
    buffer_.shrink_to_fit();
    return true;
}

// Reader implementation

Reader::Reader() = default;
Reader::~Reader() = default;

bool Reader::open(std::filesystem::path const& path) {
    if (is_loaded_) {
        data_.clear();
        is_loaded_ = false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open simulation data file: " << path << "\n";
        return false;
    }

    // Read header
    file.read(reinterpret_cast<char*>(&header_), sizeof(Header));
    if (!file.good()) {
        std::cerr << "Failed to read header\n";
        return false;
    }

    if (!header_.validate()) {
        std::cerr << "Invalid simulation data file header\n";
        return false;
    }

    // Read compressed data
    std::vector<char> compressed(header_.compressed_size);
    file.read(compressed.data(), static_cast<std::streamsize>(header_.compressed_size));
    if (!file.good()) {
        std::cerr << "Failed to read compressed data\n";
        return false;
    }

    // Decompress
    size_t const dst_size = header_.uncompressed_size;
    data_.resize(dst_size / sizeof(float));

    size_t const decompressed_size = ZSTD_decompress(
        data_.data(), dst_size, compressed.data(), header_.compressed_size);

    if (ZSTD_isError(decompressed_size)) {
        std::cerr << "ZSTD decompression error: "
                  << ZSTD_getErrorName(decompressed_size) << "\n";
        return false;
    }

    if (decompressed_size != dst_size) {
        std::cerr << "Decompressed size mismatch\n";
        return false;
    }

    is_loaded_ = true;

    std::cout << "Loaded simulation data: " << path << "\n"
              << "  Frames: " << header_.frame_count << "\n"
              << "  Pendulums: " << header_.pendulum_count << "\n"
              << "  Duration: " << header_.duration_seconds << "s\n";

    return true;
}

std::vector<PendulumState> Reader::getFrame(uint32_t frame) const {
    std::vector<PendulumState> states;
    if (!is_loaded_ || frame >= header_.frame_count) {
        return states;
    }

    states.reserve(header_.pendulum_count);
    PackedState const* packed = getFramePacked(frame);

    for (uint32_t i = 0; i < header_.pendulum_count; ++i) {
        states.push_back(packed[i].toPendulumState());
    }

    return states;
}

PackedState const* Reader::getFramePacked(uint32_t frame) const {
    if (!is_loaded_ || frame >= header_.frame_count) {
        return nullptr;
    }

    size_t const floats_per_frame =
        static_cast<size_t>(header_.pendulum_count) * header_.floats_per_pendulum;
    size_t const offset = static_cast<size_t>(frame) * floats_per_frame;

    return reinterpret_cast<PackedState const*>(data_.data() + offset);
}

void Reader::getAnglesForFrame(uint32_t frame, std::vector<double>& angle1s,
                               std::vector<double>& angle2s) const {
    angle1s.clear();
    angle2s.clear();

    if (!is_loaded_ || frame >= header_.frame_count) {
        return;
    }

    angle1s.reserve(header_.pendulum_count);
    angle2s.reserve(header_.pendulum_count);

    PackedState const* packed = getFramePacked(frame);
    for (uint32_t i = 0; i < header_.pendulum_count; ++i) {
        angle1s.push_back(static_cast<double>(packed[i].th1));
        angle2s.push_back(static_cast<double>(packed[i].th2));
    }
}

size_t Reader::memoryUsage() const {
    return data_.size() * sizeof(float);
}

// Utility functions

bool validatePhysicsMatch(Header const& header, Config const& config) {
    constexpr double epsilon = 1e-9;

    auto approx_eq = [epsilon](double a, double b) {
        return std::abs(a - b) < epsilon;
    };

    if (!approx_eq(header.gravity, config.physics.gravity)) return false;
    if (!approx_eq(header.length1, config.physics.length1)) return false;
    if (!approx_eq(header.length2, config.physics.length2)) return false;
    if (!approx_eq(header.mass1, config.physics.mass1)) return false;
    if (!approx_eq(header.mass2, config.physics.mass2)) return false;

    return true;
}

} // namespace simulation_data
