// Resumable model download through the platform downloader (libcurl or WinHTTP).
// Usage: Semantic.DownloadChecks <url> <reference-file> <scratch-dir>
// Seeds a partial file with the first half of the reference, resumes from the URL, and
// requires a byte-identical result. The scratch directory stands in for the user cache.
#include "../Semantic.cpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

static std::string Read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char** argv)
{
    assert(argc == 4);
    const std::string url = argv[1];
    const fs::path reference = fs::u8path(argv[2]), scratch = fs::absolute(fs::u8path(argv[3]));
    fs::remove_all(scratch);
#if defined(_WIN32)
    _wputenv_s(L"LOCALAPPDATA", scratch.c_str());
#else
    setenv("HOME", scratch.c_str(), 1);
#endif
    const auto cache = Semantic::Download::CacheDirectory();
    assert(cache.u8string().rfind(scratch.u8string(), 0) == 0);

    // A bundled file wins and nothing is fetched.
    fs::create_directories(scratch / "bundled");
    const auto bundled = scratch / "bundled" / reference.filename();
    std::ofstream(bundled) << "bundled";
    assert(Semantic::Download::Ensure(bundled, "") == bundled.u8string());
    fs::remove(bundled);

    const auto expected = Read(reference);
    assert(expected.size() > 2);
    fs::create_directories(cache);
    auto partial = cache / reference.filename();
    partial += ".partial";
    std::ofstream(partial, std::ios::binary).write(expected.data(), static_cast<std::streamsize>(expected.size() / 2));

    unsigned long long last = 0, total = 0;
    const auto path = Semantic::Download::Ensure(bundled, url, [&](unsigned long long done, unsigned long long size) {
        assert(done >= last); last = done; total = size;
    });
    assert(fs::u8path(path) == cache / reference.filename() && !fs::exists(partial));
    assert(total == expected.size() && last == expected.size());
    assert(Read(fs::u8path(path)) == expected);

    // Missing everywhere with no URL is a clear error, not a hang.
    bool threw = false;
    try { Semantic::Download::Ensure(scratch / "bundled" / "absent.gguf", ""); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    std::cout << "Download checks passed (resumed " << expected.size() / 2 << " of " << expected.size() << " bytes)\n";
}
