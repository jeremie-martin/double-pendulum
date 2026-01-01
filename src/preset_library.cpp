#include "preset_library.h"
#include "enum_utils.h"

#include <fstream>
#include <iostream>
#include <toml.hpp>

// All enum parsing/serialization now uses enum_utils

PresetLibrary PresetLibrary::load(std::string const& path) {
    PresetLibrary lib;
    lib.source_path = path;

    try {
        auto tbl = toml::parse_file(path);

        // Parse [color.*] presets
        if (auto color_tbl = tbl["color"].as_table()) {
            for (auto const& [name, node] : *color_tbl) {
                if (auto preset = node.as_table()) {
                    ColorParams params;

                    if (auto scheme = preset->get("scheme")) {
                        params.scheme =
                            enum_utils::fromString<ColorScheme>(
                                scheme->value<std::string>().value_or("spectrum"))
                                .value_or(ColorScheme::Spectrum);
                    }
                    if (auto start = preset->get("start")) {
                        params.start = start->value<double>().value_or(0.0);
                    }
                    if (auto end = preset->get("end")) {
                        params.end = end->value<double>().value_or(1.0);
                    }

                    lib.color[std::string(name)] = params;
                }
            }
        }

        // Parse [post_process.*] presets
        if (auto pp_tbl = tbl["post_process"].as_table()) {
            for (auto const& [name, node] : *pp_tbl) {
                if (auto preset = node.as_table()) {
                    PostProcessParams params;

                    if (auto tm = preset->get("tone_map")) {
                        params.tone_map =
                            enum_utils::fromString<ToneMapOperator>(
                                tm->value<std::string>().value_or("none"))
                                .value_or(ToneMapOperator::None);
                    }
                    if (auto wp = preset->get("reinhard_white_point")) {
                        params.reinhard_white_point = wp->value<double>().value_or(1.0);
                    }
                    if (auto exp = preset->get("exposure")) {
                        params.exposure = exp->value<double>().value_or(0.0);
                    }
                    if (auto con = preset->get("contrast")) {
                        params.contrast = con->value<double>().value_or(1.0);
                    }
                    if (auto gam = preset->get("gamma")) {
                        params.gamma = gam->value<double>().value_or(2.2);
                    }
                    if (auto norm = preset->get("normalization")) {
                        params.normalization =
                            enum_utils::fromString<NormalizationMode>(
                                norm->value<std::string>().value_or("per_frame"))
                                .value_or(NormalizationMode::PerFrame);
                    }

                    lib.post_process[std::string(name)] = params;
                }
            }
        }

        // Parse [theme.*] presets
        if (auto theme_tbl = tbl["theme"].as_table()) {
            for (auto const& [name, node] : *theme_tbl) {
                if (auto preset = node.as_table()) {
                    ThemePreset theme;

                    if (auto color_name = preset->get("color")) {
                        theme.color_preset_name = color_name->value<std::string>().value_or("");
                    }
                    if (auto pp_name = preset->get("post_process")) {
                        theme.post_process_preset_name = pp_name->value<std::string>().value_or("");
                    }

                    // Validate that referenced presets exist
                    if (!theme.color_preset_name.empty() &&
                        lib.color.find(theme.color_preset_name) == lib.color.end()) {
                        std::cerr << "Warning: Theme '" << name << "' references unknown color preset '"
                                  << theme.color_preset_name << "'\n";
                    }
                    if (!theme.post_process_preset_name.empty() &&
                        lib.post_process.find(theme.post_process_preset_name) == lib.post_process.end()) {
                        std::cerr << "Warning: Theme '" << name << "' references unknown post_process preset '"
                                  << theme.post_process_preset_name << "'\n";
                    }

                    lib.themes[std::string(name)] = theme;
                }
            }
        }

        std::cout << "Loaded preset library: " << lib.color.size() << " color, "
                  << lib.post_process.size() << " post_process, "
                  << lib.themes.size() << " theme presets\n";

    } catch (toml::parse_error const& err) {
        std::cerr << "Error parsing preset library: " << err.description() << "\n";
    }

    return lib;
}

bool PresetLibrary::save(std::string const& path) const {
    std::ofstream file(path);
    if (!file) {
        std::cerr << "Failed to open preset file for writing: " << path << "\n";
        return false;
    }

    file << "# Preset Library\n";
    file << "# Named presets for color schemes and post-processing\n\n";

    // Write color presets
    if (!color.empty()) {
        file << "# =============================================================================\n";
        file << "# Color Presets\n";
        file << "# "
                "=============================================================================\n\n";

        for (auto const& [name, params] : color) {
            file << "[color." << name << "]\n";
            file << "scheme = \"" << enum_utils::toString(params.scheme) << "\"\n";
            file << "start = " << params.start << "\n";
            file << "end = " << params.end << "\n\n";
        }
    }

    // Write post-process presets
    if (!post_process.empty()) {
        file << "# =============================================================================\n";
        file << "# Post-Processing Presets\n";
        file << "# "
                "=============================================================================\n\n";

        for (auto const& [name, params] : post_process) {
            file << "[post_process." << name << "]\n";
            file << "tone_map = \"" << enum_utils::toString(params.tone_map) << "\"\n";
            if (params.tone_map == ToneMapOperator::ReinhardExtended ||
                params.tone_map == ToneMapOperator::Logarithmic) {
                file << "reinhard_white_point = " << params.reinhard_white_point << "\n";
            }
            file << "exposure = " << params.exposure << "\n";
            file << "contrast = " << params.contrast << "\n";
            file << "gamma = " << params.gamma << "\n";
            file << "normalization = \"" << enum_utils::toString(params.normalization) << "\"\n\n";
        }
    }

    // Write theme presets
    if (!themes.empty()) {
        file << "# =============================================================================\n";
        file << "# Theme Presets\n";
        file << "# "
                "=============================================================================\n\n";

        for (auto const& [name, theme] : themes) {
            file << "[theme." << name << "]\n";
            file << "color = \"" << theme.color_preset_name << "\"\n";
            file << "post_process = \"" << theme.post_process_preset_name << "\"\n\n";
        }
    }

    std::cout << "Saved preset library to: " << path << "\n";
    return true;
}
