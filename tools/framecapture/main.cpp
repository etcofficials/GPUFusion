// GPUFusionFrameCapture — measures the frame times of running programs from outside, with Windows'
// own graphics event tracing (ETW), the same events Intel's PresentMon uses. Nothing is loaded into
// the measured programs and nothing about them is changed. No Qt.
//
// What is recorded, per process and per swap chain:
//   - Microsoft-Windows-DXGI   Present start/stop (Direct3D 10, 11 and 12 swap chains)
//   - Microsoft-Windows-D3D9   Present start/stop (Direct3D 9)
//   - Microsoft-Windows-DxgKrnl present events (Blit, Flip, PresentHistory, ...) that happen outside
//     a DXGI/D3D9 present: OpenGL, Vulkan and any other API that presents through the kernel
// Each present's time is when the program called it (the CPU-side start of the frame's present):
// the time between consecutive presents is the program's frame time ("MsBetweenPresents"). Whether
// and when a frame reached the display is not recorded.
//
// Starting an event trace session needs administrator rights (or membership of the "Performance Log
// Users" group). GPU Fusion starts this program through the Windows administrator prompt only when
// the user asks for a measurement; without the rights it reports "access denied" and measures nothing.
//
//   GPUFusionFrameCapture.exe --out <file.json> [--seconds S] [--pid N ...] [--stop-file <path>]
// Without --pid every process that presents is recorded. With --pid the capture also ends early once
// all of those processes have exited. The JSON is written to <file.json> (via a temporary file).
// Exit code: 0 = captured, 1 = could not capture (see "error"), 2 = bad arguments.
#define INITGUID
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace {

// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
constexpr GUID kDxgi = { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };
// {783ACA0A-790E-4D7F-8451-AA850511C6B9}
constexpr GUID kD3d9 = { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };
// {802EC45A-1E99-4B83-9920-87C98277BA9D}
constexpr GUID kDxgKrnl = { 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

// Event ids (from the providers' manifests on Windows 10 19041; the same ids PresentMon uses).
enum : USHORT {
    kDxgiPresentStart = 42, kDxgiPresentStop = 43, kDxgiMpoStart = 55, kDxgiMpoStop = 56,
    kD3d9PresentStart = 1, kD3d9PresentStop = 2,
    kKrnlBlit = 166, kKrnlFlip = 168, kKrnlPresentHistoryStart = 171, kKrnlPresent = 184,
    kKrnlPresentHistoryDetailedStart = 215, kKrnlPresentMpo = 251, kKrnlFlipMpo = 252,
};
constexpr UINT32 kDxgiPresentTest = 0x00000001;           // DXGI_PRESENT_TEST: not a frame
constexpr UINT32 kDxgiWasStillDrawing = 0x887A000A;       // DXGI_ERROR_WAS_STILL_DRAWING
constexpr UINT32 kD3d9WasStillDrawing = 0x8876021C;       // D3DERR_WASSTILLDRAWING
constexpr UINT32 kDxgiOccluded = 0x087A0001;              // DXGI_STATUS_OCCLUDED: presented, not shown
const wchar_t kSessionName[] = L"GPUFusion Frame Capture";
constexpr std::size_t kMaxFramesPerStream = 500000;

struct Stream {
    std::string runtime;        // "DXGI", "D3D9", "Kernel"
    std::uint64_t swapChain = 0; // 0 for kernel presents
    std::vector<double> t;      // present time, ms since the capture started
    std::vector<float> inPresentMs; // time inside the present call (DXGI/D3D9 only)
    UINT32 syncInterval = 0xFFFFFFFF, flags = 0;
    std::uint32_t occluded = 0, discarded = 0;
};

struct Process {
    std::wstring name, path;
    std::map<std::pair<std::string, std::uint64_t>, Stream> streams;
};

struct OpenPresent {
    bool d3d9 = false;
    std::uint64_t swapChain = 0;
    LONGLONG qpc = 0;
    UINT32 flags = 0, syncInterval = 0;
};

struct State {
    LONGLONG qpcStart = 0;
    double qpcPerMs = 1;
    std::set<DWORD> targets;             // empty: every process
    DWORD self = 0;
    std::unordered_map<DWORD, OpenPresent> runtimeOpen; // by thread: inside a DXGI/D3D9 present
    std::unordered_map<DWORD, LONGLONG> kernelOpen;     // by thread: inside a kernel present
    std::map<DWORD, Process> processes;
    std::uint64_t events = 0, framesDropped = 0;
} g;

double msOf(LONGLONG qpc) { return double(qpc - g.qpcStart) / g.qpcPerMs; }

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string escape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
            o += buf;
        } else {
            if (c == '"' || c == '\\')
                o += '\\';
            o += c;
        }
    }
    return o;
}

std::string num(double v, int decimals)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

Process& processFor(DWORD pid)
{
    auto it = g.processes.find(pid);
    if (it != g.processes.end())
        return it->second;
    Process& p = g.processes[pid];
    // Name and path now, while the process exists (it may have exited by the end).
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
            p.path.assign(buf, size);
            const std::size_t slash = p.path.find_last_of(L"\\/");
            p.name = slash == std::wstring::npos ? p.path : p.path.substr(slash + 1);
        }
        CloseHandle(h);
    }
    if (p.name.empty()) { // protected processes (csrss.exe, dwm.exe) cannot be opened: the name from the process list
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W e{};
        e.dwSize = sizeof e;
        for (BOOL more = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
            if (e.th32ProcessID == pid) {
                p.name = e.szExeFile;
                break;
            }
        }
        if (snap != INVALID_HANDLE_VALUE)
            CloseHandle(snap);
    }
    if (p.name.empty())
        p.name = L"process " + std::to_wstring(pid);
    return p;
}

void addFrame(DWORD pid, const char* runtime, std::uint64_t swapChain, LONGLONG qpc, double inPresentMs, UINT32 flags, UINT32 syncInterval,
              bool occluded)
{
    Stream& s = processFor(pid).streams[{ runtime, swapChain }];
    if (s.runtime.empty()) {
        s.runtime = runtime;
        s.swapChain = swapChain;
    }
    if (s.t.size() >= kMaxFramesPerStream) {
        ++g.framesDropped;
        return;
    }
    s.t.push_back(msOf(qpc));
    s.inPresentMs.push_back(float(inPresentMs));
    s.flags = flags;
    s.syncInterval = syncInterval;
    s.occluded += occluded ? 1 : 0;
}

bool wanted(DWORD pid) { return pid != g.self && pid != 0 && pid != 4 && (g.targets.empty() || g.targets.count(pid)); }

// Reads a pointer-sized value: 4 or 8 bytes depending on the process that logged the event.
bool readPointer(const EVENT_RECORD* r, std::size_t offset, std::uint64_t* out, std::size_t* size)
{
    const std::size_t n = (r->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
    if (offset + n > r->UserDataLength)
        return false;
    std::uint64_t v = 0;
    std::memcpy(&v, static_cast<const BYTE*>(r->UserData) + offset, n);
    *out = v;
    *size = n;
    return true;
}

bool readU32(const EVENT_RECORD* r, std::size_t offset, UINT32* out)
{
    if (offset + 4 > r->UserDataLength)
        return false;
    std::memcpy(out, static_cast<const BYTE*>(r->UserData) + offset, 4);
    return true;
}

void onRuntimeStart(const EVENT_RECORD* r, bool d3d9)
{
    OpenPresent o;
    o.d3d9 = d3d9;
    o.qpc = r->EventHeader.TimeStamp.QuadPart;
    std::size_t ptr = 0;
    if (readPointer(r, 0, &o.swapChain, &ptr)) {   // DXGI: pIDXGISwapChain, Flags, SyncInterval
        readU32(r, ptr, &o.flags);                  // D3D9: pSwapchain, Flags
        if (!d3d9)
            readU32(r, ptr + 4, &o.syncInterval);
    }
    g.runtimeOpen[r->EventHeader.ThreadId] = o;
}

void onRuntimeStop(const EVENT_RECORD* r)
{
    const auto it = g.runtimeOpen.find(r->EventHeader.ThreadId);
    if (it == g.runtimeOpen.end())
        return;
    const OpenPresent o = it->second;
    g.runtimeOpen.erase(it);
    UINT32 result = 0;
    readU32(r, 0, &result);
    const DWORD pid = r->EventHeader.ProcessId;
    if (!wanted(pid))
        return;
    const bool test = !o.d3d9 && (o.flags & kDxgiPresentTest);
    const bool stillDrawing = result == (o.d3d9 ? kD3d9WasStillDrawing : kDxgiWasStillDrawing);
    if (test || stillDrawing) { // the program's call did not present a frame
        ++processFor(pid).streams[{ o.d3d9 ? "D3D9" : "DXGI", o.swapChain }].discarded;
        return;
    }
    addFrame(pid, o.d3d9 ? "D3D9" : "DXGI", o.swapChain, o.qpc, double(r->EventHeader.TimeStamp.QuadPart - o.qpc) / g.qpcPerMs, o.flags,
             o.syncInterval, !o.d3d9 && result == kDxgiOccluded);
}

void WINAPI onEvent(PEVENT_RECORD r)
{
    ++g.events;
    const GUID& p = r->EventHeader.ProviderId;
    const USHORT id = r->EventHeader.EventDescriptor.Id;
    const DWORD tid = r->EventHeader.ThreadId;
    if (IsEqualGUID(p, kDxgi)) {
        if (id == kDxgiPresentStart || id == kDxgiMpoStart)
            onRuntimeStart(r, false);
        else if (id == kDxgiPresentStop || id == kDxgiMpoStop)
            onRuntimeStop(r);
    } else if (IsEqualGUID(p, kD3d9)) {
        if (id == kD3d9PresentStart)
            onRuntimeStart(r, true);
        else if (id == kD3d9PresentStop)
            onRuntimeStop(r);
    } else if (IsEqualGUID(p, kDxgKrnl)) {
        if (id == kKrnlPresent || id == kKrnlPresentMpo) { // the kernel present call ends
            g.kernelOpen.erase(tid);
            return;
        }
        // The kernel side of a present (one present can log several of these: a blit and a
        // present-history token, for example). Part of a DXGI/D3D9 present on this thread: already
        // counted. Otherwise the first of them starts a frame of a program presenting through the
        // kernel directly (OpenGL, Vulkan, ...). If the end event was missed, a begin 4 ms after the
        // last one starts a new frame.
        if (g.runtimeOpen.count(tid))
            return;
        const LONGLONG now = r->EventHeader.TimeStamp.QuadPart;
        const auto open = g.kernelOpen.find(tid);
        if (open != g.kernelOpen.end() && double(now - open->second) / g.qpcPerMs < 4.0)
            return;
        g.kernelOpen[tid] = now;
        const DWORD pid = r->EventHeader.ProcessId;
        if (wanted(pid))
            addFrame(pid, "Kernel", 0, now, -1, 0, 0xFFFFFFFF, false);
    }
}

struct ProviderSpec {
    const char* name;
    GUID guid;
    UCHAR level;
    ULONGLONG keywords;
    std::vector<USHORT> ids;
    ULONG status = ERROR_NOT_READY;
    bool filtered = false; // only the listed event ids are delivered
};

std::vector<BYTE> sessionProperties()
{
    std::vector<BYTE> buf(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName) + 16, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = ULONG(buf.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1; // QueryPerformanceCounter timestamps
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = 64;         // KB
    p->MinimumBuffers = 16;
    p->MaximumBuffers = 64;
    p->FlushTimer = 1;          // s
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buf;
}

ULONG enable(TRACEHANDLE session, ProviderSpec& spec)
{
    // Only the listed events (EVENT_FILTER_TYPE_EVENT_ID, Windows 8.1+): the kernel provider logs far
    // more than presents, and filtering at the source keeps the capture light. The filter's size must
    // be exact: the header up to Events plus one USHORT per id.
    std::vector<BYTE> filter(offsetof(EVENT_FILTER_EVENT_ID, Events) + sizeof(USHORT) * spec.ids.size(), 0);
    auto* f = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filter.data());
    f->FilterIn = TRUE;
    f->Count = USHORT(spec.ids.size());
    std::memcpy(f->Events, spec.ids.data(), sizeof(USHORT) * spec.ids.size());
    EVENT_FILTER_DESCRIPTOR d{};
    d.Ptr = ULONGLONG(filter.data());
    d.Size = ULONG(filter.size());
    d.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS params{};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &d;
    params.FilterDescCount = 1;
    spec.status = EnableTraceEx2(session, &spec.guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, spec.level, spec.keywords, 0, 0, &params);
    spec.filtered = spec.status == ERROR_SUCCESS;
    if (spec.status != ERROR_SUCCESS) // a system that rejects the filter: every event of the keyword, sorted out here
        spec.status = EnableTraceEx2(session, &spec.guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, spec.level, spec.keywords, 0, 0, nullptr);
    return spec.status;
}

double processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](const FILETIME& f) { return double((std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}

bool isElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && GetTokenInformation(token, TokenElevation, &e, sizeof e, &size);
    if (token)
        CloseHandle(token);
    return ok && e.TokenIsElevated;
}

bool anyTargetAlive()
{
    for (DWORD pid : g.targets) {
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
            CloseHandle(h);
            if (alive)
                return true;
        }
    }
    return false;
}

bool writeFile(const std::wstring& path, const std::string& text)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string errorJson(const std::string& head, ULONG code, const std::string& message)
{
    return head + ",\"ok\":false,\"errorCode\":" + std::to_string(code) + ",\"error\":\"" + escape(message) + "\"}";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring out, stopFile;
    double seconds = 20;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--out" && more)
            out = argv[++i];
        else if (a == L"--seconds" && more)
            seconds = std::clamp(std::wcstod(argv[++i], nullptr), 1.0, 3600.0);
        else if (a == L"--pid" && more)
            g.targets.insert(DWORD(std::wcstoul(argv[++i], nullptr, 10)));
        else if (a == L"--stop-file" && more)
            stopFile = argv[++i];
        else {
            std::fprintf(stderr, "usage: GPUFusionFrameCapture --out <file.json> [--seconds S] [--pid N ...] [--stop-file <path>]\n");
            return 2;
        }
    }
    // The output must be a .json file (this program may run with administrator rights).
    if (out.size() < 6 || _wcsicmp(out.c_str() + out.size() - 5, L".json") != 0) {
        std::fprintf(stderr, "--out must name a .json file\n");
        return 2;
    }
    g.self = GetCurrentProcessId();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g.qpcPerMs = double(freq.QuadPart) / 1000.0;

    SYSTEMTIME st;
    GetSystemTime(&st);
    char started[40];
    std::snprintf(started, sizeof started, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string head = std::string("{\"tool\":\"GPUFusionFrameCapture\",\"version\":1,\"started\":\"") + started + "\",\"elevated\":"
                     + (isElevated() ? "true" : "false") + ",\"requestedSeconds\":" + num(seconds, 1) + ",\"targets\":[";
    {
        bool first = true;
        for (DWORD pid : g.targets) {
            head += (first ? "" : ",") + std::to_string(pid);
            first = false;
        }
    }
    head += "]";

    // A session left behind by a capture that was killed would block this one: stop it first.
    std::vector<BYTE> props = sessionProperties();
    ControlTraceW(0, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()), EVENT_TRACE_CONTROL_STOP);
    props = sessionProperties();
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()));
    if (status != ERROR_SUCCESS) {
        const std::string why = status == ERROR_ACCESS_DENIED
            ? "Windows denied starting an event trace session: this needs administrator rights (or membership of the "
              "\"Performance Log Users\" group). Nothing was measured."
            : "Windows could not start an event trace session (error " + std::to_string(status) + "). Nothing was measured.";
        writeFile(out, errorJson(head, status, why));
        return 1;
    }

    std::vector<ProviderSpec> providers;
    providers.push_back({ "Microsoft-Windows-DXGI", kDxgi, TRACE_LEVEL_INFORMATION, 0x2,
                          { kDxgiPresentStart, kDxgiPresentStop, kDxgiMpoStart, kDxgiMpoStop } });
    providers.push_back({ "Microsoft-Windows-D3D9", kD3d9, TRACE_LEVEL_INFORMATION, 0x2, { kD3d9PresentStart, kD3d9PresentStop } });
    providers.push_back({ "Microsoft-Windows-DxgKrnl", kDxgKrnl, TRACE_LEVEL_INFORMATION, 0x1,
                          { kKrnlBlit, kKrnlFlip, kKrnlPresentHistoryStart, kKrnlPresent, kKrnlPresentHistoryDetailedStart, kKrnlPresentMpo,
                            kKrnlFlipMpo } });
    int enabled = 0;
    for (ProviderSpec& p : providers)
        enabled += enable(session, p) == ERROR_SUCCESS ? 1 : 0;

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<wchar_t*>(kSessionName);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = onEvent;
    TRACEHANDLE consumer = enabled ? OpenTraceW(&log) : INVALID_PROCESSTRACE_HANDLE;
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        const ULONG code = enabled ? GetLastError() : providers.front().status;
        props = sessionProperties();
        ControlTraceW(session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()), EVENT_TRACE_CONTROL_STOP);
        writeFile(out, errorJson(head, code, "The graphics event providers could not be enabled or read (error " + std::to_string(code) + ")."));
        return 1;
    }

    const double cpu0 = processCpuMs();
    LARGE_INTEGER q0;
    QueryPerformanceCounter(&q0);
    g.qpcStart = q0.QuadPart;
    std::thread reader([consumer] {
        TRACEHANDLE h = consumer;
        ProcessTrace(&h, 1, nullptr, nullptr);
    });

    const auto t0 = std::chrono::steady_clock::now();
    std::string endReason = "duration reached";
    for (;;) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= seconds)
            break;
        if (!stopFile.empty() && GetFileAttributesW(stopFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            endReason = "stopped on request";
            break;
        }
        if (!g.targets.empty() && elapsed > 1 && !anyTargetAlive()) {
            endReason = "every measured process exited";
            break;
        }
        Sleep(100);
    }
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    const double wallMs = double(q1.QuadPart - q0.QuadPart) / g.qpcPerMs;

    props = sessionProperties();
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data());
    ControlTraceW(session, nullptr, p, EVENT_TRACE_CONTROL_STOP); // flushes; ProcessTrace then returns
    reader.join();
    CloseTrace(consumer);
    const double cpuMs = processCpuMs() - cpu0;
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    std::string json = head + ",\"ok\":true,\"endReason\":\"" + endReason + "\",\"wallMs\":" + num(wallMs, 1) + ",\"providers\":[";
    for (std::size_t i = 0; i < providers.size(); ++i)
        json += std::string(i ? "," : "") + "{\"name\":\"" + providers[i].name + "\",\"status\":" + std::to_string(providers[i].status)
              + ",\"filtered\":" + (providers[i].filtered ? "true" : "false") + "}";
    json += "],\"events\":" + std::to_string(g.events) + ",\"eventsLost\":" + std::to_string(p->EventsLost) + ",\"buffersLost\":"
          + std::to_string(p->RealTimeBuffersLost) + ",\"framesNotStored\":" + std::to_string(g.framesDropped);
    json += ",\"overhead\":{\"cpuMs\":" + num(cpuMs, 1) + ",\"cpuPercentOfAllCores\":"
          + num(wallMs > 0 ? 100.0 * cpuMs / wallMs / std::max<DWORD>(1, si.dwNumberOfProcessors) : 0, 2)
          + ",\"peakWorkingSetMB\":" + num(double(pmc.PeakWorkingSetSize) / 1048576.0, 1) + "}";
    json += ",\"processes\":[";
    bool firstProcess = true;
    for (const auto& [pid, proc] : g.processes) {
        json += std::string(firstProcess ? "" : ",") + "{\"pid\":" + std::to_string(pid) + ",\"name\":\"" + escape(utf8(proc.name)) + "\",\"path\":\""
              + escape(utf8(proc.path)) + "\",\"streams\":[";
        firstProcess = false;
        bool firstStream = true;
        for (const auto& [key, s] : proc.streams) {
            char sc[24];
            std::snprintf(sc, sizeof sc, "0x%llX", static_cast<unsigned long long>(s.swapChain));
            json += std::string(firstStream ? "" : ",") + "{\"runtime\":\"" + s.runtime + "\",\"swapChain\":\"" + sc + "\",\"syncInterval\":"
                  + (s.syncInterval == 0xFFFFFFFF ? std::string("null") : std::to_string(s.syncInterval)) + ",\"presentFlags\":" + std::to_string(s.flags)
                  + ",\"occluded\":" + std::to_string(s.occluded) + ",\"discarded\":" + std::to_string(s.discarded) + ",\"t\":[";
            firstStream = false;
            for (std::size_t i = 0; i < s.t.size(); ++i)
                json += (i ? "," : "") + num(s.t[i], 3);
            json += "],\"inPresentMs\":[";
            if (s.runtime != "Kernel")
                for (std::size_t i = 0; i < s.inPresentMs.size(); ++i)
                    json += (i ? "," : "") + num(s.inPresentMs[i], 3);
            json += "]}";
        }
        json += "]}";
    }
    json += "]}";
    if (!writeFile(out, json)) {
        std::fprintf(stderr, "could not write the result file\n");
        return 1;
    }
    return 0;
}
