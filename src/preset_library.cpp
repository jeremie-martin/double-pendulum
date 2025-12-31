#include "preset_library.h"

#include <fstream>
#include <iostream>
#include <toml.hpp>

namespace {

ColorScheme parseColorScheme(std::string const& str) {
    // Original
    if (str == "spectrum")
        return ColorScheme::Spectrum;
    if (str == "rainbow")
        return ColorScheme::Rainbow;
    if (str == "heat")
        return ColorScheme::Heat;
    if (str == "cool")
        return ColorScheme::Cool;
    if (str == "monochrome")
        return ColorScheme::Monochrome;
    if (str == "plasma")
        return ColorScheme::Plasma;
    if (str == "viridis")
        return ColorScheme::Viridis;
    if (str == "inferno")
        return ColorScheme::Inferno;
    if (str == "sunset")
        return ColorScheme::Sunset;

    // New gradient-based
    if (str == "ember")
        return ColorScheme::Ember;
    if (str == "deep_ocean")
        return ColorScheme::DeepOcean;
    if (str == "neon_violet")
        return ColorScheme::NeonViolet;
    if (str == "aurora")
        return ColorScheme::Aurora;
    if (str == "pearl")
        return ColorScheme::Pearl;
    if (str == "turbo_pop")
        return ColorScheme::TurboPop;
    if (str == "nebula")
        return ColorScheme::Nebula;
    if (str == "blackbody")
        return ColorScheme::Blackbody;
    if (str == "magma")
        return ColorScheme::Magma;
    if (str == "cyberpunk")
        return ColorScheme::Cyberpunk;
    if (str == "biolume")
        return ColorScheme::Biolume;
    if (str == "gold")
        return ColorScheme::Gold;
    if (str == "rose_gold")
        return ColorScheme::RoseGold;
    if (str == "twilight")
        return ColorScheme::Twilight;
    if (str == "forest_fire")
        return ColorScheme::ForestFire;

    // Curve-based
    if (str == "abyssal_glow")
        return ColorScheme::AbyssalGlow;
    if (str == "molten_core")
        return ColorScheme::MoltenCore;
    if (str == "iridescent")
        return ColorScheme::Iridescent;
    if (str == "stellar_nursery")
        return ColorScheme::StellarNursery;
    if (str == "whiskey_amber")
        return ColorScheme::WhiskeyAmber;

    return ColorScheme::Spectrum;
}

std::string colorSchemeToString(ColorScheme scheme) {
    switch (scheme) {
    case ColorScheme::Spectrum:
        return "spectrum";
    case ColorScheme::Rainbow:
        return "rainbow";
    case ColorScheme::Heat:
        return "heat";
    case ColorScheme::Cool:
        return "cool";
    case ColorScheme::Monochrome:
        return "monochrome";
    case ColorScheme::Plasma:
        return "plasma";
    case ColorScheme::Viridis:
        return "viridis";
    case ColorScheme::Inferno:
        return "inferno";
    case ColorScheme::Sunset:
        return "sunset";
    case ColorScheme::Ember:
        return "ember";
    case ColorScheme::DeepOcean:
        return "deep_ocean";
    case ColorScheme::NeonViolet:
        return "neon_violet";
    case ColorScheme::Aurora:
        return "aurora";
    case ColorScheme::Pearl:
        return "pearl";
    case ColorScheme::TurboPop:
        return "turbo_pop";
    case ColorScheme::Nebula:
        return "nebula";
    case ColorScheme::Blackbody:
        return "blackbody";
    case ColorScheme::Magma:
        return "magma";
    case ColorScheme::Cyberpunk:
        return "cyberpunk";
    case ColorScheme::Biolume:
        return "biolume";
    case ColorScheme::Gold:
        return "gold";
    case ColorScheme::RoseGold:
        return "rose_gold";
    case ColorScheme::Twilight:
        return "twilight";
    case ColorScheme::ForestFire:
        return "forest_fire";
    case ColorScheme::AbyssalGlow:
        return "abyssal_glow";
    case ColorScheme::MoltenCore:
        return "molten_core";
    case ColorScheme::Iridescent:
        return "iridescent";
    case ColorScheme::StellarNursery:
        return "stellar_nursery";
    case ColorScheme::WhiskeyAmber:
        return "whiskey_amber";
    }
    return "spectrum";
}

ToneMapOperator parseToneMapOperator(std::string const& str) {
    if (str == "none")
        return ToneMapOperator::None;
    if (str == "reinhard")
        return ToneMapOperator::Reinhard;
    if (str == "reinhard_extended")
        return ToneMapOperator::ReinhardExtended;
    if (str == "aces")
        return ToneMapOperator::ACES;
    if (str == "logarithmic")
        return ToneMapOperator::Logarithmic;
    return ToneMapOperator::None;
}

std::string toneMapToString(ToneMapOperator op) {
    switch (op) {
    case ToneMapOperator::None:
        return "none";
    case ToneMapOperator::Reinhard:
        return "reinhard";
    case ToneMapOperator::ReinhardExtended:
        return "reinhard_extended";
    case ToneMapOperator::ACES:
        return "aces";
    case ToneMapOperator::Logarithmic:
        return "logarithmic";
    }
    return "none";
}

NormalizationMode parseNormalizationMode(std::string const& str) {
    if (str == "per_frame")
        return NormalizationMode::PerFrame;
    if (str == "by_count")
        return NormalizationMode::ByCount;
    return NormalizationMode::PerFrame;
}

std::string normalizationToString(NormalizationMode mode) {
    switch (mode) {
    case NormalizationMode::PerFrame:
        return "per_frame";
    case NormalizationMode::ByCount:
        return "by_count";
    }
    return "per_frame";
}

} // namespace

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
                            parseColorScheme(scheme->value<std::string>().value_or("spectrum"));
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
                            parseToneMapOperator(tm->value<std::string>().value_or("none"));
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
                        params.normalization = parseNormalizationMode(
                            norm->value<std::string>().value_or("per_frame"));
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
            file << "scheme = \"" << colorSchemeToString(params.scheme) << "\"\n";
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
            file << "tone_map = \"" << toneMapToString(params.tone_map) << "\"\n";
            if (params.tone_map == ToneMapOperator::ReinhardExtended ||
                params.tone_map == ToneMapOperator::Logarithmic) {
                file << "reinhard_white_point = " << params.reinhard_white_point << "\n";
            }
            file << "exposure = " << params.exposure << "\n";
            file << "contrast = " << params.contrast << "\n";
            file << "gamma = " << params.gamma << "\n";
            file << "normalization = \"" << normalizationToString(params.normalization) << "\"\n\n";
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
