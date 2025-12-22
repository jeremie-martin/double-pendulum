#pragma once

#include "config.h"

#include <cstdint>
#include <cstdio>
#include <string>

class VideoWriter {
public:
    VideoWriter(int width, int height, int fps, OutputParams const& params);
    ~VideoWriter();

    bool open(std::string const& output_path);
    bool writeFrame(uint8_t const* rgb_data);
    bool close();
    bool isOpen() const { return pipe_ != nullptr; }

    // Non-copyable
    VideoWriter(VideoWriter const&) = delete;
    VideoWriter& operator=(VideoWriter const&) = delete;

    // Movable
    VideoWriter(VideoWriter&& other) noexcept;

private:
    int width_, height_, fps_;
    std::string codec_;
    int crf_;
    FILE* pipe_ = nullptr;
};
