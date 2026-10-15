#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace han::reader {

struct EpubBook {
    std::string path;
    std::string file_name;
    std::string title;
    std::string author;
    std::string language;
    std::string package_path;
    std::string nav_path;
    std::vector<std::string> spine;
    std::string error;

    bool readable() const { return error.empty() && !spine.empty(); }
};

// A deliberately small ESP-IDF EPUB adapter inspired by CrossMux's Epub -> container.xml ->
// content.opf pipeline. It keeps the archive closed between operations so USB mass-storage mode
// can unmount the card safely.
class EpubReader {
public:
    static bool ParsePackage(const std::string& path, EpubBook& book);
    static bool ReadSpineText(const EpubBook& book, size_t spine_index, std::string& text,
                              std::string& error);
};

std::vector<EpubBook> ScanLibrary(const std::string& root, size_t max_books = 64);
std::vector<std::string> ScanTtfFonts(const std::string& root, size_t max_fonts = 16);

}  // namespace han::reader
