#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace reader {
inline bool ValidUploadName(const std::string& name, bool font) {
    if (name.empty() || name.size() > 240 || name.front() == '.' ||
        name.find("..") != std::string::npos)
        return false;
    for (unsigned char ch : name)
        if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            return false;
    if (name.back() == ' ' || name.back() == '.')
        return false;
    auto normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    const std::string suffix = font ? ".ttf" : ".epub";
    return normalized.size() > suffix.size() &&
           normalized.compare(normalized.size() - suffix.size(), suffix.size(), suffix) == 0;
}
inline std::string DecodeUploadName(const std::string& encoded) {
    const auto hex = [](unsigned char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        ch = std::tolower(ch);
        return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    };
    std::string result;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%')
            result.push_back(encoded[i]);
        else {
            if (i + 2 >= encoded.size())
                return {};
            int a = hex(encoded[i + 1]), b = hex(encoded[i + 2]);
            if (a < 0 || b < 0)
                return {};
            result.push_back(static_cast<char>(a * 16 + b));
            i += 2;
        }
    }
    return result;
}
}  // namespace reader
