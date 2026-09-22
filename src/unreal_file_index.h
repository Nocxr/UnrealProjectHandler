#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace uph {

namespace fs = std::filesystem;

enum class UnrealFileKind {
    Project,
    Plugin
};

enum class UnrealSearchScope {
    User,
    Engine,
    All
};

struct UnrealFileRecord {
    UnrealFileKind kind = UnrealFileKind::Project;
    fs::path path;
};

struct UnrealIndexViewStats {
    std::size_t user_records = 0;
    std::size_t user_projects = 0;
    std::size_t user_plugins = 0;
    std::size_t engine_records = 0;
    std::size_t hidden_records = 0;
};

struct UnrealIndexStats {
    std::size_t roots = 0;
    std::size_t records = 0;
    std::size_t projects = 0;
    std::size_t plugins = 0;
    std::size_t ntfs_mft_roots = 0;
    std::size_t walked_roots = 0;
    long long elapsed_ms = 0;
    std::vector<std::string> warnings;
};

class UnrealFileIndex {
public:
    bool load(const fs::path& cache_path);
    bool save(const fs::path& cache_path) const;

    UnrealIndexStats rebuild(const std::vector<fs::path>& roots);

    std::vector<UnrealFileRecord> search(const std::string& query,
                                         bool include_projects = true,
                                         bool include_plugins = true,
                                         std::size_t limit = 200,
                                         UnrealSearchScope scope = UnrealSearchScope::User) const;

    UnrealIndexViewStats view_stats() const;
    std::vector<fs::path> engine_roots() const;

    const std::vector<UnrealFileRecord>& records() const { return records_; }
    void replace_records(std::vector<UnrealFileRecord> records);

    static std::vector<fs::path> default_roots();

private:
    std::vector<UnrealFileRecord> records_;
};

const char* unreal_file_kind_name(UnrealFileKind kind);
bool run_unreal_file_index_self_test(std::string* error_message = nullptr);

} // namespace uph
