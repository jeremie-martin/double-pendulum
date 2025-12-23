#pragma once

#include "config.h"
#include "pendulum.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Binary format for saving/loading raw simulation data
// Enables fast metric iteration without re-running physics

namespace simulation_data {

// Magic number: "PNDL" + version bytes
constexpr char MAGIC[8] = {'P', 'N', 'D', 'L', 0x01, 0x00, 0x00, 0x00};
constexpr uint32_t FORMAT_VERSION = 1;

// Header structure (fixed size for easy seeking)
// All multi-byte values are little-endian
// Using pragma pack to ensure no padding for binary compatibility
#pragma pack(push, 1)
struct Header {
    char magic[8];              // "PNDL\x01\x00\x00\x00"
    uint32_t format_version;    // Currently 1
    uint32_t pendulum_count;
    uint32_t frame_count;
    double duration_seconds;
    double max_dt;

    // Physics parameters (for validation/reproducibility)
    double gravity;
    double length1;
    double length2;
    double mass1;
    double mass2;
    double initial_angle1;
    double initial_angle2;
    double initial_velocity1;
    double initial_velocity2;
    double angle_variation;

    // Data layout info
    uint32_t floats_per_pendulum;  // Always 6: x1, y1, x2, y2, th1, th2
    uint64_t uncompressed_size;    // Total bytes of frame data before compression
    uint64_t compressed_size;      // Size of ZSTD-compressed payload

    uint8_t reserved[8];           // Padding for future use

    Header();
    void initFromConfig(Config const& config, uint32_t frame_count);
    bool validate() const;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 144, "Header must be exactly 144 bytes");

// Packed pendulum state for serialization (24 bytes per pendulum)
struct PackedState {
    float x1, y1, x2, y2;  // Cartesian positions
    float th1, th2;        // Angles (radians)

    PackedState() = default;
    explicit PackedState(PendulumState const& state);
    PendulumState toPendulumState() const;
};

static_assert(sizeof(PackedState) == 24, "PackedState must be 24 bytes");

// Writer for streaming frame data to disk with ZSTD compression
class Writer {
public:
    Writer();
    ~Writer();

    // Non-copyable
    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;

    // Initialize with output path and config
    bool open(std::filesystem::path const& path, Config const& config,
              uint32_t expected_frames);

    // Write a single frame's pendulum states
    void writeFrame(std::vector<PendulumState> const& states);

    // Finalize: compress and write to disk
    bool close();

    // Check if writer is open
    bool isOpen() const { return is_open_; }

    // Get number of frames written
    uint32_t framesWritten() const { return frames_written_; }

private:
    std::filesystem::path path_;
    Header header_;
    std::vector<float> buffer_;  // Accumulates all frame data
    uint32_t frames_written_ = 0;
    bool is_open_ = false;
};

// Reader for loading simulation data
class Reader {
public:
    Reader();
    ~Reader();

    // Load from file
    bool open(std::filesystem::path const& path);

    // Check if loaded
    bool isLoaded() const { return is_loaded_; }

    // Get header info
    Header const& header() const { return header_; }

    // Get frame count
    uint32_t frameCount() const { return header_.frame_count; }

    // Get pendulum count
    uint32_t pendulumCount() const { return header_.pendulum_count; }

    // Get frame data as PendulumState vector
    std::vector<PendulumState> getFrame(uint32_t frame) const;

    // Get packed frame data (more efficient for direct use)
    PackedState const* getFramePacked(uint32_t frame) const;

    // Extract angles for metric computation (most efficient for physics metrics)
    void getAnglesForFrame(uint32_t frame,
                           std::vector<double>& angle1s,
                           std::vector<double>& angle2s) const;

    // Get total data size in memory
    size_t memoryUsage() const;

private:
    Header header_;
    std::vector<float> data_;  // Decompressed frame data
    bool is_loaded_ = false;
};

// Utility: validate that physics parameters match between header and config
bool validatePhysicsMatch(Header const& header, Config const& config);

} // namespace simulation_data
