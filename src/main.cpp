#include "batch_generator.h"
#include "music_manager.h"
#include "simulation.h"

#ifdef ENABLE_GPU
#include "gl_renderer.h"
#include "headless_gl.h"
#endif

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

void printUsage(char const* program) {
    std::cout << "Double Pendulum Simulation\n\n"
              << "Usage:\n"
              << "  " << program << " [config.toml]           Run simulation (CPU)\n"
#ifdef ENABLE_GPU
              << "  " << program << " [config.toml] --gpu     Run simulation (GPU)\n"
#endif
              << "  " << program << " [config.toml] --music <track-id|random>\n"
              << "                                      Run simulation with music\n"
              << "  " << program << " --batch <batch.toml>    Run batch generation\n"
              << "  " << program << " --batch <batch.toml> --resume\n"
              << "                                      Resume batch generation\n"
              << "  " << program << " --add-music <video> <track-id> <boom-frame> <fps>\n"
              << "                                      Add music to existing video\n"
              << "  " << program << " --list-tracks           List available music tracks\n"
              << "  " << program << " -h, --help              Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " config/default.toml\n"
#ifdef ENABLE_GPU
              << "  " << program << " config/default.toml --gpu\n"
#endif
              << "  " << program << " config/default.toml --music random\n"
              << "  " << program << " config/default.toml --music petrunko\n"
              << "  " << program << " --batch config/batch.toml\n"
              << "  " << program << " --batch config/batch.toml --resume\n"
              << "  " << program << " --add-music output/run_xxx/video.mp4 petrunko 32 60\n"
              << "  " << program << " --list-tracks\n";
}

int runSimulation(std::string const& config_path, std::string const& music_track = "") {
    // Load configuration
    std::cout << "Loading config from: " << config_path << "\n";
    Config config = Config::load(config_path);

    // Calculate video duration
    double video_duration =
        static_cast<double>(config.simulation.total_frames) / config.output.video_fps;

    // Print config summary
    std::cout << "\nSimulation parameters:\n"
              << "  Pendulums: " << config.simulation.pendulum_count << "\n"
              << "  Physics duration: " << config.simulation.duration_seconds << "s\n"
              << "  Frames: " << config.simulation.total_frames << "\n"
              << "  Video: " << video_duration << "s @ " << config.output.video_fps << " FPS\n"
              << "  Resolution: " << config.render.width << "x" << config.render.height << "\n"
              << "  Output: " << config.output.directory << "/\n";

    if (!music_track.empty()) {
        std::cout << "  Music: " << music_track << "\n";
    }
    std::cout << "\n";

    // Run simulation
    Simulation sim(config);
    auto results = sim.run();

    // Add music if requested and we have a video with boom_frame
    if (!music_track.empty() && !results.video_path.empty() && results.boom_frame) {
        MusicManager music;
        if (!music.load("music")) {
            std::cerr << "Failed to load music database\n";
            return 1;
        }

        std::optional<MusicTrack> track;
        if (music_track == "random") {
            if (music.trackCount() > 0) {
                track = music.randomTrack();
            }
        } else {
            track = music.getTrack(music_track);
        }

        if (!track) {
            std::cerr << "Track not found: " << music_track << "\n";
            std::cerr << "Use --list-tracks to see available tracks\n";
            return 1;
        }

        std::filesystem::path video_path = results.video_path;
        std::filesystem::path output_path =
            video_path.parent_path() / (video_path.stem().string() + "_with_music.mp4");

        std::cout << "\nAdding music: " << track->title << "\n";
        if (MusicManager::muxWithAudio(video_path, track->filepath, output_path,
                                       *results.boom_frame, track->drop_time_ms,
                                       config.output.video_fps)) {
            // Replace original with muxed version
            std::filesystem::remove(video_path);
            std::filesystem::rename(output_path, video_path);
            std::cout << "Music added successfully!\n";
        } else {
            std::cerr << "Failed to add music\n";
            return 1;
        }
    }

    return 0;
}

#ifdef ENABLE_GPU
#include "color_scheme.h"
#include "pendulum.h"
#include "post_process.h"
#include "variance_tracker.h"
#include "video_writer.h"

#include <chrono>
#include <iomanip>
#include <png.h>
#include <sstream>
#include <thread>

namespace {
void savePNG(char const* path, uint8_t const* data, int width, int height) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);

    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(height);
    for (int y = 0; y < height; ++y) {
        rows[y] = const_cast<png_bytep>(data + y * width * 3);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);

    png_destroy_write_struct(&png, &info);
    fclose(fp);
}
} // namespace

int runGPUSimulation(std::string const& config_path) {
    using Clock = std::chrono::high_resolution_clock;
    using Duration = std::chrono::duration<double>;

    std::cout << "Loading config from: " << config_path << "\n";
    Config config = Config::load(config_path);

    int const pendulum_count = config.simulation.pendulum_count;
    int const total_frames = config.simulation.total_frames;
    int const substeps = config.simulation.substeps_per_frame;
    double const dt = config.simulation.dt();
    int const width = config.render.width;
    int const height = config.render.height;

    double video_duration = static_cast<double>(total_frames) / config.output.video_fps;

    std::cout << "\nGPU Simulation parameters:\n"
              << "  Pendulums: " << pendulum_count << "\n"
              << "  Physics duration: " << config.simulation.duration_seconds << "s\n"
              << "  Frames: " << total_frames << "\n"
              << "  Video: " << video_duration << "s @ " << config.output.video_fps << " FPS\n"
              << "  Resolution: " << width << "x" << height << "\n"
              << "  Renderer: GPU (EGL headless)\n\n";

    // Initialize headless OpenGL
    HeadlessGL gl;
    if (!gl.init()) {
        std::cerr << "Failed to initialize headless OpenGL\n";
        return 1;
    }

    // Initialize GPU renderer
    GLRenderer renderer;
    if (!renderer.init(width, height)) {
        std::cerr << "Failed to initialize GL renderer\n";
        return 1;
    }

    // Create output directory
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    std::ostringstream dir_name;
    dir_name << config.output.directory << "/run_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_gpu";
    std::string run_dir = dir_name.str();
    std::filesystem::create_directories(run_dir);

    if (config.output.format == OutputFormat::PNG) {
        std::filesystem::create_directories(run_dir + "/frames");
    }

    std::cout << "Output directory: " << run_dir << "\n\n";

    // Timing
    Duration physics_time{0}, render_time{0}, io_time{0};
    auto total_start = Clock::now();

    // Thread count
    int thread_count = config.render.thread_count;
    if (thread_count <= 0) {
        thread_count = std::thread::hardware_concurrency();
    }

    // Initialize pendulums
    std::vector<Pendulum> pendulums(pendulum_count);
    double center_angle = config.physics.initial_angle1;
    double variation = config.simulation.angle_variation;
    for (int i = 0; i < pendulum_count; ++i) {
        double t = (pendulum_count > 1) ? static_cast<double>(i) / (pendulum_count - 1) : 0.0;
        double th1 = center_angle - variation / 2 + t * variation;
        pendulums[i] = Pendulum(config.physics.gravity, config.physics.length1, config.physics.length2,
                                config.physics.mass1, config.physics.mass2, th1,
                                config.physics.initial_angle2, config.physics.initial_velocity1,
                                config.physics.initial_velocity2);
    }

    // Pre-compute colors
    ColorSchemeGenerator color_gen(config.color);
    std::vector<Color> colors(pendulum_count);
    for (int i = 0; i < pendulum_count; ++i) {
        colors[i] = color_gen.getColorForIndex(i, pendulum_count);
    }

    // Setup video writer
    std::unique_ptr<VideoWriter> video_writer;
    if (config.output.format == OutputFormat::Video) {
        std::string video_path = run_dir + "/video.mp4";
        video_writer = std::make_unique<VideoWriter>(width, height, config.output.video_fps, config.output);
        if (!video_writer->open(video_path)) {
            std::cerr << "Failed to open video writer\n";
            return 1;
        }
    }

    // Buffers
    std::vector<PendulumState> states(pendulum_count);
    std::vector<uint8_t> rgb_buffer(width * height * 3);

    // Coordinate transform
    int centerX = width / 2;
    int centerY = height / 2;
    double scale = width / 5.0;

    // Variance tracking
    VarianceTracker variance_tracker;

    // Main loop
    for (int frame = 0; frame < total_frames; ++frame) {
        // Physics
        auto physics_start = Clock::now();
        int chunk_size = pendulum_count / thread_count;
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (int t = 0; t < thread_count; ++t) {
            int start = t * chunk_size;
            int end = (t == thread_count - 1) ? pendulum_count : start + chunk_size;
            threads.emplace_back([&, start, end]() {
                for (int i = start; i < end; ++i) {
                    for (int s = 0; s < substeps; ++s) {
                        states[i] = pendulums[i].step(dt);
                    }
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        physics_time += Clock::now() - physics_start;

        // Variance tracking
        std::vector<double> angles;
        angles.reserve(pendulum_count);
        for (auto const& state : states) {
            angles.push_back(state.th2);
        }
        variance_tracker.update(angles);

        // Render (GPU)
        auto render_start = Clock::now();
        renderer.clear();

        for (int i = 0; i < pendulum_count; ++i) {
            auto const& state = states[i];
            auto const& color = colors[i];

            float x0 = static_cast<float>(centerX);
            float y0 = static_cast<float>(centerY);
            float x1 = static_cast<float>(centerX + state.x1 * scale);
            float y1 = static_cast<float>(centerY + state.y1 * scale);
            float x2 = static_cast<float>(centerX + state.x2 * scale);
            float y2 = static_cast<float>(centerY + state.y2 * scale);

            renderer.drawLine(x0, y0, x1, y1, color.r, color.g, color.b);
            renderer.drawLine(x1, y1, x2, y2, color.r, color.g, color.b);
        }

        // Read pixels with full post-processing pipeline
        renderer.readPixels(rgb_buffer,
                            static_cast<float>(config.post_process.exposure),
                            static_cast<float>(config.post_process.contrast),
                            static_cast<float>(config.post_process.gamma),
                            config.post_process.tone_map,
                            static_cast<float>(config.post_process.reinhard_white_point));
        render_time += Clock::now() - render_start;

        // I/O
        auto io_start = Clock::now();
        if (config.output.format == OutputFormat::Video) {
            video_writer->writeFrame(rgb_buffer.data());
        } else {
            std::ostringstream path;
            path << run_dir << "/frames/" << config.output.filename_prefix << std::setfill('0')
                 << std::setw(4) << frame << ".png";
            savePNG(path.str().c_str(), rgb_buffer.data(), width, height);
        }
        io_time += Clock::now() - io_start;

        std::cout << "\rFrame " << std::setw(4) << (frame + 1) << "/" << total_frames << std::flush;
    }

    if (video_writer) {
        auto io_start = Clock::now();
        video_writer->close();
        io_time += Clock::now() - io_start;
    }

    auto total_time = Duration(Clock::now() - total_start).count();

    // Results
    std::cout << "\n\n=== GPU Simulation Complete ===\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Frames:      " << total_frames << "\n";
    std::cout << "Video:       " << video_duration << "s @ " << config.output.video_fps << " FPS\n";
    std::cout << "Pendulums:   " << pendulum_count << "\n";
    std::cout << "Total time:  " << total_time << "s\n";
    std::cout << "  Physics:   " << std::setw(5) << physics_time.count() << "s ("
              << std::setw(4) << (physics_time.count() / total_time * 100) << "%)\n";
    std::cout << "  Render:    " << std::setw(5) << render_time.count() << "s ("
              << std::setw(4) << (render_time.count() / total_time * 100) << "%)\n";
    std::cout << "  I/O:       " << std::setw(5) << io_time.count() << "s ("
              << std::setw(4) << (io_time.count() / total_time * 100) << "%)\n";
    std::cout << "\nOutput: " << run_dir << "\n";

    return 0;
}
#endif // ENABLE_GPU

int listTracks() {
    MusicManager music;
    if (!music.load("music")) {
        std::cerr << "Failed to load music database\n";
        return 1;
    }

    std::cout << "\nAvailable tracks:\n";
    for (auto const& track : music.tracks()) {
        std::cout << "  " << track.id << "\n"
                  << "    Title: " << track.title << "\n"
                  << "    File: " << track.filepath << "\n"
                  << "    Drop: " << track.drop_time_ms << "ms (" << track.dropTimeSeconds()
                  << "s)\n\n";
    }

    return 0;
}

int addMusic(std::string const& video_path, std::string const& track_id, int boom_frame, int fps) {
    MusicManager music;
    if (!music.load("music")) {
        std::cerr << "Failed to load music database\n";
        return 1;
    }

    auto track = music.getTrack(track_id);
    if (!track) {
        std::cerr << "Track not found: " << track_id << "\n";
        std::cerr << "Use --list-tracks to see available tracks\n";
        return 1;
    }

    // Generate output path
    std::filesystem::path video(video_path);
    std::filesystem::path output =
        video.parent_path() / (video.stem().string() + "_with_music" + video.extension().string());

    bool success = MusicManager::muxWithAudio(video_path, track->filepath, output, boom_frame,
                                              track->drop_time_ms, fps);

    return success ? 0 : 1;
}

int runBatch(std::string const& batch_config_path, bool resume) {
    std::cout << "Loading batch config from: " << batch_config_path << "\n";
    BatchConfig config = BatchConfig::load(batch_config_path);

    BatchGenerator generator(config);

    if (resume) {
        generator.resume();
    } else {
        generator.run();
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Default: run simulation with default config
        return runSimulation("config/default.toml");
    }

    std::string arg = argv[1];

    if (arg == "-h" || arg == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    if (arg == "--list-tracks") {
        return listTracks();
    }

    if (arg == "--batch") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " --batch <batch.toml> [--resume]\n";
            return 1;
        }
        std::string batch_config = argv[2];
        bool resume = (argc >= 4 && std::string(argv[3]) == "--resume");
        return runBatch(batch_config, resume);
    }

    if (arg == "--add-music") {
        if (argc < 6) {
            std::cerr << "Usage: " << argv[0]
                      << " --add-music <video> <track-id> <boom-frame> <fps>\n";
            return 1;
        }
        std::string video = argv[2];
        std::string track_id = argv[3];
        int boom_frame = std::stoi(argv[4]);
        int fps = std::stoi(argv[5]);
        return addMusic(video, track_id, boom_frame, fps);
    }

    // Otherwise, treat argument as config path
    // Check for options
    std::string music_track;
    bool use_gpu = false;

    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--music" && i + 1 < argc) {
            music_track = argv[i + 1];
        }
        if (std::string(argv[i]) == "--gpu") {
            use_gpu = true;
        }
    }

#ifdef ENABLE_GPU
    if (use_gpu) {
        return runGPUSimulation(arg);
    }
#else
    if (use_gpu) {
        std::cerr << "GPU rendering not available. Rebuild with -DENABLE_GPU=ON\n";
        return 1;
    }
#endif

    return runSimulation(arg, music_track);
}
