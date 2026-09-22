#include "unreal_file_index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <winsvc.h>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kServiceName[] = L"UPHUnrealIndex";
constexpr wchar_t kServiceDisplayName[] = L"UPH Unreal File Index";
constexpr wchar_t kServiceDescription[] =
    L"Maintains a fast machine-wide index of Unreal .uproject and .uplugin files.";

SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};
HANDLE g_stop_event = nullptr;
std::atomic<bool> g_console_stop{false};

struct DirectoryNode {
    std::uint64_t parent = 0;
    std::wstring name;
};

struct TargetNode {
    std::uint64_t parent = 0;
    std::wstring name;
    uph::UnrealFileKind kind = uph::UnrealFileKind::Project;
};

struct VolumeState {
    fs::path root;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::unordered_map<std::uint64_t, DirectoryNode> directories;
    std::unordered_map<std::uint64_t, TargetNode> targets;
    USN next_usn = 0;
    DWORDLONG journal_id = 0;
    bool journal_ready = false;

    VolumeState() = default;
    VolumeState(const VolumeState&) = delete;
    VolumeState& operator=(const VolumeState&) = delete;

    VolumeState(VolumeState&& other) noexcept {
        *this = std::move(other);
    }

    VolumeState& operator=(VolumeState&& other) noexcept {
        if (this == &other) return *this;
        close();
        root = std::move(other.root);
        handle = other.handle;
        directories = std::move(other.directories);
        targets = std::move(other.targets);
        next_usn = other.next_usn;
        journal_id = other.journal_id;
        journal_ready = other.journal_ready;
        other.handle = INVALID_HANDLE_VALUE;
        return *this;
    }

    ~VolumeState() {
        close();
    }

    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
};

std::wstring lower_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool classify_extension(const std::wstring& filename, uph::UnrealFileKind& kind) {
    const auto dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const auto ext = lower_copy(filename.substr(dot));
    if (ext == L".uproject") {
        kind = uph::UnrealFileKind::Project;
        return true;
    }
    if (ext == L".uplugin") {
        kind = uph::UnrealFileKind::Plugin;
        return true;
    }
    return false;
}

fs::path program_data_dir() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramData", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return fs::path(buffer) / "UnrealProjectHandler";
    }
    return fs::path(L"C:\\ProgramData\\UnrealProjectHandler");
}

fs::path cache_path() {
    return program_data_dir() / "unreal-files.idx";
}

fs::path status_path() {
    return program_data_dir() / "index-service-status.ini";
}

fs::path log_path() {
    return program_data_dir() / "index-service.log";
}

fs::path installed_service_path() {
    return program_data_dir() / "uph-index-service.exe";
}

fs::path current_executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    buffer.resize(length);
    return fs::path(buffer);
}

void append_log(const std::string& message) {
    std::error_code ec;
    fs::create_directories(program_data_dir(), ec);
    std::ofstream out(log_path(), std::ios::app);
    if (!out) return;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    out << static_cast<long long>(now) << " " << message << '\n';
}

void write_status(const std::string& state,
                  std::size_t records,
                  std::size_t projects,
                  std::size_t plugins,
                  std::size_t volumes,
                  std::size_t live_volumes,
                  const std::string& error = {}) {
    std::error_code ec;
    fs::create_directories(program_data_dir(), ec);

    auto temporary = status_path();
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return;

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    out << "state=" << state << '\n'
        << "records=" << records << '\n'
        << "projects=" << projects << '\n'
        << "plugins=" << plugins << '\n'
        << "volumes=" << volumes << '\n'
        << "live_volumes=" << live_volumes << '\n'
        << "updated=" << static_cast<long long>(now) << '\n'
        << "cache=" << cache_path().string() << '\n';
    if (!error.empty()) out << "error=" << error << '\n';
    out.close();

    MoveFileExW(temporary.wstring().c_str(), status_path().wstring().c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool is_ntfs_volume(const fs::path& root) {
    wchar_t filesystem_name[64]{};
    const auto root_text = root.root_path().wstring();
    return GetVolumeInformationW(
               root_text.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
               filesystem_name, static_cast<DWORD>(std::size(filesystem_name))) &&
           _wcsicmp(filesystem_name, L"NTFS") == 0;
}

std::vector<fs::path> ntfs_fixed_roots() {
    std::vector<fs::path> roots;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        std::wstring root = L"A:\\";
        root[0] = static_cast<wchar_t>(L'A' + i);
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED && is_ntfs_volume(root))
            roots.emplace_back(root);
    }
    return roots;
}

HANDLE open_volume(const fs::path& root, std::string& error) {
    const auto drive_name = root.root_name().wstring();
    if (drive_name.size() != 2) {
        error = "Not a whole drive root: " + root.string();
        return INVALID_HANDLE_VALUE;
    }

    const std::wstring raw_path = L"\\\\.\\" + drive_name;
    HANDLE handle = CreateFileW(
        raw_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not open " + root.string() +
                " for NTFS metadata access (error " +
                std::to_string(GetLastError()) + ")";
    }
    return handle;
}

bool query_journal(VolumeState& state, std::string& warning) {
    USN_JOURNAL_DATA journal{};
    DWORD bytes = 0;
    if (!DeviceIoControl(
            state.handle,
            FSCTL_QUERY_USN_JOURNAL,
            nullptr,
            0,
            &journal,
            sizeof(journal),
            &bytes,
            nullptr)) {
        warning = "USN journal unavailable for " + state.root.string() +
                  " (error " + std::to_string(GetLastError()) + ")";
        state.journal_ready = false;
        return false;
    }

    state.journal_id = journal.UsnJournalID;
    state.next_usn = journal.NextUsn;
    state.journal_ready = true;
    return true;
}

bool scan_volume(const fs::path& root, VolumeState& state, std::string& error) {
    VolumeState fresh;
    fresh.root = root;
    fresh.handle = open_volume(root, error);
    if (fresh.handle == INVALID_HANDLE_VALUE) return false;

    MFT_ENUM_DATA query{};
    query.StartFileReferenceNumber = 0;
    query.LowUsn = 0;
    query.HighUsn = MAXLONGLONG;

    std::vector<unsigned char> buffer(1024 * 1024);
    bool received_any = false;

    for (;;) {
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(
            fresh.handle,
            FSCTL_ENUM_USN_DATA,
            &query,
            sizeof(query),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes,
            nullptr);

        if (!ok) {
            const DWORD code = GetLastError();
            if (code != ERROR_HANDLE_EOF && code != ERROR_NO_MORE_FILES && !received_any) {
                error = "MFT enumeration failed for " + root.string() +
                        " (error " + std::to_string(code) + ")";
                return false;
            }
            break;
        }

        if (bytes <= sizeof(DWORDLONG)) break;
        received_any = true;

        const auto next_reference =
            *reinterpret_cast<const DWORDLONG*>(buffer.data());
        const unsigned char* cursor = buffer.data() + sizeof(DWORDLONG);
        const unsigned char* end = buffer.data() + bytes;

        while (cursor + sizeof(DWORD) + sizeof(WORD) <= end) {
            const auto record_length = *reinterpret_cast<const DWORD*>(cursor);
            const auto major_version =
                *reinterpret_cast<const WORD*>(cursor + sizeof(DWORD));
            if (record_length == 0 || cursor + record_length > end) break;

            if (major_version == 2 && record_length >= sizeof(USN_RECORD)) {
                const auto* record =
                    reinterpret_cast<const USN_RECORD*>(cursor);
                if (record->FileNameOffset + record->FileNameLength <= record_length) {
                    const auto name_chars =
                        record->FileNameLength / sizeof(wchar_t);
                    const auto* name_ptr =
                        reinterpret_cast<const wchar_t*>(
                            cursor + record->FileNameOffset);
                    std::wstring name(name_ptr, name_chars);

                    const auto frn =
                        static_cast<std::uint64_t>(record->FileReferenceNumber);
                    const auto parent =
                        static_cast<std::uint64_t>(record->ParentFileReferenceNumber);

                    if ((record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        fresh.directories[frn] = {parent, std::move(name)};
                    } else {
                        uph::UnrealFileKind kind{};
                        if (classify_extension(name, kind))
                            fresh.targets[frn] = {parent, std::move(name), kind};
                    }
                }
            }

            cursor += record_length;
        }

        if (next_reference <= query.StartFileReferenceNumber) break;
        query.StartFileReferenceNumber = next_reference;
    }

    if (!received_any) {
        error = "MFT enumeration returned no records for " + root.string();
        return false;
    }

    std::string journal_warning;
    if (!query_journal(fresh, journal_warning) && !journal_warning.empty())
        append_log(journal_warning);

    state = std::move(fresh);
    return true;
}

bool reconstruct_path(const VolumeState& state,
                      const TargetNode& target,
                      fs::path& result) {
    std::vector<std::wstring> pieces;
    std::unordered_set<std::uint64_t> visited;
    std::uint64_t current = target.parent;

    while (current != 0 && visited.insert(current).second) {
        const auto found = state.directories.find(current);
        if (found == state.directories.end()) return false;

        const auto& node = found->second;
        if (!node.name.empty() && node.name != L".")
            pieces.push_back(node.name);

        if (node.parent == current) break;
        current = node.parent;
    }

    result = state.root.root_path();
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it)
        result /= *it;
    result /= target.name;
    return true;
}

std::vector<uph::UnrealFileRecord> collect_records(
    const std::vector<VolumeState>& volumes) {
    std::vector<uph::UnrealFileRecord> records;
    std::size_t reserve = 0;
    for (const auto& volume : volumes) reserve += volume.targets.size();
    records.reserve(reserve);

    for (const auto& volume : volumes) {
        for (const auto& [frn, target] : volume.targets) {
            (void)frn;
            fs::path path;
            if (reconstruct_path(volume, target, path))
                records.push_back({target.kind, std::move(path)});
        }
    }
    return records;
}

bool publish_cache(const std::vector<VolumeState>& volumes,
                   std::size_t& project_count,
                   std::size_t& plugin_count,
                   std::size_t& total_count) {
    auto records = collect_records(volumes);
    project_count = 0;
    plugin_count = 0;
    for (const auto& record : records) {
        if (record.kind == uph::UnrealFileKind::Project) ++project_count;
        else ++plugin_count;
    }

    uph::UnrealFileIndex index;
    index.replace_records(std::move(records));
    total_count = index.records().size();

    std::error_code ec;
    fs::create_directories(program_data_dir(), ec);
    return index.save(cache_path());
}

bool apply_usn_record(VolumeState& state, const USN_RECORD& record) {
    const auto frn = static_cast<std::uint64_t>(record.FileReferenceNumber);
    const auto parent =
        static_cast<std::uint64_t>(record.ParentFileReferenceNumber);
    const auto reason = record.Reason;
    const auto* base = reinterpret_cast<const unsigned char*>(&record);
    const auto* name_ptr =
        reinterpret_cast<const wchar_t*>(base + record.FileNameOffset);
    std::wstring name(name_ptr, record.FileNameLength / sizeof(wchar_t));

    const bool is_directory =
        (record.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (is_directory) {
        bool changed = false;

        if ((reason & USN_REASON_FILE_DELETE) != 0) {
            changed = state.directories.erase(frn) > 0;
        }

        if ((reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)) != 0) {
            state.directories[frn] = {parent, std::move(name)};
            changed = true;
        }

        // RENAME_OLD_NAME deliberately leaves the old node in place until the
        // matching RENAME_NEW_NAME record arrives for the same FRN.
        return changed;
    }

    bool changed = false;
    if ((reason & (USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME)) != 0)
        changed = state.targets.erase(frn) > 0;

    if ((reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)) != 0) {
        uph::UnrealFileKind kind{};
        if (classify_extension(name, kind)) {
            state.targets[frn] = {parent, std::move(name), kind};
            changed = true;
        } else {
            changed = state.targets.erase(frn) > 0 || changed;
        }
    }

    return changed;
}

enum class PollResult {
    NoChange,
    Changed,
    Rescan
};

PollResult poll_volume(VolumeState& state, std::string& error) {
    if (!state.journal_ready) return PollResult::NoChange;

    READ_USN_JOURNAL_DATA query{};
    query.StartUsn = state.next_usn;
    query.ReasonMask =
        USN_REASON_FILE_CREATE |
        USN_REASON_FILE_DELETE |
        USN_REASON_RENAME_OLD_NAME |
        USN_REASON_RENAME_NEW_NAME;
    query.ReturnOnlyOnClose = FALSE;
    query.Timeout = 0;
    query.BytesToWaitFor = 0;
    query.UsnJournalID = state.journal_id;

    std::vector<unsigned char> buffer(256 * 1024);
    DWORD bytes = 0;
    if (!DeviceIoControl(
            state.handle,
            FSCTL_READ_USN_JOURNAL,
            &query,
            sizeof(query),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes,
            nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_JOURNAL_ENTRY_DELETED ||
            code == ERROR_JOURNAL_DELETE_IN_PROGRESS ||
            code == ERROR_INVALID_PARAMETER) {
            error = "USN journal changed for " + state.root.string() +
                    "; rebuilding the volume index.";
            return PollResult::Rescan;
        }

        error = "USN journal read failed for " + state.root.string() +
                " (error " + std::to_string(code) + ")";
        return PollResult::NoChange;
    }

    if (bytes < sizeof(USN)) return PollResult::NoChange;
    state.next_usn = *reinterpret_cast<const USN*>(buffer.data());

    bool changed = false;
    const unsigned char* cursor = buffer.data() + sizeof(USN);
    const unsigned char* end = buffer.data() + bytes;

    while (cursor + sizeof(DWORD) + sizeof(WORD) <= end) {
        const auto record_length = *reinterpret_cast<const DWORD*>(cursor);
        const auto major_version =
            *reinterpret_cast<const WORD*>(cursor + sizeof(DWORD));
        if (record_length == 0 || cursor + record_length > end) break;

        if (major_version == 2 && record_length >= sizeof(USN_RECORD)) {
            const auto* record =
                reinterpret_cast<const USN_RECORD*>(cursor);
            if (record->FileNameOffset + record->FileNameLength <= record_length)
                changed = apply_usn_record(state, *record) || changed;
        }

        cursor += record_length;
    }

    return changed ? PollResult::Changed : PollResult::NoChange;
}

void set_service_state(DWORD state,
                       DWORD accepted_controls = 0,
                       DWORD win32_exit = NO_ERROR,
                       DWORD wait_hint = 0) {
    if (!g_service_status_handle) return;

    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwControlsAccepted = accepted_controls;
    g_service_status.dwWin32ExitCode = win32_exit;
    g_service_status.dwServiceSpecificExitCode = 0;
    g_service_status.dwCheckPoint = 0;
    g_service_status.dwWaitHint = wait_hint;
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

DWORD WINAPI service_control_handler(DWORD control,
                                     DWORD,
                                     LPVOID,
                                     LPVOID) {
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN) {
        set_service_state(SERVICE_STOP_PENDING, 0, NO_ERROR, 5000);
        if (g_stop_event) SetEvent(g_stop_event);
        return NO_ERROR;
    }

    if (control == SERVICE_CONTROL_INTERROGATE) {
        SetServiceStatus(g_service_status_handle, &g_service_status);
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

bool should_stop(bool console_mode) {
    if (console_mode) return g_console_stop.load();
    return g_stop_event && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0;
}

BOOL WINAPI console_control_handler(DWORD control) {
    if (control == CTRL_C_EVENT ||
        control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT) {
        g_console_stop = true;
        return TRUE;
    }
    return FALSE;
}

int run_index_loop(bool console_mode) {
    const auto roots = ntfs_fixed_roots();
    if (roots.empty()) {
        const std::string error = "No fixed NTFS volumes were found.";
        append_log(error);
        write_status("error", 0, 0, 0, 0, 0, error);
        return 2;
    }

    write_status("scanning", 0, 0, 0, roots.size(), 0);
    append_log("Starting Unreal file index scan across " +
               std::to_string(roots.size()) + " NTFS volume(s).");

    std::vector<VolumeState> volumes;
    volumes.reserve(roots.size());

    for (const auto& root : roots) {
        if (should_stop(console_mode)) break;

        VolumeState state;
        std::string error;
        const auto started = std::chrono::steady_clock::now();
        if (!scan_volume(root, state, error)) {
            append_log(error);
            continue;
        }

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        append_log("Indexed " + root.string() + " via MFT in " +
                   std::to_string(elapsed) + " ms; targets=" +
                   std::to_string(state.targets.size()) +
                   (state.journal_ready ? "; USN live." : "; USN unavailable."));
        volumes.push_back(std::move(state));
    }

    if (volumes.empty()) {
        const std::string error =
            "No NTFS volume could be indexed. Check service privileges.";
        write_status("error", 0, 0, 0, 0, 0, error);
        append_log(error);
        return 2;
    }

    std::size_t projects = 0;
    std::size_t plugins = 0;
    std::size_t records = 0;
    if (!publish_cache(volumes, projects, plugins, records)) {
        const std::string error =
            "Initial index completed but the shared cache could not be written.";
        write_status("error", 0, 0, 0, volumes.size(), 0, error);
        append_log(error);
        return 2;
    }

    auto live_count = [&]() {
        return static_cast<std::size_t>(std::count_if(
            volumes.begin(), volumes.end(),
            [](const VolumeState& v) { return v.journal_ready; }));
    };

    write_status("running", records, projects, plugins,
                 volumes.size(), live_count());
    append_log("Published shared Unreal index: " +
               std::to_string(records) + " records.");

    auto last_status_write = std::chrono::steady_clock::now();

    while (!should_stop(console_mode)) {
        bool changed = false;

        for (auto& volume : volumes) {
            std::string poll_error;
            const auto result = poll_volume(volume, poll_error);

            if (!poll_error.empty()) append_log(poll_error);

            if (result == PollResult::Changed) {
                changed = true;
            } else if (result == PollResult::Rescan) {
                const auto root = volume.root;
                VolumeState rebuilt;
                std::string scan_error;
                if (scan_volume(root, rebuilt, scan_error)) {
                    volume = std::move(rebuilt);
                    changed = true;
                } else if (!scan_error.empty()) {
                    append_log(scan_error);
                }
            }
        }

        if (changed) {
            if (publish_cache(volumes, projects, plugins, records)) {
                write_status("running", records, projects, plugins,
                             volumes.size(), live_count());
            } else {
                append_log("Failed to publish an incremental index update.");
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_status_write >= std::chrono::seconds(5)) {
            write_status("running", records, projects, plugins,
                         volumes.size(), live_count());
            last_status_write = now;
        }

        if (console_mode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        } else if (g_stop_event) {
            WaitForSingleObject(g_stop_event, 250);
        }
    }

    write_status("stopped", records, projects, plugins,
                 volumes.size(), live_count());
    append_log("UPH Unreal file index service stopped.");
    return 0;
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_service_status_handle =
        RegisterServiceCtrlHandlerExW(
            kServiceName, service_control_handler, nullptr);
    if (!g_service_status_handle) return;

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        set_service_state(SERVICE_STOPPED, 0, GetLastError(), 0);
        return;
    }

    set_service_state(
        SERVICE_RUNNING,
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

    const int result = run_index_loop(false);

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    set_service_state(
        SERVICE_STOPPED, 0,
        result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

bool wait_for_service_state(SC_HANDLE service,
                            DWORD wanted,
                            DWORD timeout_ms = 15000) {
    const auto started = GetTickCount64();
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;

    while (GetTickCount64() - started < timeout_ms) {
        if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytes))
            return false;

        if (status.dwCurrentState == wanted) return true;
        if (wanted == SERVICE_STOPPED &&
            status.dwCurrentState == SERVICE_STOPPED)
            return true;

        Sleep(100);
    }
    return false;
}

bool stop_service_handle(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytes))
        return false;

    if (status.dwCurrentState == SERVICE_STOPPED) return true;

    SERVICE_STATUS ignored{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored)) {
        const DWORD code = GetLastError();
        if (code != ERROR_SERVICE_NOT_ACTIVE) return false;
    }

    return wait_for_service_state(service, SERVICE_STOPPED, 30000);
}

int install_service() {
    std::error_code ec;
    fs::create_directories(program_data_dir(), ec);
    if (ec) {
        std::cerr << "Could not create " << program_data_dir().string()
                  << ": " << ec.message() << '\n';
        return 2;
    }

    const auto source = current_executable_path();
    const auto destination = installed_service_path();
    if (source.empty()) {
        std::cerr << "Could not determine the service executable path.\n";
        return 2;
    }

    SC_HANDLE manager = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        std::cerr << "OpenSCManager failed: " << GetLastError() << '\n';
        return 2;
    }

    SC_HANDLE service = OpenServiceW(
        manager,
        kServiceName,
        SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP |
        SERVICE_CHANGE_CONFIG | DELETE);

    if (service) stop_service_handle(service);

    if (source.lexically_normal() != destination.lexically_normal()) {
        if (!CopyFileW(
                source.wstring().c_str(),
                destination.wstring().c_str(),
                FALSE)) {
            const DWORD code = GetLastError();
            if (service) CloseServiceHandle(service);
            CloseServiceHandle(manager);
            std::cerr << "Could not copy service executable to "
                      << destination.string() << " (error " << code << ")\n";
            return 2;
        }
    }

    const std::wstring binary_command =
        L"\"" + destination.wstring() + L"\" --service";

    if (!service) {
        service = CreateServiceW(
            manager,
            kServiceName,
            kServiceDisplayName,
            SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP |
            SERVICE_CHANGE_CONFIG | DELETE,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            binary_command.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);

        if (!service) {
            const DWORD code = GetLastError();
            CloseServiceHandle(manager);
            std::cerr << "CreateService failed: " << code << '\n';
            return 2;
        }
    } else {
        if (!ChangeServiceConfigW(
                service,
                SERVICE_NO_CHANGE,
                SERVICE_AUTO_START,
                SERVICE_NO_CHANGE,
                binary_command.c_str(),
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                kServiceDisplayName)) {
            const DWORD code = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            std::cerr << "ChangeServiceConfig failed: " << code << '\n';
            return 2;
        }
    }

    SERVICE_DESCRIPTIONW description{
        const_cast<LPWSTR>(kServiceDescription)
    };
    ChangeServiceConfig2W(
        service, SERVICE_CONFIG_DESCRIPTION, &description);

    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD code = GetLastError();
        if (code != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            std::cerr << "StartService failed: " << code << '\n';
            return 2;
        }
    }

    wait_for_service_state(service, SERVICE_RUNNING, 15000);

    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    std::cout << "Installed and started " << "UPH Unreal File Index"
              << ".\nShared cache: " << cache_path().string() << '\n';
    return 0;
}

int uninstall_service() {
    SC_HANDLE manager = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        std::cerr << "OpenSCManager failed: " << GetLastError() << '\n';
        return 2;
    }

    SC_HANDLE service = OpenServiceW(
        manager,
        kServiceName,
        SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);

    if (!service) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cout << "UPH Unreal File Index service is not installed.\n";
            return 0;
        }
        std::cerr << "OpenService failed: " << code << '\n';
        return 2;
    }

    stop_service_handle(service);
    if (!DeleteService(service)) {
        const DWORD code = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        std::cerr << "DeleteService failed: " << code << '\n';
        return 2;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    std::error_code ec;
    fs::remove(cache_path(), ec);
    ec.clear();
    fs::remove(status_path(), ec);

    std::cout << "Removed UPH Unreal File Index service.\n";
    return 0;
}

int start_service_command() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return 2;
    SC_HANDLE service = OpenServiceW(
        manager, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return 2;
    }

    int result = 0;
    if (!StartServiceW(service, 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        result = 2;
    else if (!wait_for_service_state(service, SERVICE_RUNNING, 15000))
        result = 2;

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return result;
}

int stop_service_command() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return 2;
    SC_HANDLE service = OpenServiceW(
        manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return 2;
    }

    const bool ok = stop_service_handle(service);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok ? 0 : 2;
}

const char* service_state_name(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return "stopped";
        case SERVICE_START_PENDING: return "starting";
        case SERVICE_STOP_PENDING: return "stopping";
        case SERVICE_RUNNING: return "running";
        case SERVICE_CONTINUE_PENDING: return "continue-pending";
        case SERVICE_PAUSE_PENDING: return "pause-pending";
        case SERVICE_PAUSED: return "paused";
        default: return "unknown";
    }
}

int print_service_status() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        std::cerr << "UPH index service: unavailable (SCM error "
                  << GetLastError() << ")\n";
        return 2;
    }

    SC_HANDLE service = OpenServiceW(
        manager, kServiceName, SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cout << "UPH index service: not installed\n";
            return 1;
        }
        std::cerr << "UPH index service: query failed (error "
                  << code << ")\n";
        return 2;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool ok = QueryServiceStatusEx(
        service,
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status),
        sizeof(status),
        &bytes);

    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    if (!ok) return 2;

    std::cout << "UPH index service: "
              << service_state_name(status.dwCurrentState) << '\n'
              << "Service cache: " << cache_path().string() << '\n'
              << "Service log:   " << log_path().string() << '\n';

    std::ifstream in(status_path());
    if (in) {
        std::string line;
        while (std::getline(in, line))
            std::cout << "  " << line << '\n';
    }
    return status.dwCurrentState == SERVICE_RUNNING ? 0 : 1;
}

int run_self_test() {
    std::string error;
    if (!uph::run_unreal_file_index_self_test(&error)) {
        std::cerr << "Index core self-test failed: " << error << '\n';
        return 2;
    }

    const auto roots = ntfs_fixed_roots();
    std::cout << "UPH index service self-test passed."
              << " Fixed NTFS volumes visible: " << roots.size() << '\n';
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring command = argc >= 2 ? argv[1] : L"--service";

    if (command == L"--install") return install_service();
    if (command == L"--uninstall") return uninstall_service();
    if (command == L"--start") return start_service_command();
    if (command == L"--stop") return stop_service_command();
    if (command == L"--status") return print_service_status();
    if (command == L"--self-test") return run_self_test();

    if (command == L"--console") {
        SetConsoleCtrlHandler(console_control_handler, TRUE);
        return run_index_loop(true);
    }

    if (command != L"--service") {
        std::cerr
            << "Usage: uph-index-service.exe "
               "--service|--console|--install|--uninstall|--start|--stop|--status|--self-test\n";
        return 2;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD code = GetLastError();
        if (code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::cerr
                << "This executable is normally launched by the Windows Service Control Manager.\n"
                << "Use --console for an elevated foreground test or --install to register it.\n";
        } else {
            std::cerr << "StartServiceCtrlDispatcher failed: "
                      << code << '\n';
        }
        return 2;
    }

    return 0;
}
