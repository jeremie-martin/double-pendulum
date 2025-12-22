#include "preset_library.h"

#include <iostream>
#include <toml.hpp>

namespace {

ColorScheme parseColorScheme(std::string const& str) {
    if (str == "spectrum") return ColorScheme::Spectrum;
    if (str == "rainbow") return ColorScheme::Rainbow;
    if (str == "heat") return ColorScheme::Heat;
    if (str == "cool") return ColorScheme::Cool;
    if (str == "monochrome") return ColorScheme::Monochrome;
    return ColorScheme::Spectrum;
}

ToneMapOperator parseToneMapOperator(std::string const& str) {
    if (str == "none") return ToneMapOperator::None;
    if (str == "reinhard") return ToneMapOperator::Reinhard;
    if (str == "reinhard_extended") return ToneMapOperator::ReinhardExtended;
    if (str == "aces") return ToneMapOperator::ACES;
    if (str == "logarithmic") return ToneMapOperator::Logarithmic;
    return ToneMapOperator::None;
}

NormalizationMode parseNormalizationMode(std::string const& str) {
    if (str == "per_frame") return NormalizationMode::PerFrame;
    if (str == "by_count") return NormalizationMode::ByCount;
    return NormalizationMode::PerFrame;
}

} // namespace

PresetLibrary PresetLibrary::load(std::string const& path) {
    PresetLibrary lib;

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
                        params.normalization =
                            parseNormalizationMode(norm->value<std::string>().value_or("per_frame"));
                    }

                    lib.post_process[std::string(name)] = params;
                }
            }
        }

        std::cout << "Loaded preset library: " << lib.color.size() << " color, "
                  << lib.post_process.size() << " post_process presets\n";

    } catch (toml::parse_error const& err) {
        std::cerr << "Error parsing preset library: " << err.description() << "\n";
    }

    return lib;
}
