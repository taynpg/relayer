#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace relayer::test {

// 测试用临时目录：构造时创建唯一目录，析构时递归删除。
//
// 不放在系统临时目录的根上，而是统一放到 <tmp>/relayer-test/<pid>-<seq>/，
// 这样测试崩溃后残留可以一眼认出是谁留下的，也方便手工清理。
class TempDir
{
public:
    TempDir()
    {
        static std::atomic<unsigned> seq{0};
        const unsigned n = seq.fetch_add(1, std::memory_order_relaxed);
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec) / "relayer-test";
        std::filesystem::create_directories(base, ec);
        path_ = base / ("t" + std::to_string(static_cast<unsigned long>(n)));
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    // 以 UTF-8 返回，与 IFileStore 的路径约定一致
    [[nodiscard]] std::string utf8() const
    {
        // C++17 下 path::u8string() 返回 std::string，C++20 起返回 std::u8string，
        // 这里走 generic_string() 再由调用方的 _wfopen 走宽字符路径，
        // 避免依赖标准版本差异。
#ifdef _WIN32
        const std::wstring w = path_.generic_wstring();
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
        return out;
#else
        return path_.generic_string();
#endif
    }

    [[nodiscard]] std::string path(const std::string& leaf) const
    {
        return utf8() + "/" + leaf;
    }

    [[nodiscard]] const std::filesystem::path& native() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// 用 C 流写一个文件（测试自身准备数据用，不经过被测实现）
inline bool writeFile(const std::string& utf8Path, const std::string& content)
{
#ifdef _WIN32
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), static_cast<int>(utf8Path.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), static_cast<int>(utf8Path.size()), wide.data(), need);
    std::FILE* fp = ::_wfopen(wide.c_str(), L"wb");
#else
    std::FILE* fp = std::fopen(utf8Path.c_str(), "wb");
#endif
    if (fp == nullptr) {
        return false;
    }
    const bool ok = content.empty() || std::fwrite(content.data(), 1, content.size(), fp) == content.size();
    std::fclose(fp);
    return ok;
}

}   // namespace relayer::test
