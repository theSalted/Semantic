#ifndef SEMANTIC_DOWNLOAD_HPP
#define SEMANTIC_DOWNLOAD_HPP

// First-use model download into a per-user cache. Resumable; a file appears under its
// final name only after the transfer completes.

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace Semantic::Download
{
    namespace fs = std::filesystem;
    // Bytes on disk so far and expected total (0 when unknown).
    using Progress = std::function<void(unsigned long long done, unsigned long long total)>;

    inline fs::path CacheDirectory()
    {
#if defined(_WIN32)
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        if (!local || !*local) throw std::runtime_error("LOCALAPPDATA is not set; cannot choose a model cache.");
        return fs::path(local) / L"Semantic" / L"models";
#elif defined(__APPLE__)
        const char* home = std::getenv("HOME");
        if (!home || !*home) throw std::runtime_error("HOME is not set; cannot choose a model cache.");
        return fs::path(home) / "Library" / "Caches" / "Semantic" / "models";
#else
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return fs::path(xdg) / "semantic" / "models";
        const char* home = std::getenv("HOME");
        if (!home || !*home) throw std::runtime_error("HOME is not set; cannot choose a model cache.");
        return fs::path(home) / ".cache" / "semantic" / "models";
#endif
    }

    struct FileCloser { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
    using File = std::unique_ptr<std::FILE, FileCloser>;

    inline File Open(const fs::path& path, bool append)
    {
#if defined(_WIN32)
        return File(_wfopen(path.c_str(), append ? L"ab" : L"wb"));
#else
        return File(std::fopen(path.c_str(), append ? "ab" : "wb"));
#endif
    }

#if defined(_WIN32)
    struct Handle
    {
        HINTERNET value = nullptr;
        ~Handle() { if (value) WinHttpCloseHandle(value); }
    };

    inline std::runtime_error Failure(const char* step)
    {
        return std::runtime_error(std::string("Model download failed at ") + step + " (Windows error " + std::to_string(GetLastError()) + ").");
    }

    // Appends to `partial` from its current size. WinHTTP follows HTTPS redirects by default.
    inline void Fetch(const std::string& url, const fs::path& partial, const Progress& progress)
    {
        const std::wstring wide = fs::u8path(url).wstring();
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) throw Failure("URL parsing");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Handle session{WinHttpOpen(L"Semantic", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value) throw Failure("session");
        Handle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
        if (!connection.value) throw Failure("connect");
        Handle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
        if (!request.value) throw Failure("request");
        const auto offset = fs::exists(partial) ? fs::file_size(partial) : 0;
        const std::wstring range = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-" : L"";
        if (!WinHttpSendRequest(request.value, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr))
            throw Failure("send");
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            throw Failure("status");
        if (status != 200 && status != 206) throw std::runtime_error("Model download returned HTTP " + std::to_string(status) + ".");
        wchar_t length_text[32]{};
        DWORD length_size = sizeof length_text;
        const bool known = WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size, WINHTTP_NO_HEADER_INDEX);
        auto file = Open(partial, status == 206); // A full response restarts the file.
        if (!file) throw std::runtime_error("Cannot write " + partial.u8string());
        unsigned long long received = 0;
        const unsigned long long base = status == 206 ? offset : 0;
        const unsigned long long total = known ? base + std::wcstoull(length_text, nullptr, 10) : 0;
        std::string buffer(1 << 20, '\0');
        for (DWORD available = 0;;)
        {
            if (!WinHttpQueryDataAvailable(request.value, &available)) throw Failure("read");
            if (available == 0) break;
            DWORD read = 0;
            if (!WinHttpReadData(request.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read)) throw Failure("read");
            if (std::fwrite(buffer.data(), 1, read, file.get()) != read) throw std::runtime_error("Cannot write " + partial.u8string());
            received += read;
            if (progress) progress(base + received, total);
        }
        // A dropped connection can end the stream quietly; never promote a short file.
        if (known && base + received != total)
            throw std::runtime_error("Model download ended early. Retrying resumes it.");
    }
#else
    struct Sink { const fs::path* partial; CURL* curl; File file; bool failed = false; const Progress* progress; curl_off_t offset; };

    inline int Report(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
    {
        const auto& sink = *static_cast<Sink*>(user);
        long status = 0;
        curl_easy_getinfo(sink.curl, CURLINFO_RESPONSE_CODE, &status);
        // Redirect bodies are not the model; a full (200) response restarts from zero.
        const curl_off_t base = status == 206 ? sink.offset : 0;
        if (*sink.progress && total > 0 && (status == 200 || status == 206)) (*sink.progress)(base + now, base + total);
        return 0;
    }

    inline std::size_t Write(char* data, std::size_t size, std::size_t count, void* user)
    {
        auto& sink = *static_cast<Sink*>(user);
        if (!sink.file)
        {
            long status = 0;
            curl_easy_getinfo(sink.curl, CURLINFO_RESPONSE_CODE, &status);
            if (status != 200 && status != 206) return 0;
            sink.file = Open(*sink.partial, status == 206); // A full response restarts the file.
            if (!sink.file) { sink.failed = true; return 0; }
        }
        return std::fwrite(data, size, count, sink.file.get()) * size;
    }

    // Appends to `partial` from its current size, following redirects.
    inline void Fetch(const std::string& url, const fs::path& partial, const Progress& progress)
    {
        static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (initialized != CURLE_OK) throw std::runtime_error("Could not initialize the model downloader.");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) throw std::runtime_error("Could not start the model download.");
        const curl_off_t offset = fs::exists(partial) ? static_cast<curl_off_t>(fs::file_size(partial)) : 0;
        Sink sink{&partial, curl.get(), nullptr, false, &progress, offset};
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_RESUME_FROM_LARGE, offset);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Semantic");
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Write);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, Report);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &sink);
        // Abort a stalled transfer; a later attempt resumes from the partial file.
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
        const CURLcode result = curl_easy_perform(curl.get());
        if (sink.failed) throw std::runtime_error("Cannot write " + partial.u8string());
        if (result != CURLE_OK) throw std::runtime_error(std::string("Model download failed: ") + curl_easy_strerror(result) + ". Retrying resumes it.");
    }
#endif

    // Returns `bundled` when that file exists; otherwise the cached copy, downloading it first if needed.
    inline std::string Ensure(const fs::path& bundled, const std::string& url, const Progress& progress = {})
    {
        if (fs::exists(bundled)) return bundled.u8string();
        const auto target = CacheDirectory() / bundled.filename();
        if (fs::exists(target)) return target.u8string();
        if (url.empty()) throw std::runtime_error("Model not found at " + bundled.u8string() + " or " + target.u8string() + ", and no download URL is configured.");
        fs::create_directories(target.parent_path());
        auto partial = target;
        partial += ".partial";
        Fetch(url, partial, progress);
        fs::rename(partial, target);
        return target.u8string();
    }
}

#endif
