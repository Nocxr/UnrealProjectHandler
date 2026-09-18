#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

struct Target { std::string name; std::string type; };
struct Engine { std::string label; fs::path path; };
struct ToolRow { std::string group; std::string name; fs::path path; bool found; };
struct ToolMeta { std::string version; std::string install; std::string code; };
struct FavoritePlugin { std::string name; std::string url; };
struct PluginGitState { bool submodule = false; std::string revision; };
struct ProjectGitState { fs::path root; std::string branch; std::vector<std::string> branches; };
struct LogEntry { std::string text; bool error = false; };

struct AppState {
    fs::path project;
    std::vector<fs::path> recent_projects;
    fs::path engine;
    fs::path output;
    std::vector<Engine> engines;
    std::vector<Target> targets;
    std::vector<ToolRow> tools;
    std::map<std::string, std::vector<fs::path>> plugin_scan_cache;
    std::map<std::string, PluginGitState> plugin_git_cache;
    std::map<std::string, fs::path> tool_overrides;
    std::map<std::string, std::map<std::string, ToolMeta>> tool_catalogs;
    std::vector<std::string> tool_catalog_versions;
    int tool_catalog_version = 0;
    std::string selected_tool_catalog_version;
    std::string pending_override_key;
    std::vector<FavoritePlugin> favorite_plugins;
    std::vector<LogEntry> logs;
    std::array<const char*, 4> configs{"Development", "Debug", "Shipping", "Test"};
    std::array<const char*, 5> platforms{"Windows", "Mac", "Android", "iOS", "VisionOS"};
    std::array<bool, 8> operations{true, true, true, true, true, true, false, false};
    int compile_target = 0;
    int compile_config = 0;
    int package_config = 0;
    int package_platform = 0;
    int unrealsharp_target = 0;
    bool unrealsharp = false;
    bool clean_output = false;
    bool project_has_cpp_module = false;
    bool auto_scroll = true;
    bool clear_on_run = false;
    bool log_expanded = true;
    std::set<int> selected_logs;
    int log_selection_anchor = -1;
    std::atomic<bool> process_running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> catalog_running{false};
    std::atomic<bool> git_refresh_requested{true};
    std::atomic<bool> git_refresh_running{false};
    std::atomic<bool> git_refresh_ready{false};
    std::atomic<bool> plugin_refresh_requested{false};
    std::atomic<bool> plugin_details_running{false};
    std::atomic<bool> plugin_details_ready{false};
    std::mutex mutex;
    std::mutex plugin_mutex;
    std::mutex git_mutex;
    std::map<std::string, PluginGitState> pending_plugin_git_cache;
    ProjectGitState pending_git_state;
    fs::path git_root;
    std::string git_branch;
    std::vector<std::string> git_branches;
};

static AppState g;
static SDL_Window* g_window = nullptr;
static std::atomic<bool> g_package_after_output_pick{false};
static constexpr const char* TOOL_CATALOG_TEMPLATE_URL =
    "https://raw.githubusercontent.com/Nocxr/UnrealProjectHandler/main/config/tool-catalog.json";

static fs::path settings_path() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
#else
    const char* base = std::getenv("HOME");
#endif
    return fs::path(base ? base : ".") / ".uph-native.ini";
}

static fs::path tool_catalog_path() {
    return settings_path().parent_path() / ".uph-tools.json";
}

static fs::path bundled_tool_catalog_path() {
    return fs::current_path() / "config/tool-catalog.json";
}

static std::string read_file_text(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static bool write_file_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
    return (bool)out;
}

static std::string normalized_text(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

static std::string normalized_git_url(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (!value.empty() && value.front() == '[') {
        auto link_start = value.find("](");
        auto link_end = value.rfind(')');
        if (link_start != std::string::npos && link_end == value.size() - 1)
            value = value.substr(link_start + 2, link_end - link_start - 2);
    }
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\'')))
        value = value.substr(1, value.size() - 2);
    return value;
}

static std::string fetch_github_tool_catalog() {
#ifdef _WIN32
    std::string command = std::string("powershell -NoProfile -ExecutionPolicy Bypass -Command \"")
        + "$ProgressPreference='SilentlyContinue'; "
        + "(Invoke-WebRequest -UseBasicParsing '" + TOOL_CATALOG_TEMPLATE_URL + "').Content\"";
#else
    std::string command = std::string("curl -fsSL '") + TOOL_CATALOG_TEMPLATE_URL + "'";
#endif
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    char buffer[4096]{};
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    if (pclose(pipe) != 0) return {};
    return output;
}

static std::string json_escape(const std::string& text) {
    std::string escaped;
    for (char c : text) {
        if (c == '\\' || c == '"') escaped += '\\';
        if (c == '\n') escaped += "\\n";
        else escaped += c;
    }
    return escaped;
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

static std::string json_field(const std::string& line, const std::string& name) {
    return json_string_value(line, name);
}

static std::string tool_key(const std::string& group, const std::string& name) {
    return group + "|" + name;
}

static void ensure_default_tool_catalog() {
    auto path = tool_catalog_path();
    if (fs::exists(path)) return;
    auto bundled = bundled_tool_catalog_path();
    if (fs::exists(bundled)) {
        write_file_text(path, read_file_text(bundled));
        return;
    }
    std::ofstream out(path);
    auto item = [&](const char* group, const char* tool, const std::string& version, const std::string& install, const std::string& code, bool comma = true) {
        out << "      {\"group\":\"" << json_escape(group) << "\",\"tool\":\"" << json_escape(tool)
            << "\",\"version\":\"" << json_escape(version) << "\",\"install\":\"" << json_escape(install)
            << "\",\"code\":\"" << json_escape(code) << "\"}" << (comma ? "," : "") << "\n";
    };
    out << "{\n  \"versions\": {\n";
#ifdef _WIN32
    out << "    \"Default\": [\n";
    item("Windows", "Windows command processor", "Windows built-in", "", "");
    item("Windows", "Unreal build script", "Installed with Unreal Engine", "Install Unreal Engine with Epic Games Launcher, then refresh engine discovery.", "");
    item("Windows", "Unreal AutomationTool", "Installed with Unreal Engine", "Install Unreal Engine with Epic Games Launcher, then refresh engine discovery.", "");
    item("Windows", "Unreal Editor", "Installed with Unreal Engine", "Install Unreal Engine with Epic Games Launcher, then refresh engine discovery.", "");
    item("Android", "ADB platform tools", "Android SDK platform-tools", "Install Android SDK platform-tools or run Unreal SetupAndroid.", "winget install Google.PlatformTools");
    item("Android", "Android SDK", "Android command line tools", "Install Android Studio command line tools or run Unreal SetupAndroid.", "winget install Google.AndroidStudio");
    item("Android", "Java runtime", "JDK 21 recommended", "Install Temurin JDK 21.", "winget install EclipseAdoptium.Temurin.21.JDK");
    item("iOS", "OpenSSH client", "Windows OpenSSH", "Install OpenSSH client.", "winget install Microsoft.OpenSSH.Beta");
    item("UnrealSharp Windows", "UnrealSharp plugin", "Project plugin", "Install or repair UnrealSharp in your project's Plugins folder.", "", false);
    out << "    ]";
    for (const char* version : {"5.4", "5.5", "5.6", "5.7", "5.8"}) {
        out << ",\n    \"" << version << "\": [\n";
        item("Android", "Java runtime", "JDK 21 recommended", "Install Temurin JDK 21.", "winget install EclipseAdoptium.Temurin.21.JDK");
        item("Android", "Android SDK", std::string("UE ") + version + " Android tooling", std::string("Run SetupAndroid.bat from your UE ") + version + " install.", "");
        item("Android", "ADB platform tools", std::string("UE ") + version + " Android tooling", std::string("Run SetupAndroid.bat from your UE ") + version + " install.", "", false);
        out << "    ]";
    }
#else
    out << "    \"Default\": [\n";
    item("macOS", "Xcode build tools", "Current Xcode CLI tools", "Install Xcode command line tools.", "xcode-select --install");
    item("macOS", "Apple Clang", "Current Xcode CLI tools", "Install Xcode command line tools.", "xcode-select --install");
    item("macOS", "Metal shader compiler", "Current Xcode CLI tools", "Install Xcode command line tools.", "xcode-select --install");
    item("macOS", "Metal library linker", "Current Xcode CLI tools", "Install Xcode command line tools.", "xcode-select --install");
    item("iOS", "Xcode signing tools", "Current Xcode CLI tools", "Install Xcode command line tools.", "xcode-select --install");
    item("iOS", "OpenSSH client", "OpenSSH", "Install OpenSSH.", "brew install openssh");
    item("Android", "Java runtime", "JDK 21 recommended", "Install OpenJDK 21.", "brew install openjdk@21", false);
    out << "    ]";
#endif
    out << "\n  }\n}\n";
}

static void load_tool_catalog() {
    ensure_default_tool_catalog();
    auto selected = !g.selected_tool_catalog_version.empty() ? g.selected_tool_catalog_version :
        (g.tool_catalog_versions.empty() ? std::string{} :
            g.tool_catalog_versions[std::clamp(g.tool_catalog_version, 0, (int)g.tool_catalog_versions.size() - 1)]);
    g.tool_catalogs.clear();
    g.tool_catalog_versions.clear();
    std::ifstream in(tool_catalog_path());
    std::string line, section;
    while (std::getline(in, line)) {
        auto quote = line.find('"');
        auto end_quote = quote == std::string::npos ? std::string::npos : line.find('"', quote + 1);
        if (quote != std::string::npos && end_quote != std::string::npos && line.find('[') != std::string::npos) {
            section = line.substr(quote + 1, end_quote - quote - 1);
            if (!g.tool_catalogs.contains(section)) {
                g.tool_catalogs[section] = {};
                g.tool_catalog_versions.push_back(section);
            }
            continue;
        }
        if (line.find("\"group\"") == std::string::npos) continue;
        auto group = json_field(line, "group");
        auto tool = json_field(line, "tool");
        if (group.empty() || tool.empty() || section.empty()) continue;
        g.tool_catalogs[section][tool_key(group, tool)] = {
            json_field(line, "version"),
            json_field(line, "install"),
            json_field(line, "code")
        };
    }
    auto match = std::find(g.tool_catalog_versions.begin(), g.tool_catalog_versions.end(), selected);
    g.tool_catalog_version = match == g.tool_catalog_versions.end() ? 0 : (int)std::distance(g.tool_catalog_versions.begin(), match);
    if (!g.tool_catalog_versions.empty()) g.selected_tool_catalog_version = g.tool_catalog_versions[g.tool_catalog_version];
}

static std::string quote(const fs::path& value) {
    std::string text = value.string();
#ifdef _WIN32
    std::string escaped;
    for (char c : text) escaped += c == '"' ? "\\\"" : std::string(1, c);
    return "\"" + escaped + "\"";
#else
    std::string escaped;
    for (char c : text) escaped += c == '\'' ? "'\\''" : std::string(1, c);
    return "'" + escaped + "'";
#endif
}

static std::string timestamped(const std::string& text) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    return "[" + std::string(stamp) + "] " + text;
}

static void log_entry(const std::string& text, bool error = false) {
    std::lock_guard lock(g.mutex);
    g.logs.push_back({text, error});
}

static void log_line(const std::string& text) {
    log_entry(timestamped(text), text.find("[ERROR]") != std::string::npos || text.find("ERROR:") != std::string::npos);
}

static void save_settings() {
    std::ofstream out(settings_path());
    out << "project=" << g.project.string() << '\n';
    for (const auto& project : g.recent_projects) if (!project.empty()) out << "recent_project=" << project.string() << '\n';
    out << "engine=" << g.engine.string() << '\n';
    out << "output=" << g.output.string() << '\n';
    out << "compile_target=" << g.compile_target << '\n';
    out << "compile_config=" << g.compile_config << '\n';
    out << "package_config=" << g.package_config << '\n';
    out << "package_platform=" << g.package_platform << '\n';
    out << "unrealsharp_target=" << g.unrealsharp_target << '\n';
    out << "unrealsharp=" << g.unrealsharp << '\n';
    out << "clean_output=" << g.clean_output << '\n';
    if (!g.selected_tool_catalog_version.empty()) out << "tool_catalog_version=" << g.selected_tool_catalog_version << '\n';
    for (const auto& [key, path] : g.tool_overrides) if (!path.empty()) out << "tool_override=" << key << '|' << path.string() << '\n';
    for (const auto& plugin : g.favorite_plugins) if (!plugin.name.empty() && !plugin.url.empty())
        out << "favorite_plugin=" << plugin.name << '|' << plugin.url << '\n';
    for (size_t i = 0; i < g.operations.size(); ++i) out << "operation" << i << '=' << g.operations[i] << '\n';
}

static void load_settings() {
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
            else if (key == "compile_target") g.compile_target = std::stoi(value);
            else if (key == "compile_config") g.compile_config = std::stoi(value);
            else if (key == "package_config") g.package_config = std::stoi(value);
            else if (key == "package_platform") g.package_platform = std::stoi(value);
            else if (key == "unrealsharp_target") g.unrealsharp_target = std::stoi(value);
            else if (key == "unrealsharp") g.unrealsharp = std::stoi(value) != 0;
            else if (key == "clean_output") g.clean_output = std::stoi(value) != 0;
            else if (key == "tool_catalog_version") g.selected_tool_catalog_version = value;
            else if (key == "tool_override") {
                auto split_override = value.find('|');
                if (split_override != std::string::npos) g.tool_overrides[value.substr(0, split_override)] = value.substr(split_override + 1);
            }
            else if (key == "favorite_plugin") {
                auto split_override = value.find('|');
                if (split_override != std::string::npos && !value.substr(split_override + 1).empty())
                    g.favorite_plugins.push_back({value.substr(0, split_override), normalized_git_url(value.substr(split_override + 1))});
            }
            else if (key.rfind("operation", 0) == 0) {
                auto index = static_cast<size_t>(std::stoi(key.substr(9)));
                if (index < g.operations.size()) g.operations[index] = std::stoi(value) != 0;
            }
        } catch (...) {}
    }
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static std::string engine_version(const fs::path& engine) {
    auto file = engine / "Engine/Build/Build.version";
    auto text = read_text(file);
    auto number = [&](const char* key) {
        auto p = text.find(key);
        if (p == std::string::npos) return std::string{};
        p = text.find(':', p);
        auto e = text.find_first_of(",\n", p + 1);
        auto value = text.substr(p + 1, e - p - 1);
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c){ return std::isspace(c); }), value.end());
        return value;
    };
    auto major = number("MajorVersion"), minor = number("MinorVersion");
    return major.empty() ? engine.filename().string() : "Unreal Engine " + major + "." + minor;
}

static bool valid_engine(const fs::path& path) {
    return fs::exists(path / "Engine/Build/Build.version") && fs::exists(path / "Engine/Binaries");
}

#ifdef _WIN32
static bool excluded_engine_path(const fs::path& path) {
    auto text = path.string();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return text.find("fortnite") != std::string::npos;
}
#endif

#ifdef _WIN32
static std::vector<fs::path> registry_engine_paths() {
    std::vector<fs::path> paths;
    auto add_value_paths = [&](HKEY root, const char* subkey) {
        HKEY key{};
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
        for (DWORD index = 0;; ++index) {
            char value_name[512]{};
            char value_data[MAX_PATH * 4]{};
            DWORD value_name_size = sizeof(value_name);
            DWORD value_data_size = sizeof(value_data);
            DWORD type{};
            auto result = RegEnumValueA(key, index, value_name, &value_name_size, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(value_data), &value_data_size);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result == ERROR_SUCCESS && type == REG_SZ && value_data[0]) paths.emplace_back(value_data);
        }
        RegCloseKey(key);
    };
    auto add_version_paths = [&](HKEY root, const char* subkey) {
        HKEY key{};
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
        for (DWORD index = 0;; ++index) {
            char child[256]{};
            DWORD child_size = sizeof(child);
            auto result = RegEnumKeyExA(key, index, child, &child_size, nullptr, nullptr, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS) continue;
            std::string child_key = std::string(subkey) + "\\" + child;
            char install_dir[MAX_PATH * 4]{};
            DWORD install_dir_size = sizeof(install_dir);
            DWORD type{};
            if (RegGetValueA(root, child_key.c_str(), "InstalledDirectory", RRF_RT_REG_SZ, &type,
                             install_dir, &install_dir_size) == ERROR_SUCCESS && install_dir[0]) {
                paths.emplace_back(install_dir);
            }
        }
        RegCloseKey(key);
    };
    add_value_paths(HKEY_CURRENT_USER, "Software\\Epic Games\\Unreal Engine\\Builds");
    add_version_paths(HKEY_LOCAL_MACHINE, "SOFTWARE\\EpicGames\\Unreal Engine");
    return paths;
}
#endif

static fs::path find_on_path(const std::string& name) {
    const char* raw = std::getenv("PATH");
    if (!raw) return {};
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::stringstream paths(raw);
    std::string directory;
    while (std::getline(paths, directory, separator)) {
        auto candidate = fs::path(directory) / name;
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

static fs::path first_existing(std::initializer_list<fs::path> candidates) {
    for (const auto& candidate : candidates) if (!candidate.empty() && fs::exists(candidate)) return candidate;
    return {};
}

#ifndef _WIN32
static fs::path command_path(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    char buffer[4096]{};
    std::string line = fgets(buffer, sizeof(buffer), pipe) ? buffer : "";
    pclose(pipe);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return fs::path(line);
}
#endif

static void inspect_tooling() {
    g.tools.clear();
    auto add = [](std::string group, std::string name, fs::path path) {
        auto key = tool_key(group, name);
        if (auto override = g.tool_overrides.find(key); override != g.tool_overrides.end() && !override->second.empty()) path = override->second;
        g.tools.push_back({std::move(group), std::move(name), path, !path.empty() && fs::exists(path)});
    };
    auto add_with_status = [](std::string group, std::string name, fs::path path, bool ready) {
        auto key = tool_key(group, name);
        if (auto override = g.tool_overrides.find(key); override != g.tool_overrides.end() && !override->second.empty()) {
            path = override->second;
            ready = fs::exists(path);
        }
        g.tools.push_back({std::move(group), std::move(name), std::move(path), ready});
    };
    add("Windows", "Windows command processor", find_on_path("cmd.exe"));
    add("Windows", "Unreal build script", g.engine / "Engine/Build/BatchFiles/Build.bat");
    add("Windows", "Unreal AutomationTool", g.engine / "Engine/Build/BatchFiles/RunUAT.bat");
    add("Windows", "Unreal Editor", g.engine / "Engine/Binaries/Win64/UnrealEditor.exe");
    add("macOS", "Unreal macOS platform support", first_existing({g.engine / "Engine/Platforms/Mac", g.engine / "Engine/Source/Programs/UnrealBuildTool/Platform/Mac"}));
    add("macOS", "Xcode build tools", find_on_path("xcodebuild"));
    add("macOS", "Apple Clang", find_on_path("clang++"));
    add("macOS", "Metal shader compiler", 
#ifdef _WIN32
        {}
#else
        command_path("xcrun --find metal 2>/dev/null")
#endif
    );
    add("macOS", "Metal library linker", 
#ifdef _WIN32
        {}
#else
        command_path("xcrun --find metallib 2>/dev/null")
#endif
    );
    add("Linux", "Unreal Linux platform support", first_existing({g.engine / "Engine/Platforms/Linux", g.engine / "Engine/Source/Programs/UnrealBuildTool/Platform/Linux"}));
    add("Linux", "Clang compiler", find_on_path("clang++"));
    auto android_home = std::getenv("ANDROID_SDK_ROOT") ? fs::path(std::getenv("ANDROID_SDK_ROOT")) :
                        (std::getenv("ANDROID_HOME") ? fs::path(std::getenv("ANDROID_HOME")) : fs::path{});
    add("Android", "Android SDK", android_home);
#ifdef _WIN32
    add("Android", "ADB platform tools", android_home / "platform-tools/adb.exe");
#else
    add("Android", "ADB platform tools", android_home / "platform-tools/adb");
#endif
    add("Android", "Java runtime", find_on_path(
#ifdef _WIN32
        "java.exe"
#else
        "java"
#endif
    ));
    add("Android", "Unreal Android setup", g.engine / "Engine/Extras/Android/SetupAndroid.command");
    add("iOS", "Unreal iOS platform support", first_existing({g.engine / "Engine/Platforms/IOS", g.engine / "Engine/Source/Programs/UnrealBuildTool/Platform/IOS"}));
    add("iOS", "Xcode signing tools", find_on_path("xcrun"));
    add("iOS", "OpenSSH client", find_on_path(
#ifdef _WIN32
        "ssh.exe"
#else
        "ssh"
#endif
    ));
    add("VisionOS", "Unreal visionOS platform support", first_existing({g.engine / "Engine/Platforms/VisionOS", g.engine / "Engine/Source/Programs/UnrealBuildTool/Platform/VisionOS"}));
    add("VisionOS", "Xcode build tools (15.3+ required by UE)", find_on_path("xcodebuild"));
    add("VisionOS", "Xcode visionOS SDK (xros)",
#ifdef _WIN32
        {}
#else
        command_path("xcrun --sdk xros --show-sdk-path 2>/dev/null")
#endif
    );
    add("VisionOS", "Xcode signing tools", find_on_path("xcrun"));
    add("VisionOS", "Metal shader compiler", 
#ifdef _WIN32
        {}
#else
        command_path("xcrun --find metal 2>/dev/null")
#endif
    );
    auto plugin = first_existing({g.project.parent_path() / "Plugins/UnrealSharp", g.project.parent_path() / "Plugins/unrealsharp"});
    if (plugin.empty()) plugin = g.project.parent_path() / "Plugins/UnrealSharp";
    auto dotnet = find_on_path(
#ifdef _WIN32
        "dotnet.exe"
#else
        "dotnet"
#endif
    );
    auto mapping = plugin / "Build/Scripts/Utilities/DotNetSdkUtilities.cs";
    auto mapping_text = read_text(mapping);
    auto unrealsharp_package = plugin / "Build/Scripts/BuildCommands/PackageProject.cs";
    auto unrealsharp_package_text = read_text(unrealsharp_package);
    auto add_unrealsharp = [&](const std::string& group, const std::string& token, bool package_mapping) {
        add(group, "UnrealSharp plugin", plugin / "UnrealSharp.uplugin");
        add(group, "Automation scripts", plugin / "Build/Scripts");
        add(group, "Required .NET SDK manifest", plugin / "Managed/global.json");
        add(group, "Managed binaries", plugin / "Binaries/Managed/net10.0");
        add(group, ".NET host", dotnet);
        const auto& source = package_mapping ? unrealsharp_package_text : mapping_text;
        const auto& source_path = package_mapping ? unrealsharp_package : mapping;
        add_with_status(group, token + (package_mapping ? " PackageProject mapping" : " .NET publish mapping"), source_path,
                        source.find("UnrealTargetPlatform." + token) != std::string::npos);
    };
    add_unrealsharp("UnrealSharp Windows", "Win64", false);
    add_unrealsharp("UnrealSharp Mac", "Mac", false);
    add_unrealsharp("UnrealSharp iOS", "IOS", false);
    add_unrealsharp("UnrealSharp Android", "Android", true);
    add_unrealsharp("UnrealSharp XROS", "VisionOS", true);
}

static void discover_engines() {
    g.engines.clear();
    auto add = [](const fs::path& path) {
#ifdef _WIN32
        if (excluded_engine_path(path)) return;
#endif
        if (!valid_engine(path)) return;
        auto canonical = fs::weakly_canonical(path);
        if (std::none_of(g.engines.begin(), g.engines.end(), [&](const Engine& e){ return e.path == canonical; }))
            g.engines.push_back({engine_version(canonical), canonical});
    };
    if (!g.engine.empty()) add(g.engine);
#ifdef _WIN32
    for (const auto& path : registry_engine_paths()) add(path);
    for (const char* root : {"C:/Program Files/Epic Games", "D:/Epic Games"}) {
#else
    for (const char* root : {"/Users/Shared/Epic Games", "/Applications"}) {
#endif
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) add(entry.path());
    }
    std::sort(g.engines.begin(), g.engines.end(), [](const Engine& a, const Engine& b){ return a.label > b.label; });
    if (g.engine.empty() && !g.engines.empty()) g.engine = g.engines.front().path;
    inspect_tooling();
}

static void inspect_project() {
    g.targets.clear();
    g.project_has_cpp_module = false;
    if (g.project.empty()) {
        inspect_tooling();
        return;
    }
    auto source = g.project.parent_path() / "Source";
    std::error_code ec;
    if (fs::exists(source, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(source, ec)) {
            if (!entry.is_regular_file()) continue;
            auto filename = entry.path().filename().string();
            if (filename.size() >= 9 && filename.ends_with(".Build.cs"))
                g.project_has_cpp_module = true;
            if (filename.find("Target.cs") == std::string::npos) continue;
            auto text = read_text(entry.path());
            std::string type = "Game";
            for (auto candidate : {"Editor", "Client", "Server", "Program"})
                if (text.find("TargetType." + std::string(candidate)) != std::string::npos) type = candidate;
            auto name = filename;
            name.erase(name.size() - std::string(".Target.cs").size());
            g.targets.push_back({name, type});
        }
    }
    inspect_tooling();
}

static std::string capture_command(const std::string& command) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return {};
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE null_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = null_error != INVALID_HANDLE_VALUE ? null_error : write_pipe;

    int wide_count = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
    std::wstring wide_command;
    if (wide_count > 0) {
        wide_command.resize(static_cast<size_t>(wide_count));
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide_command.data(), wide_count);
        if (!wide_command.empty() && wide_command.back() == L'\0') wide_command.pop_back();
    } else {
        wide_command.assign(command.begin(), command.end());
    }

    std::wstring command_line = L"cmd.exe /D /S /C \"" + wide_command + L"\"";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    PROCESS_INFORMATION process{};
    BOOL started = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(write_pipe);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (null_error != INVALID_HANDLE_VALUE) CloseHandle(null_error);

    if (!started) {
        CloseHandle(read_pipe);
        return {};
    }

    std::string output;
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0)
        output.append(buffer, buffer + bytes_read);

    CloseHandle(read_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) return {};
    return normalized_text(output);
#else
    auto wrapped = command + " 2>/dev/null";
    FILE* pipe = popen(wrapped.c_str(), "r");
    if (!pipe) return {};
    char buffer[1024];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    if (pclose(pipe) != 0) return {};
    return normalized_text(output);
#endif
}

static ProjectGitState inspect_project_git_state(const fs::path& project) {
    ProjectGitState state;
    if (project.empty()) return state;

    auto project_dir = project.parent_path();
    auto identity = capture_command("git -C " + quote(project_dir) + " rev-parse --show-toplevel --abbrev-ref HEAD");
    if (identity.empty()) return state;

    std::stringstream identity_lines(identity);
    std::string root;
    std::string branch;
    std::getline(identity_lines, root);
    std::getline(identity_lines, branch);
    if (!root.empty() && root.back() == '\r') root.pop_back();
    if (!branch.empty() && branch.back() == '\r') branch.pop_back();
    if (root.empty()) return state;

    state.root = fs::path(root);
    state.branch = branch == "HEAD" ? std::string{} : branch;

    auto branches = capture_command("git -C " + quote(state.root) + " branch --list --no-color");
    std::stringstream lines(branches);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        while (!line.empty() && (line.front() == '*' || std::isspace(static_cast<unsigned char>(line.front()))))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (!line.empty()) state.branches.push_back(line);
    }

    if (state.branch.empty()) {
        auto sha = capture_command("git -C " + quote(state.root) + " rev-parse --short HEAD");
        if (!sha.empty()) state.branch = "detached @ " + sha;
    }
    return state;
}

static void request_project_git_refresh() {
    if (g.git_refresh_running.exchange(true)) {
        g.git_refresh_requested = true;
        return;
    }

    auto project = g.project;
    g.git_refresh_ready = false;
    std::thread([project] {
        auto state = inspect_project_git_state(project);
        {
            std::lock_guard lock(g.git_mutex);
            g.pending_git_state = std::move(state);
        }
        g.git_refresh_ready = true;
        g.git_refresh_running = false;
    }).detach();
}

static std::string normalized_path_key(const fs::path& path) {
    std::error_code ec;
    auto normalized = fs::weakly_canonical(path, ec);
    auto text = (ec ? path.lexically_normal() : normalized).string();
#ifdef _WIN32
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#endif
    return text;
}

static void remember_project(const fs::path& project) {
    if (project.empty()) return;
    auto key = normalized_path_key(project);
    std::erase_if(g.recent_projects, [&](const fs::path& recent){ return normalized_path_key(recent) == key; });
    g.recent_projects.insert(g.recent_projects.begin(), project);
    if (g.recent_projects.size() > 10) g.recent_projects.resize(10);
}

static void match_project_engine(const fs::path& project) {
    auto association = json_string_value(read_text(project), "EngineAssociation");
    if (association.empty()) return;
#ifdef _WIN32
    char install_dir[MAX_PATH * 4]{};
    DWORD install_dir_size = sizeof(install_dir);
    DWORD type{};
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Epic Games\\Unreal Engine\\Builds", association.c_str(),
                     RRF_RT_REG_SZ, &type, install_dir, &install_dir_size) == ERROR_SUCCESS && install_dir[0]) {
        auto association_path = normalized_path_key(install_dir);
        for (const auto& engine : g.engines) if (normalized_path_key(engine.path) == association_path) {
            g.engine = engine.path;
            return;
        }
    }
#endif
    auto association_key = association;
    std::transform(association_key.begin(), association_key.end(), association_key.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    for (const auto& engine : g.engines) {
        auto label = engine.label;
        auto folder = engine.path.filename().string();
        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (label == "unreal engine " + association_key || folder == "ue_" + association_key || folder == association_key) {
            g.engine = engine.path;
            return;
        }
    }
}

static void select_project(fs::path project) {
    if (g.process_running) { log_line("[ERROR] Stop the current operation before switching projects."); return; }
    if (project.empty()) return;
    g.project = project;
    g.output.clear();
    g.plugin_scan_cache.clear();
    remember_project(project);
    match_project_engine(project);
    inspect_project();
    g.git_refresh_requested = true;
    log_line("[SYSTEM] Project: " + project.string());
    save_settings();
}

enum class DialogKind { Project, Engine, Output, PackageOutput, SaveLog, ToolOverrideFile, ToolOverrideFolder };
struct DialogRequest { DialogKind kind; std::string key; };
struct DialogResult { DialogKind kind; fs::path path; std::string error; std::string key; };
constexpr Uint32 DIALOG_RESULT_EVENT = SDL_EVENT_USER + 1;

static void SDLCALL dialog_result(void* userdata, const char* const* files, int) {
    auto request = std::unique_ptr<DialogRequest>(static_cast<DialogRequest*>(userdata));
    if (files && !files[0]) return;
    auto* result = new DialogResult{request->kind, files ? fs::path(files[0]) : fs::path{}, files ? "" : SDL_GetError(), request->key};
    SDL_Event event{};
    event.type = DIALOG_RESULT_EVENT;
    event.user.data1 = result;
    if (!SDL_PushEvent(&event)) delete result;
}

static void apply_dialog_result(std::unique_ptr<DialogResult> result) {
    if (!result->error.empty()) {
        log_line("[ERROR] File dialog failed: " + result->error);
        return;
    }
    if (result->kind == DialogKind::Project) {
        select_project(result->path);
    } else if (result->kind == DialogKind::Engine) {
        if (g.process_running) {
            log_line("[ERROR] Stop the current operation before switching engines.");
            return;
        }
        if (!valid_engine(result->path)) {
            log_line("[ERROR] Selected folder is not a usable Unreal Engine installation.");
            return;
        }
        g.engine = result->path;
        g.plugin_scan_cache.clear();
        discover_engines();
        log_line("[SYSTEM] Engine: " + result->path.string());
    } else if (result->kind == DialogKind::ToolOverrideFile || result->kind == DialogKind::ToolOverrideFolder) {
        if (!result->key.empty()) {
            g.tool_overrides[result->key] = result->path;
            inspect_tooling();
            log_line("[SYSTEM] Tool override: " + result->path.string());
        }
    } else {
        if (result->kind == DialogKind::SaveLog) {
            std::ofstream out(result->path);
            std::lock_guard lock(g.mutex);
            for (const auto& entry : g.logs) out << entry.text << '\n';
            return;
        }
        g.output = result->path;
        log_line("[SYSTEM] Package output: " + result->path.string());
        if (result->kind == DialogKind::PackageOutput) g_package_after_output_pick = true;
    }
    save_settings();
}

static void pick_project() {
    static const SDL_DialogFileFilter filters[] = {{"Unreal Project", "uproject"}, {"All Files", "*"}};
    std::string initial = g.project.empty() ? std::string{} : g.project.parent_path().string();
    SDL_ShowOpenFileDialog(dialog_result, new DialogRequest{DialogKind::Project, {}}, g_window, filters, 2,
                           initial.empty() ? nullptr : initial.c_str(), false);
}

static void pick_folder(DialogKind kind, const fs::path& initial) {
    auto text = initial.string();
    SDL_ShowOpenFolderDialog(dialog_result, new DialogRequest{kind, {}}, g_window,
                             text.empty() ? nullptr : text.c_str(), false);
}

static void pick_override_file(const std::string& key, const fs::path& initial) {
    static const SDL_DialogFileFilter filters[] = {{"All Files", "*"}};
    auto text = initial.empty() ? std::string{} : (fs::is_directory(initial) ? initial : initial.parent_path()).string();
    SDL_ShowOpenFileDialog(dialog_result, new DialogRequest{DialogKind::ToolOverrideFile, key}, g_window, filters, 1,
                           text.empty() ? nullptr : text.c_str(), false);
}

static void pick_override_folder(const std::string& key, const fs::path& initial) {
    auto text = initial.string();
    SDL_ShowOpenFolderDialog(dialog_result, new DialogRequest{DialogKind::ToolOverrideFolder, key}, g_window,
                             text.empty() ? nullptr : text.c_str(), false);
}

static void save_log_dialog() {
    static const SDL_DialogFileFilter filters[] = {{"Text Log", "txt;log"}, {"All Files", "*"}};
    SDL_ShowSaveFileDialog(dialog_result, new DialogRequest{DialogKind::SaveLog, {}}, g_window, filters, 2, "uph-log.txt");
}

static fs::path build_script() {
#ifdef _WIN32
    return g.engine / "Engine/Build/BatchFiles/Build.bat";
#else
    return g.engine / "Engine/Build/BatchFiles/Mac/Build.sh";
#endif
}

static fs::path run_uat() {
#ifdef _WIN32
    return g.engine / "Engine/Build/BatchFiles/RunUAT.bat";
#else
    return g.engine / "Engine/Build/BatchFiles/RunUAT.sh";
#endif
}

static fs::path editor_path() {
#ifdef _WIN32
    return g.engine / "Engine/Binaries/Win64/UnrealEditor.exe";
#else
    return g.engine / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor";
#endif
}

static fs::path default_package_output() {
    if (g.project.empty()) return {};
    return g.project.parent_path() / g.platforms[g.package_platform];
}

static fs::path package_output() {
    if (!g.output.empty()) return g.output;
    return default_package_output();
}

static fs::path unrealsharp_scripts() {
    if (g.project.empty()) return {};
    for (const auto& name : {"UnrealSharp", "unrealsharp"}) {
        auto candidate = g.project.parent_path() / "Plugins" / name / "Build/Scripts";
        if (fs::exists(candidate)) return candidate;
    }
    return g.project.parent_path() / "Plugins/UnrealSharp/Build/Scripts";
}

static std::string compile_command(bool clean = false) {
    if (g.project.empty() || g.engine.empty() || g.targets.empty()) return {};
    std::ostringstream command;
#ifdef _WIN32
    command << quote(build_script());
#else
    command << "bash " << quote(build_script());
#endif
    command << ' ' << g.targets[std::min<int>(g.compile_target, g.targets.size() - 1)].name;
#ifdef _WIN32
    command << " Win64 ";
#else
    command << " Mac ";
#endif
    command << g.configs[g.compile_config] << " -Project=" << quote(g.project) << " -WaitMutex -Verbose";
    if (clean) command << " -clean";
    return command.str();
}

static std::string package_command() {
    if (g.project.empty() || g.engine.empty()) return {};
    static const char* uat_platforms[] = {"Win64", "Mac", "Android", "IOS", "VisionOS"};
    static const char* flags[] = {"-build", "-cook", "-stage", "-pak", "-package", "-archive", "-deploy", "-run"};
    auto uat_launch = [] {
        std::ostringstream c;
#ifdef _WIN32
        c << quote(run_uat());
#else
        c << "bash " << quote(run_uat());
#endif
        return c.str();
    };
    if (g.unrealsharp) {
        std::vector<std::string> types;
        for (const auto& target : g.targets)
            if ((target.type == "Game" || target.type == "Client" || target.type == "Server") &&
                std::find(types.begin(), types.end(), target.type) == types.end()) types.push_back(target.type);
        if (types.empty()) return {};
        g.unrealsharp_target = std::min<int>(g.unrealsharp_target, types.size() - 1);
        std::ostringstream command;
        command << uat_launch() << " PackageProject -Verbose -ScriptDir=" << quote(unrealsharp_scripts())
                << " -Project=" << quote(g.project)
                << " -ArchiveDirectory=" << quote(package_output())
                << " -UETargetType=" << types[g.unrealsharp_target]
                << " -UEBuildConfig=" << g.configs[g.package_config];
        if (g.package_platform != 0) command << " -TargetPlatform=" << uat_platforms[g.package_platform];
        if (g.package_platform >= 2) command << " -TargetArchitecture=Arm64";
        return command.str();
    }
    auto build_cook = [&] {
        std::ostringstream c;
        c << uat_launch() << " BuildCookRun -Verbose -project=" << quote(g.project) << " -noP4 -platform=" << uat_platforms[g.package_platform]
          << " -clientconfig=" << g.configs[g.package_config];
        return c.str();
    };
#ifdef _WIN32
    const bool standalone_finalize = false;
#else
    const bool standalone_finalize = g.package_platform == 1 || g.package_platform == 3 || g.package_platform == 4;
#endif
    if (standalone_finalize) {
        const char* ub_run = "UE_BUILD_FROM_XCODE=1 ";
        static const char* plat_folder[] = {"", "Mac", "", "IOS", "VisionOS"};
        std::string scheme_name = g.project.stem().string();
        for (const auto& target : g.targets)
            if (target.type == "Game" || target.type == "Client") { scheme_name = target.name; break; }
        std::vector<std::string> steps;
        {
            std::ostringstream first;
            first << ub_run << build_cook();
            for (size_t i = 0; i < g.operations.size(); ++i) {
                if (!g.operations[i] || i == 4 || i == 5) continue;
                first << ' ' << flags[i];
            }
            steps.push_back(first.str());
        }
        const bool needs_finalize = g.operations[2] || g.operations[3] || g.operations[4] || g.operations[5];
        if (needs_finalize) {
            auto json = g.project.parent_path() / "Intermediate/Build" / plat_folder[g.package_platform] / "arm64"
                        / scheme_name / g.configs[g.package_config]
                        / ("PostBuildSync_" + scheme_name + ".json");
            std::ostringstream finalize;
            finalize << "/usr/bin/env -u UBA_DETOURED -u DYLD_INSERT_LIBRARIES -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH"
                     << " TMPDIR=/private/tmp TMP=/private/tmp"
                     << ' ' << quote(g.engine / "Engine/Binaries/ThirdParty/DotNet/10.0/mac-arm64/dotnet")
                     << ' ' << quote(g.engine / "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll")
                     << " -Mode=ApplePostBuildSync -Input=" << quote(json);
            steps.push_back(finalize.str());
            std::ostringstream restage;
            restage << ub_run << build_cook() << " -cook -stage -pak";
            if (g.operations[4]) restage << " -package";
            steps.push_back(restage.str());
        }
        if (g.operations[5]) {
            auto app = g.project.parent_path() / "Binaries" / plat_folder[g.package_platform] / (g.project.stem().string() + ".app");
            std::ostringstream archive;
            archive << "ditto " << quote(app) << " " << quote(package_output() / (g.project.stem().string() + ".app"));
            steps.push_back(archive.str());
        }
        std::ostringstream joined;
        for (size_t i = 0; i < steps.size(); ++i) {
            if (i) joined << " && ";
            joined << steps[i];
        }
        return joined.str();
    }
    std::ostringstream command;
    command << build_cook();
    for (size_t i = 0; i < g.operations.size(); ++i) if (g.operations[i]) command << ' ' << flags[i];
    if (g.operations[5]) command << " -archivedirectory=" << quote(package_output());
    return command.str();
}

static void run_command(std::string command, const std::string& name, bool refresh_plugins = false, bool refresh_git = false) {
    if (g.process_running.exchange(true)) { log_line("[ERROR] Another operation is already running."); return; }
    g.stop_requested = false;
    if (g.clear_on_run) { std::lock_guard lock(g.mutex); g.logs.clear(); }
    log_line("[SYSTEM] Starting " + name + ": " + command);
    std::thread([command = std::move(command), name, refresh_plugins, refresh_git] {
#ifdef _WIN32
        auto wrapped = "cmd /S /C \"" + command + " 2>&1\"";
#else
        auto wrapped = command + " 2>&1";
#endif
        FILE* pipe = popen(wrapped.c_str(), "r");
        if (!pipe) { log_line("[ERROR] Could not start " + name); g.process_running = false; return; }
        char buffer[4096];
        fs::path detailed_log;
        std::string failure_chunk;
        enum class LogBlock { None, BuildFailure, Exception };
        LogBlock block = LogBlock::None;
        auto flush_block = [&] {
            if (!failure_chunk.empty()) log_entry(failure_chunk, true);
            failure_chunk.clear();
            block = LogBlock::None;
        };
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            constexpr const char* detail_marker = "See log for more details. (";
            auto detail_start = line.find(detail_marker);
            if (detail_start != std::string::npos && !line.empty() && line.back() == ')') {
                detail_start += std::char_traits<char>::length(detail_marker);
                detailed_log = line.substr(detail_start, line.size() - detail_start - 1);
            }
            if (block == LogBlock::None && line.find("** BUILD FAILED **") != std::string::npos)
                block = LogBlock::BuildFailure;
            else if (block == LogBlock::None && (line.find("Unhandled exception") != std::string::npos || line.rfind("Traceback ", 0) == 0))
                block = LogBlock::Exception;
            if (block != LogBlock::None) {
                if (!failure_chunk.empty()) failure_chunk += '\n';
                failure_chunk += timestamped(line);
                bool failure_summary_end = block == LogBlock::BuildFailure && !line.empty() && line.front() == '(' &&
                                           line.find("failure") != std::string::npos && line.back() == ')';
                bool exception_end = block == LogBlock::Exception && line.empty() &&
                                     std::count(failure_chunk.begin(), failure_chunk.end(), '\n') > 1;
                if (failure_summary_end || exception_end) flush_block();
            } else {
                log_line(line);
            }
            if (g.stop_requested) break;
        }
        flush_block();
        int status = pclose(pipe);
        if (status != 0 && !detailed_log.empty() && fs::is_regular_file(detailed_log)) {
            log_line("[SYSTEM] Importing detailed Unreal log: " + detailed_log.string());
            std::ifstream detail(detailed_log);
            std::string detail_line;
            while (std::getline(detail, detail_line)) {
                if (!detail_line.empty() && detail_line.back() == '\r') detail_line.pop_back();
                log_entry(timestamped("[DETAIL] " + detail_line),
                          detail_line.find("ERROR") != std::string::npos || detail_line.find("Error") != std::string::npos);
            }
        }
        log_line(status == 0 ? "[SYSTEM] " + name + " completed." : "[ERROR] " + name + " exited with an error.");
        if (refresh_plugins) g.plugin_refresh_requested = true;
        if (refresh_git) g.git_refresh_requested = true;
        g.process_running = false;
    }).detach();
}

static void open_path(const fs::path& path) {
    if (path.empty() || !fs::exists(path)) { log_line("[ERROR] Path does not exist: " + path.string()); return; }
#ifdef _WIN32
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) log_line("[ERROR] Could not open path: " + path.string());
#else
    std::string command = "open " + quote(path);
    std::system(command.c_str());
#endif
}

static void open_url(const std::string& url) {
    if (url.empty()) return;
#ifdef _WIN32
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", std::wstring(url.begin(), url.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) log_line("[ERROR] Could not open URL: " + url);
#else
    std::string command = "open " + quote(fs::path(url));
    std::system(command.c_str());
#endif
}

static void launch_editor(bool game) {
    if (!fs::exists(editor_path()) || !fs::exists(g.project)) { log_line("[ERROR] Select a valid engine and project first."); return; }
#ifdef _WIN32
    std::wstring args = L"\"" + g.project.wstring() + L"\"" + (game ? L" -game -log" : L"");
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", editor_path().wstring().c_str(),
                                                          args.c_str(), nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        log_line("[ERROR] Could not launch Unreal Editor.");
        return;
    }
#else
    std::string command = quote(editor_path()) + " " + quote(g.project) + (game ? " -game -log" : "");
    command += " >/dev/null 2>&1 &";
    std::system(command.c_str());
#endif
    log_line(game ? "[SYSTEM] Game launched." : "[SYSTEM] Unreal Editor launched.");
}

static void launch_editor_home() {
    if (!fs::exists(editor_path())) { log_line("[ERROR] Select a valid engine first."); return; }
#ifdef _WIN32
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", editor_path().wstring().c_str(),
                                                          nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        log_line("[ERROR] Could not launch Unreal Editor.");
        return;
    }
#else
    std::string command = quote(editor_path()) + " >/dev/null 2>&1 &";
    std::system(command.c_str());
#endif
    log_line("[SYSTEM] Unreal Editor launched.");
}

static bool protected_output(const fs::path& path) {
    std::error_code ec;
    auto target = fs::weakly_canonical(path, ec);
    if (target.empty() || target == target.root_path()) return true;
    auto home = fs::path(std::getenv(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    ));
    if (target == home || target == home / "Desktop" || target == home / "Documents" || target == home / "Downloads") return true;
    for (auto critical : {g.project.parent_path(), g.engine}) {
        auto current = fs::weakly_canonical(critical, ec);
        while (!current.empty()) {
            if (target == current) return true;
            if (current == current.root_path()) break;
            current = current.parent_path();
        }
    }
    return false;
}

static void clean_output() {
    auto output = package_output();
    if (protected_output(output)) { log_line("[ERROR] Refusing to remove a protected directory."); return; }
    std::error_code ec;
    auto count = fs::remove_all(output, ec);
    if (ec) log_line("[ERROR] Failed to clean output: " + ec.message());
    else log_line("[SYSTEM] Cleaned package output (" + std::to_string(count) + " entries).");
}


static std::string wrapped_for_preview(const std::string& text, float max_width) {
    std::string wrapped;
    std::string line;
    std::string word;
    auto flush_word = [&] {
        if (word.empty()) return;
        auto candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && ImGui::CalcTextSize(candidate.c_str()).x > max_width) {
            wrapped += line + '\n';
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') {
            flush_word();
            wrapped += line + '\n';
            line.clear();
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            flush_word();
        } else {
            word += c;
        }
    }
    flush_word();
    wrapped += line;
    return wrapped;
}

static void command_preview(const std::string& command, const char* id) {
    auto shown = command.empty() ? std::string("Select an engine and project to preview the command.") : command;
    float preview_width = std::max(120.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2.0f);
    auto wrapped = wrapped_for_preview(shown, preview_width);
    std::vector<char> buffer(wrapped.begin(), wrapped.end());
    buffer.push_back('\0');
    ImGui::InputTextMultiline(id, buffer.data(), buffer.size(), ImVec2(-1, 72),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
}

static void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(440.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static bool readiness_button(const char* label, bool ready) {
    if (ready) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.48f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.13f, 0.62f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.19f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.10f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.10f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.10f, 0.10f, 1.0f));
        ImGui::BeginDisabled();
    }
    bool clicked = ImGui::Button(label);
    if (!ready) ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    return clicked;
}

static void push_tool_tab_colors(int found, int total) {
    ImVec4 base, hovered, selected, overline;
    if (total > 0 && found == total) {
        base = ImVec4(0.08f, 0.25f, 0.13f, 1.0f);
        hovered = ImVec4(0.10f, 0.48f, 0.23f, 1.0f);
        selected = ImVec4(0.08f, 0.40f, 0.18f, 1.0f);
        overline = ImVec4(0.30f, 0.90f, 0.45f, 1.0f);
    } else if (found > 0) {
        base = ImVec4(0.29f, 0.23f, 0.06f, 1.0f);
        hovered = ImVec4(0.58f, 0.45f, 0.08f, 1.0f);
        selected = ImVec4(0.48f, 0.36f, 0.06f, 1.0f);
        overline = ImVec4(1.00f, 0.78f, 0.18f, 1.0f);
    } else {
        base = ImVec4(0.29f, 0.07f, 0.07f, 1.0f);
        hovered = ImVec4(0.55f, 0.11f, 0.11f, 1.0f);
        selected = ImVec4(0.45f, 0.08f, 0.08f, 1.0f);
        overline = ImVec4(1.00f, 0.28f, 0.24f, 1.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Tab, base);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, selected);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, overline);
}

static bool tooling_group_ready(const std::string& group) {
    int found = 0;
    int total = 0;
    for (const auto& tool : g.tools) {
        if (tool.group != group) continue;
        ++total;
        if (tool.found) ++found;
    }
    return total > 0 && found == total;
}

static bool package_platform_ready(int platform) {
    static const char* groups[] = {"Windows", "macOS", "Android", "iOS", "VisionOS"};
    static const char* unrealsharp_groups[] = {"UnrealSharp Windows", "UnrealSharp Mac", "UnrealSharp Android", "UnrealSharp iOS", "UnrealSharp XROS"};
    bool ready = tooling_group_ready(groups[platform]);
    if (g.unrealsharp) ready = ready && tooling_group_ready(unrealsharp_groups[platform]);
    return ready;
}


#ifdef _WIN32
static constexpr UINT WM_UPH_TRAY = WM_APP + 37;
static constexpr UINT ID_TRAY_OPEN = 41001;
static constexpr UINT ID_TRAY_PROJECT_COMPILE = 41002;
static constexpr UINT ID_TRAY_PROJECT_EDITOR = 41003;
static constexpr UINT ID_TRAY_PROJECT_PACKAGE = 41004;
static constexpr UINT ID_TRAY_PROJECT_LAUNCH = 41005;
static constexpr UINT ID_TRAY_EDITOR_LAUNCH = 41006;
static constexpr UINT ID_TRAY_EXIT = 41007;
static constexpr UINT ID_TRAY_RECENT_OPEN = 41008;
static constexpr UINT ID_TRAY_PROJECT_FOLDER = 41009;
static constexpr UINT ID_TRAY_ENGINE_FOLDER = 41010;
static constexpr UINT ID_TRAY_ENGINE_ADD = 41011;
static constexpr UINT ID_TRAY_PROJECT_NEW = 41012;
static constexpr UINT ID_TRAY_RECENT_PROJECT_BASE = 41100;
static constexpr UINT ID_TRAY_ENGINE_BASE = 41200;
static constexpr UINT ID_TRAY_PACKAGE_PLATFORM_BASE = 41300;
static constexpr UINT ID_TRAY_PACKAGE_CONFIG_BASE = 41400;

static HWND g_tray_hwnd = nullptr;
static WNDPROC g_original_window_proc = nullptr;
static NOTIFYICONDATAW g_tray_icon{};
static HICON g_tray_hicon = nullptr;
static bool g_tray_installed = false;
static UINT g_taskbar_created_message = 0;
static std::atomic<bool> g_tray_exit_requested{false};

static std::wstring tray_widen(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (count <= 0) return std::wstring(text.begin(), text.end());
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), count);
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}

static std::wstring tray_engine_label(const Engine& engine) {
    std::string label = engine.label;
    int matches = 0;
    for (const auto& other : g.engines) if (other.label == engine.label) ++matches;
    if (matches > 1) {
        auto suffix = engine.path.filename().string();
        if (suffix.empty()) suffix = engine.path.string();
        label += " (" + suffix + ")";
    }
    return tray_widen(label);
}

static void set_imgui_platform_windows_visible(bool visible) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    for (ImGuiViewport* viewport : platform_io.Viewports) {
        if (!viewport || viewport == main_viewport) continue;
        HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
        if (hwnd) ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

static void restore_main_window_from_tray() {
    if (!g_window) return;
    SDL_ShowWindow(g_window);
    set_imgui_platform_windows_visible(true);
    SDL_RaiseWindow(g_window);
    if (g_tray_hwnd) {
        ShowWindow(g_tray_hwnd, SW_RESTORE);
        SetForegroundWindow(g_tray_hwnd);
    }
}

static void hide_main_window_to_tray() {
    if (!g_window) return;
    set_imgui_platform_windows_visible(false);
    SDL_HideWindow(g_window);
}

static bool tray_can_compile() {
    return fs::is_regular_file(g.project) && fs::exists(build_script()) && !g.targets.empty() && !g.process_running;
}

static bool tray_can_launch_project() {
    return fs::is_regular_file(g.project) && fs::exists(editor_path());
}

static bool tray_can_package() {
    return fs::is_regular_file(g.project) && fs::exists(run_uat()) && package_platform_ready(g.package_platform) &&
           !g.process_running && (!g.unrealsharp || fs::exists(unrealsharp_scripts()));
}

static bool tray_can_launch_editor() {
    return fs::exists(editor_path());
}

static void tray_compile() {
    if (!tray_can_compile()) {
        log_line("[ERROR] Compile is not available for the current project/settings.");
        return;
    }
    auto command = compile_command();
    if (!command.empty()) run_command(command, "Compile");
}

static void tray_launch_project_in_editor() {
    if (!tray_can_launch_project()) {
        log_line("[ERROR] Launch in Editor requires a valid project and engine.");
        return;
    }
    launch_editor(false);
}

static void tray_launch_project() {
    if (!tray_can_launch_project()) {
        log_line("[ERROR] Launch requires a valid project and engine.");
        return;
    }
    launch_editor(true);
}

static void tray_package() {
    if (!tray_can_package()) {
        log_line("[ERROR] Package is not available for the current project/settings.");
        return;
    }
    if (g.clean_output) clean_output();
    auto command = package_command();
    if (!command.empty()) run_command(command, "Package");
}

static void tray_launch_editor() {
    if (!tray_can_launch_editor()) {
        log_line("[ERROR] Select a valid engine first.");
        return;
    }
    launch_editor_home();
}

static void select_tray_engine(size_t index) {
    if (g.process_running) {
        log_line("[ERROR] Stop the current operation before switching engines.");
        return;
    }
    if (index >= g.engines.size()) return;
    g.engine = g.engines[index].path;
    g.plugin_scan_cache.clear();
    discover_engines();
    inspect_project();
    log_line("[SYSTEM] Engine: " + g.engine.string());
    save_settings();
}

static void show_tray_menu() {
    if (!g_tray_hwnd) return;

    POINT menu_origin{};
    GetCursorPos(&menu_origin);

    for (;;) {
        HMENU menu = CreatePopupMenu();
        HMENU recent_menu = CreatePopupMenu();
        HMENU engine_menu = CreatePopupMenu();
        HMENU package_menu = CreatePopupMenu();
        HMENU package_platform_menu = CreatePopupMenu();
        HMENU package_config_menu = CreatePopupMenu();
        if (!menu || !recent_menu || !engine_menu || !package_menu || !package_platform_menu || !package_config_menu) {
            if (recent_menu) DestroyMenu(recent_menu);
            if (engine_menu) DestroyMenu(engine_menu);
            if (package_menu) DestroyMenu(package_menu);
            if (package_platform_menu) DestroyMenu(package_platform_menu);
            if (package_config_menu) DestroyMenu(package_config_menu);
            if (menu) DestroyMenu(menu);
            return;
        }

        AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open UPH");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        // Standard Win32 popup menus do not expose per-item text colors without
        // owner-drawing the entire item. Colored markers keep native menu styling.
        std::wstring current_project = L"\U0001F535 Project: ";
        if (g.project.empty()) current_project += L"None";
        else {
            auto label = g.project.stem().wstring();
            current_project += label.empty() ? g.project.filename().wstring() : label;
        }

        if (g.recent_projects.empty()) {
            AppendMenuW(recent_menu, MF_STRING | MF_GRAYED, 0, L"No recent projects");
        } else {
            const size_t count = std::min<size_t>(g.recent_projects.size(), 100);
            for (size_t i = 0; i < count; ++i) {
                const auto& project = g.recent_projects[i];
                bool available = fs::is_regular_file(project);
                bool selected = normalized_path_key(project) == normalized_path_key(g.project);
                std::wstring label = project.stem().wstring();
                if (label.empty()) label = project.filename().wstring();
                if (!available) label += L" (missing)";
                UINT flags = MF_STRING | (available && !g.process_running ? MF_ENABLED : MF_GRAYED);
                if (selected) flags |= MF_CHECKED;
                AppendMenuW(recent_menu, flags, ID_TRAY_RECENT_PROJECT_BASE + static_cast<UINT>(i), label.c_str());
            }
        }
        AppendMenuW(recent_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(recent_menu, MF_STRING | (g.process_running ? MF_GRAYED : MF_ENABLED), ID_TRAY_RECENT_OPEN, L"Add...");
        AppendMenuW(recent_menu, MF_STRING | (tray_can_launch_editor() ? MF_ENABLED : MF_GRAYED), ID_TRAY_PROJECT_NEW, L"New...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recent_menu), current_project.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_STRING | (tray_can_compile() ? MF_ENABLED : MF_GRAYED), ID_TRAY_PROJECT_COMPILE, L"Compile");
        AppendMenuW(menu, MF_STRING | (tray_can_launch_project() ? MF_ENABLED : MF_GRAYED), ID_TRAY_PROJECT_EDITOR, L"Edit");

        AppendMenuW(package_menu, MF_STRING | (tray_can_package() ? MF_ENABLED : MF_GRAYED),
                    ID_TRAY_PROJECT_PACKAGE, L"Package Now");

        for (size_t i = 0; i < g.platforms.size(); ++i) {
            bool ready = package_platform_ready(static_cast<int>(i));
            UINT flags = MF_STRING | ((!ready || g.process_running) ? MF_GRAYED : MF_ENABLED);
            if (static_cast<int>(i) == g.package_platform) flags |= MF_CHECKED;
            std::string label_text = std::string(g.platforms[i]) + (ready ? "" : " (tooling incomplete)");
            std::wstring label = tray_widen(label_text);
            AppendMenuW(package_platform_menu, flags,
                        ID_TRAY_PACKAGE_PLATFORM_BASE + static_cast<UINT>(i), label.c_str());
        }

        for (size_t i = 0; i < g.configs.size(); ++i) {
            UINT flags = MF_STRING | (g.process_running ? MF_GRAYED : MF_ENABLED);
            if (static_cast<int>(i) == g.package_config) flags |= MF_CHECKED;
            std::wstring label = tray_widen(g.configs[i]);
            AppendMenuW(package_config_menu, flags,
                        ID_TRAY_PACKAGE_CONFIG_BASE + static_cast<UINT>(i), label.c_str());
        }

        AppendMenuW(package_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(package_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(package_platform_menu), L"Platform");
        AppendMenuW(package_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(package_config_menu), L"Config");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(package_menu), L"Package");

        AppendMenuW(menu, MF_STRING | (tray_can_launch_project() ? MF_ENABLED : MF_GRAYED), ID_TRAY_PROJECT_LAUNCH, L"Run");
        bool can_open_project_folder = !g.project.empty() && fs::is_directory(g.project.parent_path());
        AppendMenuW(menu, MF_STRING | (can_open_project_folder ? MF_ENABLED : MF_GRAYED), ID_TRAY_PROJECT_FOLDER, L"Open Folder");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        std::wstring current_engine = L"\U0001F7E0 Engine: ";
        if (g.engine.empty()) current_engine += L"None";
        else {
            const Engine* active = nullptr;
            for (const auto& engine : g.engines) {
                if (normalized_path_key(engine.path) == normalized_path_key(g.engine)) {
                    active = &engine;
                    break;
                }
            }
            current_engine += active ? tray_engine_label(*active) : tray_widen(engine_version(g.engine));
        }

        if (g.engines.empty()) {
            AppendMenuW(engine_menu, MF_STRING | MF_GRAYED, 0, L"No engines found");
        } else {
            const size_t count = std::min<size_t>(g.engines.size(), 100);
            for (size_t i = 0; i < count; ++i) {
                const auto& engine = g.engines[i];
                std::wstring label = tray_engine_label(engine);
                UINT flags = MF_STRING | (g.process_running ? MF_GRAYED : MF_ENABLED);
                if (normalized_path_key(engine.path) == normalized_path_key(g.engine)) flags |= MF_CHECKED;
                AppendMenuW(engine_menu, flags, ID_TRAY_ENGINE_BASE + static_cast<UINT>(i), label.c_str());
            }
        }
        AppendMenuW(engine_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(engine_menu, MF_STRING | (g.process_running ? MF_GRAYED : MF_ENABLED), ID_TRAY_ENGINE_ADD, L"Add...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(engine_menu), current_engine.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        bool can_open_engine_folder = !g.engine.empty() && fs::is_directory(g.engine);
        AppendMenuW(menu, MF_STRING | (tray_can_launch_editor() ? MF_ENABLED : MF_GRAYED), ID_TRAY_EDITOR_LAUNCH, L"Launch");
        AppendMenuW(menu, MF_STRING | (can_open_engine_folder ? MF_ENABLED : MF_GRAYED), ID_TRAY_ENGINE_FOLDER, L"Open Folder");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

        SetForegroundWindow(g_tray_hwnd);
        UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                      menu_origin.x, menu_origin.y, 0, g_tray_hwnd, nullptr);
        DestroyMenu(menu); // destroys attached submenus too
        PostMessageW(g_tray_hwnd, WM_NULL, 0, 0);

        if (command == 0) return;

        // A native Win32 popup always dismisses when a command is chosen. For
        // selector-style commands, rebuild it immediately so the tray menu stays up.
        if (command >= ID_TRAY_RECENT_PROJECT_BASE && command < ID_TRAY_RECENT_PROJECT_BASE + 100) {
            size_t index = static_cast<size_t>(command - ID_TRAY_RECENT_PROJECT_BASE);
            if (index < g.recent_projects.size() && fs::is_regular_file(g.recent_projects[index])) {
                fs::path project = g.recent_projects[index];
                select_project(std::move(project));
            }
            continue;
        }

        if (command >= ID_TRAY_ENGINE_BASE && command < ID_TRAY_ENGINE_BASE + 100) {
            select_tray_engine(static_cast<size_t>(command - ID_TRAY_ENGINE_BASE));
            continue;
        }

        if (command >= ID_TRAY_PACKAGE_PLATFORM_BASE &&
            command < ID_TRAY_PACKAGE_PLATFORM_BASE + static_cast<UINT>(g.platforms.size())) {
            g.package_platform = static_cast<int>(command - ID_TRAY_PACKAGE_PLATFORM_BASE);
            save_settings();
            continue;
        }

        if (command >= ID_TRAY_PACKAGE_CONFIG_BASE &&
            command < ID_TRAY_PACKAGE_CONFIG_BASE + static_cast<UINT>(g.configs.size())) {
            g.package_config = static_cast<int>(command - ID_TRAY_PACKAGE_CONFIG_BASE);
            save_settings();
            continue;
        }

        switch (command) {
            case ID_TRAY_OPEN:
                restore_main_window_from_tray();
                return;
            case ID_TRAY_RECENT_OPEN:
                restore_main_window_from_tray();
                pick_project();
                return;
            case ID_TRAY_PROJECT_NEW:
                tray_launch_editor();
                return;
            case ID_TRAY_PROJECT_COMPILE:
                tray_compile();
                return;
            case ID_TRAY_PROJECT_EDITOR:
                tray_launch_project_in_editor();
                return;
            case ID_TRAY_PROJECT_PACKAGE: {
                fs::path initial = !g.output.empty() ? g.output :
                                   (!g.project.empty() ? g.project.parent_path() : fs::path{});
                pick_folder(DialogKind::PackageOutput, initial);
                return;
            }
            case ID_TRAY_PROJECT_LAUNCH:
                tray_launch_project();
                return;
            case ID_TRAY_PROJECT_FOLDER:
                if (!g.project.empty()) open_path(g.project.parent_path());
                return;
            case ID_TRAY_EDITOR_LAUNCH:
                tray_launch_editor();
                return;
            case ID_TRAY_ENGINE_ADD:
                restore_main_window_from_tray();
                pick_folder(DialogKind::Engine, g.engine);
                return;
            case ID_TRAY_ENGINE_FOLDER:
                if (!g.engine.empty()) open_path(g.engine);
                return;
            case ID_TRAY_EXIT:
                g_tray_exit_requested = true;
                return;
            default:
                return;
        }
    }
}

static LRESULT CALLBACK uph_tray_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE && g_tray_installed) {
        hide_main_window_to_tray();
        return 0;
    }

    if (g_taskbar_created_message && message == g_taskbar_created_message && g_tray_installed) {
        Shell_NotifyIconW(NIM_ADD, &g_tray_icon);
        return 0;
    }

    if (message == WM_UPH_TRAY) {
        switch (static_cast<UINT>(lparam)) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                show_tray_menu();
                return 0;
            case WM_LBUTTONDBLCLK:
                restore_main_window_from_tray();
                return 0;
            default:
                break;
        }
    }

    return g_original_window_proc ? CallWindowProcW(g_original_window_proc, hwnd, message, wparam, lparam)
                                  : DefWindowProcW(hwnd, message, wparam, lparam);
}

static HICON default_application_icon() {
    return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

static bool install_tray_icon() {
    if (!g_window || g_tray_installed) return g_tray_installed;

    auto properties = SDL_GetWindowProperties(g_window);
    g_tray_hwnd = static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!g_tray_hwnd) {
        log_line("[ERROR] Could not get the native window handle for the tray icon.");
        return false;
    }

    g_original_window_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_tray_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(uph_tray_window_proc)));
    if (!g_original_window_proc) {
        log_line("[ERROR] Could not install the tray window procedure.");
        g_tray_hwnd = nullptr;
        return false;
    }

    wchar_t executable[MAX_PATH]{};
    SHFILEINFOW file_info{};
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) &&
        SHGetFileInfoW(executable, 0, &file_info, sizeof(file_info), SHGFI_ICON | SHGFI_SMALLICON)) {
        g_tray_hicon = file_info.hIcon;
    }
    if (!g_tray_hicon) g_tray_hicon = default_application_icon();

    g_tray_icon = {};
    g_tray_icon.cbSize = sizeof(g_tray_icon);
    g_tray_icon.hWnd = g_tray_hwnd;
    g_tray_icon.uID = 1;
    g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray_icon.uCallbackMessage = WM_UPH_TRAY;
    g_tray_icon.hIcon = g_tray_hicon;
    lstrcpynW(g_tray_icon.szTip, L"UPH - Unreal Project Handler",
              static_cast<int>(sizeof(g_tray_icon.szTip) / sizeof(g_tray_icon.szTip[0])));

    if (!Shell_NotifyIconW(NIM_ADD, &g_tray_icon)) {
        log_line("[ERROR] Could not add the UPH tray icon.");
        SetWindowLongPtrW(g_tray_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_window_proc));
        g_original_window_proc = nullptr;
        HICON default_icon = default_application_icon();
        if (g_tray_hicon && g_tray_hicon != default_icon) DestroyIcon(g_tray_hicon);
        g_tray_hicon = nullptr;
        g_tray_hwnd = nullptr;
        return false;
    }

    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    g_tray_installed = true;
    return true;
}

static void remove_tray_icon() {
    if (g_tray_installed) Shell_NotifyIconW(NIM_DELETE, &g_tray_icon);
    g_tray_installed = false;

    if (g_tray_hwnd && g_original_window_proc)
        SetWindowLongPtrW(g_tray_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_window_proc));
    g_original_window_proc = nullptr;
    g_tray_hwnd = nullptr;

    HICON default_icon = default_application_icon();
    if (g_tray_hicon && g_tray_hicon != default_icon) DestroyIcon(g_tray_hicon);
    g_tray_hicon = nullptr;
}
#else
static void remove_tray_icon() {}
#endif

static void draw_tooling_ui() {
    if (ImGui::Button("Refresh Tooling")) inspect_tooling();
    if (ImGui::BeginTabBar("##tooling_tabs")) {
        for (const char* group : {"Windows", "macOS", "Linux", "iOS", "VisionOS", "Android", "UnrealSharp"}) {
            int found = 0, total = 0;
            for (const auto& tool : g.tools) {
                bool belongs = std::string(group) == "UnrealSharp" ? tool.group.rfind("UnrealSharp ", 0) == 0 : tool.group == group;
                if (belongs) { ++total; if (tool.found) ++found; }
            }
            auto label = std::string(group) + " " + std::to_string(found) + "/" + std::to_string(total);
            push_tool_tab_colors(found, total);
            if (ImGui::BeginTabItem(label.c_str())) {
                auto draw_rows = [](const std::string& selected_group) {
                    for (const auto& tool : g.tools) if (tool.group == selected_group) {
                        auto color = tool.found ? ImVec4(0.18f, 0.78f, 0.30f, 1.0f) : ImVec4(0.90f, 0.22f, 0.20f, 1.0f);
                        ImGui::TextColored(color, "%-7s", tool.found ? "OK" : "MISSING");
                        ImGui::SameLine();
                        ImGui::TextUnformatted(tool.name.c_str());
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tool.path.empty() ? "Not found" : tool.path.string().c_str());
                    }
                };
                if (std::string(group) == "UnrealSharp") {
                    if (ImGui::BeginTabBar("##unrealsharp_platform_tabs")) {
                        for (const auto& subtab : std::array<std::pair<const char*, const char*>, 5>{{
                            {"Windows", "UnrealSharp Windows"}, {"Mac", "UnrealSharp Mac"},
                            {"iOS", "UnrealSharp iOS"}, {"Android", "UnrealSharp Android"},
                            {"XROS", "UnrealSharp XROS"}}}) {
                            int sub_found = 0, sub_total = 0;
                            for (const auto& tool : g.tools) if (tool.group == subtab.second) { ++sub_total; if (tool.found) ++sub_found; }
                            auto sub_label = std::string(subtab.first) + " " + std::to_string(sub_found) + "/" + std::to_string(sub_total);
                            push_tool_tab_colors(sub_found, sub_total);
                            if (ImGui::BeginTabItem(sub_label.c_str())) {
                                draw_rows(subtab.second);
                                ImGui::EndTabItem();
                            }
                            ImGui::PopStyleColor(4);
                        }
                        ImGui::EndTabBar();
                    }
                } else {
                    draw_rows(group);
                }
                ImGui::EndTabItem();
            }
            ImGui::PopStyleColor(4);
        }
        ImGui::EndTabBar();
    }
}

static ToolMeta meta_for_tool(const ToolRow& tool) {
    auto key = tool_key(tool.group, tool.name);
    auto section = g.tool_catalog_versions.empty() ? std::string{} :
        g.tool_catalog_versions[std::clamp(g.tool_catalog_version, 0, (int)g.tool_catalog_versions.size() - 1)];
    if (auto catalog = g.tool_catalogs.find(section); catalog != g.tool_catalogs.end())
        if (auto found = catalog->second.find(key); found != catalog->second.end()) return found->second;
    if (auto catalog = g.tool_catalogs.find("Default"); catalog != g.tool_catalogs.end())
        if (auto found = catalog->second.find(key); found != catalog->second.end()) return found->second;
    return {};
}

static std::string install_command_for_tool(const ToolRow& tool) {
    return meta_for_tool(tool).install;
}

static std::string version_hint_for_tool(const ToolRow& tool) {
    return meta_for_tool(tool).version;
}

static std::string install_code_for_tool(const ToolRow& tool) {
    return meta_for_tool(tool).code;
}

static void draw_engine_ui();

static void check_catalog_update_async(bool update) {
    if (g.catalog_running.exchange(true)) {
        log_line("[ERROR] Catalog update already running.");
        return;
    }
    log_line(update ? "[SYSTEM] Updating tool catalog..." : "[SYSTEM] Checking tool catalog...");
    std::thread([update] {
        auto remote = fetch_github_tool_catalog();
        if (remote.empty()) {
            log_line("[ERROR] Could not fetch GitHub tool catalog.");
        } else if (update) {
            write_file_text(tool_catalog_path(), remote);
            load_tool_catalog();
            log_line("[SYSTEM] Tool catalog updated from GitHub.");
        } else if (normalized_text(remote) == normalized_text(read_file_text(tool_catalog_path()))) {
            log_line("[SYSTEM] Tool catalog is up to date.");
        } else {
            log_line("[SYSTEM] Tool catalog update available.");
        }
        g.catalog_running = false;
    }).detach();
}

static bool tool_override_uses_folder(const ToolRow& tool) {
    auto name = tool.name;
    return name.find("SDK") != std::string::npos ||
           name.find("platform support") != std::string::npos ||
           name.find("Automation scripts") != std::string::npos ||
           name.find("Managed binaries") != std::string::npos ||
           name.find("UnrealSharp plugin") != std::string::npos;
}

static void draw_settings_ui() {
    draw_engine_ui();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reload Tool Catalog")) load_tool_catalog();
    ImGui::SameLine();
    bool catalog_running = g.catalog_running;
    if (catalog_running) ImGui::BeginDisabled();
    if (ImGui::Button("Check Catalog Update")) check_catalog_update_async(false);
    ImGui::SameLine();
    if (ImGui::Button("Update Catalog")) check_catalog_update_async(true);
    if (catalog_running) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open Catalog Folder")) open_path(tool_catalog_path().parent_path());
    if (g.catalog_running) {
        ImGui::SameLine();
        ImGui::TextDisabled("Working...");
    }
    ImGui::TextDisabled("%s", tool_catalog_path().string().c_str());
    if (!g.tool_catalog_versions.empty()) {
        g.tool_catalog_version = std::clamp(g.tool_catalog_version, 0, (int)g.tool_catalog_versions.size() - 1);
        std::vector<const char*> labels;
        for (const auto& version : g.tool_catalog_versions) labels.push_back(version.c_str());
        if (ImGui::Combo("Catalog Version", &g.tool_catalog_version, labels.data(), (int)labels.size())) {
            g.selected_tool_catalog_version = g.tool_catalog_versions[g.tool_catalog_version];
            save_settings();
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Tooling");
    draw_tooling_ui();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Tool Overrides")) {
        ImGui::TextDisabled("Optional paths used instead of auto-detected tooling.");
        bool refresh = false;
        std::vector<std::string> groups;
        for (const auto& tool : g.tools)
            if (std::find(groups.begin(), groups.end(), tool.group) == groups.end()) groups.push_back(tool.group);
        for (const auto& group : groups) {
            int found = 0, total = 0;
            for (const auto& tool : g.tools) if (tool.group == group) { ++total; if (tool.found) ++found; }
            auto label = group + " " + std::to_string(found) + "/" + std::to_string(total);
            if (!ImGui::CollapsingHeader(label.c_str())) continue;
            for (const auto& tool : g.tools) if (tool.group == group) {
                auto key = tool_key(tool.group, tool.name);
                ImGui::PushID(key.c_str());
                ImGui::SeparatorText(tool.name.c_str());
                ImGui::TextColored(tool.found ? ImVec4(0.18f, 0.78f, 0.30f, 1.0f) : ImVec4(0.90f, 0.22f, 0.20f, 1.0f),
                                   "%s", tool.found ? "OK" : "MISSING");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tool.path.empty() ? "No path detected" : tool.path.string().c_str());
                auto version = version_hint_for_tool(tool);
                if (!version.empty()) ImGui::TextDisabled("Version: %s", version.c_str());
                auto current = g.tool_overrides.contains(key) ? g.tool_overrides[key].string() : std::string{};
                std::array<char, 4096> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "%s", current.c_str());
                float actions_width = ImGui::CalcTextSize("Browse...").x + ImGui::CalcTextSize("Clear").x +
                                      ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
                ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - actions_width));
                if (ImGui::InputTextWithHint("##override", "Manual override path", buffer.data(), buffer.size())) {
                    if (buffer[0]) g.tool_overrides[key] = buffer.data();
                    else g.tool_overrides.erase(key);
                    refresh = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) {
                    auto initial = current.empty() ? tool.path : fs::path(current);
                    if (tool_override_uses_folder(tool)) pick_override_folder(key, fs::is_regular_file(initial) ? initial.parent_path() : initial);
                    else pick_override_file(key, initial);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear")) {
                    g.tool_overrides.erase(key);
                    refresh = true;
                }
                auto install = install_command_for_tool(tool);
                if (!install.empty()) ImGui::TextWrapped("%s", install.c_str());
                auto code = install_code_for_tool(tool);
                if (code.empty()) ImGui::BeginDisabled();
                if (ImGui::Button("Copy Code")) SDL_SetClipboardText(code.c_str());
                if (code.empty()) ImGui::EndDisabled();
                if (!code.empty()) tooltip(code.c_str());
                ImGui::PopID();
            }
        }
        if (refresh) {
            inspect_tooling();
            save_settings();
        }
    }
}

static const std::vector<fs::path>& list_plugins(const fs::path& root, bool refresh = false) {
    static const std::vector<fs::path> empty;
    if (root.empty()) return empty;
    auto cache_key = normalized_path_key(root);
    if (!refresh) {
        auto cached = g.plugin_scan_cache.find(cache_key);
        if (cached != g.plugin_scan_cache.end()) return cached->second;
    }
    std::vector<fs::path> result;
    std::error_code ec;
    if (fs::exists(root, ec)) {
        fs::recursive_directory_iterator entry(root, fs::directory_options::skip_permission_denied, ec), end;
        while (entry != end) {
            if (entry->is_directory(ec)) {
                auto name = entry->path().filename().string();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                if (entry.depth() >= 3 || name == ".git" || name == "binaries" || name == "content" ||
                    name == "intermediate" || name == "saved" || name == "source" || name == "thirdparty")
                    entry.disable_recursion_pending();
            } else if (entry->path().extension() == ".uplugin") {
                auto dir = entry->path().parent_path();
                if (std::find(result.begin(), result.end(), dir) == result.end()) result.push_back(dir);
            }
            entry.increment(ec);
            if (ec) ec.clear();
        }
    }
    std::sort(result.begin(), result.end());
    return g.plugin_scan_cache.insert_or_assign(cache_key, std::move(result)).first->second;
}

static fs::path engine_marketplace_dir() {
    if (g.engine.empty()) return {};
    return g.engine / "Engine/Plugins/Marketplace";
}

static std::string plugin_display_name(const fs::path& dir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".uplugin") continue;
        auto friendly = json_string_value(read_text(entry.path()), "FriendlyName");
        if (!friendly.empty()) return friendly;
        return entry.path().stem().string();
    }
    return dir.filename().string();
}

static std::string name_from_plugin_url(const std::string& url) {
    auto name = url;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0) name.erase(name.size() - 4);
    if (name.empty()) name = url;
    return name;
}

static bool valid_plugin_folder_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    fs::path path(name);
    return path == path.filename() && !path.is_absolute();
}

static std::string add_plugin_submodule_command(const fs::path& root, const fs::path& relative,
                                                const std::string& url, const std::string& folder_name) {
    auto target = root / relative;
    std::string command = "git -C " + quote(root) + " submodule add " + quote(fs::path(url)) + ' ' + quote(relative) +
                          " && git -C " + quote(target) + " submodule update --init --recursive";
    auto identity = folder_name + " " + url;
    std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (identity.find("unrealsharp") != std::string::npos) {
        auto managed = target / "Managed";
#ifdef _WIN32
        command += " && cd /D " + quote(managed) + " && dotnet restore " + quote(fs::path("UnrealSharp/UnrealSharp.sln"));
#else
        command += " && cd " + quote(managed) + " && dotnet restore " + quote(fs::path("UnrealSharp/UnrealSharp.sln"));
#endif
    }
    return command;
}

static PluginGitState inspect_plugin_git_state(const fs::path& dir, const fs::path& root) {
    PluginGitState state;
    if (root.empty()) return state;
    std::error_code ec;
    auto relative = fs::relative(dir, root, ec);
    if (ec) return state;
    auto status = capture_command("git -C " + quote(root) + " submodule status -- " + quote(relative));
    if (status.empty()) return state;
    state.submodule = true;
    auto status_sha = status;
    if (!status_sha.empty() && (status_sha.front() == ' ' || status_sha.front() == '-' || status_sha.front() == '+')) status_sha.erase(status_sha.begin());
    if (auto end = status_sha.find_first_of(" \t\r\n"); end != std::string::npos) status_sha.erase(end);
    auto branch = capture_command("git -C " + quote(dir) + " symbolic-ref --quiet --short HEAD");
    auto sha = capture_command("git -C " + quote(dir) + " rev-parse --short HEAD");
    if (sha.empty() && !status_sha.empty()) sha = status_sha.substr(0, std::min<size_t>(12, status_sha.size()));
    state.revision = branch.empty() ? (sha.empty() ? "detached" : "detached @ " + sha) : branch;
    return state;
}

static void request_plugin_details(std::vector<fs::path> plugins) {
    if (plugins.empty() || g.project.empty() || g.plugin_details_running.exchange(true)) return;
    auto root = g.project.parent_path();
    g.plugin_details_ready = false;
    std::thread([plugins = std::move(plugins), root] {
        std::map<std::string, PluginGitState> details;
        for (const auto& dir : plugins)
            details[normalized_path_key(dir)] = inspect_plugin_git_state(dir, root);
        {
            std::lock_guard lock(g.plugin_mutex);
            g.pending_plugin_git_cache = std::move(details);
        }
        g.plugin_details_ready = true;
        g.plugin_details_running = false;
    }).detach();
}

static void remove_gitmodules_entry(const fs::path& root, const fs::path& relative) {
    auto path = root / ".gitmodules";
    if (!fs::is_regular_file(path)) return;
    std::ifstream in(path);
    std::vector<std::vector<std::string>> blocks(1);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.front() == '[') blocks.emplace_back();
        blocks.back().push_back(line);
    }
    in.close();
    auto wanted = normalized_path_key(relative);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& block : blocks) {
        bool matches = false;
        for (const auto& item : block) {
            auto equal = item.find('=');
            if (equal == std::string::npos) continue;
            auto key = item.substr(0, equal);
            key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c){ return std::isspace(c); }), key.end());
            auto value = item.substr(equal + 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            if (key == "path" && normalized_path_key(fs::path(value)) == wanted) matches = true;
        }
        if (!matches) for (const auto& item : block) out << item << '\n';
    }
}

static void make_tree_writable(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    while (it != end) {
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        ec.clear();
        it.increment(ec);
        if (ec) ec.clear();
    }
}

static void remove_plugin_submodule(const fs::path& root, const fs::path& relative, const std::string& name) {
    if (g.process_running.exchange(true)) { log_line("[ERROR] Another operation is already running."); return; }
    g.stop_requested = false;
    log_line("[SYSTEM] Removing plugin submodule " + relative.generic_string());
    std::thread([root, relative, name] {
        auto run = [&](const std::string& command, bool required = false) {
            int status = std::system(command.c_str());
            if (required && status != 0) log_line("[ERROR] Command failed: " + command);
            return status == 0;
        };
        run("git -C " + quote(root) + " submodule deinit -f -- " + quote(relative));
        auto target = root / relative;
        make_tree_writable(target);
        std::error_code ec;
        fs::remove_all(target, ec);
        bool worktree_removed = !fs::exists(target);
        if (ec || !worktree_removed) log_line("[ERROR] Could not remove plugin worktree " + target.string() + (ec ? ": " + ec.message() : " (files are in use)"));
        run("git -C " + quote(root) + " rm --cached -f --ignore-unmatch -- " + quote(relative), true);
        remove_gitmodules_entry(root, relative);
        auto metadata = root / ".git/modules" / relative;
        make_tree_writable(metadata);
        ec.clear();
        fs::remove_all(metadata, ec);
        if (ec) log_line("[ERROR] Could not remove submodule metadata " + metadata.string() + ": " + ec.message());
        if (fs::is_regular_file(root / ".gitmodules")) run("git -C " + quote(root) + " add -- .gitmodules");
        if (worktree_removed && !ec) log_line("[SYSTEM] Remove Submodule " + name + " completed.");
        else log_line("[ERROR] Remove Submodule " + name + " finished with files still locked. Close only the application holding the reported path, then retry.");
        g.plugin_refresh_requested = true;
        g.process_running = false;
    }).detach();
}

static void draw_plugin_list(const fs::path& root, bool refresh = false, bool show_git_details = false) {
    if (root.empty()) { ImGui::TextDisabled("Select a project on the Project tab first."); return; }
    if (refresh && !g.plugin_details_running) g.plugin_git_cache.clear();
    if (!fs::exists(root)) { ImGui::TextDisabled("No plugins in %s", root.string().c_str()); return; }
    const auto& plugins = list_plugins(root, refresh);
    if (plugins.empty()) { ImGui::TextDisabled("No plugins found in %s", root.string().c_str()); return; }
    if (show_git_details) {
        std::vector<fs::path> missing;
        for (const auto& dir : plugins)
            if (!g.plugin_git_cache.contains(normalized_path_key(dir))) missing.push_back(dir);
        request_plugin_details(std::move(missing));
        if (g.plugin_details_running) {
            ImGui::SameLine();
            ImGui::TextDisabled("Checking Git details...");
        }
    }
    for (const auto& dir : plugins) {
        ImGui::PushID(dir.string().c_str());
        ImGui::TextUnformatted(plugin_display_name(dir).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dir.string().c_str());
        ImGui::SameLine();
        if (show_git_details) {
            auto found = g.plugin_git_cache.find(normalized_path_key(dir));
            if (found == g.plugin_git_cache.end()) ImGui::TextDisabled("Checking...");
            else ImGui::TextColored(found->second.submodule ? ImVec4(0.30f, 0.68f, 1.0f, 1.0f) : ImVec4(0.58f, 0.58f, 0.58f, 1.0f),
                                    "%s", found->second.submodule ? ("Submodule | " + found->second.revision).c_str() : "Local folder");
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Open")) open_path(dir);
        ImGui::PopID();
    }
}

static void draw_engine_marketplace_plugins() {
    if (!ImGui::CollapsingHeader("Engine Marketplace", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto root = engine_marketplace_dir();
    if (root.empty()) { ImGui::TextDisabled("Select an engine in Settings first."); return; }
    if (!fs::exists(root)) { ImGui::TextDisabled("No marketplace folder at %s", root.string().c_str()); return; }
    bool refresh = ImGui::SmallButton("Refresh##engine_plugins");
    ImGui::SameLine(); ImGui::TextDisabled("Cached after first scan");
    draw_plugin_list(root, refresh, false);
}

static void draw_project_plugins() {
    if (!ImGui::CollapsingHeader("Project Plugins", ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool refresh = ImGui::SmallButton("Refresh##project_plugins");
    if (refresh && g.plugin_details_running) g.plugin_refresh_requested = true;
    ImGui::SameLine(); ImGui::TextDisabled("Cached after first scan");
    draw_plugin_list(g.project.empty() ? fs::path{} : g.project.parent_path() / "Plugins", refresh, true);
}

static void draw_favorite_plugins() {
    if (!ImGui::CollapsingHeader("Favorite Plugins", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("Save a Git URL and its destination folder. Add Submodule tracks it in the project repository; Clone does not.");
    static char name_buf[256];
    static char url_buf[512] = "https://github.com/";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##fav_name", "Folder Name", name_buf, sizeof(name_buf));
    tooltip("Folder created inside the project's Plugins directory. This is also the short label shown below.");
    ImGui::SameLine();
    auto add_width = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 200.0f - ImGui::GetStyle().ItemSpacing.x - add_width));
    bool add_clicked = ImGui::InputTextWithHint("##fav_url", "https://github.com/org/plugin.git", url_buf, sizeof(url_buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add")) add_clicked = true;
    if (add_clicked && url_buf[0]) {
        auto url = normalized_git_url(url_buf);
        auto folder_name = name_buf[0] ? std::string(name_buf) : name_from_plugin_url(url);
        if (!valid_plugin_folder_name(folder_name)) {
            log_line("[ERROR] Folder Name must be a single folder name, without slashes or '..'.");
        } else {
            g.favorite_plugins.push_back({folder_name, url});
            name_buf[0] = 0;
            std::snprintf(url_buf, sizeof(url_buf), "%s", "https://github.com/");
            save_settings();
        }
    }
    if (g.favorite_plugins.empty()) { ImGui::TextDisabled("No favorite plugins yet."); return; }
    ImGui::Separator();
    int remove = -1;
    for (int i = 0; i < (int)g.favorite_plugins.size(); ++i) {
        auto& plugin = g.favorite_plugins[i];
        ImGui::PushID(i);
        bool valid_folder = valid_plugin_folder_name(plugin.name);
        auto target = g.project.empty() ? fs::path{} : g.project.parent_path() / "Plugins" / plugin.name;
        auto submodule_path = fs::path("Plugins") / plugin.name;
        auto git_key = normalized_path_key(target);
        bool target_exists = !target.empty() && fs::exists(target);
        if (target_exists && !g.plugin_git_cache.contains(git_key)) request_plugin_details({target});
        auto git_state = g.plugin_git_cache.find(git_key);
        bool submodule_exists = target_exists && git_state != g.plugin_git_cache.end() && git_state->second.submodule;
        bool can_clone = !g.project.empty() && !g.process_running && valid_folder;
        bool project_is_git = !g.project.empty() && fs::exists(g.project.parent_path() / ".git");
        bool can_submodule = can_clone && project_is_git && (submodule_exists || !target_exists);
        if (!can_submodule) ImGui::BeginDisabled();
        if (ImGui::SmallButton(submodule_exists ? "Remove Submodule" : "Add Submodule")) {
            auto root = g.project.parent_path();
            if (submodule_exists) {
                remove_plugin_submodule(root, submodule_path, plugin.name);
            } else {
                run_command(add_plugin_submodule_command(root, submodule_path, plugin.url, plugin.name),
                            "Add Submodule " + plugin.name, true);
            }
        }
        if (!can_submodule) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!valid_folder) ImGui::SetTooltip("Folder Name must be a single folder name, without slashes or '..'.");
            else if (!project_is_git) ImGui::SetTooltip("This project is not a Git repository. Use Clone, or initialize Git for the project first.");
            else if (target_exists && !submodule_exists) ImGui::SetTooltip("That folder already exists and is not a submodule.");
            else if (g.process_running) ImGui::SetTooltip("Wait for the current operation to finish.");
            else ImGui::SetTooltip("Select a project first.");
        }
        ImGui::SameLine();
        bool clone_ready = can_clone && !target_exists;
        if (!clone_ready) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Clone")) {
            run_command("git clone " + quote(fs::path(plugin.url)) + ' ' + quote(target), "Clone Plugin " + plugin.name, true);
        }
        if (!clone_ready) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(target_exists ? "The destination folder already exists." : "Select a project and wait for the current operation to finish.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open URL")) open_url(plugin.url);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", plugin.url.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(plugin.name.c_str());
        ImGui::PopID();
    }
    if (remove >= 0) {
        g.favorite_plugins.erase(g.favorite_plugins.begin() + remove);
        save_settings();
    }
}

static void draw_plugins_ui() {
    draw_engine_marketplace_plugins();
    draw_project_plugins();
    draw_favorite_plugins();
}

static const Engine* active_engine() {
    for (const auto& engine : g.engines) if (engine.path == g.engine) return &engine;
    return nullptr;
}

static std::string engine_display_label(const Engine& engine) {
    std::string label = engine.label;
    int matches = 0;
    for (const auto& other : g.engines) if (other.label == engine.label) ++matches;
    if (matches > 1) {
        auto suffix = engine.path.filename().string();
        if (suffix.empty()) suffix = engine.path.string();
        label += " (" + suffix + ")";
    }
    return label;
}

static std::string current_engine_label() {
    if (g.engine.empty()) return "No engine selected";
    if (auto* active = active_engine()) return engine_display_label(*active);
    return engine_version(g.engine);
}

static std::string current_project_label() {
    if (g.project.empty()) return "No project selected";
    auto name = g.project.stem().string();
    return name.empty() ? g.project.filename().string() : name;
}

static void project_combo_items() {
    fs::path selected;
    int remove_index = -1;

    for (int i = 0; i < (int)g.recent_projects.size(); ++i) {
        const auto& project = g.recent_projects[i];
        ImGui::PushID(i);
        bool available = fs::is_regular_file(project);
        bool active = normalized_path_key(project) == normalized_path_key(g.project);
        auto label = project.stem().string() + (available ? "" : " (missing)");

        float remove_width = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                             ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - remove_width));
        if (!available) ImGui::BeginDisabled();
        if (ImGui::Selectable(label.c_str(), active, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - remove_width, 0)))
            selected = project;
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", project.string().c_str());

        ImGui::SameLine();
        if (ImGui::SmallButton("X")) remove_index = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this project from the recent-project list.");
        ImGui::PopID();
    }

    if (remove_index >= 0 && remove_index < (int)g.recent_projects.size()) {
        g.recent_projects.erase(g.recent_projects.begin() + remove_index);
        save_settings();
    }

    if (!g.recent_projects.empty()) ImGui::Separator();
    if (ImGui::Selectable("Browse for project...")) pick_project();

    if (!selected.empty()) select_project(selected);
}

static void engine_combo_items() {
    const Engine* selected = nullptr;
    for (const auto& engine : g.engines) {
        ImGui::PushID(&engine);
        if (ImGui::Selectable(engine_display_label(engine).c_str(), engine.path == g.engine)) selected = &engine;
        ImGui::PopID();
    }
    if (selected) {
        g.engine = selected->path;
        g.plugin_scan_cache.clear();
        discover_engines();
        save_settings();
    }
}

static void draw_engine_ui() {
    if (ImGui::CollapsingHeader("Engines", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto* active = active_engine();
        std::string current = g.engine.empty() ? "No Unreal installations found"
                                                : (active ? engine_display_label(*active) : engine_version(g.engine));
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float label_width = ImGui::CalcTextSize("Engine Version").x;
        float add_width = ImGui::CalcTextSize("Add Engine...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float refresh_width = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float combo_width = ImGui::GetContentRegionAvail().x - label_width - add_width - refresh_width - spacing * 5.0f;
        ImGui::SetNextItemWidth(std::max(160.0f, combo_width));
        if (g.process_running) ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##engine_version", current.c_str())) {
            engine_combo_items();
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Engine Version");
        ImGui::SameLine();
        if (ImGui::Button("Add Engine...")) pick_folder(DialogKind::Engine, g.engine);
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) discover_engines();
        if (g.process_running) ImGui::EndDisabled();
        ImGui::TextDisabled("%s", g.engine.empty() ? "No engine selected" : g.engine.string().c_str());
        bool can_launch_editor = fs::exists(editor_path());
        if (ImGui::Button("Open Engine Folder")) open_path(g.engine);
        ImGui::SameLine();
        if (readiness_button("Launch Editor", can_launch_editor)) launch_editor_home();
    }
}

static void initialize_project_git() {
    if (g.project.empty()) return;
    auto root = g.project.parent_path();
    auto ignore = root / ".gitignore";
    if (!fs::exists(ignore)) {
        std::ofstream out(ignore);
        out <<
            "# Unreal generated files\n"
            "Binaries/\n"
            "DerivedDataCache/\n"
            "Intermediate/\n"
            "Saved/\n"
            ".vs/\n"
            ".idea/\n"
            "*.VC.db\n"
            "*.VC.opendb\n"
            "*.sln\n"
            "*.suo\n"
            "*.opensdf\n"
            "*.sdf\n"
            "*.tmp\n"
            "*.log\n";
    }
    run_command("git -C " + quote(root) + " init", "Git Init", false, true);
}

static void draw_project_git_ui() {
    ImGui::SeparatorText("Git");
    if (g.git_refresh_running) {
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshing...");
    }
    if (g.git_root.empty()) {
        ImGui::TextDisabled(g.git_refresh_running ? "Checking project repository..." : "Project folder is not a Git repository.");
        if (!g.git_refresh_running && !g.project.empty()) {
            const bool disable_init = g.process_running;
            if (disable_init) ImGui::BeginDisabled();
            if (ImGui::Button("Initialize Git Repository")) initialize_project_git();
            if (disable_init) ImGui::EndDisabled();
            tooltip("Run git init in the project folder and create a starter Unreal .gitignore if one does not already exist.");
        }
        return;
    }

    ImGui::TextDisabled("%s", g.git_root.string().c_str());

    bool git_available = !g.process_running;
    if (!git_available) ImGui::BeginDisabled();

    if (ImGui::Button("Fetch")) {
        run_command("git -C " + quote(g.git_root) + " fetch --all --prune", "Git Fetch", false, true);
    }
    tooltip("Fetch updates from all remotes and prune deleted remote branches.");
    ImGui::SameLine();
    if (ImGui::Button("Pull")) {
        run_command("git -C " + quote(g.git_root) + " pull", "Git Pull", false, true);
    }
    tooltip("Pull the current branch from its configured upstream.");
    ImGui::SameLine();
    if (g.git_refresh_running) ImGui::BeginDisabled();
    if (ImGui::Button("Refresh Branches")) g.git_refresh_requested = true;
    if (g.git_refresh_running) ImGui::EndDisabled();

    if (!git_available) ImGui::EndDisabled();

    const char* branch_label = g.git_branch.empty() ? "No branch" : g.git_branch.c_str();
    ImGui::SetNextItemWidth(std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.45f));
    if (g.process_running) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("Branch", branch_label)) {
        for (const auto& branch : g.git_branches) {
            bool selected = branch == g.git_branch;
            if (ImGui::Selectable(branch.c_str(), selected) && !selected) {
                run_command("git -C " + quote(g.git_root) + " switch " + quote(fs::path(branch)),
                            "Switch Branch " + branch, false, true);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (g.process_running) ImGui::EndDisabled();
    tooltip("Switch between local Git branches for this project.");
}

static void draw_project_ui() {
    draw_project_git_ui();

    ImGui::SeparatorText("Compile");
    const bool has_cpp_module = g.project_has_cpp_module && !g.targets.empty();
    if (!has_cpp_module) {
        ImGui::TextDisabled("No C++ module found for this project.");
        ImGui::BeginDisabled();
    }
    std::string target = g.targets.empty() ? "No project targets found" : g.targets[std::min<int>(g.compile_target, g.targets.size()-1)].name;
    if (ImGui::BeginCombo("Target", target.c_str())) {
        for (size_t i = 0; i < g.targets.size(); ++i)
            if (ImGui::Selectable((g.targets[i].name + " (" + g.targets[i].type + ")").c_str(), g.compile_target == (int)i))
                g.compile_target = (int)i;
        ImGui::EndCombo();
    }
    ImGui::Combo("Configuration##compile", &g.compile_config, g.configs.data(), (int)g.configs.size());
    bool can_compile = has_cpp_module && fs::is_regular_file(g.project) && fs::exists(build_script()) && !g.process_running;
    if (readiness_button("Compile Project", can_compile)) run_command(compile_command(), "Compile");
    ImGui::SameLine();
    if (readiness_button("Clean", can_compile)) run_command(compile_command(true), "Clean");

    auto compile_preview = compile_command();
    ImGui::TextUnformatted("Compile Command Preview");
    ImGui::SameLine();
    if (compile_preview.empty()) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Copy Command##compile")) SDL_SetClipboardText(compile_preview.c_str());
    if (compile_preview.empty()) ImGui::EndDisabled();
    command_preview(compile_preview, "##compile_preview");
    if (!has_cpp_module) ImGui::EndDisabled();

    ImGui::SeparatorText("Package");
    ImGui::Checkbox("Use UnrealSharp PackageProject", &g.unrealsharp);
    tooltip("Use UnrealSharp's PackageProject automation command instead of Unreal BuildCookRun.");
    bool selected_platform_ready = package_platform_ready(g.package_platform);
    if (!selected_platform_ready) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.38f, 0.32f, 1.0f));
    if (ImGui::BeginCombo("Platform", g.platforms[g.package_platform])) {
        for (int i = 0; i < (int)g.platforms.size(); ++i) {
            bool ready = package_platform_ready(i);
            if (!ready) ImGui::BeginDisabled();
            if (ImGui::Selectable((std::string(g.platforms[i]) + (ready ? "" : " (tooling incomplete)")).c_str(), g.package_platform == i)) {
                g.package_platform = i;
                g.output.clear();
            }
            if (!ready) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Complete the %s tooling checks to enable this platform.", g.platforms[i]);
            }
        }
        ImGui::EndCombo();
    }
    tooltip("Target platform for the packaged build. Platforms with incomplete tooling remain visible but disabled.");
    if (!selected_platform_ready) ImGui::PopStyleColor();
    ImGui::Combo("Configuration##package", &g.package_config, g.configs.data(), (int)g.configs.size());
    tooltip("Unreal build configuration used for packaging.");
    if (g.unrealsharp) {
        std::vector<std::string> target_types;
        for (const auto& target : g.targets)
            if ((target.type == "Game" || target.type == "Client" || target.type == "Server") &&
                std::find(target_types.begin(), target_types.end(), target.type) == target_types.end()) target_types.push_back(target.type);
        std::vector<const char*> labels;
        for (const auto& type : target_types) labels.push_back(type.c_str());
        if (labels.empty()) ImGui::TextDisabled("No packageable project target types found");
        else ImGui::Combo("Target Type", &g.unrealsharp_target, labels.data(), (int)labels.size());
        tooltip("Target type passed to UnrealSharp PackageProject.");
    }
    auto output = package_output();
    ImGui::TextUnformatted("Output Directory");
    ImGui::SetNextItemWidth(-95.0f);
    auto output_text = output.string();
    std::array<char, 4096> output_buffer{};
    std::snprintf(output_buffer.data(), output_buffer.size(), "%s", output_text.c_str());
    ImGui::InputText("##output", output_buffer.data(), output_buffer.size(), ImGuiInputTextFlags_ReadOnly);
    tooltip("Directory where archived packaged builds are written.");
    ImGui::SameLine(); if (ImGui::Button("Browse...##output")) pick_folder(DialogKind::Output, output);
    tooltip("Choose the package output directory.");
    ImGui::Checkbox("Clean Output Before Package", &g.clean_output);
    tooltip("Delete the previous package output before starting. Unreal's cook cache is not removed.");
    if (!g.unrealsharp) {
        static const char* names[] = {"Build", "Cook", "Stage", "Pak", "Package", "Archive", "Deploy", "Run"};
        static const char* help[] = {
            "Compile project code before continuing.",
            "Convert Unreal assets into platform-specific runtime data.",
            "Copy binaries, cooked assets, configuration, and dependencies into a staging directory.",
            "Pack cooked content into Unreal .pak container files.",
            "Create the distributable application or platform package.",
            "Copy the packaged build into the selected output directory.",
            "Install the packaged build onto a connected device.",
            "Launch the staged or deployed build after packaging."
        };
        ImGui::TextUnformatted("Build Pipeline");
        for (size_t i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine();
            auto id = std::string(names[i]) + "##package_operation_" + std::to_string(i);
            ImGui::Checkbox(id.c_str(), &g.operations[i]);
            tooltip(help[i]);
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Device Operations");
        for (size_t i = 6; i < g.operations.size(); ++i) {
            if (i > 6) ImGui::SameLine();
            auto id = std::string(names[i]) + "##package_operation_" + std::to_string(i);
            ImGui::Checkbox(id.c_str(), &g.operations[i]);
            tooltip(help[i]);
        }
    }
    bool can_package = fs::is_regular_file(g.project) && fs::exists(run_uat()) &&
                       package_platform_ready(g.package_platform) && !g.process_running &&
                       (!g.unrealsharp || fs::exists(unrealsharp_scripts()));
    if (readiness_button("Package Project", can_package)) {
        if (g.clean_output) clean_output();
        auto command = package_command();
        if (!command.empty()) run_command(command, "Package");
    }
    tooltip(can_package ? "Run the selected packaging backend." : "Packaging is disabled until the selected platform's required tooling is ready.");
    ImGui::SameLine(); if (ImGui::Button("Clean Output")) clean_output();
    tooltip("Immediately delete the selected package output directory without clearing Unreal's cook cache.");
    ImGui::SameLine(); if (ImGui::Button("Open Output")) open_path(output);
    tooltip("Open the current package output directory.");
    ImGui::SameLine(); if (ImGui::Button("Stop") && g.process_running) { g.stop_requested = true; log_line("[SYSTEM] Stop requested."); }
    tooltip("Stop the currently tracked compile or package operation.");

    auto package_preview = package_command();
    ImGui::TextUnformatted("Package Command Preview");
    ImGui::SameLine();
    if (package_preview.empty()) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Copy Command##package")) SDL_SetClipboardText(package_preview.c_str());
    if (package_preview.empty()) ImGui::EndDisabled();
    command_preview(package_preview, "##package_preview");
    tooltip("Command that will be executed with the current package settings.");

    ImGui::SeparatorText("Process Status");
    ImGui::TextColored(g.process_running ? ImVec4(0.90f, 0.22f, 0.20f, 1.0f) : ImVec4(0.18f, 0.78f, 0.30f, 1.0f),
                       "%s", g.process_running ? "BUSY - build operation running" : "READY - okay to compile or package");
    save_settings();
}

static void draw_top_context_selectors() {
    std::string project = current_project_label();
    std::string engine = current_engine_label();

    ImGui::TextDisabled("Project");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (g.process_running) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##top_selected_project", project.c_str())) {
        project_combo_items();
        ImGui::EndCombo();
    }
    if (g.process_running) ImGui::EndDisabled();
    tooltip(g.process_running ? "Project switching is locked while an operation is running." : "Switch projects or browse for another .uproject file.");

    bool can_launch_project = fs::is_regular_file(g.project) && fs::exists(editor_path());
    ImGui::SameLine();
    if (readiness_button("Launch in Editor##top_project", can_launch_project)) launch_editor(false);
    tooltip("Open the selected project in Unreal Editor.");

    ImGui::SameLine();
    if (readiness_button("Play##top_project", can_launch_project)) launch_editor(true);
    tooltip("Run the selected project as a game.");

    ImGui::SameLine();
    if (ImGui::Button("Open Project Folder##top_project") && !g.project.empty()) open_path(g.project.parent_path());
    tooltip("Open the selected project's folder.");

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    ImGui::TextDisabled("Engine");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (g.process_running) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##top_selected_engine", engine.c_str())) {
        engine_combo_items();
        ImGui::EndCombo();
    }
    if (g.process_running) ImGui::EndDisabled();
    tooltip(g.process_running ? "Engine switching is locked while an operation is running." : "Pick the active Unreal Engine.");

    ImGui::SameLine();
    if (readiness_button("Launch##top_engine", fs::exists(editor_path()))) launch_editor_home();
    tooltip("Launch Unreal Editor without a project to open the project browser/new-project window.");

    ImGui::Separator();
}

static void draw_footer() {
    const float footer_height = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0f;
    const float footer_y = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - footer_height;
    if (ImGui::GetCursorPosY() < footer_y) ImGui::SetCursorPosY(footer_y);
    ImGui::Separator();
    ImGui::TextDisabled("` Toggle Build Log    |    Ctrl + Q Quit");
}

static void draw_log_panel() {
    ImGui::Checkbox("Auto-scroll", &g.auto_scroll); ImGui::SameLine(); ImGui::Checkbox("Clear on run", &g.clear_on_run);
    tooltip("Clear the log when a compile or package operation starts.");
    ImGui::SameLine(); if (ImGui::Button("Clear")) { std::lock_guard lock(g.mutex); g.logs.clear(); g.selected_logs.clear(); g.log_selection_anchor = -1; }
    bool has_selection;
    {
        std::lock_guard lock(g.mutex);
        has_selection = !g.selected_logs.empty();
    }
    if (!has_selection) ImGui::BeginDisabled();
    if (ImGui::Button("Copy Selected") && has_selection) {
        std::lock_guard lock(g.mutex);
        std::string selected;
        for (int index : g.selected_logs) if (index >= 0 && index < (int)g.logs.size()) {
            if (!selected.empty()) selected += '\n';
            selected += g.logs[index].text;
        }
        SDL_SetClipboardText(selected.c_str());
    }
    if (!has_selection) ImGui::EndDisabled();
    tooltip("Copy all selected log entries in display order.");
    ImGui::SameLine();
    if (ImGui::Button("Select All")) {
        std::lock_guard lock(g.mutex);
        g.selected_logs.clear();
        for (int i = 0; i < (int)g.logs.size(); ++i) g.selected_logs.insert(i);
        g.log_selection_anchor = g.logs.empty() ? -1 : 0;
    }
    tooltip("Select every log entry.");
    ImGui::SameLine();
    if (!has_selection) ImGui::BeginDisabled();
    if (ImGui::Button("Clear Selection")) { g.selected_logs.clear(); g.log_selection_anchor = -1; }
    if (!has_selection) ImGui::EndDisabled();
    tooltip("Deselect all log entries without clearing the log.");
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string all;
        std::lock_guard lock(g.mutex);
        for (const auto& entry : g.logs) { if (!all.empty()) all += '\n'; all += entry.text; }
        SDL_SetClipboardText(all.c_str());
    }
    tooltip("Copy the complete log to the clipboard.");
    ImGui::SameLine(); if (ImGui::Button("Save Log")) save_log_dialog();
    tooltip("Save the complete log to a text file.");
    ImGui::SameLine();
    if (g.process_running) ImGui::TextUnformatted("Running"); else ImGui::TextDisabled("Idle");
    if (has_selection && ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_C)) {
        std::lock_guard lock(g.mutex);
        std::string selected;
        for (int index : g.selected_logs) if (index >= 0 && index < (int)g.logs.size()) {
            if (!selected.empty()) selected += '\n';
            selected += g.logs[index].text;
        }
        SDL_SetClipboardText(selected.c_str());
    }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard lock(g.mutex);
        for (size_t i = 0; i < g.logs.size(); ++i) {
            const auto& entry = g.logs[i];
            int line_count = 1 + (int)std::count(entry.text.begin(), entry.text.end(), '\n');
            float height = ImGui::GetTextLineHeightWithSpacing() * line_count + 6.0f;
            float width = ImGui::GetContentRegionAvail().x;
            std::stringstream lines(entry.text);
            std::string line;
            while (std::getline(lines, line)) width = std::max(width, ImGui::CalcTextSize(line.c_str()).x + 12.0f);
            ImGui::PushStyleColor(ImGuiCol_Header, entry.error ? ImVec4(0.40f, 0.10f, 0.10f, 0.75f) : ImGui::GetStyleColorVec4(ImGuiCol_Header));
            bool clicked = ImGui::Selectable(("##log_entry_" + std::to_string(i)).c_str(), g.selected_logs.contains((int)i),
                                             ImGuiSelectableFlags_AllowDoubleClick, ImVec2(width, height));
            ImGui::PopStyleColor();
            auto min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 5.0f, min.y + 3.0f),
                ImGui::GetColorU32(entry.error ? ImVec4(1.0f, 0.72f, 0.68f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text)), entry.text.c_str());
            if (clicked) {
                bool range = ImGui::GetIO().KeyShift && g.log_selection_anchor >= 0;
                bool toggle = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
                if (range) {
                    if (!toggle) g.selected_logs.clear();
                    int first = std::min(g.log_selection_anchor, (int)i);
                    int last = std::max(g.log_selection_anchor, (int)i);
                    for (int index = first; index <= last; ++index) g.selected_logs.insert(index);
                } else if (toggle) {
                    if (g.selected_logs.contains((int)i)) g.selected_logs.erase((int)i);
                    else g.selected_logs.insert((int)i);
                    g.log_selection_anchor = (int)i;
                } else {
                    g.selected_logs = {(int)i};
                    g.log_selection_anchor = (int)i;
                }
                std::string selected;
                for (int index : g.selected_logs) if (index >= 0 && index < (int)g.logs.size()) {
                    if (!selected.empty()) selected += '\n';
                    selected += g.logs[index].text;
                }
                SDL_SetClipboardText(selected.c_str());
            }
        }
    }
    if (g.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static float draw_ui() {
    if (g.plugin_details_ready.exchange(false)) {
        std::lock_guard lock(g.plugin_mutex);
        for (auto& [path, state] : g.pending_plugin_git_cache)
            g.plugin_git_cache.insert_or_assign(path, std::move(state));
        g.pending_plugin_git_cache.clear();
    }
    if (g.plugin_refresh_requested.exchange(false)) {
        if (g.plugin_details_running) {
            g.plugin_refresh_requested = true;
        } else {
            g.plugin_scan_cache.clear();
            g.plugin_git_cache.clear();
        }
    }
    if (g.git_refresh_ready.exchange(false)) {
        std::lock_guard lock(g.git_mutex);
        g.git_root = std::move(g.pending_git_state.root);
        g.git_branch = std::move(g.pending_git_state.branch);
        g.git_branches = std::move(g.pending_git_state.branches);
        g.pending_git_state = {};
    }
    if (g.git_refresh_requested.exchange(false)) request_project_git_refresh();

    draw_top_context_selectors();

    const float available_height = ImGui::GetContentRegionAvail().y;
    const float footer_reserve = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 8.0f;
    const float expanded_log_height = available_height * 0.52f;
    const float collapsed_log_height = ImGui::GetFrameHeightWithSpacing() + 6.0f;
    const float log_height = g.log_expanded ? expanded_log_height : collapsed_log_height;
    const float content_height = std::max(120.0f, available_height - log_height - footer_reserve - ImGui::GetStyle().ItemSpacing.y);

    static bool project_tab_active = true;
    ImGuiWindowFlags content_flags = project_tab_active ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : ImGuiWindowFlags_None;
    ImGui::BeginChild("##main_content", ImVec2(0, content_height), ImGuiChildFlags_None, content_flags);
    if (ImGui::BeginTabBar("##main_tabs")) {
        if (ImGui::BeginTabItem("Project")) {
            project_tab_active = true;
            draw_project_ui();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Plugins")) {
            project_tab_active = false;
            draw_plugins_ui();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            project_tab_active = false;
            draw_settings_ui();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginChild("##embedded_build_log", ImVec2(0, log_height), ImGuiChildFlags_None);
    if (ImGui::Selectable(g.log_expanded ? "v Build Log" : "> Build Log", false,
                          ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetFrameHeight()))) {
        g.log_expanded = !g.log_expanded;
    }
    if (g.log_expanded) draw_log_panel();
    ImGui::EndChild();

    draw_footer();
    return ImGui::GetCursorPosY();
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init failed: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("UPH - Unreal Project Handler", 1100, 1000, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) { SDL_Log("Window creation failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetWindowMinimumSize(g_window, 1000, 900);
    auto context = SDL_GL_CreateContext(g_window);
    SDL_GL_MakeCurrent(g_window, context);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;
    ImFontConfig font_config;
    font_config.SizePixels = 15.0f;
    io.Fonts->AddFontDefault(&font_config);
#ifdef _WIN32
    static const ImWchar icon_ranges[] = {0x25A0, 0x25FF, 0x2690, 0x26FF, 0};
    ImFontConfig icon_config;
    icon_config.MergeMode = true;
    icon_config.PixelSnapH = true;
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisym.ttf", 15.0f, &icon_config, icon_ranges);
#endif
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.Colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.16f, 0.21f, 1.00f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.12f, 0.42f, 0.58f, 1.00f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.05f, 0.50f, 0.47f, 1.00f);
    style.Colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.25f, 0.88f, 0.78f, 1.00f);
    style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.08f, 0.11f, 0.14f, 1.00f);
    style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.08f, 0.30f, 0.31f, 1.00f);
    ImGui_ImplSDL3_InitForOpenGL(g_window, context);
    ImGui_ImplOpenGL3_Init("#version 150");
    load_settings();
    if (!g.project.empty()) remember_project(g.project);
    load_tool_catalog();
    discover_engines();
    if (!g.project.empty()) match_project_engine(g.project);
    inspect_project();
#ifdef _WIN32
    if (install_tray_icon()) log_line("[SYSTEM] Tray icon ready. Closing the main window hides UPH to the tray.");
#endif
    log_line("[SYSTEM] UPH native started.");
    bool running = true;
    while (running) {
        auto frame_started = std::chrono::steady_clock::now();
        SDL_Event event;
        auto handle_event = [&](SDL_Event& current) {
            ImGui_ImplSDL3_ProcessEvent(&current);
            if (current.type == DIALOG_RESULT_EVENT)
                apply_dialog_result(std::unique_ptr<DialogResult>(static_cast<DialogResult*>(current.user.data1)));
            if (current.type == SDL_EVENT_QUIT) {
#ifdef _WIN32
                if (!g_tray_installed) running = false;
#else
                running = false;
#endif
            }
            if (current.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && current.window.windowID == SDL_GetWindowID(g_window)) {
#ifdef _WIN32
                if (g_tray_installed) hide_main_window_to_tray();
                else running = false;
#else
                running = false;
#endif
            }
        };
        int idle_wait_ms = (g.process_running || g.catalog_running || g.git_refresh_running) ? 33 : 100;
        if (SDL_WaitEventTimeout(&event, idle_wait_ms)) {
            handle_event(event);
            while (SDL_PollEvent(&event)) handle_event(event);
        }
#ifdef _WIN32
        if (g_package_after_output_pick.exchange(false)) tray_package();
        if (g_tray_exit_requested.exchange(false)) running = false;
#endif
        if (!running) break;
#ifdef _WIN32
        if (g_tray_installed && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_HIDDEN)) continue;
#endif
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
            g.log_expanded = !g.log_expanded;
        if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            running = false;
            continue;
        }
        auto* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(main_viewport->ID);
        ImGui::SetNextWindowPos(main_viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(main_viewport->WorkSize, ImGuiCond_Always);
        ImGui::Begin("UPH", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        draw_ui();
        ImGui::End();
        ImGui::Render();
        int width, height;
        SDL_GetWindowSizeInPixels(g_window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.07f, 0.075f, 0.085f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_window, backup_context);
        }
        SDL_GL_SwapWindow(g_window);
        auto frame_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - frame_started);
        constexpr auto minimum_frame_time = std::chrono::milliseconds(33);
        if (frame_elapsed < minimum_frame_time)
            SDL_Delay(static_cast<Uint32>((minimum_frame_time - frame_elapsed).count()));
    }
    remove_tray_icon();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
