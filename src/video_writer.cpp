#include "video_writer.h"

#include <sstream>

VideoWriter::VideoWriter(int width, int height, int fps, OutputParams const& params)
    : width_(width), height_(height), fps_(fps), codec_(params.video_codec),
      crf_(params.video_crf) {}

VideoWriter::~VideoWriter() {
    close();
}

bool VideoWriter::open(std::string const& output_path) {
    std::ostringstream cmd;
    cmd << "ffmpeg -y "
        << "-f rawvideo "
        << "-pix_fmt rgb24 "
        << "-s " << width_ << "x" << height_ << " "
        << "-r " << fps_ << " "
        << "-i - " // Read from stdin
        << "-c:v " << codec_ << " "
        << "-pix_fmt yuv420p "
        << "-crf " << crf_ << " "
        << "\"" << output_path << "\" "
        << "2>/dev/null"; // Suppress ffmpeg output

    pipe_ = popen(cmd.str().c_str(), "w");
    return pipe_ != nullptr;
}

bool VideoWriter::writeFrame(uint8_t const* rgb_data) {
    if (!pipe_)
        return false;

    size_t frame_size = static_cast<size_t>(width_) * height_ * 3;
    size_t written = fwrite(rgb_data, 1, frame_size, pipe_);
    return written == frame_size;
}

bool VideoWriter::close() {
    if (pipe_) {
        int result = pclose(pipe_);
        pipe_ = nullptr;
        return result == 0;
    }
    return true;
}

VideoWriter::VideoWriter(VideoWriter&& other) noexcept
    : width_(other.width_), height_(other.height_), fps_(other.fps_),
      codec_(std::move(other.codec_)), crf_(other.crf_), pipe_(other.pipe_) {
    other.pipe_ = nullptr;
}
