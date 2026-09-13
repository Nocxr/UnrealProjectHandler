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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
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
struct LogEntry { std::string text; bool error = false; };

struct AppState {
    fs::path project;
    fs::path engine;
    fs::path output;
    std::vector<Engine> engines;
    std::vector<Target> targets;
    std::vector<ToolRow> tools;
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
    bool auto_scroll = true;
    bool clear_on_run = false;
    bool dock_log = true;
    std::set<int> selected_logs;
    int log_selection_anchor = -1;
    std::atomic<bool> process_running{false};
    std::atomic<bool> stop_requested{false};
    std::mutex mutex;
};

static AppState g;
static SDL_Window* g_window = nullptr;

static fs::path settings_path() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
#else
    const char* base = std::getenv("HOME");
#endif
    return fs::path(base ? base : ".") / ".uph-native.ini";
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
    out << "engine=" << g.engine.string() << '\n';
    out << "output=" << g.output.string() << '\n';
    out << "compile_target=" << g.compile_target << '\n';
    out << "compile_config=" << g.compile_config << '\n';
    out << "package_config=" << g.package_config << '\n';
    out << "package_platform=" << g.package_platform << '\n';
    out << "unrealsharp_target=" << g.unrealsharp_target << '\n';
    out << "unrealsharp=" << g.unrealsharp << '\n';
    out << "clean_output=" << g.clean_output << '\n';
    out << "dock_log=" << g.dock_log << '\n';
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
            else if (key == "engine") g.engine = value;
            else if (key == "output") g.output = value;
            else if (key == "compile_target") g.compile_target = std::stoi(value);
            else if (key == "compile_config") g.compile_config = std::stoi(value);
            else if (key == "package_config") g.package_config = std::stoi(value);
            else if (key == "package_platform") g.package_platform = std::stoi(value);
            else if (key == "unrealsharp_target") g.unrealsharp_target = std::stoi(value);
            else if (key == "unrealsharp") g.unrealsharp = std::stoi(value) != 0;
            else if (key == "clean_output") g.clean_output = std::stoi(value) != 0;
            else if (key == "dock_log") g.dock_log = std::stoi(value) != 0;
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

static fs::path command_path(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    char buffer[4096]{};
    std::string line = fgets(buffer, sizeof(buffer), pipe) ? buffer : "";
    pclose(pipe);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return fs::path(line);
}

static void inspect_tooling() {
    g.tools.clear();
    auto add = [](std::string group, std::string name, fs::path path) {
        g.tools.push_back({std::move(group), std::move(name), path, !path.empty() && fs::exists(path)});
    };
    auto add_with_status = [](std::string group, std::string name, fs::path path, bool ready) {
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
        if (!valid_engine(path)) return;
        auto canonical = fs::weakly_canonical(path);
        if (std::none_of(g.engines.begin(), g.engines.end(), [&](const Engine& e){ return e.path == canonical; }))
            g.engines.push_back({engine_version(canonical), canonical});
    };
    if (!g.engine.empty()) add(g.engine);
#ifdef _WIN32
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
    if (g.project.empty()) {
        inspect_tooling();
        return;
    }
    auto source = g.project.parent_path() / "Source";
    std::error_code ec;
    if (fs::exists(source, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(source, ec)) {
            if (!entry.is_regular_file() || entry.path().filename().string().find("Target.cs") == std::string::npos) continue;
            auto text = read_text(entry.path());
            std::string type = "Game";
            for (auto candidate : {"Editor", "Client", "Server", "Program"})
                if (text.find("TargetType." + std::string(candidate)) != std::string::npos) type = candidate;
            auto name = entry.path().filename().string();
            name.erase(name.size() - std::string(".Target.cs").size());
            g.targets.push_back({name, type});
        }
    }
    inspect_tooling();
}

enum class DialogKind { Project, Engine, Output, SaveLog };
struct DialogRequest { DialogKind kind; };
struct DialogResult { DialogKind kind; fs::path path; std::string error; };
constexpr Uint32 DIALOG_RESULT_EVENT = SDL_EVENT_USER + 1;

static void SDLCALL dialog_result(void* userdata, const char* const* files, int) {
    auto request = std::unique_ptr<DialogRequest>(static_cast<DialogRequest*>(userdata));
    if (files && !files[0]) return;
    auto* result = new DialogResult{request->kind, files ? fs::path(files[0]) : fs::path{}, files ? "" : SDL_GetError()};
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
        g.project = result->path;
        inspect_project();
        log_line("[SYSTEM] Project: " + result->path.string());
    } else if (result->kind == DialogKind::Engine) {
        if (!valid_engine(result->path)) {
            log_line("[ERROR] Selected folder is not a usable Unreal Engine installation.");
            return;
        }
        g.engine = result->path;
        discover_engines();
        log_line("[SYSTEM] Engine: " + result->path.string());
    } else {
        if (result->kind == DialogKind::SaveLog) {
            std::ofstream out(result->path);
            std::lock_guard lock(g.mutex);
            for (const auto& entry : g.logs) out << entry.text << '\n';
            return;
        }
        g.output = result->path;
        log_line("[SYSTEM] Package output: " + result->path.string());
    }
    save_settings();
}

static void pick_project() {
    static const SDL_DialogFileFilter filters[] = {{"Unreal Project", "uproject"}, {"All Files", "*"}};
    std::string initial = g.project.empty() ? std::string{} : g.project.parent_path().string();
    SDL_ShowOpenFileDialog(dialog_result, new DialogRequest{DialogKind::Project}, g_window, filters, 2,
                           initial.empty() ? nullptr : initial.c_str(), false);
}

static void pick_folder(DialogKind kind, const fs::path& initial) {
    auto text = initial.string();
    SDL_ShowOpenFolderDialog(dialog_result, new DialogRequest{kind}, g_window,
                             text.empty() ? nullptr : text.c_str(), false);
}

static void save_log_dialog() {
    static const SDL_DialogFileFilter filters[] = {{"Text Log", "txt;log"}, {"All Files", "*"}};
    SDL_ShowSaveFileDialog(dialog_result, new DialogRequest{DialogKind::SaveLog}, g_window, filters, 2, "uph-log.txt");
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

static fs::path package_output() {
    if (!g.output.empty()) return g.output;
    if (g.project.empty()) return {};
    return g.project.parent_path() / "Builds" / g.platforms[g.package_platform] / g.configs[g.package_config];
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

static void run_command(std::string command, const std::string& name) {
    if (g.process_running.exchange(true)) { log_line("[ERROR] Another operation is already running."); return; }
    g.stop_requested = false;
    if (g.clear_on_run) { std::lock_guard lock(g.mutex); g.logs.clear(); }
    log_line("[SYSTEM] Starting " + name + ": " + command);
    std::thread([command = std::move(command), name] {
        auto wrapped = command + " 2>&1";
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
        g.process_running = false;
    }).detach();
}

static void open_path(const fs::path& path) {
    if (path.empty() || !fs::exists(path)) { log_line("[ERROR] Path does not exist: " + path.string()); return; }
#ifdef _WIN32
    std::string command = "start \"\" " + quote(path);
#else
    std::string command = "open " + quote(path);
#endif
    std::system(command.c_str());
}

static void launch_editor(bool game) {
    if (!fs::exists(editor_path()) || !fs::exists(g.project)) { log_line("[ERROR] Select a valid engine and project first."); return; }
    std::string command = quote(editor_path()) + " " + quote(g.project) + (game ? " -game -log" : "");
#ifdef _WIN32
    command = "start \"\" " + command;
#else
    command += " >/dev/null 2>&1 &";
#endif
    std::system(command.c_str());
    log_line(game ? "[SYSTEM] Game launched." : "[SYSTEM] Unreal Editor launched.");
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

static void path_row(const char* label, const fs::path& path, const char* button, void (*action)()) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-95.0f);
    std::array<char, 4096> buffer{};
    auto text = path.string();
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
    ImGui::InputText((std::string("##") + label).c_str(), buffer.data(), buffer.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(button)) action();
}

static void command_preview(const std::string& command, const char* id) {
    auto shown = command.empty() ? std::string("Select an engine and project to preview the command.") : command;
    ImGui::InputTextMultiline(id, shown.data(), shown.size() + 1, ImVec2(-1, 58), ImGuiInputTextFlags_ReadOnly);
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

static void draw_ui() {
    if (ImGui::CollapsingHeader("Project", ImGuiTreeNodeFlags_DefaultOpen)) {
        path_row("Project", g.project, "Browse...", pick_project);
        if (ImGui::Button("Open Project Folder")) open_path(g.project.parent_path());
    }
    if (ImGui::CollapsingHeader("Engine", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string current = g.engine.empty() ? "No Unreal installations found" : engine_version(g.engine);
        if (ImGui::BeginCombo("Engine Version", current.c_str())) {
            for (const auto& engine : g.engines) if (ImGui::Selectable(engine.label.c_str(), engine.path == g.engine)) {
                g.engine = engine.path; save_settings();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Engine...")) pick_folder(DialogKind::Engine, g.engine);
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) discover_engines();
        ImGui::TextDisabled("%s", g.engine.empty() ? "No engine selected" : g.engine.string().c_str());
        if (ImGui::Button("Open Engine Folder")) open_path(g.engine);
        bool can_launch = fs::is_regular_file(g.project) && fs::exists(editor_path());
        ImGui::SameLine(); if (readiness_button("Launch Editor", can_launch)) launch_editor(false);
        ImGui::SameLine(); if (readiness_button("Run Game", can_launch)) launch_editor(true);
    }
    if (ImGui::CollapsingHeader("Tooling")) {
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
    if (ImGui::CollapsingHeader("Compile", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string target = g.targets.empty() ? "No project targets found" : g.targets[std::min<int>(g.compile_target, g.targets.size()-1)].name;
        if (ImGui::BeginCombo("Target", target.c_str())) {
            for (size_t i = 0; i < g.targets.size(); ++i) if (ImGui::Selectable((g.targets[i].name + " (" + g.targets[i].type + ")").c_str(), g.compile_target == (int)i)) g.compile_target = (int)i;
            ImGui::EndCombo();
        }
        ImGui::Combo("Configuration##compile", &g.compile_config, g.configs.data(), (int)g.configs.size());
        bool can_compile = fs::is_regular_file(g.project) && fs::exists(build_script()) && !g.targets.empty() && !g.process_running;
        if (readiness_button("Compile Project", can_compile)) run_command(compile_command(), "Compile");
        ImGui::SameLine(); if (readiness_button("Clean + Rebuild", can_compile)) run_command(compile_command(true), "Rebuild");
        ImGui::TextUnformatted("Compile Command Preview"); command_preview(compile_command(), "##compile_preview");
    }
    if (ImGui::CollapsingHeader("Package", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Use UnrealSharp PackageProject", &g.unrealsharp);
        tooltip("Use UnrealSharp's PackageProject automation command instead of Unreal BuildCookRun.");
        bool selected_platform_ready = package_platform_ready(g.package_platform);
        if (!selected_platform_ready) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.38f, 0.32f, 1.0f));
        if (ImGui::BeginCombo("Platform", g.platforms[g.package_platform])) {
            for (int i = 0; i < (int)g.platforms.size(); ++i) {
                bool ready = package_platform_ready(i);
                if (!ready) ImGui::BeginDisabled();
                if (ImGui::Selectable((std::string(g.platforms[i]) + (ready ? "" : " (tooling incomplete)")).c_str(), g.package_platform == i))
                    g.package_platform = i;
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
            auto c = package_command(); if (!c.empty()) run_command(c, "Package");
        }
        tooltip(can_package ? "Run the selected packaging backend." : "Packaging is disabled until the selected platform's required tooling is ready.");
        ImGui::SameLine(); if (ImGui::Button("Clean Output")) clean_output();
        tooltip("Immediately delete the selected package output directory without clearing Unreal's cook cache.");
        ImGui::SameLine(); if (ImGui::Button("Open Output")) open_path(output);
        tooltip("Open the current package output directory.");
        ImGui::SameLine(); if (ImGui::Button("Stop") && g.process_running) { g.stop_requested = true; log_line("[SYSTEM] Stop requested."); }
        tooltip("Stop the currently tracked compile or package operation.");
        ImGui::TextUnformatted("Package Command Preview"); command_preview(package_command(), "##package_preview");
        tooltip("Command that will be executed with the current package settings.");
    }
    ImGui::SeparatorText("Process Status");
    ImGui::TextColored(g.process_running ? ImVec4(0.90f, 0.22f, 0.20f, 1.0f) : ImVec4(0.18f, 0.78f, 0.30f, 1.0f),
                       "%s", g.process_running ? "BUSY - build operation running" : "READY - okay to compile or package");
    ImGui::Checkbox("Lock Log to Side", &g.dock_log);
    save_settings();
}

static void draw_log_window() {
    auto* viewport = ImGui::GetMainViewport();
    if (g.dock_log) {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x + 12.0f, viewport->Pos.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(760.0f, viewport->Size.y), ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowSize(ImVec2(760.0f, 700.0f), ImGuiCond_FirstUseEver);
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (g.dock_log) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    ImGui::Begin("Build Log", nullptr, flags);
    ImGui::Checkbox("Auto-scroll", &g.auto_scroll); ImGui::SameLine(); ImGui::Checkbox("Clear on run", &g.clear_on_run);
    tooltip("Clear the log when a compile or package operation starts.");
    ImGui::SameLine(); if (ImGui::Button("Clear")) { std::lock_guard lock(g.mutex); g.logs.clear(); g.selected_logs.clear(); g.log_selection_anchor = -1; }
    ImGui::SameLine();
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
    ImGui::End();
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init failed: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("UPH - Unreal Project Handler", 900, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) { SDL_Log("Window creation failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
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
    discover_engines();
    inspect_project();
    log_line("[SYSTEM] UPH native started.");
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == DIALOG_RESULT_EVENT)
                apply_dialog_result(std::unique_ptr<DialogResult>(static_cast<DialogResult*>(event.user.data1)));
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(g_window))) running = false;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        auto* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(main_viewport->ID);
        ImGui::SetNextWindowPos(main_viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(main_viewport->WorkSize, ImGuiCond_Always);
        ImGui::Begin("UPH", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        draw_ui();
        ImGui::End();
        draw_log_window();
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
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
