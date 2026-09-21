#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

static constexpr ULONG_PTR UPH_COPYDATA_SELECT_ENGINE = 0x55504801;
static constexpr ULONG_PTR UPH_COPYDATA_SELECT_PROJECT = 0x55504802;
static constexpr ULONG_PTR UPH_COPYDATA_EDITOR = 0x55504803;
static constexpr ULONG_PTR UPH_COPYDATA_OPEN = 0x55504804;
static constexpr ULONG_PTR UPH_COPYDATA_RUN = 0x55504805;
static constexpr ULONG_PTR UPH_COPYDATA_BUILD = 0x55504806;
static constexpr ULONG_PTR UPH_COPYDATA_PACKAGE = 0x55504807;

struct Engine {
    std::string label;
    fs::path path;
};

struct CliState {
    fs::path project;
    std::vector<fs::path> recent_projects;
    fs::path engine;
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
#else
    const char* base = std::getenv("HOME");
#endif
    return fs::path(base ? base : ".") / ".uph-native.ini";
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

static std::string json_string_value(const std::string& text, const std::string& key) {
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
            else if (key == "output") g.output = value;
            else if (key == "compile_config") g.compile_config = std::stoi(value);
            else if (key == "package_config") g.package_config = std::stoi(value);
            else if (key == "package_platform") g.package_platform = std::stoi(value);
        } catch (...) {}
    }
}

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
        if (!valid_engine(path)) return;
        auto canonical = fs::weakly_canonical(path, ec);
        if (ec) canonical = path;
        auto key = normalized_path_key(canonical);
        for (const auto& e : engines)
            if (normalized_path_key(e.path) == key) return;
        engines.push_back({engine_version(canonical), canonical});
    };

    if (!g.engine.empty()) add(g.engine);
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

static bool send_uph_copydata(ULONG_PTR command, const std::string& value) {
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
        "  uph status\n"
        "  uph project list|current|select <name|path>\n"
        "  uph engine list|current|select <name|path>\n"
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

    if (action == "select") {
        if (argc < 4) {
            std::cerr << "Usage: uph project select <name|path>\n";
            return 2;
        }
        auto project = resolve_project_selector(argv[3]);
        if (project.empty()) {
            std::cerr << "UPH: project not found or selector is ambiguous: " << argv[3] << '\n';
            return 2;
        }
#ifdef _WIN32
        if (!ensure_app_for_action() ||
            !send_uph_copydata(UPH_COPYDATA_SELECT_PROJECT, project.string())) {
            std::cerr << "UPH: the app rejected the project change.\n";
            return 2;
        }
#else
        std::cerr << "UPH: project selection IPC is currently implemented on Windows.\n";
        return 2;
#endif
        std::cout << "Selected project: " << project.string() << '\n';
        return 0;
    }

    std::cerr << "Usage: uph project list|current|select <name|path>\n";
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

    if (action == "select") {
        if (argc < 4) {
            std::cerr << "Usage: uph engine select <name|path>\n";
            return 2;
        }
        fs::path engine;
        if (!resolve_engine_selector(argv[3], engine)) {
            std::cerr << "UPH: engine not found or selector is ambiguous: " << argv[3] << '\n';
            return 2;
        }
#ifdef _WIN32
        if (!ensure_app_for_action() ||
            !send_uph_copydata(UPH_COPYDATA_SELECT_ENGINE, engine.string())) {
            std::cerr << "UPH: the app rejected the engine change.\n";
            return 2;
        }
#else
        std::cerr << "UPH: engine selection IPC is currently implemented on Windows.\n";
        return 2;
#endif
        std::cout << "Selected engine: " << engine_version(engine) << '\n'
                  << engine.string() << '\n';
        return 0;
    }

    std::cerr << "Usage: uph engine list|current|select <name|path>\n";
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
        std::cout << "Project:  " << (g.project.empty() ? "None" : g.project.string()) << '\n';
        std::cout << "Engine:   " << (g.engine.empty() ? "None" : engine_version(g.engine)) << '\n';
        if (!g.engine.empty()) std::cout << "          " << g.engine.string() << '\n';
        std::cout << "Compile:  " << CONFIGS[std::clamp(g.compile_config, 0, 3)] << '\n';
        std::cout << "Package:  " << PLATFORMS[std::clamp(g.package_platform, 0, 4)]
                  << " / " << CONFIGS[std::clamp(g.package_config, 0, 3)] << '\n';
        return 0;
    }

    if (command == "project") return command_project(argc, argv);
    if (command == "engine") return command_engine(argc, argv);

#ifdef _WIN32
    if (!ensure_app_for_action()) {
        std::cerr << "UPH: could not start or connect to the desktop app.\n";
        return 2;
    }

    if (command == "editor") {
        if (!send_uph_copydata(UPH_COPYDATA_EDITOR, "")) {
            std::cerr << "UPH: the app rejected the editor launch.\n";
            return 2;
        }
        std::cout << "Editor launch requested through UPH.\n";
        return 0;
    }

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
        auto ipc = command == "run" ? UPH_COPYDATA_RUN : UPH_COPYDATA_OPEN;
        if (!send_uph_copydata(ipc, project_payload(project))) {
            std::cerr << "UPH: the app rejected the " << command << " request.\n";
            return 2;
        }
        std::cout << (command == "run" ? "Run" : "Open") << " requested through UPH.\n";
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
