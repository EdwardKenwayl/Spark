#include "platform/binary_io.h"

#include "core/common.h"

#include <chrono>
#include "platform/filesystem.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
namespace fs = spark::fs;
int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

fs::path CreateTestDirectory() {
    const auto parent = fs::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 128; ++attempt) {
        const auto candidate = parent / ("spark-binary-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        std::error_code error;
        if (fs::create_directory(candidate, error)) return candidate;
        if (error) throw fs::filesystem_error("create binary I/O test directory", candidate, error);
    }
    throw std::runtime_error("could not create a unique binary I/O test directory");
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    spark::fs_ofstream file(path, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
}
}  // namespace

int main() {
    using namespace spark;
    fs::path root;
    try {
        root = CreateTestDirectory();
        const auto unicode = root / NativePath(u8"image-\u6d4b\u8bd5-\U0001f680.PNG");
        std::string bytes("\xEF\xBB\xBF", 3);
        for (int byte = 0; byte < 256; ++byte) bytes.push_back(static_cast<char>(byte));
        bytes.append((1u << 20) + 37, '\0');
        bytes.back() = static_cast<char>(0xFE);
        WriteBytes(unicode, bytes);
        Check(ReadAttachmentBytes(PathToUtf8(unicode)) == std::optional<std::string>(bytes),
              "Unicode paths preserve BOM, NUL, all byte values and data across read chunks");

        const auto utf16 = root / "utf16-bom.bin";
        const std::string utf16_bytes("\xFF\xFE\0\x80\r\n", 6);
        WriteBytes(utf16, utf16_bytes);
        Check(ReadAttachmentBytes(PathToUtf8(utf16)) == std::optional<std::string>(utf16_bytes),
              "UTF-16 BOM and CRLF bytes are neither decoded nor stripped");

        const auto single = root / "single.bin";
        WriteBytes(single, std::string(1, '\0'));
        Check(ReadAttachmentBytes(PathToUtf8(single)) == std::optional<std::string>(std::string(1, '\0')),
              "a one-byte NUL file is nonempty and readable");

        const auto boundary = root / "boundary.bin";
        std::string maximum(16u * 1024u * 1024u, '\0');
        maximum.front() = static_cast<char>(0xFF);
        maximum.back() = static_cast<char>(0xFE);
        WriteBytes(boundary, maximum);
        Check(ReadAttachmentBytes(PathToUtf8(boundary)) == std::optional<std::string>(maximum),
              "the inclusive 16 MiB limit preserves every byte");
        fs::resize_file(boundary, maximum.size() + 1);
        Check(!ReadAttachmentBytes(PathToUtf8(boundary)), "16 MiB plus one byte is rejected");

        const auto empty = root / "empty.bin";
        WriteBytes(empty, {});
        Check(!ReadAttachmentBytes(PathToUtf8(empty)), "an empty regular file is rejected");
        Check(!ReadAttachmentBytes(PathToUtf8(root / "missing.bin")), "a missing file is rejected");
        Check(!ReadAttachmentBytes({}), "an empty path is rejected");
        const std::string nul_path = PathToUtf8(unicode) + std::string("\0ignored", 8);
        Check(!ReadAttachmentBytes(nul_path), "an embedded NUL cannot read a truncated path");

        const auto directory = root / "directory";
        fs::create_directory(directory);
        Check(!ReadAttachmentBytes(PathToUtf8(directory)), "a directory is rejected");
#ifndef _WIN32
        const auto link = root / "regular-link.PNG";
        fs::create_symlink(unicode.filename(), link);
        Check(ReadAttachmentBytes(PathToUtf8(link)) == std::optional<std::string>(bytes),
              "a symlink to a regular attachment remains readable");
        const auto missing_link = root / "missing-link";
        fs::create_symlink("missing.bin", missing_link);
        Check(!ReadAttachmentBytes(PathToUtf8(missing_link)), "a dangling symlink is rejected");

        const auto fifo = root / "no-writer.fifo";
        const bool fifo_created = ::mkfifo(fifo.c_str(), 0600) == 0;
        Check(fifo_created, "create a FIFO fixture");
        if (fifo_created) {
            Check(!ReadAttachmentBytes(PathToUtf8(fifo)), "a FIFO without a writer is rejected without blocking");
            const auto fifo_link = root / "fifo-link";
            fs::create_symlink(fifo.filename(), fifo_link);
            Check(!ReadAttachmentBytes(PathToUtf8(fifo_link)), "a symlink to a FIFO is rejected without blocking");
        }
#endif
    } catch (const std::exception& exception) {
        Check(false, exception.what());
    }
    if (!root.empty()) {
        std::error_code error;
        fs::remove_all(root, error);
        Check(!error, "remove the binary I/O test directory");
    }
    if (failures == 0) std::cout << "PASS: native bounded attachment reads preserve binary contents\n";
    return failures == 0 ? 0 : 1;
}
