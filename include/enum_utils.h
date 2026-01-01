#pragma once

#include <algorithm>
#include <cctype>
#include <magic_enum/magic_enum.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Unified enum utilities using magic_enum
// Provides automatic string conversion for all enums

namespace enum_utils {

// Convert PascalCase to snake_case
// e.g., "DeepOcean" -> "deep_ocean"
inline std::string toSnakeCase(std::string_view pascal) {
    std::string result;
    result.reserve(pascal.size() + 4); // Room for underscores

    for (size_t i = 0; i < pascal.size(); ++i) {
        char c = pascal[i];
        if (std::isupper(c)) {
            if (i > 0) {
                result += '_';
            }
            result += static_cast<char>(std::tolower(c));
        } else {
            result += c;
        }
    }
    return result;
}

// Convert snake_case to PascalCase
// e.g., "deep_ocean" -> "DeepOcean"
inline std::string toPascalCase(std::string_view snake) {
    std::string result;
    result.reserve(snake.size());

    bool capitalize_next = true;
    for (char c : snake) {
        if (c == '_') {
            capitalize_next = true;
        } else if (capitalize_next) {
            result += static_cast<char>(std::toupper(c));
            capitalize_next = false;
        } else {
            result += static_cast<char>(std::tolower(c));
        }
    }
    return result;
}

// Get enum value as snake_case string (for TOML serialization)
template <typename E>
std::string toString(E value) {
    return toSnakeCase(magic_enum::enum_name(value));
}

// Get enum value as PascalCase string (for display)
template <typename E>
std::string toDisplayString(E value) {
    return std::string(magic_enum::enum_name(value));
}

// Parse enum from snake_case string (for TOML parsing)
template <typename E>
std::optional<E> fromString(std::string_view str) {
    // First try direct match (handles both PascalCase and snake_case input)
    auto direct = magic_enum::enum_cast<E>(str, magic_enum::case_insensitive);
    if (direct.has_value()) {
        return direct;
    }

    // Try converting snake_case to PascalCase
    std::string pascal = toPascalCase(str);
    return magic_enum::enum_cast<E>(pascal);
}

// Get all enum values
template <typename E>
constexpr auto values() {
    return magic_enum::enum_values<E>();
}

// Get count of enum values
template <typename E>
constexpr size_t count() {
    return magic_enum::enum_count<E>();
}

// Get all enum names as snake_case strings
template <typename E>
std::vector<std::string> names() {
    std::vector<std::string> result;
    result.reserve(magic_enum::enum_count<E>());
    for (auto value : magic_enum::enum_values<E>()) {
        result.push_back(toString(value));
    }
    return result;
}

// Get all enum names as display strings (PascalCase)
template <typename E>
std::vector<std::string> displayNames() {
    std::vector<std::string> result;
    result.reserve(magic_enum::enum_count<E>());
    for (auto value : magic_enum::enum_values<E>()) {
        result.push_back(std::string(magic_enum::enum_name(value)));
    }
    return result;
}

} // namespace enum_utils
