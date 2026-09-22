#include "unreal_file_index.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#if defined(__MINGW32__) || defined(__MINGW64__)
using MftEnumDataV0 = MFT_ENUM_DATA;
using UsnRecordV2 = USN_RECORD;
#else
using MftEnumDataV0 = MFT_ENUM_DATA_V0;
using UsnRecordV2 = USN_RECORD_V2;
#endif
#endif

namespace uph {
namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::wstring lower_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool classify_extension(const fs::path& path, UnrealFileKind& kind) {
    const auto ext = lower_copy(path.extension().string());
    if (ext == ".uproject") {
        kind = UnrealFileKind::Project;
        return true;
    }
    if (ext == ".uplugin") {
        kind = UnrealFileKind::Plugin;
        return true;
    }
    return false;
}

bool classify_extension(const std::wstring& filename, UnrealFileKind& kind) {
    const auto dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const auto ext = lower_copy(filename.substr(dot));
    if (ext == L".uproject") {
        kind = UnrealFileKind::Project;
        return true;
    }
    if (ext == L".uplugin") {
        kind = UnrealFileKind::Plugin;
        return true;
    }
    return false;
}

std::string normalized_key(const fs::path& path) {
    auto value = path.lexically_normal().string();
#ifdef _WIN32
    value = lower_copy(value);
#endif
    return value;
}

void dedupe_and_sort(std::vector<UnrealFileRecord>& records) {
    std::set<std::string> seen;
    records.erase(std::remove_if(records.begin(), records.end(),
        [&](const UnrealFileRecord& record) {
            return !seen.insert(normalized_key(record.path)).second;
        }), records.end());

    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        auto an = lower_copy(a.path.stem().string());
        auto bn = lower_copy(b.path.stem().string());
        if (an != bn) return an < bn;
        return lower_copy(a.path.string()) < lower_copy(b.path.string());
    });
}

bool should_skip_directory(const fs::path& path) {
    const auto name = lower_copy(path.filename().string());
    static const std::unordered_set<std::string> skipped{
        "$recycle.bin",
        "system volume information",
        ".git",
        ".svn",
        ".hg",
        "node_modules",
        "intermediate",
        "saved",
        "deriveddatacache",
        "__pycache__"
    };
    return skipped.contains(name);
}

void walk_root(const fs::path& root,
               std::vector<UnrealFileRecord>& output,
               std::vector<std::string>& warnings) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        warnings.push_back("Root does not exist: " + root.string());
        return;
    }

    if (fs::is_regular_file(root, ec)) {
        UnrealFileKind kind{};
        if (classify_extension(root, kind)) output.push_back({kind, root});
        return;
    }

    fs::recursive_directory_iterator it(
        root,
        fs::directory_options::skip_permission_denied,
        ec);
    fs::recursive_directory_iterator end;

    while (it != end) {
        if (ec) {
            ec.clear();
            it.increment(ec);
            continue;
        }

        const auto& entry = *it;
        std::error_code type_ec;
        if (entry.is_directory(type_ec)) {
            if (should_skip_directory(entry.path()))
                it.disable_recursion_pending();
        } else if (entry.is_regular_file(type_ec)) {
            UnrealFileKind kind{};
            if (classify_extension(entry.path(), kind))
                output.push_back({kind, entry.path()});
        }

        it.increment(ec);
    }
}

int fuzzy_score(std::string_view text_view, std::string_view query_view) {
    const auto text = lower_copy(std::string(text_view));
    const auto query = lower_copy(std::string(query_view));
    if (query.empty()) return 0;

    if (text == query) return 10000;
    if (text.starts_with(query)) return 7000 - static_cast<int>(text.size() - query.size());

    auto contiguous = text.find(query);
    if (contiguous != std::string::npos)
        return 5000 - static_cast<int>(contiguous) * 4 - static_cast<int>(text.size() - query.size());

    int score = 0;
    std::size_t at = 0;
    int previous = -2;
    for (char wanted : query) {
        auto found = text.find(wanted, at);
        if (found == std::string::npos) return -1;
        score += 30;
        if (static_cast<int>(found) == previous + 1) score += 20;
        if (found == 0 || text[found - 1] == ' ' || text[found - 1] == '\\' ||
            text[found - 1] == '/' || text[found - 1] == '_' || text[found - 1] == '-')
            score += 12;
        score -= static_cast<int>(found - at);
        previous = static_cast<int>(found);
        at = found + 1;
    }
    return score - static_cast<int>(text.size() - query.size()) / 8;
}


enum class RecordClass {
    User,
    Engine,
    Hidden
};

std::string normalized_slash_path(const fs::path& path) {
    auto value = lower_copy(path.lexically_normal().string());
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool is_hidden_record(const UnrealFileRecord& record) {
    const auto filename = lower_copy(record.path.filename().string());
    if (filename.empty() || filename == ".uproject" || filename == ".uplugin")
        return true;

    const auto path = normalized_slash_path(record.path);
    static constexpr std::string_view hidden_fragments[] = {
        "/$recycle.bin/",
        "/system volume information/",
        "/appdata/roaming/code/user/history",
        "/appdata/roaming/cursor/user/history",
        "/appdata/roaming/vscodium/user/history",
        "/.vscode/",
        "/.idea/",
        "/.git/",
        "/.svn/",
        "/.hg/",
        "/node_modules/",
        "/intermediate/",
        "/saved/",
        "/deriveddatacache/",
        "/__pycache__/",
        "/temp/",
        "/tmp/"
    };

    for (const auto fragment : hidden_fragments) {
        if (path.find(fragment) != std::string::npos) return true;
    }

    // Plugin packaging/build tools commonly create a throwaway HostProject.
    if (path.find("/hostproject/") != std::string::npos) return true;

    return false;
}

std::vector<std::string> infer_engine_roots(
    const std::vector<UnrealFileRecord>& records) {
    std::set<std::string> roots;

    for (const auto& record : records) {
        const auto path = normalized_slash_path(record.path);
        const auto marker = path.find("/engine/");
        if (marker == std::string::npos || marker == 0) continue;
        roots.insert(path.substr(0, marker));
    }

    return {roots.begin(), roots.end()};
}

bool path_is_under_root(std::string_view path, std::string_view root) {
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    return path.starts_with(root) && path[root.size()] == '/';
}

RecordClass classify_record(
    const UnrealFileRecord& record,
    const std::vector<std::string>& engine_roots) {
    if (is_hidden_record(record)) return RecordClass::Hidden;

    const auto path = normalized_slash_path(record.path);
    for (const auto& root : engine_roots) {
        if (path_is_under_root(path, root)) return RecordClass::Engine;
    }

    return RecordClass::User;
}

#ifdef _WIN32
struct DirectoryNode {
    std::uint64_t parent = 0;
    std::wstring name;
};

struct TargetNode {
    std::uint64_t parent = 0;
    std::wstring name;
    UnrealFileKind kind = UnrealFileKind::Project;
};

bool is_drive_root(const fs::path& root) {
    auto normalized = root.lexically_normal();
    return normalized.has_root_name() &&
           normalized.has_root_directory() &&
           normalized.relative_path().empty() &&
           normalized.root_name().wstring().size() == 2;
}

bool is_ntfs_volume(const fs::path& root) {
    wchar_t filesystem_name[64]{};
    auto root_text = root.root_path().wstring();
    return GetVolumeInformationW(root_text.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                 filesystem_name,
                                 static_cast<DWORD>(std::size(filesystem_name))) &&
           _wcsicmp(filesystem_name, L"NTFS") == 0;
}

fs::path reconstruct_target_path(
    const fs::path& drive_root,
    const TargetNode& target,
    const std::unordered_map<std::uint64_t, DirectoryNode>& directories) {

    std::vector<std::wstring> pieces;
    std::unordered_set<std::uint64_t> visited;
    std::uint64_t current = target.parent;

    while (current != 0 && visited.insert(current).second) {
        auto found = directories.find(current);
        if (found == directories.end()) break;
        const auto& node = found->second;
        if (!node.name.empty() && node.name != L".")
            pieces.push_back(node.name);
        if (node.parent == current) break;
        current = node.parent;
    }

    fs::path result = drive_root.root_path();
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it)
        result /= *it;
    result /= target.name;
    return result;
}

bool enumerate_ntfs_mft(const fs::path& root,
                        std::vector<UnrealFileRecord>& output,
                        std::string& warning) {
    if (!is_drive_root(root) || !is_ntfs_volume(root)) return false;

    const auto drive_name = root.root_name().wstring();
    const std::wstring volume_path = L"\\\\.\\" + drive_name;

    HANDLE volume = CreateFileW(
        volume_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (volume == INVALID_HANDLE_VALUE) {
        warning = "NTFS MFT access unavailable for " + root.string() +
                  " (error " + std::to_string(GetLastError()) +
                  "). Install/start the UPH index service for fast whole-drive indexing.";
        return false;
    }

    MftEnumDataV0 query{};
    query.StartFileReferenceNumber = 0;
    query.LowUsn = 0;
    query.HighUsn = MAXLONGLONG;

    std::vector<unsigned char> buffer(1024 * 1024);
    std::unordered_map<std::uint64_t, DirectoryNode> directories;
    std::vector<TargetNode> targets;
    bool received_any = false;

    for (;;) {
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(
            volume,
            FSCTL_ENUM_USN_DATA,
            &query,
            sizeof(query),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes,
            nullptr);

        if (!ok) {
            const auto error = GetLastError();
            if (error != ERROR_HANDLE_EOF && error != ERROR_NO_MORE_FILES && !received_any) {
                warning = "NTFS MFT enumeration failed for " + root.string() +
                          " (error " + std::to_string(error) +
                          "). The UPH index service can rebuild this volume with elevated access.";
                CloseHandle(volume);
                return false;
            }
            break;
        }

        if (bytes <= sizeof(DWORDLONG)) break;
        received_any = true;

        const auto next_reference = *reinterpret_cast<const DWORDLONG*>(buffer.data());
        const unsigned char* cursor = buffer.data() + sizeof(DWORDLONG);
        const unsigned char* end = buffer.data() + bytes;

        while (cursor + 8 <= end) {
            const auto record_length = *reinterpret_cast<const DWORD*>(cursor);
            const auto major_version = *reinterpret_cast<const WORD*>(cursor + sizeof(DWORD));
            if (record_length == 0 || cursor + record_length > end) break;

            if (major_version == 2 && record_length >= sizeof(UsnRecordV2)) {
                const auto* record = reinterpret_cast<const UsnRecordV2*>(cursor);
                const auto name_chars = record->FileNameLength / sizeof(wchar_t);
                const auto* name_ptr = reinterpret_cast<const wchar_t*>(
                    cursor + record->FileNameOffset);
                std::wstring name(name_ptr, name_chars);

                const auto frn = static_cast<std::uint64_t>(record->FileReferenceNumber);
                const auto parent = static_cast<std::uint64_t>(record->ParentFileReferenceNumber);

                if ((record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    directories[frn] = {parent, std::move(name)};
                } else {
                    UnrealFileKind kind{};
                    if (classify_extension(name, kind))
                        targets.push_back({parent, std::move(name), kind});
                }
            }

            cursor += record_length;
        }

        if (next_reference <= query.StartFileReferenceNumber) break;
        query.StartFileReferenceNumber = next_reference;
    }

    CloseHandle(volume);

    if (!received_any) {
        warning = "NTFS MFT enumeration returned no records for " + root.string() +
                  "; refusing a slow whole-drive fallback.";
        return false;
    }

    output.reserve(output.size() + targets.size());
    for (const auto& target : targets)
        output.push_back({target.kind, reconstruct_target_path(root, target, directories)});
    return true;
}
#endif

} // namespace

const char* unreal_file_kind_name(UnrealFileKind kind) {
    return kind == UnrealFileKind::Project ? "PROJECT" : "PLUGIN";
}

bool UnrealFileIndex::load(const fs::path& cache_path) {
    records_.clear();
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;

    std::string header;
    if (!std::getline(in, header) || header != "UPH_UNREAL_INDEX_V1") return false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 3 || line[1] != '\t') continue;
        UnrealFileKind kind{};
        if (line[0] == 'P') kind = UnrealFileKind::Project;
        else if (line[0] == 'U') kind = UnrealFileKind::Plugin;
        else continue;
        records_.push_back({kind, fs::path(line.substr(2))});
    }

    dedupe_and_sort(records_);
    return true;
}

bool UnrealFileIndex::save(const fs::path& cache_path) const {
    std::error_code ec;
    if (!cache_path.parent_path().empty())
        fs::create_directories(cache_path.parent_path(), ec);

    auto temporary = cache_path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out << "UPH_UNREAL_INDEX_V1\n";
    for (const auto& record : records_)
        out << (record.kind == UnrealFileKind::Project ? 'P' : 'U')
            << '\t' << record.path.string() << '\n';
    out.close();
    if (!out) return false;

#ifdef _WIN32
    const auto destination_w = cache_path.wstring();
    const auto temporary_w = temporary.wstring();
    if (MoveFileExW(temporary_w.c_str(), destination_w.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    fs::rename(temporary, cache_path, ec);
    if (!ec) return true;
#endif

    ec.clear();
    fs::copy_file(temporary, cache_path, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;

    std::error_code cleanup_ec;
    fs::remove(temporary, cleanup_ec);
    return true;
}

UnrealIndexStats UnrealFileIndex::rebuild(const std::vector<fs::path>& roots) {
    const auto started = std::chrono::steady_clock::now();
    records_.clear();

    UnrealIndexStats stats;
    std::set<std::string> seen_roots;

    for (const auto& requested_root : roots) {
        if (requested_root.empty()) continue;

        std::error_code ec;
        auto root = fs::absolute(requested_root, ec);
        if (ec) root = requested_root;
        root = root.lexically_normal();

        const auto key = normalized_key(root);
        if (!seen_roots.insert(key).second) continue;
        ++stats.roots;

        bool fast_path = false;
#ifdef _WIN32
        const bool whole_ntfs_volume = is_drive_root(root) && is_ntfs_volume(root);
        std::string warning;
        fast_path = enumerate_ntfs_mft(root, records_, warning);
        if (fast_path) {
            ++stats.ntfs_mft_roots;
        } else if (whole_ntfs_volume) {
            if (!warning.empty()) stats.warnings.push_back(std::move(warning));
            else stats.warnings.push_back(
                "Fast NTFS indexing was unavailable for " + root.string() +
                "; refusing a slow whole-drive directory walk.");
        } else {
            if (!warning.empty()) stats.warnings.push_back(std::move(warning));
            walk_root(root, records_, stats.warnings);
            ++stats.walked_roots;
        }
#else
        (void)fast_path;
        walk_root(root, records_, stats.warnings);
        ++stats.walked_roots;
#endif
    }

    dedupe_and_sort(records_);
    stats.records = records_.size();
    for (const auto& record : records_) {
        if (record.kind == UnrealFileKind::Project) ++stats.projects;
        else ++stats.plugins;
    }

    stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return stats;
}

std::vector<UnrealFileRecord> UnrealFileIndex::search(const std::string& query,
                                                       bool include_projects,
                                                       bool include_plugins,
                                                       std::size_t limit,
                                                       UnrealSearchScope scope) const {
    struct Scored {
        int score = 0;
        const UnrealFileRecord* record = nullptr;
    };

    std::vector<Scored> scored;
    scored.reserve(records_.size());
    const auto engine_roots = infer_engine_roots(records_);

    for (const auto& record : records_) {
        if (record.kind == UnrealFileKind::Project && !include_projects) continue;
        if (record.kind == UnrealFileKind::Plugin && !include_plugins) continue;

        const auto record_class = classify_record(record, engine_roots);
        if (scope == UnrealSearchScope::User && record_class != RecordClass::User) continue;
        if (scope == UnrealSearchScope::Engine && record_class != RecordClass::Engine) continue;

        int score = 0;
        if (!query.empty()) {
            const auto name_score = fuzzy_score(record.path.stem().string(), query);
            const auto path_score = fuzzy_score(record.path.string(), query);
            score = std::max(name_score < 0 ? -1 : name_score + 1000, path_score);
            if (score < 0) continue;
        }
        scored.push_back({score, &record});
    }

    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.record->kind != b.record->kind) return a.record->kind < b.record->kind;
        return lower_copy(a.record->path.string()) < lower_copy(b.record->path.string());
    });

    if (limit > 0 && scored.size() > limit) scored.resize(limit);

    std::vector<UnrealFileRecord> result;
    result.reserve(scored.size());
    for (const auto& item : scored) result.push_back(*item.record);
    return result;
}

UnrealIndexViewStats UnrealFileIndex::view_stats() const {
    UnrealIndexViewStats stats;
    const auto engine_roots = infer_engine_roots(records_);

    for (const auto& record : records_) {
        switch (classify_record(record, engine_roots)) {
            case RecordClass::User:
                ++stats.user_records;
                if (record.kind == UnrealFileKind::Project) ++stats.user_projects;
                else ++stats.user_plugins;
                break;
            case RecordClass::Engine:
                ++stats.engine_records;
                break;
            case RecordClass::Hidden:
                ++stats.hidden_records;
                break;
        }
    }

    return stats;
}

void UnrealFileIndex::replace_records(std::vector<UnrealFileRecord> records) {
    records_ = std::move(records);
    dedupe_and_sort(records_);
}

std::vector<fs::path> UnrealFileIndex::default_roots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        std::wstring root = L"A:\\";
        root[0] = static_cast<wchar_t>(L'A' + i);
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED)
            roots.emplace_back(root);
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        roots.emplace_back(home);
    roots.emplace_back("/Users/Shared");
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        roots.emplace_back(home);
#endif
    return roots;
}

bool run_unreal_file_index_self_test(std::string* error_message) {
    auto fail = [&](const std::string& message) {
        if (error_message) *error_message = message;
        return false;
    };

    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("uph-unreal-index-test-" + unique);
    auto project = root / "Games" / "TestProject" / "TestProject.uproject";
    auto plugin = root / "Games" / "TestProject" / "Plugins" / "Fancy" / "Fancy.uplugin";
    auto ignored = root / "Games" / "TestProject" / "readme.txt";
    auto cache = root / "index.tsv";

    std::error_code ec;
    fs::create_directories(project.parent_path(), ec);
    fs::create_directories(plugin.parent_path(), ec);
    {
        std::ofstream(project) << "{}\n";
        std::ofstream(plugin) << "{}\n";
        std::ofstream(ignored) << "ignored\n";
    }

    UnrealFileIndex built;
    auto stats = built.rebuild({root});
    if (stats.projects != 1 || stats.plugins != 1 || stats.records != 2) {
        fs::remove_all(root, ec);
        return fail("scan did not find exactly one project and one plugin");
    }

    if (!built.save(cache)) {
        fs::remove_all(root, ec);
        return fail("failed to save index cache");
    }

    UnrealFileIndex loaded;
    if (!loaded.load(cache)) {
        fs::remove_all(root, ec);
        return fail("failed to load index cache");
    }

    const auto projects = loaded.search(
        "testproject", true, false, 10, UnrealSearchScope::All);
    if (projects.size() != 1 || projects.front().kind != UnrealFileKind::Project ||
        projects.front().path.filename() != project.filename()) {
        fs::remove_all(root, ec);
        return fail("project search returned the wrong result");
    }

    const auto plugins = loaded.search(
        "fancy", false, true, 10, UnrealSearchScope::All);
    if (plugins.size() != 1 || plugins.front().kind != UnrealFileKind::Plugin ||
        plugins.front().path.filename() != plugin.filename()) {
        fs::remove_all(root, ec);
        return fail("plugin search returned the wrong result");
    }

    UnrealFileIndex classified;
    classified.replace_records({
        {UnrealFileKind::Project, fs::path("H:\\projects\\unreal\\MyGame\\MyGame.uproject")},
        {UnrealFileKind::Plugin, fs::path("H:\\projects\\unreal\\MyPlugin\\MyPlugin.uplugin")},
        {UnrealFileKind::Plugin, fs::path("H:\\unreal\\UE_5.8\\Engine\\Plugins\\Runtime\\EngineThing.uplugin")},
        {UnrealFileKind::Project, fs::path("H:\\unreal\\UE_5.8\\Samples\\Games\\Lyra\\Lyra.uproject")},
        {UnrealFileKind::Project, fs::path("C:\\Users\\Test\\AppData\\Roaming\\Code\\User\\History-abc\\Old.uproject")},
        {UnrealFileKind::Project, fs::path("H:\\projects\\PluginDev\\HostProject\\HostProject.uproject")},
        {UnrealFileKind::Project, fs::path("/broken/.uproject")}
    });

    const auto user_view = classified.search("", true, true, 0);
    if (user_view.size() != 2) {
        fs::remove_all(root, ec);
        return fail("default search did not filter engine/generated descriptors");
    }

    const auto engine_view = classified.search(
        "", true, true, 0, UnrealSearchScope::Engine);
    if (engine_view.size() != 2) {
        fs::remove_all(root, ec);
        return fail("engine search scope did not isolate engine descriptors");
    }

    const auto all_view = classified.search(
        "", true, true, 0, UnrealSearchScope::All);
    if (all_view.size() != 7) {
        fs::remove_all(root, ec);
        return fail("raw search scope did not preserve all descriptors");
    }

    const auto view_stats = classified.view_stats();
    if (view_stats.user_projects != 1 || view_stats.user_plugins != 1 ||
        view_stats.engine_records != 2 || view_stats.hidden_records != 3) {
        fs::remove_all(root, ec);
        return fail("filtered index stats are incorrect");
    }

    fs::remove_all(root, ec);
    if (error_message) error_message->clear();
    return true;
}

} // namespace uph
