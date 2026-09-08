#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

// Allocation-free framing of a JSON array containing object elements.
// JSON semantic validation remains the responsibility of cJSON.
template <typename Callback>
bool ForEachJsonObject(std::string_view input, Callback&& callback) {
    size_t pos = 0;
    auto space = [&]() { while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos; };
    space();
    if (pos == input.size() || input[pos++] != '[') return false;
    space();
    if (pos < input.size() && input[pos] == ']') { ++pos; space(); return pos == input.size(); }
    for (;;) {
        space();
        if (pos == input.size() || input[pos] != '{') return false;
        const size_t begin = pos++;
        int object_depth = 1;
        int array_depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (; pos < input.size(); ++pos) {
            const char c = input[pos];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == '{') ++object_depth;
            else if (c == '}') {
                if (--object_depth == 0 && array_depth == 0) {
                    ++pos;
                    if (!callback(input.substr(begin, pos - begin))) return false;
                    break;
                }
            } else if (c == '[') ++array_depth;
            else if (c == ']') {
                if (array_depth == 0) return false;
                --array_depth;
            }
        }
        if (object_depth != 0 || array_depth != 0 || in_string || escaped) return false;
        space();
        if (pos == input.size()) return false;
        if (input[pos] == ']') { ++pos; space(); return pos == input.size(); }
        if (input[pos++] != ',') return false;
    }
}
