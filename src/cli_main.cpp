#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "unreal_file_index.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <conio.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

static constexpr std::uintptr_t UPH_COPYDATA_SELECT_ENGINE = 0x55504801;
static constexpr std::uintptr_t UPH_COPYDATA_SELECT_PROJECT = 0x55504802;
static constexpr std::uintptr_t UPH_COPYDATA_EDITOR = 0x55504803;
static constexpr std::uintptr_t UPH_COPYDATA_OPEN = 0x55504804;
static constexpr std::uintptr_t UPH_COPYDATA_RUN = 0x55504805;
static constexpr std::uintptr_t UPH_COPYDATA_BUILD = 0x55504806;
static constexpr std::uintptr_t UPH_COPYDATA_PACKAGE = 0x55504807;
static constexpr std::uintptr_t UPH_COPYDATA_STOP = 0x55504808;
static constexpr std::uintptr_t UPH_COPYDATA_RERUN = 0x55504809;
static constexpr std::uintptr_t UPH_COPYDATA_DEPLOY = 0x5550480A;
static constexpr std::uintptr_t UPH_COPYDATA_ADD_PROJECT = 0x5550480B;
static constexpr std::uintptr_t UPH_COPYDATA_REMOVE_PROJECT = 0x5550480C;
static constexpr std::uintptr_t UPH_COPYDATA_ADD_ENGINE = 0x5550480D;
static constexpr std::uintptr_t UPH_COPYDATA_REMOVE_ENGINE = 0x5550480E;

struct Engine {
    std::string label;
    fs::path path;
};

struct CliState {
    fs::path project;
    std::vector<fs::path> recent_projects;
    fs::path engine;
    std::vector<fs::path> known_engines;
    std::vector<fs::path> hidden_engines;
    fs::path output;
    int compile_config = 0;
    int package_config = 0;
    int package_platform = 0;
};

static CliState g;
static constexpr std::array<const char*, 4> CONFIGS{"Development", "Debug", "Shipping", "Test"};
static constexpr std::array<const char*, 5> PLATFORMS{"Windows", "Mac", "Android", "iOS", "VisionOS"};

static std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return value;
}

static fs::path settings_path() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    return fs::path(base ? base : ".") / "UnrealProjectHandler" / "settings.ini";
#elif defined(__APPLE__)
    const char* base = std::getenv("HOME");
    return fs::path(base ? base : ".") / "Library" / "Application Support" / "UnrealProjectHandler" / "settings.ini";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "UnrealProjectHandler" / "settings.ini";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".config" / "UnrealProjectHandler" / "settings.ini";
#endif
}
static fs::path runtime_status_path() {
    return settings_path().parent_path() / "runtime-status.ini";
}

static fs::path user_unreal_index_path() {
    return settings_path().parent_path() / "unreal-files.idx";
}

static fs::path service_data_dir() {
#ifdef _WIN32
    const char* base = std::getenv("PROGRAMDATA");
    return fs::path(base ? base : "C:\\ProgramData") / "UnrealProjectHandler";
#else
    return {};
#endif
}

static fs::path service_unreal_index_path() {
#ifdef _WIN32
    return service_data_dir() / "unreal-files.idx";
#else
    return {};
#endif
}

static fs::path unreal_index_path() {
#ifdef _WIN32
    std::error_code ec;
    const auto shared = service_unreal_index_path();
    if (!shared.empty() && fs::is_regular_file(shared, ec)) return shared;
#endif
    return user_unreal_index_path();
}

static fs::path runtime_log_path() {
    return settings_path().parent_path() / "runtime.log";
}

#ifdef _WIN32
static HWND find_running_uph_window();
#endif

static std::map<std::string, std::string> read_key_values(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto split = line.find('=');
        if (split == std::string::npos) continue;
        values[line.substr(0, split)] = line.substr(split + 1);
    }
    return values;
}

static void print_runtime_logs(bool follow) {
    const auto path = runtime_log_path();
    std::uintmax_t offset = 0;

    auto print_new = [&]() {
        std::error_code ec;
        if (!fs::exists(path, ec)) return;
        auto size = fs::file_size(path, ec);
        if (ec) return;
        if (size < offset) offset = 0;

        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        in.seekg(static_cast<std::streamoff>(offset));
        std::string line;
        while (std::getline(in, line)) std::cout << line << '\n';
        auto pos = in.tellg();
        offset = pos < 0 ? size : static_cast<std::uintmax_t>(pos);
        std::cout.flush();
    };

    print_new();
    if (!follow) return;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        print_new();
#ifdef _WIN32
        if (!find_running_uph_window()) break;
#endif
    }
}



static int fuzzy_score(const std::string& text, const std::string& query) {
    if (query.empty()) return 0;
    const auto haystack = lower_copy(text);
    const auto needle = lower_copy(query);

    int score = 0;
    size_t at = 0;
    int previous = -2;
    for (char wanted : needle) {
        auto found = haystack.find(wanted, at);
        if (found == std::string::npos) return -1;
        score += 10;
        if (static_cast<int>(found) == previous + 1) score += 8;
        if (found == 0 || haystack[found - 1] == ' ' || haystack[found - 1] == '\\' ||
            haystack[found - 1] == '/' || haystack[found - 1] == '_' || haystack[found - 1] == '-')
            score += 5;
        score -= static_cast<int>(found - at);
        previous = static_cast<int>(found);
        at = found + 1;
    }
    score -= static_cast<int>(haystack.size() - needle.size()) / 8;
    return score;
}

static std::vector<size_t> fuzzy_matches(const std::vector<std::string>& items, const std::string& query) {
    std::vector<std::pair<int,size_t>> scored;
    scored.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        int score = fuzzy_score(items[i], query);
        if (score >= 0) scored.push_back({score, i});
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<size_t> result;
    result.reserve(scored.size());
    for (const auto& [score, index] : scored) {
        (void)score;
        result.push_back(index);
    }
    return result;
}

static std::optional<size_t> fallback_numbered_picker(const std::string& title,
                                                      const std::vector<std::string>& items) {
    if (items.empty()) return std::nullopt;
    std::cout << title << "\n";
    for (size_t i = 0; i < items.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << items[i] << "\n";
    std::cout << "Select [1-" << items.size() << "] or 0 to cancel: ";
    std::string input;
    if (!std::getline(std::cin, input)) return std::nullopt;
    try {
        auto value = std::stoul(input);
        if (value == 0 || value > items.size()) return std::nullopt;
        return value - 1;
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<size_t> interactive_picker(const std::string& title,
                                                const std::vector<std::string>& items,
                                                const std::string& initial_query = {}) {
    if (items.empty()) return std::nullopt;
#ifdef _WIN32
    if (!_isatty(_fileno(stdin)) || !_isatty(_fileno(stdout)))
        return fallback_numbered_picker(title, items);

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD original_mode = 0;
    if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &original_mode))
        return fallback_numbered_picker(title, items);

    if (!SetConsoleMode(output, original_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return fallback_numbered_picker(title, items);

    struct ConsoleRestore {
        HANDLE output;
        DWORD mode;
        ~ConsoleRestore() {
            std::cout << "\x1b[0m\x1b[?25h\x1b[?1049l";
            std::cout.flush();
            SetConsoleMode(output, mode);
        }
    } restore{output, original_mode};

    auto split_item = [](const std::string& item) {
        const auto separator = item.rfind("  ");
        if (separator == std::string::npos)
            return std::pair<std::string, std::string>{item, {}};
        return std::pair<std::string, std::string>{
            item.substr(0, separator),
            item.substr(separator + 2)
        };
    };

    auto visible_rows = [&]() -> size_t {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info)) return 7;
        const int height = info.srWindow.Bottom - info.srWindow.Top + 1;
        // Header/filter/footer use about six rows; each result uses two rows.
        const int available = std::max(6, height - 6);
        return static_cast<size_t>(std::clamp(available / 2, 3, 10));
    };

    std::string query = initial_query;
    size_t selected = 0;

    auto render = [&](const std::vector<size_t>& matches) {
        const size_t max_visible = visible_rows();
        size_t start_row = 0;
        if (!matches.empty()) {
            if (selected >= max_visible)
                start_row = selected - max_visible + 1;
            if (matches.size() > max_visible)
                start_row = std::min(start_row, matches.size() - max_visible);
        }
        const size_t end_row = std::min(matches.size(), start_row + max_visible);

        // Alternate screen + full redraw keeps navigation out of terminal
        // scrollback and avoids cursor bookkeeping getting out of sync.
        std::cout << "\x1b[H\x1b[2J";
        std::cout << "\x1b[1;36m" << title << "\x1b[0m";
        if (!matches.empty()) {
            std::cout << "  \x1b[2m"
                      << (selected + 1) << "/" << matches.size()
                      << "\x1b[0m";
        }
        std::cout << "\n";

        std::cout << "\x1b[2mFilter:\x1b[0m "
                  << (query.empty() ? "\x1b[2m(type to search)\x1b[0m" : query)
                  << "\n\n";

        if (matches.empty()) {
            std::cout << "  \x1b[2mNo matches\x1b[0m\n";
        } else {
            for (size_t row = start_row; row < end_row; ++row) {
                const auto [primary, secondary] = split_item(items[matches[row]]);
                const bool active = row == selected;

                if (active) {
                    std::cout << "\x1b[1;32m> " << primary << "\x1b[0m\n";
                    if (!secondary.empty())
                        std::cout << "  \x1b[2m" << secondary << "\x1b[0m\n";
                    else
                        std::cout << "\n";
                } else {
                    std::cout << "  " << primary << "\n";
                    if (!secondary.empty())
                        std::cout << "  \x1b[2m" << secondary << "\x1b[0m\n";
                    else
                        std::cout << "\n";
                }
            }
        }

        std::cout << "\n\x1b[2m"
                  << "Type to filter  |  Up/Down move  |  PgUp/PgDn jump  |  Enter select  |  Esc cancel"
                  << "\x1b[0m";
        std::cout.flush();
    };

    std::cout << "\x1b[?1049h\x1b[?25l";

    for (;;) {
        auto matches = fuzzy_matches(items, query);
        if (selected >= matches.size())
            selected = matches.empty() ? 0 : matches.size() - 1;
        render(matches);

        int key = _getwch();
        if (key == 0 || key == 224) {
            const int extended = _getwch();
            if (extended == 72 && selected > 0) {
                --selected; // Up
            } else if (extended == 80 && !matches.empty() && selected + 1 < matches.size()) {
                ++selected; // Down
            } else if (extended == 73 && selected > 0) {
                const size_t jump = visible_rows();
                selected = selected > jump ? selected - jump : 0; // Page Up
            } else if (extended == 81 && !matches.empty()) {
                const size_t jump = visible_rows();
                selected = std::min(selected + jump, matches.size() - 1); // Page Down
            } else if (extended == 71) {
                selected = 0; // Home
            } else if (extended == 79 && !matches.empty()) {
                selected = matches.size() - 1; // End
            }
            continue;
        }

        if (key == 27)
            return std::nullopt;

        if (key == 13) {
            if (!matches.empty())
                return matches[selected];
            continue;
        }

        if (key == 8 || key == 127) {
            if (!query.empty()) query.pop_back();
            selected = 0;
            continue;
        }

        if (key >= 32 && key <= 126) {
            query.push_back(static_cast<char>(key));
            selected = 0;
        }
    }
#else
    (void)initial_query;
    return fallback_numbered_picker(title, items);
#endif
}

static std::string normalized_path_key(const fs::path& path) {
    std::error_code ec;
    auto normalized = fs::weakly_canonical(path, ec);
    auto text = (ec ? path.lexically_normal() : normalized).string();
#ifdef _WIN32
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#endif
    return text;
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[maybe_unused]] static std::string json_string_value(const std::string& text, const std::string& key) {
    auto search = "\"" + key + "\"";
    auto p = text.find(search);
    if (p == std::string::npos) return {};
    p = text.find(':', p);
    if (p == std::string::npos) return {};
    p = text.find('"', p);
    if (p == std::string::npos) return {};
    std::string value;
    bool escape = false;
    for (++p; p < text.size(); ++p) {
        char c = text[p];
        if (escape) {
            value += c == 'n' ? '\n' : c;
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            break;
        } else {
            value += c;
        }
    }
    return value;
}

static void load_settings() {
    g = {};
    std::ifstream in(settings_path());
    std::string line;
    while (std::getline(in, line)) {
        auto split = line.find('=');
        if (split == std::string::npos) continue;
        auto key = line.substr(0, split);
        auto value = line.substr(split + 1);
        try {
            if (key == "project") g.project = value;
            else if (key == "recent_project" && !value.empty()) g.recent_projects.emplace_back(value);
            else if (key == "engine") g.engine = value;
            else if (key == "known_engine" && !value.empty()) g.known_engines.emplace_back(value);
            else if (key == "hidden_engine" && !value.empty()) g.hidden_engines.emplace_back(value);
            else if (key == "output") g.output = value;
            else if (key == "compile_config") g.compile_config = std::stoi(value);
            else if (key == "package_config") g.package_config = std::stoi(value);
            else if (key == "package_platform") g.package_platform = std::stoi(value);
        } catch (...) {}
    }
}

static void save_cli_selection_state() {
    std::vector<std::string> preserved;
    {
        std::ifstream in(settings_path());
        std::string line;
        while (std::getline(in, line)) {
            auto split = line.find('=');
            auto key = split == std::string::npos ? line : line.substr(0, split);
            if (key == "project" || key == "recent_project" || key == "engine" ||
                key == "known_engine" || key == "hidden_engine")
                continue;
            preserved.push_back(line);
        }
    }

    std::error_code ec;
    fs::create_directories(settings_path().parent_path(), ec);
    std::ofstream out(settings_path(), std::ios::trunc);
    out << "project=" << g.project.string() << '\n';
    for (const auto& project : g.recent_projects)
        if (!project.empty()) out << "recent_project=" << project.string() << '\n';
    out << "engine=" << g.engine.string() << '\n';
    for (const auto& engine : g.known_engines)
        if (!engine.empty()) out << "known_engine=" << engine.string() << '\n';
    for (const auto& engine : g.hidden_engines)
        if (!engine.empty()) out << "hidden_engine=" << engine.string() << '\n';
    for (const auto& line : preserved) out << line << '\n';
}

static void remember_cli_project(const fs::path& project) {
    if (project.empty()) return;
    auto key = normalized_path_key(project);
    g.recent_projects.erase(std::remove_if(g.recent_projects.begin(), g.recent_projects.end(),
        [&](const fs::path& item){ return normalized_path_key(item) == key; }), g.recent_projects.end());
    g.recent_projects.insert(g.recent_projects.begin(), project);
    if (g.recent_projects.size() > 20) g.recent_projects.resize(20);
}

static void remember_cli_engine(const fs::path& engine) {
    auto key = normalized_path_key(engine);
    g.hidden_engines.erase(std::remove_if(g.hidden_engines.begin(), g.hidden_engines.end(),
        [&](const fs::path& item){ return normalized_path_key(item) == key; }), g.hidden_engines.end());
    if (std::none_of(g.known_engines.begin(), g.known_engines.end(),
                     [&](const fs::path& item){ return normalized_path_key(item) == key; }))
        g.known_engines.push_back(engine);
}

static std::string powershell_single_quote(std::string text) {
    size_t at = 0;
    while ((at = text.find('\'', at)) != std::string::npos) {
        text.insert(at, "'");
        at += 2;
    }
    return text;
}

#ifdef _WIN32
static std::string capture_powershell_dialog(const std::string& script) {
    auto command = "powershell -NoProfile -STA -Command \"" + script + "\"";
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    char buffer[2048];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    _pclose(pipe);
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n')) output.pop_back();
    return output;
}

static fs::path browse_uproject(const fs::path& initial) {
    auto start = powershell_single_quote(initial.string());
    auto script =
        "Add-Type -AssemblyName System.Windows.Forms; "
        "$d=New-Object System.Windows.Forms.OpenFileDialog; "
        "$d.Filter='Unreal Project (*.uproject)|*.uproject'; "
        "$d.InitialDirectory='" + start + "'; "
        "if($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){[Console]::Write($d.FileName)}";
    return fs::path(capture_powershell_dialog(script));
}

static fs::path browse_engine_folder(const fs::path& initial) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        if (uninitialize) CoUninitialize();
        return {};
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select Unreal Engine Root Folder");

    if (!initial.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.wstring().c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    fs::path selected;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR raw_path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path) {
                selected = fs::path(raw_path);
                CoTaskMemFree(raw_path);
            }
            result->Release();
        }
    }

    dialog->Release();
    if (uninitialize) CoUninitialize();
    return selected;
}
#endif


static fs::path project_in_directory(const fs::path& directory) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return {};
    fs::path found;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file()) continue;
        if (lower_copy(entry.path().extension().string()) != ".uproject") continue;
        if (!found.empty()) return {};
        found = entry.path();
    }
    return found;
}

static fs::path current_directory_project() {
    std::error_code ec;
    return project_in_directory(fs::current_path(ec));
}

static fs::path resolve_project_selector(const std::string& selector) {
    fs::path candidate = selector;
    std::error_code ec;
    if (fs::exists(candidate, ec)) {
        if (fs::is_directory(candidate, ec)) return project_in_directory(candidate);
        if (lower_copy(candidate.extension().string()) == ".uproject")
            return fs::absolute(candidate, ec);
    }

    const auto wanted = lower_copy(selector);
    std::vector<fs::path> matches;
    auto consider = [&](const fs::path& project) {
        if (project.empty()) return;
        if (lower_copy(project.stem().string()) == wanted ||
            lower_copy(project.filename().string()) == wanted ||
            lower_copy(project.string()) == wanted)
            matches.push_back(project);
    };

    consider(current_directory_project());
    consider(g.project);
    for (const auto& project : g.recent_projects) consider(project);

    uph::UnrealFileIndex index;
    if (index.load(unreal_index_path())) {
        for (const auto& record : index.search(selector, true, false, 50))
            consider(record.path);
    }

    if (matches.empty()) return {};

    const auto first = normalized_path_key(matches.front());
    for (const auto& match : matches)
        if (normalized_path_key(match) != first) return {};
    return matches.front();
}

static bool valid_engine(const fs::path& path) {
    return fs::exists(path / "Engine/Build/Build.version") &&
           fs::exists(path / "Engine/Binaries");
}

static bool excluded_engine_path(const fs::path& path) {
    auto text = lower_copy(path.string());
    return text.find("fortnite") != std::string::npos;
}

static std::string engine_version(const fs::path& engine) {
    auto text = read_text(engine / "Engine/Build/Build.version");
    auto number = [&](const char* key) {
        auto p = text.find(key);
        if (p == std::string::npos) return std::string{};
        p = text.find(':', p);
        auto e = text.find_first_of(",\n", p + 1);
        auto value = text.substr(p + 1, e - p - 1);
        value.erase(std::remove_if(value.begin(), value.end(),
            [](unsigned char c){ return std::isspace(c); }), value.end());
        return value;
    };
    auto major = number("MajorVersion");
    auto minor = number("MinorVersion");
    return major.empty() ? engine.filename().string() : "Unreal Engine " + major + "." + minor;
}

#ifdef _WIN32
static std::vector<fs::path> registry_engine_paths() {
    std::vector<fs::path> paths;
    auto read_key = [&](HKEY root, const char* subkey) {
        HKEY key{};
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
        for (DWORD index = 0;; ++index) {
            char name[512]{};
            char value[4096]{};
            DWORD name_size = sizeof(name);
            DWORD value_size = sizeof(value);
            DWORD type{};
            auto result = RegEnumValueA(key, index, name, &name_size, nullptr, &type,
                                        reinterpret_cast<BYTE*>(value), &value_size);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && value[0])
                paths.emplace_back(value);
        }
        RegCloseKey(key);
    };
    read_key(HKEY_CURRENT_USER, "Software\\Epic Games\\Unreal Engine\\Builds");
    read_key(HKEY_LOCAL_MACHINE, "SOFTWARE\\EpicGames\\Unreal Engine");
    return paths;
}
#endif

static std::vector<Engine> discover_engines() {
    std::vector<Engine> engines;
    auto add = [&](fs::path path) {
        std::error_code ec;
        if (!valid_engine(path) || excluded_engine_path(path)) return;
        auto path_key = normalized_path_key(path);
        if (std::any_of(g.hidden_engines.begin(), g.hidden_engines.end(),
                        [&](const fs::path& item){ return normalized_path_key(item) == path_key; }))
            return;
        auto canonical = fs::weakly_canonical(path, ec);
        if (ec) canonical = path;
        auto key = normalized_path_key(canonical);
        for (const auto& e : engines)
            if (normalized_path_key(e.path) == key) return;
        engines.push_back({engine_version(canonical), canonical});
    };

    if (!g.engine.empty()) add(g.engine);
    for (const auto& engine : g.known_engines) add(engine);

    uph::UnrealFileIndex index;
    if (index.load(unreal_index_path())) {
        for (const auto& root : index.engine_roots()) add(root);
    }

#ifdef _WIN32
    for (const auto& path : registry_engine_paths()) add(path);
    for (const char* root : {"C:/Program Files/Epic Games", "D:/Epic Games", "H:/unreal"}) {
#else
    for (const char* root : {"/Users/Shared/Epic Games", "/Applications"}) {
#endif
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) add(entry.path());
    }

    std::sort(engines.begin(), engines.end(),
        [](const Engine& a, const Engine& b){ return a.label > b.label; });
    return engines;
}

static bool resolve_engine_selector(const std::string& selector, fs::path& result) {
    fs::path candidate = selector;
    if (valid_engine(candidate)) {
        std::error_code ec;
        result = fs::weakly_canonical(candidate, ec);
        if (ec) result = candidate;
        return true;
    }

    auto engines = discover_engines();
    const auto wanted = lower_copy(selector);
    std::vector<fs::path> exact, partial;
    for (const auto& engine : engines) {
        auto label = lower_copy(engine.label);
        auto folder = lower_copy(engine.path.filename().string());
        if (label == wanted || folder == wanted || label == "unreal engine " + wanted ||
            folder == "ue_" + wanted)
            exact.push_back(engine.path);
        else if (label.find(wanted) != std::string::npos || folder.find(wanted) != std::string::npos)
            partial.push_back(engine.path);
    }

    const auto& matches = !exact.empty() ? exact : partial;
    if (matches.size() != 1) return false;
    result = matches.front();
    return true;
}

static int config_index(const std::string& value) {
    auto wanted = lower_copy(value);
    for (size_t i = 0; i < CONFIGS.size(); ++i)
        if (lower_copy(CONFIGS[i]) == wanted) return static_cast<int>(i);
    return -1;
}

static int platform_index(const std::string& value) {
    auto wanted = lower_copy(value);
    if (wanted == "win64" || wanted == "win") wanted = "windows";
    if (wanted == "osx" || wanted == "macos") wanted = "mac";
    if (wanted == "vision") wanted = "visionos";
    for (size_t i = 0; i < PLATFORMS.size(); ++i)
        if (lower_copy(PLATFORMS[i]) == wanted) return static_cast<int>(i);
    return -1;
}

#ifdef _WIN32
static HWND find_running_uph_window() {
    return FindWindowW(nullptr, L"UPH - Unreal Project Handler");
}

static bool foreground_running_uph() {
    HWND hwnd = find_running_uph_window();
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_RESTORE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    return true;
}

static fs::path current_executable_path() {
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    buffer.resize(length);
    return fs::path(buffer);
}


static fs::path index_service_executable_path() {
    auto sibling = current_executable_path().parent_path() / "uph-index-service.exe";
    std::error_code ec;
    if (fs::is_regular_file(sibling, ec)) return sibling;

    auto installed = service_data_dir() / "uph-index-service.exe";
    ec.clear();
    if (fs::is_regular_file(installed, ec)) return installed;
    return sibling;
}

static int run_index_service_helper(const std::wstring& argument, bool elevate) {
    const auto executable = index_service_executable_path();
    std::error_code ec;
    if (!fs::is_regular_file(executable, ec)) {
        std::cerr << "UPH: index service executable not found: "
                  << executable.string() << '\n'
                  << "Run: make cli\n";
        return 2;
    }

    if (elevate) {
        const auto executable_w = executable.wstring();
        const auto directory_w = executable.parent_path().wstring();

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = executable_w.c_str();
        info.lpParameters = argument.c_str();
        info.lpDirectory = directory_w.c_str();
        info.nShow = SW_HIDE;

        if (!ShellExecuteExW(&info)) {
            const DWORD code = GetLastError();
            if (code == ERROR_CANCELLED)
                std::cerr << "UPH: service command cancelled at the UAC prompt.\n";
            else
                std::cerr << "UPH: could not elevate index service command (error "
                          << code << ").\n";
            return 2;
        }

        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD exit_code = 2;
        GetExitCodeProcess(info.hProcess, &exit_code);
        CloseHandle(info.hProcess);
        return static_cast<int>(exit_code);
    }

    std::wstring command_line =
        L"\"" + executable.wstring() + L"\" " + argument;
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.wstring().c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            executable.parent_path().wstring().c_str(),
            &startup,
            &process)) {
        std::cerr << "UPH: could not run index service helper (error "
                  << GetLastError() << ").\n";
        return 2;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 2;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

static fs::path selected_editor_path() {
    if (g.engine.empty()) return {};
    return g.engine / "Engine/Binaries/Win64/UnrealEditor.exe";
}

static bool launch_project_direct(const fs::path& project, bool game) {
    auto editor = selected_editor_path();
    if (!fs::is_regular_file(project) || !fs::is_regular_file(editor)) return false;

    std::wstring parameters = L"\"" + project.wstring() + L"\"";
    if (game) parameters += L" -game -log";

    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", editor.wstring().c_str(), parameters.c_str(),
        editor.parent_path().wstring().c_str(), SW_SHOWNORMAL));
    return result > 32;
}

static bool launch_uph_app() {
    if (find_running_uph_window()) return true;
    auto app = current_executable_path().parent_path() / "uph-app.exe";
    if (!fs::exists(app)) {
        std::cerr << "UPH: could not find " << app.string() << "\n";
        return false;
    }
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", app.wstring().c_str(), nullptr,
        app.parent_path().wstring().c_str(), SW_SHOWNORMAL));
    if (result <= 32) return false;

    for (int i = 0; i < 150; ++i) {
        if (find_running_uph_window()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static bool send_uph_copydata(std::uintptr_t command, const std::string& value) {
    HWND hwnd = find_running_uph_window();
    if (!hwnd) return false;
    std::string payload = value.empty() ? std::string("\n") : value;
    COPYDATASTRUCT copy{};
    copy.dwData = command;
    copy.cbData = static_cast<DWORD>(payload.size() + 1);
    copy.lpData = payload.data();
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 15000, &result))
        return false;
    return result == TRUE;
}

static bool ensure_app_for_action() {
    return find_running_uph_window() || launch_uph_app();
}
#else
static bool launch_uph_app() {
    return std::system("uph-app >/dev/null 2>&1 &") == 0;
}
#endif

static std::string project_payload(const fs::path& project) {
    std::ostringstream out;
    if (!project.empty()) out << "project=" << project.string() << '\n';
    return out.str();
}

static void print_help() {
    std::cout <<
        "UPH - Unreal Project Handler\n\n"
        "Usage:\n"
        "  uph                         Start UPH, or foreground the running app\n"
        "  uph status                  Show saved settings and live app state\n"
        "  uph logs [--follow]         Show/follow UPH runtime logs\n"
        "  uph find [query] [--projects|--plugins] [--engine|--all] [--limit N]\n"
        "                              Search useful Unreal descriptors by default\n"
        "  uph index status|rebuild [root...]|clear|test\n"
        "  uph index service status|install|start|stop|uninstall\n"
        "                              Manage the Unreal file index/service\n"
        "  uph stop                    Stop the current tracked UPH operation\n"
        "  uph rerun                   Repeat the last CLI build/package/deploy\n"
        "  uph deploy [project] [config]\n"
        "                              Package Android, install, and launch\n"
        "  uph project list|current|select [name|path]\n"
        "  uph project add [path] | remove [name|path]\n"
        "  uph engine list|current|select [name|path]\n"
        "  uph engine add [path] | remove [name|path]\n"
        "                              select/remove with no value opens fuzzy picker\n"
        "  uph editor\n"
        "  uph open [project]\n"
        "  uph run [project]\n"
        "  uph build [project] [config]\n"
        "  uph package [project] [platform] [config]\n"
        "      --project <project>\n"
        "      --platform <platform>\n"
        "      --config <config>\n"
        "      --output <directory>\n";
}

static std::vector<fs::path> selectable_projects() {
    std::vector<fs::path> projects;
    std::set<std::string> seen;
    auto add = [&](const fs::path& project) {
        if (project.empty() || !fs::is_regular_file(project)) return;
        auto key = normalized_path_key(project);
        if (seen.insert(key).second) projects.push_back(project);
    };
    add(current_directory_project());
    add(g.project);
    for (const auto& project : g.recent_projects) add(project);

    uph::UnrealFileIndex index;
    if (index.load(unreal_index_path())) {
        for (const auto& record : index.search("", true, false, 0))
            add(record.path);
    }

    return projects;
}


static int command_unreal_index(int argc, char** argv) {
    const std::string action = argc >= 3 ? lower_copy(argv[2]) : "status";

#ifdef _WIN32
    if (action == "service") {
        const std::string service_action =
            argc >= 4 ? lower_copy(argv[3]) : "status";

        if (service_action == "status")
            return run_index_service_helper(L"--status", false);

        if (service_action == "install") {
            const int result = run_index_service_helper(L"--install", true);
            if (result == 0)
                std::cout
                    << "UPH index service installed and started.\n"
                    << "It is building the shared MFT index now; use "
                       "uph index service status to inspect it.\n";
            return result;
        }

        if (service_action == "start")
            return run_index_service_helper(L"--start", true);
        if (service_action == "stop")
            return run_index_service_helper(L"--stop", true);
        if (service_action == "uninstall")
            return run_index_service_helper(L"--uninstall", true);

        std::cerr
            << "Usage: uph index service "
               "status|install|start|stop|uninstall\n";
        return 2;
    }
#endif

    if (action == "status") {
        const auto cache = unreal_index_path();
        uph::UnrealFileIndex index;
        if (!index.load(cache)) {
            std::cout << "Unreal file index: not built\n"
                      << "Cache: " << cache.string() << '\n';
#ifdef _WIN32
            std::cout
                << "For fast whole-drive indexing: "
                   "uph index service install\n";
#endif
            return 1;
        }

        const auto view = index.view_stats();

        std::cout << "Unreal file index: ready\n"
                  << "Cache:    " << cache.string() << '\n';
#ifdef _WIN32
        std::cout << "Source:   "
                  << (cache == service_unreal_index_path()
                          ? "UPH index service"
                          : "per-user manual cache")
                  << '\n';
#endif
        std::cout << "User projects:      " << view.user_projects << '\n'
                  << "User plugins:       " << view.user_plugins << '\n'
                  << "User total:         " << view.user_records << '\n'
                  << "Engine descriptors: " << view.engine_records << '\n'
                  << "Hidden/generated:   " << view.hidden_records << '\n'
                  << "Raw indexed total:  " << index.records().size() << '\n';
        return 0;
    }

    if (action == "clear") {
        const auto cache = user_unreal_index_path();
        std::error_code ec;
        const bool removed = fs::remove(cache, ec);
        if (ec) {
            std::cerr << "UPH: failed to remove manual index: "
                      << ec.message() << '\n';
            return 2;
        }
        std::cout
            << (removed
                    ? "Cleared per-user manual Unreal file index.\n"
                    : "Per-user manual Unreal file index was already empty.\n");
#ifdef _WIN32
        ec.clear();
        if (fs::is_regular_file(service_unreal_index_path(), ec))
            std::cout
                << "The shared service index remains active. Stop/uninstall "
                   "the service to remove it.\n";
#endif
        return 0;
    }

    if (action == "test") {
        std::string error;
        if (!uph::run_unreal_file_index_self_test(&error)) {
            std::cerr << "UPH: index self-test failed: "
                      << error << '\n';
            return 2;
        }
        std::cout << "Unreal file index self-test passed.\n";
        return 0;
    }

    if (action == "rebuild") {
        std::vector<fs::path> roots;
        for (int i = 3; i < argc; ++i)
            roots.emplace_back(argv[i]);
        if (roots.empty())
            roots = uph::UnrealFileIndex::default_roots();

        if (roots.empty()) {
            std::cerr
                << "UPH: no index roots available. Supply one or more paths.\n";
            return 2;
        }

        std::cout << "Indexing .uproject and .uplugin files";
        if (roots.size() == 1)
            std::cout << " under " << roots.front().string();
        else
            std::cout << " across " << roots.size() << " roots";
        std::cout << "...\n";

        uph::UnrealFileIndex index;
        const auto stats = index.rebuild(roots);

        for (const auto& warning : stats.warnings)
            std::cerr << "Warning: " << warning << '\n';

        const auto completed_roots =
            stats.ntfs_mft_roots + stats.walked_roots;
        if (completed_roots == 0) {
            std::cerr
                << "UPH: no requested root could be indexed.\n";
#ifdef _WIN32
            std::cerr
                << "For a whole NTFS drive, install the privileged live "
                   "indexer once:\n"
                << "  uph index service install\n";
#endif
            return 2;
        }

        const auto cache = user_unreal_index_path();
        if (!index.save(cache)) {
            std::cerr
                << "UPH: scan completed but the index could not be saved to "
                << cache.string() << '\n';
            return 2;
        }

        const auto view = index.view_stats();
        std::cout << "Indexed " << stats.records << " raw Unreal files in "
                  << stats.elapsed_ms << " ms\n"
                  << "  User projects: " << view.user_projects << '\n'
                  << "  User plugins:  " << view.user_plugins << '\n'
                  << "  Engine descriptors: " << view.engine_records << '\n'
                  << "  Hidden/generated: " << view.hidden_records << '\n'
                  << "  NTFS MFT roots: " << stats.ntfs_mft_roots << '\n'
                  << "  Directory-walk roots: " << stats.walked_roots << '\n'
                  << "Manual cache: " << cache.string() << '\n';
        return completed_roots == stats.roots ? 0 : 1;
    }

    std::cerr
        << "Usage: uph index "
           "status|rebuild [root...]|clear|test|service ...\n";
    return 2;
}

static int command_find(int argc, char** argv) {
    bool include_projects = true;
    bool include_plugins = true;
    bool type_filter_set = false;
    uph::UnrealSearchScope scope = uph::UnrealSearchScope::User;
    bool scope_set = false;
    std::size_t limit = 50;
    std::vector<std::string> query_parts;

    for (int i = 2; i < argc; ++i) {
        const std::string value = argv[i];
        if (value == "--projects" || value == "--project") {
            if (!type_filter_set) {
                include_projects = false;
                include_plugins = false;
                type_filter_set = true;
            }
            include_projects = true;
            continue;
        }
        if (value == "--plugins" || value == "--plugin") {
            if (!type_filter_set) {
                include_projects = false;
                include_plugins = false;
                type_filter_set = true;
            }
            include_plugins = true;
            continue;
        }
        if (value == "--engine") {
            if (scope_set && scope != uph::UnrealSearchScope::Engine) {
                std::cerr << "UPH: --engine and --all cannot be combined.\n";
                return 2;
            }
            scope = uph::UnrealSearchScope::Engine;
            scope_set = true;
            continue;
        }
        if (value == "--all" || value == "--raw") {
            if (scope_set && scope != uph::UnrealSearchScope::All) {
                std::cerr << "UPH: --engine and --all cannot be combined.\n";
                return 2;
            }
            scope = uph::UnrealSearchScope::All;
            scope_set = true;
            continue;
        }
        if (value == "--limit" && i + 1 < argc) {
            try {
                limit = static_cast<std::size_t>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "UPH: invalid --limit value.\n";
                return 2;
            }
            continue;
        }
        query_parts.push_back(value);
    }

    std::string query;
    for (const auto& part : query_parts) {
        if (!query.empty()) query += ' ';
        query += part;
    }

    uph::UnrealFileIndex index;
    if (!index.load(unreal_index_path())) {
        std::cerr << "UPH: Unreal file index has not been built yet.\n"
#ifdef _WIN32
                  << "Recommended: uph index service install\n"
#endif
                  << "For a smaller manual root: uph index rebuild <path>\n";
        return 2;
    }

    const auto matches = index.search(
        query, include_projects, include_plugins, limit, scope);
    if (matches.empty()) {
        std::cout << "No matching Unreal projects or plugins.\n";
        return 1;
    }

    if (query.empty()) {
        std::vector<std::string> labels;
        labels.reserve(matches.size());
        for (const auto& record : matches)
            labels.push_back(std::string(uph::unreal_file_kind_name(record.kind)) + "  " +
                             record.path.stem().string() + "  " + record.path.string());
        auto picked = interactive_picker("Find Unreal Project / Plugin", labels);
        if (!picked) return 1;
        const auto& record = matches[*picked];
        std::cout << uph::unreal_file_kind_name(record.kind) << "  "
                  << record.path.stem().string() << '\n'
                  << record.path.string() << '\n';
        return 0;
    }

    for (const auto& record : matches)
        std::cout << uph::unreal_file_kind_name(record.kind) << "  "
                  << record.path.stem().string() << "  "
                  << record.path.string() << '\n';
    return 0;
}

static int command_project(int argc, char** argv) {
    std::string action = argc >= 3 ? lower_copy(argv[2]) : "current";
    if (action == "list") {
        std::set<std::string> seen;
        auto print = [&](const fs::path& project) {
            if (project.empty()) return;
            auto key = normalized_path_key(project);
            if (!seen.insert(key).second) return;
            bool current = !g.project.empty() && key == normalized_path_key(g.project);
            std::cout << (current ? "* " : "  ") << project.stem().string()
                      << "  " << project.string() << '\n';
        };
        print(current_directory_project());
        print(g.project);
        for (const auto& project : g.recent_projects) print(project);
        return 0;
    }

    if (action == "current") {
        auto local = current_directory_project();
        if (!local.empty()) {
            std::cout << local.string() << '\n';
            return 0;
        }
        if (g.project.empty()) {
            std::cout << "No project selected.\n";
            return 1;
        }
        std::cout << g.project.string() << '\n';
        return 0;
    }

    if (action == "add") {
        fs::path project;
        if (argc >= 4) {
            fs::path supplied = argv[3];
            std::error_code ec;
            if (fs::is_directory(supplied, ec)) project = project_in_directory(supplied);
            else if (fs::is_regular_file(supplied, ec) && lower_copy(supplied.extension().string()) == ".uproject")
                project = fs::absolute(supplied, ec);
        } else {
            project = current_directory_project();
#ifdef _WIN32
            if (project.empty()) {
                std::error_code ec;
                project = browse_uproject(fs::current_path(ec));
            }
#endif
        }
        if (project.empty() || !fs::is_regular_file(project)) {
            std::cerr << "UPH: no valid .uproject selected.\n";
            return 2;
        }
#ifdef _WIN32
        if (find_running_uph_window()) {
            if (!send_uph_copydata(UPH_COPYDATA_ADD_PROJECT, project.string())) {
                std::cerr << "UPH: the app rejected the project add.\n";
                return 2;
            }
        } else
#endif
        {
            remember_cli_project(project);
            save_cli_selection_state();
        }
        std::cout << "Added project: " << project.string() << '\n';
        return 0;
    }

    if (action == "remove") {
        fs::path project;
        if (argc >= 4) {
            project = resolve_project_selector(argv[3]);
        } else {
            auto projects = selectable_projects();
            std::vector<std::string> labels;
            for (const auto& candidate : projects)
                labels.push_back(candidate.stem().string() + "  " + candidate.string());
            auto picked = interactive_picker("Remove Unreal Project", labels);
            if (!picked) return 1;
            project = projects[*picked];
        }
        if (project.empty()) {
            std::cerr << "UPH: project not found.\n";
            return 2;
        }
#ifdef _WIN32
        if (find_running_uph_window()) {
            if (!send_uph_copydata(UPH_COPYDATA_REMOVE_PROJECT, project.string())) {
                std::cerr << "UPH: the app rejected the project removal.\n";
                return 2;
            }
        } else
#endif
        {
            auto key = normalized_path_key(project);
            g.recent_projects.erase(std::remove_if(g.recent_projects.begin(), g.recent_projects.end(),
                [&](const fs::path& item){ return normalized_path_key(item) == key; }), g.recent_projects.end());
            if (!g.project.empty() && normalized_path_key(g.project) == key) g.project.clear();
            save_cli_selection_state();
        }
        std::cout << "Removed project: " << project.string() << '\n';
        return 0;
    }

    if (action == "select") {
        fs::path project;
        if (argc < 4) {
            auto projects = selectable_projects();
            std::vector<std::string> labels;
            labels.reserve(projects.size());
            for (const auto& candidate : projects)
                labels.push_back(candidate.stem().string() + "  " + candidate.string());
            auto picked = interactive_picker("Select Unreal Project", labels);
            if (!picked) return 1;
            project = projects[*picked];
        } else {
            project = resolve_project_selector(argv[3]);
            if (project.empty()) {
                auto projects = selectable_projects();
                std::vector<std::string> labels;
                labels.reserve(projects.size());
                for (const auto& candidate : projects)
                    labels.push_back(candidate.stem().string() + "  " + candidate.string());
                auto picked = interactive_picker("Select Unreal Project", labels, argv[3]);
                if (!picked) return 1;
                project = projects[*picked];
            }
        }
#ifdef _WIN32
        if (find_running_uph_window()) {
            if (!send_uph_copydata(UPH_COPYDATA_SELECT_PROJECT, project.string())) {
                std::cerr << "UPH: the app rejected the project change.\n";
                return 2;
            }
        } else {
            g.project = project;
            remember_cli_project(project);
            save_cli_selection_state();
        }
#else
        g.project = project;
        remember_cli_project(project);
        save_cli_selection_state();
#endif
        std::cout << "Selected project: " << project.string() << '\n';
        return 0;
    }

    std::cerr << "Usage: uph project list|current|select|add|remove [name|path]\n";
    return 2;
}

static int command_engine(int argc, char** argv) {
    std::string action = argc >= 3 ? lower_copy(argv[2]) : "current";
    if (action == "list") {
        for (const auto& engine : discover_engines()) {
            bool current = !g.engine.empty() &&
                normalized_path_key(engine.path) == normalized_path_key(g.engine);
            std::cout << (current ? "* " : "  ") << engine.label
                      << "  " << engine.path.string() << '\n';
        }
        return 0;
    }

    if (action == "current") {
        if (g.engine.empty()) {
            std::cout << "No engine selected.\n";
            return 1;
        }
        std::cout << engine_version(g.engine) << '\n' << g.engine.string() << '\n';
        return 0;
    }

    if (action == "add") {
        fs::path engine;
        if (argc >= 4) {
            engine = fs::path(argv[3]);
        } else {
            std::error_code ec;
            auto cwd = fs::current_path(ec);
            if (valid_engine(cwd)) engine = cwd;
#ifdef _WIN32
            else engine = browse_engine_folder(cwd);
#endif
        }
        if (!valid_engine(engine) || excluded_engine_path(engine)) {
            std::cerr << "UPH: selected path is not a usable Unreal Engine root.\n";
            return 2;
        }
        std::error_code ec;
        engine = fs::weakly_canonical(engine, ec);
#ifdef _WIN32
        if (find_running_uph_window()) {
            if (!send_uph_copydata(UPH_COPYDATA_ADD_ENGINE, engine.string())) {
                std::cerr << "UPH: the app rejected the engine add.\n";
                return 2;
            }
        } else
#endif
        {
            remember_cli_engine(engine);
            save_cli_selection_state();
        }
        std::cout << "Added engine: " << engine_version(engine) << '\n'
                  << engine.string() << '\n';
        return 0;
    }

    if (action == "remove") {
        fs::path engine;
        if (argc >= 4) {
            if (!resolve_engine_selector(argv[3], engine)) {
                auto engines = discover_engines();
                std::vector<std::string> labels;
                for (const auto& candidate : engines)
                    labels.push_back(candidate.label + "  " + candidate.path.string());
                auto picked = interactive_picker("Remove Unreal Engine", labels, argv[3]);
                if (!picked) return 1;
                engine = engines[*picked].path;
            }
        } else {
            auto engines = discover_engines();
            std::vector<std::string> labels;
            for (const auto& candidate : engines)
                labels.push_back(candidate.label + "  " + candidate.path.string());
            auto picked = interactive_picker("Remove Unreal Engine", labels);
            if (!picked) return 1;
            engine = engines[*picked].path;
        }
        if (engine.empty()) {
            std::cerr << "UPH: engine not found.\n";
            return 2;
        }
#ifdef _WIN32
        if (find_running_uph_window()) {
            if (!send_uph_copydata(UPH_COPYDATA_REMOVE_ENGINE, engine.string())) {
                std::cerr << "UPH: the app rejected the engine removal.\n";
                return 2;
            }
        } else
#endif
        {
            auto key = normalized_path_key(engine);
            g.known_engines.erase(std::remove_if(g.known_engines.begin(), g.known_engines.end(),
                [&](const fs::path& item){ return normalized_path_key(item) == key; }), g.known_engines.end());
            if (std::none_of(g.hidden_engines.begin(), g.hidden_engines.end(),
                             [&](const fs::path& item){ return normalized_path_key(item) == key; }))
                g.hidden_engines.push_back(engine);
            if (!g.engine.empty() && normalized_path_key(g.engine) == key) g.engine.clear();
            save_cli_selection_state();
        }
        std::cout << "Removed engine: " << engine.string() << '\n';
        return 0;
    }

    if (action == "select") {
        fs::path engine;
        if (argc < 4) {
            auto engines = discover_engines();
            std::vector<std::string> labels;
            labels.reserve(engines.size());
            for (const auto& candidate : engines)
                labels.push_back(candidate.label + "  " + candidate.path.string());
            auto picked = interactive_picker("Select Unreal Engine", labels);
            if (!picked) return 1;
            engine = engines[*picked].path;
        } else if (!resolve_engine_selector(argv[3], engine)) {
            auto engines = discover_engines();
            std::vector<std::string> labels;
            labels.reserve(engines.size());
            for (const auto& candidate : engines)
                labels.push_back(candidate.label + "  " + candidate.path.string());
            auto picked = interactive_picker("Select Unreal Engine", labels, argv[3]);
            if (!picked) return 1;
            engine = engines[*picked].path;
        }
#ifdef _WIN32
        if (find_running_uph_window()) {
            if (!send_uph_copydata(UPH_COPYDATA_SELECT_ENGINE, engine.string())) {
                std::cerr << "UPH: the app rejected the engine change.\n";
                return 2;
            }
        } else {
            g.engine = engine;
            remember_cli_engine(engine);
            save_cli_selection_state();
        }
#else
        g.engine = engine;
        remember_cli_engine(engine);
        save_cli_selection_state();
#endif
        std::cout << "Selected engine: " << engine_version(engine) << '\n'
                  << engine.string() << '\n';
        return 0;
    }

    std::cerr << "Usage: uph engine list|current|select|add|remove [name|path]\n";
    return 2;
}

int main(int argc, char** argv) {
    if (argc <= 1) {
#ifdef _WIN32
        if (foreground_running_uph()) return 0;
#endif
        return launch_uph_app() ? 0 : 2;
    }

    load_settings();
    const std::string command = lower_copy(argv[1]);

    if (command == "help" || command == "--help" || command == "-h") {
        print_help();
        return 0;
    }

    if (command == "status") {
        std::cout << "Project:   " << (g.project.empty() ? "None" : g.project.string()) << '\n';
        std::cout << "Engine:    " << (g.engine.empty() ? "None" : engine_version(g.engine)) << '\n';
        if (!g.engine.empty()) std::cout << "           " << g.engine.string() << '\n';
        std::cout << "Compile:   " << CONFIGS[std::clamp(g.compile_config, 0, 3)] << '\n';
        std::cout << "Package:   " << PLATFORMS[std::clamp(g.package_platform, 0, 4)]
                  << " / " << CONFIGS[std::clamp(g.package_config, 0, 3)] << '\n';

        auto runtime = read_key_values(runtime_status_path());
#ifdef _WIN32
        const bool app_running = find_running_uph_window() != nullptr;
#else
        const bool app_running = runtime["app_running"] == "1";
#endif
        std::cout << "App:       " << (app_running ? "Running" : "Not running") << '\n';
        if (!runtime.empty()) {
            const auto running = runtime["process_running"] == "1";
            auto operation = runtime["operation"];
            auto result = runtime["result"];
            auto progress = runtime["progress"];
            std::cout << "Operation: " << (running ? (operation.empty() ? "Running" : operation) : "Idle");
            if (!result.empty() && result != "none") std::cout << " (" << result << ")";
            std::cout << '\n';
            if (!progress.empty()) std::cout << "Progress:  " << progress << '\n';
            if (!runtime["device"].empty()) std::cout << "Device:    " << runtime["device"] << '\n';
            if (!runtime["android_package"].empty()) std::cout << "Android:   " << runtime["android_package"] << '\n';
            if (!runtime["adb_status"].empty()) std::cout << "ADB:       " << runtime["adb_status"] << '\n';
        }
        return 0;
    }

    if (command == "logs") {
        bool follow = argc >= 3 && (std::string(argv[2]) == "--follow" || std::string(argv[2]) == "-f");
        print_runtime_logs(follow);
        return 0;
    }

    if (command == "find") return command_find(argc, argv);
    if (command == "index") return command_unreal_index(argc, argv);
    if (command == "project") return command_project(argc, argv);
    if (command == "engine") return command_engine(argc, argv);

#ifdef _WIN32
    if (command == "open" || command == "run") {
        fs::path project;
        if (argc >= 3) project = resolve_project_selector(argv[2]);
        else {
            project = current_directory_project();
            if (project.empty()) project = g.project;
        }
        if (project.empty()) {
            std::cerr << "UPH: no project selected and no .uproject found in the current directory.\n";
            return 2;
        }

        if (find_running_uph_window()) {
            auto ipc = command == "run" ? UPH_COPYDATA_RUN : UPH_COPYDATA_OPEN;
            if (!send_uph_copydata(ipc, project_payload(project))) {
                std::cerr << "UPH: the running app rejected the " << command << " request.\n";
                return 2;
            }
            std::cout << (command == "run" ? "Run" : "Open") << " requested through UPH.\n";
            return 0;
        }

        if (!launch_project_direct(project, command == "run")) {
            std::cerr << "UPH: could not launch the selected Unreal Editor directly. Check the selected engine.\n";
            return 2;
        }
        std::cout << "Launching " << project.stem().string()
                  << (command == "run" ? " as game" : " in Unreal Editor") << ".\n";
        return 0;
    }

    if (!ensure_app_for_action()) {
        std::cerr << "UPH: could not start or connect to the desktop app.\n";
        return 2;
    }

    if (command == "stop") {
        if (!send_uph_copydata(UPH_COPYDATA_STOP, "")) {
            std::cerr << "UPH: no tracked operation is currently running.\n";
            return 2;
        }
        std::cout << "Stop requested.\n";
        return 0;
    }

    if (command == "rerun") {
        if (!send_uph_copydata(UPH_COPYDATA_RERUN, "")) {
            std::cerr << "UPH: nothing rerunnable is available, or UPH is busy.\n";
            return 2;
        }
        std::cout << "Last UPH operation started again.\n";
        return 0;
    }

    if (command == "editor") {
        if (!send_uph_copydata(UPH_COPYDATA_EDITOR, "")) {
            std::cerr << "UPH: the app rejected the editor launch.\n";
            return 2;
        }
        std::cout << "Editor launch requested through UPH.\n";
        return 0;
    }

    if (command == "build" || command == "compile") {
        fs::path project = current_directory_project();
        if (project.empty()) project = g.project;
        int config = g.compile_config;

        for (int i = 2; i < argc; ++i) {
            std::string value = argv[i];
            if (value == "--project" && ++i < argc) {
                project = resolve_project_selector(argv[i]);
                continue;
            }
            if (value == "--config" && ++i < argc) {
                config = config_index(argv[i]);
                continue;
            }
            int parsed_config = config_index(value);
            if (parsed_config >= 0) {
                config = parsed_config;
                continue;
            }
            auto parsed_project = resolve_project_selector(value);
            if (!parsed_project.empty()) {
                project = parsed_project;
                continue;
            }
            std::cerr << "UPH: unknown build argument: " << value << '\n';
            return 2;
        }

        if (project.empty() || config < 0) {
            std::cerr << "UPH: invalid project or build configuration.\n";
            return 2;
        }

        std::ostringstream payload;
        payload << "project=" << project.string() << '\n';
        payload << "config=" << config << '\n';
        if (!send_uph_copydata(UPH_COPYDATA_BUILD, payload.str())) {
            std::cerr << "UPH: the app rejected the build request (it may be busy).\n";
            return 2;
        }
        std::cout << "Building " << project.stem().string()
                  << " (" << CONFIGS[config] << ")\n"
                  << "Build started in the running UPH app.\n";
        return 0;
    }

    if (command == "deploy") {
        fs::path project = current_directory_project();
        if (project.empty()) project = g.project;
        fs::path output;
        int config = g.package_config;

        for (int i = 2; i < argc; ++i) {
            std::string value = argv[i];
            if (value == "--project" && ++i < argc) {
                project = resolve_project_selector(argv[i]);
                continue;
            }
            if (value == "--config" && ++i < argc) {
                config = config_index(argv[i]);
                continue;
            }
            if (value == "--output" && ++i < argc) {
                output = argv[i];
                continue;
            }

            int parsed_config = config_index(value);
            if (parsed_config >= 0) {
                config = parsed_config;
                continue;
            }
            auto parsed_project = resolve_project_selector(value);
            if (!parsed_project.empty()) {
                project = parsed_project;
                continue;
            }

            std::cerr << "UPH: unknown deploy argument: " << value << '\n';
            return 2;
        }

        if (project.empty() || config < 0) {
            std::cerr << "UPH: invalid deploy project/configuration.\n";
            return 2;
        }
        if (output.empty())
            output = project.parent_path() / "Builds" / "Android" / CONFIGS[config];

        std::ostringstream payload;
        payload << "project=" << project.string() << '\n';
        payload << "config=" << config << '\n';
        payload << "output=" << output.string() << '\n';

        if (!send_uph_copydata(UPH_COPYDATA_DEPLOY, payload.str())) {
            std::cerr << "UPH: deploy was rejected (UPH may be busy, Android tooling may be incomplete, or no device is selected).\n";
            return 2;
        }

        std::cout << "Deploying " << project.stem().string() << " for Android ("
                  << CONFIGS[config] << ")\n"
                  << "Package -> Install -> Launch started in UPH.\n";
        return 0;
    }

    if (command == "package") {
        fs::path project = current_directory_project();
        if (project.empty()) project = g.project;
        fs::path output = g.output;
        int platform = g.package_platform;
        int config = g.package_config;
        bool platform_set = false;
        bool config_set = false;

        for (int i = 2; i < argc; ++i) {
            std::string value = argv[i];
            if (value == "--project" && ++i < argc) {
                project = resolve_project_selector(argv[i]);
                continue;
            }
            if (value == "--platform" && ++i < argc) {
                platform = platform_index(argv[i]);
                platform_set = true;
                continue;
            }
            if (value == "--config" && ++i < argc) {
                config = config_index(argv[i]);
                config_set = true;
                continue;
            }
            if (value == "--output" && ++i < argc) {
                output = argv[i];
                continue;
            }

            if (!platform_set) {
                int parsed = platform_index(value);
                if (parsed >= 0) {
                    platform = parsed;
                    platform_set = true;
                    continue;
                }
            }
            if (!config_set) {
                int parsed = config_index(value);
                if (parsed >= 0) {
                    config = parsed;
                    config_set = true;
                    continue;
                }
            }
            auto parsed_project = resolve_project_selector(value);
            if (!parsed_project.empty()) {
                project = parsed_project;
                continue;
            }
            std::cerr << "UPH: unknown package argument: " << value << '\n';
            return 2;
        }

        if (project.empty() || platform < 0 || config < 0) {
            std::cerr << "UPH: invalid package project/platform/configuration.\n";
            return 2;
        }
        if (output.empty())
            output = project.parent_path() / "Builds" / PLATFORMS[platform] / CONFIGS[config];

        std::ostringstream payload;
        payload << "project=" << project.string() << '\n';
        payload << "platform=" << platform << '\n';
        payload << "config=" << config << '\n';
        payload << "output=" << output.string() << '\n';

        if (!send_uph_copydata(UPH_COPYDATA_PACKAGE, payload.str())) {
            std::cerr << "UPH: the app rejected the package request (it may be busy or tooling is incomplete).\n";
            return 2;
        }

        std::cout << "Packaging " << project.stem().string() << " for "
                  << PLATFORMS[platform] << " (" << CONFIGS[config] << ")\n"
                  << "Output: " << output.string() << '\n'
                  << "Package started in the running UPH app.\n";
        return 0;
    }
#else
    (void)argc;
    (void)argv;
#endif

    std::cerr << "UPH: unknown command: " << argv[1] << "\n\n";
    print_help();
    return 2;
}
