#include "epub_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string_view>

#ifndef HAN_UI_HOST_SIM
#include <esp_log.h>
#include <dirent.h>
#include <miniz.h>
#endif

namespace han::reader {
namespace {

constexpr size_t kMaxPackageBytes = 1024 * 1024;
constexpr size_t kMaxChapterBytes = 2 * 1024 * 1024;
constexpr size_t kMaxArchiveEntries = 4096;

bool EndsWithCaseInsensitive(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size())
        return false;
    value.remove_prefix(value.size() - suffix.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

std::string FileName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Stem(const std::string& path) {
    auto name = FileName(path);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.resize(dot);
    return name;
}

std::string DecodeEntities(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '&') {
            output.push_back(value[i++]);
            continue;
        }
        const auto end = value.find(';', i + 1);
        if (end == std::string_view::npos || end - i > 12) {
            output.push_back(value[i++]);
            continue;
        }
        const auto entity = value.substr(i + 1, end - i - 1);
        if (entity == "amp")
            output.push_back('&');
        else if (entity == "lt")
            output.push_back('<');
        else if (entity == "gt")
            output.push_back('>');
        else if (entity == "quot")
            output.push_back('"');
        else if (entity == "apos")
            output.push_back('\'');
        else if (entity == "nbsp")
            output.push_back(' ');
        else {
            output.append(value.substr(i, end - i + 1));
        }
        i = end + 1;
    }
    return output;
}

std::string Attribute(std::string_view tag, std::string_view name) {
    size_t cursor = 0;
    while ((cursor = tag.find(name, cursor)) != std::string_view::npos) {
        const bool left_ok =
            cursor == 0 || std::isspace(static_cast<unsigned char>(tag[cursor - 1]));
        size_t equals = cursor + name.size();
        while (equals < tag.size() && std::isspace(static_cast<unsigned char>(tag[equals])))
            ++equals;
        if (!left_ok || equals >= tag.size() || tag[equals] != '=') {
            cursor += name.size();
            continue;
        }
        ++equals;
        while (equals < tag.size() && std::isspace(static_cast<unsigned char>(tag[equals])))
            ++equals;
        if (equals >= tag.size() || (tag[equals] != '"' && tag[equals] != '\''))
            return {};
        const char quote = tag[equals++];
        const auto end = tag.find(quote, equals);
        return end == std::string_view::npos ? std::string{}
                                             : DecodeEntities(tag.substr(equals, end - equals));
    }
    return {};
}

std::string TagText(std::string_view xml, std::string_view name) {
    const std::string opening = "<" + std::string(name);
    const auto start = xml.find(opening);
    if (start == std::string_view::npos)
        return {};
    const auto body = xml.find('>', start + opening.size());
    if (body == std::string_view::npos)
        return {};
    const std::string closing = "</" + std::string(name) + ">";
    const auto end = xml.find(closing, body + 1);
    return end == std::string_view::npos ? std::string{}
                                         : DecodeEntities(xml.substr(body + 1, end - body - 1));
}

std::string NormalizeArchivePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::vector<std::string> parts;
    size_t cursor = 0;
    while (cursor <= path.size()) {
        const auto slash = path.find('/', cursor);
        const auto part =
            path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos)
            break;
        cursor = slash + 1;
    }
    std::string normalized;
    for (const auto& part : parts) {
        if (!normalized.empty())
            normalized.push_back('/');
        normalized += part;
    }
    return normalized;
}

std::string ResolvePath(const std::string& base_file, const std::string& reference) {
    if (reference.empty())
        return {};
    const auto fragment = reference.find_first_of("?#");
    const auto clean = reference.substr(0, fragment);
    const auto slash = base_file.find_last_of('/');
    return NormalizeArchivePath(
        (slash == std::string::npos ? std::string{} : base_file.substr(0, slash + 1)) + clean);
}

std::string CollapseWhitespace(std::string text) {
    std::string output;
    output.reserve(text.size());
    bool space = false;
    int newlines = 0;
    for (unsigned char ch : text) {
        if (ch == '\n') {
            while (!output.empty() && output.back() == ' ')
                output.pop_back();
            if (!output.empty() && newlines < 2)
                output.push_back('\n');
            ++newlines;
            space = false;
        } else if (ch < 0x80 && std::isspace(ch)) {
            space = true;
        } else {
            if (space && !output.empty() && output.back() != '\n')
                output.push_back(' ');
            output.push_back(static_cast<char>(ch));
            space = false;
            newlines = 0;
        }
    }
    while (!output.empty() && (output.back() == ' ' || output.back() == '\n'))
        output.pop_back();
    return output;
}

std::string HtmlToText(std::string_view html) {
    std::string output;
    output.reserve(html.size());
    bool hidden = false;
    for (size_t cursor = 0; cursor < html.size();) {
        if (html[cursor] != '<') {
            const auto tag = html.find('<', cursor);
            if (!hidden)
                output += DecodeEntities(html.substr(
                    cursor, tag == std::string_view::npos ? std::string_view::npos : tag - cursor));
            if (tag == std::string_view::npos)
                break;
            cursor = tag;
            continue;
        }
        const auto end = html.find('>', cursor + 1);
        if (end == std::string_view::npos)
            break;
        auto tag = html.substr(cursor + 1, end - cursor - 1);
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front())))
            tag.remove_prefix(1);
        bool closing = !tag.empty() && tag.front() == '/';
        if (closing)
            tag.remove_prefix(1);
        size_t name_end = 0;
        while (name_end < tag.size() && std::isalpha(static_cast<unsigned char>(tag[name_end])))
            ++name_end;
        std::string name(tag.substr(0, name_end));
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name == "script" || name == "style")
            hidden = !closing;
        if (!hidden && (name == "br" || name == "p" || name == "div" || name == "li" ||
                        name == "h1" || name == "h2" || name == "h3" || name == "h4"))
            output.push_back('\n');
        cursor = end + 1;
    }
    return CollapseWhitespace(std::move(output));
}

#ifndef HAN_UI_HOST_SIM
class ZipArchive {
public:
    explicit ZipArchive(const std::string& path) {
        file_ = fopen(path.c_str(), "rb");
        if (!file_ || fseek(file_, 0, SEEK_END) != 0)
            return;
        const long size = ftell(file_);
        if (size < 22)
            return;
        file_size_ = static_cast<uint32_t>(size);
        open_ = ReadDirectory();
    }

    ~ZipArchive() {
        if (file_)
            fclose(file_);
    }

    bool Read(const std::string& path, size_t limit, std::string& output) {
        output.clear();
        if (!open_)
            return false;
        const auto normalized = NormalizeArchivePath(path);
        Entry entry;
        if (!FindEntry(normalized, entry) || (entry.flags & 1) != 0 ||
            entry.uncompressed_size == 0 || entry.uncompressed_size > limit ||
            entry.compressed_size > limit + 65536)
            return false;

        uint8_t header[30];
        if (!ReadAt(entry.local_header_offset, header, sizeof(header)) ||
            Read32(header) != 0x04034b50)
            return false;
        const uint64_t data_offset = static_cast<uint64_t>(entry.local_header_offset) +
                                     sizeof(header) + Read16(header + 26) + Read16(header + 28);
        if (data_offset > UINT32_MAX ||
            !RangeWithin(static_cast<uint32_t>(data_offset), entry.compressed_size))
            return false;

        std::vector<uint8_t> compressed(entry.compressed_size);
        if (!ReadAt(static_cast<uint32_t>(data_offset), compressed.data(), compressed.size()))
            return false;
        output.resize(entry.uncompressed_size);
        if (entry.method == 0) {
            if (entry.compressed_size != entry.uncompressed_size) {
                output.clear();
                return false;
            }
            memcpy(output.data(), compressed.data(), compressed.size());
        } else if (entry.method == 8) {
            const size_t written = tinfl_decompress_mem_to_mem(
                output.data(), output.size(), compressed.data(), compressed.size(), 0);
            if (written != output.size()) {
                output.clear();
                return false;
            }
        } else {
            output.clear();
            return false;
        }
        if (mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(output.data()),
                     output.size()) != entry.crc32) {
            output.clear();
            return false;
        }
        return true;
    }

    bool ready() const { return open_; }

private:
    struct Entry {
        uint16_t flags = 0;
        uint16_t method = 0;
        uint32_t crc32 = 0;
        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint32_t local_header_offset = 0;
    };

    static uint16_t Read16(const uint8_t* data) {
        return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    }

    static uint32_t Read32(const uint8_t* data) {
        return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    }

    bool RangeWithin(uint32_t offset, size_t length) const {
        return offset <= file_size_ && length <= file_size_ - offset;
    }

    bool ReadAt(uint32_t offset, void* data, size_t length) {
        return RangeWithin(offset, length) &&
               fseek(file_, static_cast<long>(offset), SEEK_SET) == 0 &&
               fread(data, 1, length, file_) == length;
    }

    bool ReadDirectory() {
        constexpr size_t kMaxEocdSearch = 22 + 65535;
        const size_t scan_size = std::min<size_t>(file_size_, kMaxEocdSearch);
        std::vector<uint8_t> tail(scan_size);
        if (!ReadAt(file_size_ - scan_size, tail.data(), tail.size()))
            return false;
        for (size_t cursor = tail.size() - 22;; --cursor) {
            const uint8_t* eocd = tail.data() + cursor;
            if (Read32(eocd) == 0x06054b50) {
                if (cursor + 22 + Read16(eocd + 20) != tail.size() || Read16(eocd + 4) != 0 ||
                    Read16(eocd + 6) != 0 || Read16(eocd + 8) != Read16(eocd + 10))
                    return false;
                entry_count_ = Read16(eocd + 10);
                directory_size_ = Read32(eocd + 12);
                directory_offset_ = Read32(eocd + 16);
                return entry_count_ > 0 && entry_count_ <= kMaxArchiveEntries &&
                       RangeWithin(directory_offset_, directory_size_);
            }
            if (cursor == 0)
                break;
        }
        return false;
    }

    bool FindEntry(const std::string& wanted, Entry& entry) {
        uint32_t offset = directory_offset_;
        for (uint16_t index = 0; index < entry_count_; ++index) {
            uint8_t header[46];
            if (!ReadAt(offset, header, sizeof(header)) || Read32(header) != 0x02014b50)
                return false;
            const uint16_t name_length = Read16(header + 28);
            const uint16_t extra_length = Read16(header + 30);
            const uint16_t comment_length = Read16(header + 32);
            const size_t record_length =
                sizeof(header) + name_length + extra_length + comment_length;
            if (name_length == 0 || !RangeWithin(offset, record_length))
                return false;
            std::string name(name_length, '\0');
            if (!ReadAt(offset + sizeof(header), name.data(), name.size()))
                return false;
            if (NormalizeArchivePath(name) == wanted) {
                entry.flags = Read16(header + 8);
                entry.method = Read16(header + 10);
                entry.crc32 = Read32(header + 16);
                entry.compressed_size = Read32(header + 20);
                entry.uncompressed_size = Read32(header + 24);
                entry.local_header_offset = Read32(header + 42);
                return true;
            }
            offset += static_cast<uint32_t>(record_length);
        }
        return false;
    }

    FILE* file_ = nullptr;
    uint32_t file_size_ = 0;
    uint32_t directory_offset_ = 0;
    uint32_t directory_size_ = 0;
    uint16_t entry_count_ = 0;
    bool open_ = false;
};
#endif

}  // namespace

bool EpubReader::ParsePackage(const std::string& path, EpubBook& book) {
    book = {};
    book.path = path;
    book.file_name = FileName(path);
    book.title = Stem(path);
#ifdef HAN_UI_HOST_SIM
    book.author = "模拟器示例";
    book.language = "zh-CN";
    book.package_path = "OEBPS/content.opf";
    book.spine = {"OEBPS/chapter1.xhtml"};
    return true;
#else
    ZipArchive archive(path);
    if (!archive.ready()) {
        book.error = "不是有效的 EPUB/ZIP 文件";
        return false;
    }
    std::string container;
    if (!archive.Read("META-INF/container.xml", kMaxPackageBytes, container)) {
        book.error = "缺少 META-INF/container.xml";
        return false;
    }
    const auto rootfile = container.find("<rootfile");
    const auto rootfile_end =
        rootfile == std::string::npos ? std::string::npos : container.find('>', rootfile);
    if (rootfile_end == std::string::npos ||
        (book.package_path =
             Attribute(std::string_view(container).substr(rootfile, rootfile_end - rootfile + 1),
                       "full-path"))
            .empty()) {
        book.error = "container.xml 没有 rootfile";
        return false;
    }
    book.package_path = NormalizeArchivePath(book.package_path);
    std::string package;
    if (!archive.Read(book.package_path, kMaxPackageBytes, package)) {
        book.error = "无法读取 OPF package";
        return false;
    }
    auto title = TagText(package, "dc:title");
    if (title.empty())
        title = TagText(package, "title");
    if (!title.empty())
        book.title = title;
    book.author = TagText(package, "dc:creator");
    if (book.author.empty())
        book.author = TagText(package, "creator");
    book.language = TagText(package, "dc:language");

    std::map<std::string, std::string> manifest;
    size_t cursor = 0;
    while ((cursor = package.find("<item", cursor)) != std::string::npos) {
        if (cursor + 5 < package.size() &&
            !std::isspace(static_cast<unsigned char>(package[cursor + 5])) &&
            package[cursor + 5] != '>') {
            cursor += 5;
            continue;
        }
        const auto end = package.find('>', cursor + 5);
        if (end == std::string::npos)
            break;
        const auto tag = std::string_view(package).substr(cursor, end - cursor + 1);
        const auto id = Attribute(tag, "id");
        const auto href = Attribute(tag, "href");
        if (!id.empty() && !href.empty())
            manifest[id] = ResolvePath(book.package_path, href);
        const auto properties = Attribute(tag, "properties");
        if (properties.find("nav") != std::string::npos)
            book.nav_path = ResolvePath(book.package_path, href);
        cursor = end + 1;
    }

    cursor = 0;
    while ((cursor = package.find("<itemref", cursor)) != std::string::npos) {
        const auto end = package.find('>', cursor + 8);
        if (end == std::string::npos)
            break;
        const auto idref =
            Attribute(std::string_view(package).substr(cursor, end - cursor + 1), "idref");
        const auto found = manifest.find(idref);
        if (found != manifest.end())
            book.spine.push_back(found->second);
        cursor = end + 1;
    }
    if (book.spine.empty()) {
        book.error = "OPF spine 为空";
        return false;
    }
    ESP_LOGI("EpubReader", "Parsed %s: title=%s author=%s spine=%u", path.c_str(),
             book.title.c_str(), book.author.c_str(), static_cast<unsigned>(book.spine.size()));
    return true;
#endif
}

bool EpubReader::ReadSpineText(const EpubBook& book, size_t spine_index, std::string& text,
                               std::string& error) {
    text.clear();
    error.clear();
    if (spine_index >= book.spine.size()) {
        error = "章节索引越界";
        return false;
    }
#ifdef HAN_UI_HOST_SIM
    text = "这是 EPUB 阅读页面的模拟内容。\n\n真机将从 TF 卡解压 XHTML，并保留段落后进行分页。";
    return true;
#else
    ZipArchive archive(book.path);
    std::string html;
    if (!archive.ready() || !archive.Read(book.spine[spine_index], kMaxChapterBytes, html)) {
        error = "无法读取章节 XHTML";
        return false;
    }
    text = HtmlToText(html);
    if (text.empty()) {
        error = "章节没有可显示文本";
        return false;
    }
    return true;
#endif
}

std::vector<EpubBook> ScanLibrary(const std::string& root, size_t max_books) {
    std::vector<EpubBook> books;
    books.reserve(std::min<size_t>(max_books, 64));
#ifdef HAN_UI_HOST_SIM
    EpubBook demo;
    EpubReader::ParsePackage(root + "/示例图书.epub", demo);
    books.push_back(std::move(demo));
#else
    DIR* directory = opendir(root.c_str());
    if (!directory)
        return books;
    while (auto entry = readdir(directory)) {
        if (books.size() >= max_books)
            break;
        if (entry->d_name[0] == '.' || !EndsWithCaseInsensitive(entry->d_name, ".epub"))
            continue;
        EpubBook book;
        const auto path = root + "/" + entry->d_name;
        EpubReader::ParsePackage(path, book);
        books.push_back(std::move(book));
    }
    closedir(directory);
#endif
    std::sort(books.begin(), books.end(),
              [](const EpubBook& left, const EpubBook& right) { return left.title < right.title; });
    return books;
}

std::vector<std::string> ScanTtfFonts(const std::string& root, size_t max_fonts) {
    std::vector<std::string> fonts;
    fonts.reserve(std::min<size_t>(max_fonts, 16));
#ifndef HAN_UI_HOST_SIM
    DIR* directory = opendir(root.c_str());
    if (!directory)
        return fonts;
    while (auto entry = readdir(directory)) {
        if (fonts.size() >= max_fonts)
            break;
        if (entry->d_name[0] != '.' && EndsWithCaseInsensitive(entry->d_name, ".ttf"))
            fonts.push_back(root + "/" + entry->d_name);
    }
    closedir(directory);
#endif
    std::sort(fonts.begin(), fonts.end());
    return fonts;
}

}  // namespace han::reader
