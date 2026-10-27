#include <cstdlib>
#include <iostream>
#include <string>
#include "main/reader/file_transfer_policy.h"

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << description << '\n';
        std::exit(1);
    }
}

int main() {
    using reader::DecodeUploadName;
    using reader::ValidUploadName;
    Check(ValidUploadName("book.EPUB", false), "case-insensitive EPUB");
    Check(ValidUploadName("LXGWWenKai.ttf", true), "TTF font");
    Check(ValidUploadName(u8"中文书名.epub", false), "UTF-8 book name");
    Check(ValidUploadName("volume 1 (2026).epub", false), "spaces and parentheses");
    Check(!ValidUploadName("book.epub", true), "book cannot enter fonts");
    Check(!ValidUploadName("font.ttf", false), "font cannot enter books");
    for (const auto* name : {"", ".epub", ".hidden.epub", "../book.epub", "folder/book.epub",
                             "folder\\book.epub", "F:book.epub", "book..epub", "book.epub ",
                             "book.epub.", "book?.epub", "book.epub.exe", "a\n.epub"})
        Check(!ValidUploadName(name, false), "invalid or unsafe filename accepted");
    Check(!ValidUploadName(std::string("a\0.epub", 7), false), "embedded NUL");
    Check(!ValidUploadName(std::string(241, 'a') + ".epub", false), "length limit");
    Check(DecodeUploadName("hello%20world.epub") == "hello world.epub", "percent decoding");
    Check(DecodeUploadName("%E4%B9%A6.epub") == u8"书.epub", "encoded UTF-8");
    Check(DecodeUploadName("a+b.epub") == "a+b.epub", "literal plus is preserved");
    for (const auto* encoded : {"%", "%2", "%xx", "abc%0z.epub"})
        Check(DecodeUploadName(encoded).empty(), "malformed encoding accepted");
    Check(!ValidUploadName(DecodeUploadName("%2e%2e%2fbook.epub"), false), "encoded traversal");
    Check(!ValidUploadName(DecodeUploadName("book%00.epub"), false), "encoded NUL");
    std::cout << "Reader transfer filename checks passed\n";
}
