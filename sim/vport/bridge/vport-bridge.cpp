/*
 * vport-bridge — bidirectional Ethernet frame bridge between two TAP-Windows adapters.
 *
 * Alternative to vport.sys for machines where test-signing is blocked.
 * Uses two TAP-Windows virtual NICs (signed by OpenVPN Foundation — no kernel
 * signing required) connected by this user-mode bridge process.
 *
 * Architecture:
 *   TwinCAT → TAP-A ←→ vport-bridge.exe ←→ TAP-B ← ec-core (Npcap)
 *
 * Usage:
 *   vport-bridge --list
 *       List all TAP-Windows adapters found on this machine (GUID + friendly name).
 *
 *   vport-bridge <GUID-A> <GUID-B>
 *       Forward Ethernet frames between adapter A and adapter B until Ctrl-C.
 *       A = adapter assigned to TwinCAT (master side).
 *       B = adapter that ec-core opens via Npcap (slave side).
 *
 * Setup (one-time):
 *   1. Install OpenVPN — this installs the signed TAP-Windows driver.
 *   2. Run: .\sim\vport\scripts\Setup-TapBridge.ps1
 *      Creates two TAP adapters named "EtherCAT-Master" and "EtherCAT-Slave".
 *   3. vport-bridge --list   → copy the two GUIDs.
 *   4. vport-bridge <GUID-Master> <GUID-Slave>   → start bridging.
 *   5. TwinCAT: Adapter tab → Search → Show all adapters → "EtherCAT-Master".
 *   6. ec-core --list → find "EtherCAT-Slave" → ec-core --serve <NPF_GUID>.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winreg.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/* ── TAP-Windows IOCTL codes (from OpenVPN tap-windows.h) ────────────────── */

#define TAP_WIN_CONTROL_CODE(req, method) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, (req), (method), FILE_ANY_ACCESS)

#define TAP_WIN_IOCTL_SET_MEDIA_STATUS  TAP_WIN_CONTROL_CODE(6, METHOD_BUFFERED)

/* ── Registry path for network adapters ─────────────────────────────────── */

static constexpr const char* kNetClassKey =
    "SYSTEM\\CurrentControlSet\\Control\\Class\\"
    "{4D36E972-E325-11CE-BFC1-08002BE10318}";

/* ── Adapter descriptor ─────────────────────────────────────────────────── */

struct TapAdapter {
    std::string guid;   /* e.g. {A1B2C3D4-...} — used to open the device   */
    std::string name;   /* friendly name (DriverDesc registry value)         */
};

/* ── Registry helpers ───────────────────────────────────────────────────── */

static std::string reg_read_sz(HKEY hKey, const char* value)
{
    char buf[512] = {};
    DWORD len = sizeof(buf);
    RegQueryValueExA(hKey, value, nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(buf), &len);
    return buf;
}

/* ── List all TAP-Windows adapters ─────────────────────────────────────── */

static std::vector<TapAdapter> list_tap_adapters()
{
    HKEY hClass = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kNetClassKey, 0, KEY_READ, &hClass)
            != ERROR_SUCCESS)
        return {};

    std::vector<TapAdapter> result;
    char subkey[256];
    DWORD subkeyLen = sizeof(subkey);

    for (DWORD i = 0;
         RegEnumKeyExA(hClass, i, subkey, &subkeyLen,
                       nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
         ++i, subkeyLen = sizeof(subkey))
    {
        HKEY hDev = nullptr;
        if (RegOpenKeyExA(hClass, subkey, 0, KEY_READ, &hDev) != ERROR_SUCCESS)
            continue;

        const std::string comp_id = reg_read_sz(hDev, "ComponentId");

        /* TAP-Windows 9.x reports "tap0901"; newer versions use "tap-windows6". */
        if (comp_id.find("tap") != std::string::npos) {
            TapAdapter a;
            a.guid = reg_read_sz(hDev, "NetCfgInstanceId");
            a.name = reg_read_sz(hDev, "DriverDesc");
            if (!a.guid.empty())
                result.push_back(std::move(a));
        }

        RegCloseKey(hDev);
    }

    RegCloseKey(hClass);
    return result;
}

/* ── Open a TAP device handle ───────────────────────────────────────────── */

static HANDLE open_tap(const std::string& guid)
{
    /* TAP-Windows exposes each adapter as a Win32 file: \\.\Global\{GUID}.tap */
    const std::string path = "\\\\.\\Global\\" + guid + ".tap";
    return CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED,
        nullptr);
}

/* ── Bring the virtual link up ──────────────────────────────────────────── */

static bool set_media_status(HANDLE h, bool connected)
{
    ULONG status  = connected ? 1u : 0u;
    DWORD returned = 0;
    return DeviceIoControl(h,
        TAP_WIN_IOCTL_SET_MEDIA_STATUS,
        &status, sizeof(status),
        &status, sizeof(status),
        &returned, nullptr) != FALSE;
}

/* ── Frame forwarding loop (runs in its own thread) ─────────────────────── */

/*
 * Reads raw Ethernet frames from `src` and writes them to `dst`.
 * Exits when `stop_event` is signalled or the handles become invalid.
 */
static void forward_loop(HANDLE src, HANDLE dst, HANDLE stop_event,
                         std::atomic<std::size_t>& counter)
{
    constexpr std::size_t kMaxFrame = 2048;
    auto buf = std::make_unique<unsigned char[]>(kMaxFrame);

    HANDLE read_done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!read_done) return;

    for (;;) {
        OVERLAPPED ov = {};
        ov.hEvent = read_done;
        ResetEvent(read_done);

        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(src, buf.get(),
                                 static_cast<DWORD>(kMaxFrame),
                                 &bytes_read, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) break;

            HANDLE events[2] = { read_done, stop_event };
            const DWORD w = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) break; /* stop_event fired */

            if (!GetOverlappedResult(src, &ov, &bytes_read, FALSE)) break;
        }

        if (bytes_read > 0) {
            DWORD written = 0;
            WriteFile(dst, buf.get(), bytes_read, &written, nullptr);
            ++counter;
        }
    }

    CloseHandle(read_done);
}

/* ── Ctrl-C handler ─────────────────────────────────────────────────────── */

static HANDLE g_stop_event = nullptr;

static BOOL WINAPI ctrl_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        std::printf("\nstopping...\n");
        if (g_stop_event) SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string_view(argv[1]) == "--list") {
        const auto adapters = list_tap_adapters();
        if (adapters.empty()) {
            std::printf("No TAP-Windows adapters found.\n"
                        "Install OpenVPN to get the TAP-Windows driver, then run\n"
                        "  .\\sim\\vport\\scripts\\Setup-TapBridge.ps1\n"
                        "to create the two virtual NICs.\n");
            return 1;
        }
        std::printf("%-42s  %s\n", "GUID", "Name");
        std::printf("%-42s  %s\n",
                    "------------------------------------------",
                    "-----------------------------");
        for (const auto& a : adapters)
            std::printf("%-42s  %s\n", a.guid.c_str(), a.name.c_str());
        return 0;
    }

    if (argc < 3) {
        std::fprintf(stderr,
            "vport-bridge — EtherCAT virtual crossover cable (TAP edition)\n"
            "\n"
            "usage:\n"
            "  vport-bridge --list              list TAP-Windows adapter GUIDs\n"
            "  vport-bridge <GUID-A> <GUID-B>   bridge A <-> B (Ctrl-C to stop)\n"
            "\n"
            "  GUID-A  TAP adapter used by TwinCAT (master)\n"
            "  GUID-B  TAP adapter used by ec-core via Npcap (slave)\n"
            "\n"
            "First-time setup:\n"
            "  1. Install OpenVPN (provides the signed TAP-Windows driver)\n"
            "  2. .\\sim\\vport\\scripts\\Setup-TapBridge.ps1\n"
            "  3. vport-bridge --list\n");
        return 2;
    }

    const std::string guid_a = argv[1];
    const std::string guid_b = argv[2];

    const HANDLE tap_a = open_tap(guid_a);
    if (tap_a == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
            "error: cannot open TAP adapter %s (error %lu)\n"
            "  Run 'vport-bridge --list' to see valid GUIDs.\n"
            "  This process may need Administrator privileges.\n",
            guid_a.c_str(), GetLastError());
        return 1;
    }

    const HANDLE tap_b = open_tap(guid_b);
    if (tap_b == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
            "error: cannot open TAP adapter %s (error %lu)\n",
            guid_b.c_str(), GetLastError());
        CloseHandle(tap_a);
        return 1;
    }

    /* Bring both virtual links up so TwinCAT/Npcap see "connected". */
    set_media_status(tap_a, true);
    set_media_status(tap_b, true);

    g_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    std::atomic<std::size_t> a_to_b{0}, b_to_a{0};

    std::printf("bridging  %s\n"
                "      <-> %s\n"
                "(Ctrl-C to stop)\n",
                guid_a.c_str(), guid_b.c_str());

    std::thread t_a2b(forward_loop, tap_a, tap_b, g_stop_event, std::ref(a_to_b));
    std::thread t_b2a(forward_loop, tap_b, tap_a, g_stop_event, std::ref(b_to_a));

    /* Print a statistics line every 5 seconds while bridging. */
    while (WaitForSingleObject(g_stop_event, 5000) == WAIT_TIMEOUT) {
        std::printf("[bridge] A→B: %zu frames   B→A: %zu frames\n",
                    a_to_b.load(), b_to_a.load());
    }

    /* Signal threads to exit and wait. */
    SetEvent(g_stop_event);
    t_a2b.join();
    t_b2a.join();

    std::printf("[bridge] A→B: %zu frames   B→A: %zu frames  (final)\n",
                a_to_b.load(), b_to_a.load());

    CloseHandle(tap_a);
    CloseHandle(tap_b);
    CloseHandle(g_stop_event);
    return 0;
}
