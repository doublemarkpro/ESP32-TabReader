#include "epub_reader.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>

#ifndef HAN_UI_HOST_SIM
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <miniz.h>
#include <sys/stat.h>

#include "display/lvgl_display/jpg/jpeg_to_image.h"
#endif

namespace han::reader {
namespace {

constexpr size_t kMaxPackageBytes = 1024 * 1024;
constexpr size_t kMaxChapterBytes = 2 * 1024 * 1024;
constexpr size_t kMaxCoverBytes = 4 * 1024 * 1024;
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

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
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
    int hidden_depth = 0;
    bool paragraph_indent_pending = false;
    for (size_t cursor = 0; cursor < html.size();) {
        if (html[cursor] != '<') {
            const auto tag = html.find('<', cursor);
            if (hidden_depth == 0) {
                const auto decoded = DecodeEntities(html.substr(
                    cursor, tag == std::string_view::npos ? std::string_view::npos : tag - cursor));
                const bool has_visible_text =
                    std::any_of(decoded.begin(), decoded.end(), [](unsigned char ch) {
                        return ch >= 0x80 || !std::isspace(ch);
                    });
                if (paragraph_indent_pending && has_visible_text) {
                    output += "　　";
                    paragraph_indent_pending = false;
                }
                output += decoded;
            }
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
        const bool hidden_tag = name == "head" || name == "script" || name == "style";
        if (hidden_tag) {
            if (closing)
                hidden_depth = std::max(0, hidden_depth - 1);
            else if (tag.empty() || tag.back() != '/')
                ++hidden_depth;
            cursor = end + 1;
            continue;
        }
        if (hidden_depth == 0) {
            if (name == "p" && !closing) {
                // Most Chinese EPUBs express first-line indentation in CSS, which this compact
                // parser intentionally does not evaluate. Preserve the expected book typography
                // by inserting two ideographic spaces before the first visible text. Deferring
                // the insertion keeps image-only/empty paragraphs from becoming fake text pages.
                output.push_back('\n');
                paragraph_indent_pending = true;
            } else if (name == "br" || name == "p" || name == "div" || name == "li" ||
                       name == "h1" || name == "h2" || name == "h3" || name == "h4") {
                output.push_back('\n');
                if (name == "p")
                    paragraph_indent_pending = false;
            }
        }
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
            // tinfl_decompress_mem_to_mem() keeps its roughly 11 KiB decoder state on the
            // caller's stack, which overflows the 8 KiB content-worker stack. EPUB parsing
            // runs on that worker, so keep the decoder state on the heap instead.
            auto decompressor = std::unique_ptr<tinfl_decompressor, decltype(&free)>(
                static_cast<tinfl_decompressor*>(calloc(1, sizeof(tinfl_decompressor))), free);
            if (!decompressor) {
                output.clear();
                return false;
            }
            tinfl_init(decompressor.get());
            size_t input_bytes = compressed.size();
            size_t output_bytes = output.size();
            const auto status = tinfl_decompress(
                decompressor.get(), compressed.data(), &input_bytes,
                reinterpret_cast<uint8_t*>(output.data()),
                reinterpret_cast<uint8_t*>(output.data()), &output_bytes,
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if (status != TINFL_STATUS_DONE || input_bytes != compressed.size() ||
                output_bytes != output.size()) {
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

bool SaveCoverToCache(const std::string& book_path, const std::string& archive_path,
                      const std::string& media_type, const std::string& data,
                      std::string& output_path) {
    const auto lower_path = Lowercase(archive_path);
    const auto lower_type = Lowercase(media_type);
    const bool jpeg = lower_type == "image/jpeg" || EndsWithCaseInsensitive(lower_path, ".jpg") ||
                      EndsWithCaseInsensitive(lower_path, ".jpeg");
    if (!jpeg || data.empty())
        return false;

    constexpr size_t kThumbnailWidth = 80;
    constexpr size_t kThumbnailHeight = 120;
    constexpr size_t kBmpHeaderSize = 54;
    constexpr size_t kBmpRowSize = (kThumbnailWidth * 3 + 3) & ~size_t(3);
    constexpr size_t kBmpSize = kBmpHeaderSize + kBmpRowSize * kThumbnailHeight;
    mkdir("/sdcard/handict/cache", 0775);
    mkdir("/sdcard/handict/cache/covers", 0775);
    uint32_t hash = 2166136261u;
    for (unsigned char ch : book_path + "\n" + archive_path) {
        hash ^= ch;
        hash *= 16777619u;
    }
    char file_name[96];
    snprintf(file_name, sizeof(file_name), "/sdcard/handict/cache/covers/%08x.bmp",
             static_cast<unsigned>(hash));
    if (FILE* cached = fopen(file_name, "rb")) {
        const bool seek_ok = fseek(cached, 0, SEEK_END) == 0;
        const long cached_size = seek_ok ? ftell(cached) : -1;
        const bool valid = cached_size == static_cast<long>(kBmpSize);
        fclose(cached);
        if (valid) {
            output_path = file_name;
            return true;
        }
    }

    uint8_t* decoded = nullptr;
    size_t decoded_size = 0;
    size_t source_width = 0;
    size_t source_height = 0;
    size_t source_stride = 0;
    if (jpeg_to_image(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &decoded,
                      &decoded_size, &source_width, &source_height, &source_stride) != ESP_OK ||
        decoded == nullptr || source_width == 0 || source_height == 0) {
        if (decoded != nullptr)
            heap_caps_free(decoded);
        return false;
    }

    std::vector<uint8_t> bmp(kBmpSize, 0);
    std::fill(bmp.begin() + kBmpHeaderSize, bmp.end(), 0xff);
    const auto put_u16 = [&bmp](size_t offset, uint16_t value) {
        bmp[offset] = static_cast<uint8_t>(value);
        bmp[offset + 1] = static_cast<uint8_t>(value >> 8);
    };
    const auto put_u32 = [&bmp](size_t offset, uint32_t value) {
        bmp[offset] = static_cast<uint8_t>(value);
        bmp[offset + 1] = static_cast<uint8_t>(value >> 8);
        bmp[offset + 2] = static_cast<uint8_t>(value >> 16);
        bmp[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put_u32(2, static_cast<uint32_t>(bmp.size()));
    put_u32(10, kBmpHeaderSize);
    put_u32(14, 40);
    put_u32(18, kThumbnailWidth);
    put_u32(22, kThumbnailHeight);
    put_u16(26, 1);
    put_u16(28, 24);
    put_u32(34, static_cast<uint32_t>(kBmpRowSize * kThumbnailHeight));

    size_t draw_width = kThumbnailWidth;
    size_t draw_height = std::max<size_t>(1, source_height * kThumbnailWidth / source_width);
    if (draw_height > kThumbnailHeight) {
        draw_height = kThumbnailHeight;
        draw_width = std::max<size_t>(1, source_width * kThumbnailHeight / source_height);
    }
    const size_t offset_x = (kThumbnailWidth - draw_width) / 2;
    const size_t offset_y = (kThumbnailHeight - draw_height) / 2;
    for (size_t y = 0; y < draw_height; ++y) {
        const size_t source_y = y * source_height / draw_height;
        const size_t bmp_y = kThumbnailHeight - 1 - (offset_y + y);
        for (size_t x = 0; x < draw_width; ++x) {
            const size_t source_x = x * source_width / draw_width;
            const size_t source_offset = source_y * source_stride + source_x * 2;
            if (source_offset + 1 >= decoded_size)
                continue;
            const uint16_t pixel = static_cast<uint16_t>(decoded[source_offset]) |
                                   (static_cast<uint16_t>(decoded[source_offset + 1]) << 8);
            const uint8_t red = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
            const uint8_t green = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
            const uint8_t blue = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
            const size_t bmp_offset = kBmpHeaderSize + bmp_y * kBmpRowSize +
                                      (offset_x + x) * 3;
            bmp[bmp_offset] = blue;
            bmp[bmp_offset + 1] = green;
            bmp[bmp_offset + 2] = red;
        }
    }
    heap_caps_free(decoded);

    FILE* output = fopen(file_name, "wb");
    if (!output)
        return false;
    const bool written = fwrite(bmp.data(), 1, bmp.size(), output) == bmp.size();
    const bool closed = fclose(output) == 0;
    if (!written || !closed) {
        remove(file_name);
        return false;
    }
    output_path = file_name;
    return true;
}
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
    size_t rootfile = 0;
    while ((rootfile = container.find("<rootfile", rootfile)) != std::string::npos) {
        const size_t name_end = rootfile + sizeof("<rootfile") - 1;
        if (name_end >= container.size() ||
            std::isspace(static_cast<unsigned char>(container[name_end])) ||
            container[name_end] == '>' || container[name_end] == '/')
            break;
        // Do not confuse the enclosing <rootfiles> element with <rootfile>.
        rootfile = name_end;
    }
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

    std::string cover_id;
    size_t meta_cursor = 0;
    while ((meta_cursor = package.find("<meta", meta_cursor)) != std::string::npos) {
        const auto end = package.find('>', meta_cursor + 5);
        if (end == std::string::npos)
            break;
        const auto tag = std::string_view(package).substr(meta_cursor, end - meta_cursor + 1);
        if (Lowercase(Attribute(tag, "name")) == "cover") {
            cover_id = Attribute(tag, "content");
            break;
        }
        meta_cursor = end + 1;
    }

    std::map<std::string, std::string> manifest;
    std::string cover_archive_path;
    std::string cover_media_type;
    int cover_priority = 0;
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
        const auto resolved_path = ResolvePath(book.package_path, href);
        if (!id.empty() && !resolved_path.empty())
            manifest[id] = resolved_path;
        const auto properties = Attribute(tag, "properties");
        if (properties.find("nav") != std::string::npos)
            book.nav_path = resolved_path;
        const auto media_type = Attribute(tag, "media-type");
        if (!resolved_path.empty() && Lowercase(media_type).find("image/") == 0) {
            const auto searchable = Lowercase(id + " " + href);
            int priority = 0;
            if (!cover_id.empty() && id == cover_id)
                priority = 3;
            else if (Lowercase(properties).find("cover-image") != std::string::npos)
                priority = 2;
            else if (searchable.find("cover") != std::string::npos)
                priority = 1;
            if (priority > cover_priority) {
                cover_priority = priority;
                cover_archive_path = resolved_path;
                cover_media_type = media_type;
            }
        }
        cursor = end + 1;
    }

    if (!cover_archive_path.empty()) {
        std::string cover_data;
        if (archive.Read(cover_archive_path, kMaxCoverBytes, cover_data) &&
            SaveCoverToCache(path, cover_archive_path, cover_media_type, cover_data,
                             book.cover_path)) {
            ESP_LOGI("EpubReader", "Cached cover %s -> %s", cover_archive_path.c_str(),
                     book.cover_path.c_str());
        } else {
            ESP_LOGW("EpubReader", "Unable to cache cover %s", cover_archive_path.c_str());
        }
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

bool EpubReader::FindReadableSpineText(const EpubBook& book, size_t start_index, int direction,
                                       size_t& actual_index, std::string& text,
                                       std::string& error) {
    text.clear();
    error.clear();
    if (book.spine.empty() || start_index >= book.spine.size()) {
        error = "章节索引越界";
        return false;
    }
    const int step = direction < 0 ? -1 : 1;
    for (int64_t index = static_cast<int64_t>(start_index);
         index >= 0 && index < static_cast<int64_t>(book.spine.size()); index += step) {
        std::string candidate;
        std::string candidate_error;
        if (ReadSpineText(book, static_cast<size_t>(index), candidate, candidate_error) &&
            !candidate.empty()) {
            actual_index = static_cast<size_t>(index);
            text = std::move(candidate);
            return true;
        }
    }
    error = direction < 0 ? "前面没有可显示的正文" : "后面没有可显示的正文";
    return false;
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
    if (!directory) {
        ESP_LOGW("EpubReader", "Unable to scan %s: errno=%d (%s)", root.c_str(), errno,
                 std::strerror(errno));
        return books;
    }
    ESP_LOGI("EpubReader", "Scanning library %s", root.c_str());
    while (auto entry = readdir(directory)) {
        if (books.size() >= max_books)
            break;
        if (entry->d_name[0] == '.' || !EndsWithCaseInsensitive(entry->d_name, ".epub"))
            continue;
        EpubBook book;
        const auto path = root + "/" + entry->d_name;
        if (!EpubReader::ParsePackage(path, book)) {
            ESP_LOGW("EpubReader", "Rejected %s: %s", path.c_str(), book.error.c_str());
        }
        books.push_back(std::move(book));
    }
    closedir(directory);
    ESP_LOGI("EpubReader", "Library scan complete: %u EPUB file(s)",
             static_cast<unsigned>(books.size()));
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
