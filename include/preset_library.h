#pragma once

#include "config.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

// Preset library loaded from TOML file
// Contains named presets for each config category
struct PresetLibrary {
    // Named presets for each category
    std::map<std::string, ColorParams> color;
    std::map<std::string, PostProcessParams> post_process;

    // Load preset library from TOML file
    static PresetLibrary load(std::string const& path);

    // Get preset by name, returns nullopt if not found
    std::optional<ColorParams> getColor(std::string const& name) const {
        auto it = color.find(name);
        return it != color.end() ? std::optional{it->second} : std::nullopt;
    }

    std::optional<PostProcessParams> getPostProcess(std::string const& name) const {
        auto it = post_process.find(name);
        return it != post_process.end() ? std::optional{it->second} : std::nullopt;
    }

    // Check if library has any presets
    bool empty() const { return color.empty() && post_process.empty(); }
};
