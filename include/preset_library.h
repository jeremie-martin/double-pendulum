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

    // Path to the loaded preset file (for saving back)
    std::string source_path;

    // Load preset library from TOML file
    static PresetLibrary load(std::string const& path);

    // Save preset library to TOML file
    bool save(std::string const& path) const;
    bool save() const { return !source_path.empty() && save(source_path); }

    // Add/update presets
    void setColor(std::string const& name, ColorParams const& params) { color[name] = params; }

    void setPostProcess(std::string const& name, PostProcessParams const& params) {
        post_process[name] = params;
    }

    // Get preset by name, returns nullopt if not found
    std::optional<ColorParams> getColor(std::string const& name) const {
        auto it = color.find(name);
        return it != color.end() ? std::optional{it->second} : std::nullopt;
    }

    std::optional<PostProcessParams> getPostProcess(std::string const& name) const {
        auto it = post_process.find(name);
        return it != post_process.end() ? std::optional{it->second} : std::nullopt;
    }

    // Get sorted list of preset names
    std::vector<std::string> getColorNames() const {
        std::vector<std::string> names;
        names.reserve(color.size());
        for (auto const& [name, _] : color) {
            names.push_back(name);
        }
        return names;
    }

    // Get color preset names filtered by scheme
    std::vector<std::string> getColorNamesForScheme(ColorScheme scheme) const {
        std::vector<std::string> names;
        for (auto const& [name, params] : color) {
            if (params.scheme == scheme) {
                names.push_back(name);
            }
        }
        return names;
    }

    std::vector<std::string> getPostProcessNames() const {
        std::vector<std::string> names;
        names.reserve(post_process.size());
        for (auto const& [name, _] : post_process) {
            names.push_back(name);
        }
        return names;
    }

    // Delete presets
    bool deleteColor(std::string const& name) {
        return color.erase(name) > 0;
    }

    bool deletePostProcess(std::string const& name) {
        return post_process.erase(name) > 0;
    }

    // Check if library has any presets
    bool empty() const { return color.empty() && post_process.empty(); }
};
