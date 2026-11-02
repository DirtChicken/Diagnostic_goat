/* Diagnostic Chicken — Windows desktop app.
   Runs only while this window is open. CPU load is read here.
   GPU temperature comes from the AMD or NVIDIA driver already installed.
   The CPU figure is the board thermal sensor. Diagnosis reads the event log. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <pdh.h>
#include <winevt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define HIST 80
#define COL_BG RGB(16, 18, 14)
#define COL_CARD RGB(28, 31, 24)
#define COL_LINE RGB(52, 56, 44)
#define COL_TEXT RGB(232, 236, 224)
#define COL_MUTED RGB(154, 163, 146)
#define COL_AMBER RGB(230, 179, 37)
#define COL_OK RGB(143, 191, 106)
#define COL_WARM RGB(232, 146, 58)
#define COL_HOT RGB(214, 84, 68)

static HWND g_hwnd, g_status, g_cpu_lbl, g_gpu_lbl, g_load_lbl;
static HWND g_cpu_temp, g_gpu_temp, g_load;
static HWND g_cpu_hint, g_gpu_hint, g_load_hint;
static HWND g_cpu_name, g_gpu_name, g_notes, g_log, g_run;
static HWND g_cpu_chart, g_gpu_chart, g_load_chart;
static HFONT g_font, g_font_big, g_font_small;
static HBRUSH g_bg, g_card;
static CRITICAL_SECTION g_lock;
static PDH_HQUERY g_query;
static PDH_HCOUNTER g_counter;
static int g_pdh_ready;
static int g_diagnosing;

static wchar_t g_cpu_name_s[220];
static wchar_t g_gpu_name_s[220];
static wchar_t g_cpu_src[48];
static wchar_t g_gpu_src[48];
static wchar_t g_note[400];
static double g_cpu_c = -1, g_gpu_c = -1, g_load_v = -1, g_gpu_load = -1;
static int g_cpu_tone, g_gpu_tone, g_load_tone;

static double g_cpu_hist[HIST], g_gpu_hist[HIST], g_load_hist[HIST];
static unsigned char g_cpu_ok[HIST], g_gpu_ok[HIST], g_load_ok[HIST];
static int g_hist_i, g_hist_n;

typedef struct {
  int id;
  const wchar_t *provider;
  int rank;
  const wchar_t *title;
  const wchar_t *what;
  const wchar_t *fix;
} Rule;

static const Rule g_rules[] = {
  {41, L"Kernel-Power", 100, L"Unexpected restart",
   L"Windows restarted without a clean shutdown. A BugcheckCode of zero means power loss, a hard freeze, the power button, or the board cutting power from heat. Any other code is a blue screen.",
   L"Read BugcheckCode in that event. Look at the minutes before it for disk and display errors. Note whether it happens at idle or only under load."},
  {6008, L"EventLog", 70, L"Previous shutdown was dirty",
   L"The log service only confirms the last shutdown was unexpected. Read it next to Kernel-Power 41.",
   L"Pair the time with Kernel-Power 41 and any bugcheck 1001."},
  {1074, L"User32", 10, L"A restart was requested",
   L"A person or a program asked Windows to restart. This is planned, not a crash.",
   L"Read the process name in the event before treating it as hardware."},
  {1001, L"Windows Error Reporting", 95, L"A bugcheck was recorded",
   L"Windows saved a blue-screen report. The stop code in the event is the useful part.",
   L"Copy the stop code. If a dump was written, the faulting driver is often named there."},
  {18, L"WHEA", 100, L"Fatal hardware error",
   L"The CPU, memory, or a PCIe device reported an error Windows could not correct.",
   L"Remove overclocks and test one memory stick at a time. Read which component the event names."},
  {19, L"WHEA", 40, L"Corrected hardware error",
   L"The hardware fixed an error itself. One event can be nothing. The same part repeating is a warning, usually memory.",
   L"If it names memory, test at the standard memory profile before replacing parts."},
  {47, L"WHEA", 40, L"Corrected hardware error",
   L"The hardware fixed an error itself. Repeats from the same part are the warning.",
   L"Count how often it repeats and whether the component name stays the same."},
  {153, L"disk", 80, L"A disk operation was retried",
   L"Windows had to retry a read or write. A pattern means the drive, cable, slot, or firmware is timing out.",
   L"Back up if these repeat. Check SMART, reseat the drive, and update its firmware."},
  {51, L"disk", 75, L"Paging I/O failed",
   L"An error happened while Windows used the page file. The system drive or the memory in front of it is the suspect.",
   L"Back up, check SMART, and test memory if the disk looks healthy."},
  {7, L"disk", 90, L"Bad block on a disk",
   L"The disk driver reported a bad block. That drive is no longer trustworthy.",
   L"Copy important files off now. Replace the drive if the errors continue."},
  {129, L"stor", 78, L"A storage device was reset",
   L"The controller reset a drive after it stopped answering. Often seen beside disk event 153.",
   L"Update the storage driver and drive firmware. On a SATA disk, try another cable and port."},
  {55, L"Ntfs", 92, L"The file system is corrupt",
   L"NTFS found a structure it cannot use. That follows a dirty shutdown, or the disk is failing.",
   L"Back up what you can still read before any repair. If it returns, replace the drive."},
  {4101, L"Display", 76, L"The display driver stopped responding",
   L"Windows reset the GPU driver after it hung. The screen may have flickered, then come back.",
   L"Remove any GPU overclock. Install a clean driver from AMD, NVIDIA, or Intel. Skip tuner packs."},
  {14, L"nvlddmkm", 76, L"The NVIDIA driver reported an error",
   L"If the text contains an Xid number, that code is more specific than the event number.",
   L"Reseat the GPU power cables. A split cable from a weak supply is a common cause under load."},
  {7031, L"Service Control Manager", 30, L"A service crashed",
   L"One named service stopped. That service is the problem, not the whole PC.",
   L"Read the service name. Update or remove that program, or set it to Manual if you do not need it."},
  {7034, L"Service Control Manager", 30, L"A service stopped",
   L"One named service ended unexpectedly.",
   L"Read the service name and update or remove that one program."},
  {7023, L"Service Control Manager", 30, L"A service failed to start",
   L"One named service could not start.",
   L"Read the service name. The rest of the PC is not the fault."},
  {1000, L"Application Error", 25, L"An application crashed",
   L"A program faulted. The useful part is the application name and the module name, not the number 1000.",
   L"Update that program. Overlays and injectors are a common cause."},
  {10016, L"DistributedCOM", 1, L"DCOM permission noise",
   L"A Windows component was denied a COM class. On a home PC this is almost never why the machine is hot or restarting.",
   L"Ignore it while you chase crashes, disks, and hardware errors. Do not rewrite DCOM security."},
  {37, L"Kernel-Processor-Power", 15, L"The processor is limited by firmware",
   L"The firmware is holding the CPU below full speed. That is a power limit, not a crash.",
   L"Check the power plan and, on a laptop, the charger."},
};

static int is_admin(void) {
  BOOL admin = FALSE;
  SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  PSID sid = NULL;
  if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) return 0;
  CheckTokenMembership(NULL, sid, &admin);
  FreeSid(sid);
  return admin ? 1 : 0;
}

static void relaunch_admin(void) {
  wchar_t path[MAX_PATH];
  SHELLEXECUTEINFOW info;
  GetModuleFileNameW(NULL, path, MAX_PATH);
  ZeroMemory(&info, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = path;
  info.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&info)) ExitProcess(0);
}

static void app_dir(wchar_t *out, size_t cap) {
  wchar_t base[MAX_PATH];
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) GetTempPathW(MAX_PATH, base);
  _snwprintf(out, cap, L"%s\\DiagnosticChicken", base);
  out[cap - 1] = 0;
  CreateDirectoryW(out, NULL);
}

static int contains_ci(const wchar_t *hay, const wchar_t *needle) {
  size_t nlen, i, j;
  if (!hay || !needle || !needle[0]) return 0;
  nlen = wcslen(needle);
  for (i = 0; hay[i]; i++) {
    for (j = 0; j < nlen; j++) {
      wchar_t a = hay[i + j], b = needle[j];
      if (!a) return 0;
      if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
      if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
      if (a != b) break;
    }
    if (j == nlen) return 1;
  }
  return 0;
}

static int weak_name(const wchar_t *name) {
  if (!name || !name[0]) return 1;
  return contains_ci(name, L"virtual") || contains_ci(name, L"meta") || contains_ci(name, L"basic display") || contains_ci(name, L"indirect") || contains_ci(name, L"iddcx") || contains_ci(name, L"parsec") || contains_ci(name, L"mirror");
}

static void set_text(HWND hwnd, const wchar_t *text) {
  if (hwnd) SetWindowTextW(hwnd, text ? text : L"");
}

static int tone_of_c(double c) {
  if (c < 0) return 0;
  if (c >= 90) return 3;
  if (c >= 80) return 2;
  return 1;
}

static int tone_of_load(double load) {
  if (load < 0) return 0;
  if (load >= 90) return 3;
  if (load >= 70) return 2;
  return 1;
}

static void format_f(wchar_t *out, size_t cap, double c) {
  if (c < 0) _snwprintf(out, cap, L"—");
  else _snwprintf(out, cap, L"%.0f°F", c * 9.0 / 5.0 + 32.0);
  out[cap - 1] = 0;
}

static void read_cpu_name(void) {
  HKEY key;
  DWORD n = sizeof(g_cpu_name_s), type = 0;
  g_cpu_name_s[0] = 0;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) != ERROR_SUCCESS) return;
  if (RegQueryValueExW(key, L"ProcessorNameString", NULL, &type, (BYTE *)g_cpu_name_s, &n) != ERROR_SUCCESS) g_cpu_name_s[0] = 0;
  RegCloseKey(key);
  while (g_cpu_name_s[0] == L' ') memmove(g_cpu_name_s, g_cpu_name_s + 1, (wcslen(g_cpu_name_s) + 1) * sizeof(wchar_t));
}

static void read_gpu_name_fallback(void) {
  DISPLAY_DEVICEW dd;
  DWORD i;
  wchar_t best[220];
  best[0] = 0;
  ZeroMemory(&dd, sizeof(dd));
  dd.cb = sizeof(dd);
  for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
    if (weak_name(dd.DeviceString)) continue;
    if (!best[0] || contains_ci(dd.DeviceString, L"radeon") || contains_ci(dd.DeviceString, L"nvidia") || contains_ci(dd.DeviceString, L"geforce") || contains_ci(dd.DeviceString, L"arc")) {
      wcsncpy(best, dd.DeviceString, 219);
      best[219] = 0;
      if (contains_ci(best, L"radeon") || contains_ci(best, L"geforce") || contains_ci(best, L"nvidia")) break;
    }
    dd.cb = sizeof(dd);
  }
  if (best[0]) {
    EnterCriticalSection(&g_lock);
    if (!g_gpu_name_s[0]) wcsncpy(g_gpu_name_s, best, 219);
    LeaveCriticalSection(&g_lock);
  }
}

static void pdh_open(void) {
  if (PdhOpenQueryW(NULL, 0, &g_query) != ERROR_SUCCESS) return;
  if (PdhAddEnglishCounterW(g_query, L"\\Processor(_Total)\\% Processor Time", 0, &g_counter) != ERROR_SUCCESS) {
    PdhCloseQuery(g_query);
    g_query = NULL;
    return;
  }
  PdhCollectQueryData(g_query);
  g_pdh_ready = 1;
}

static void pdh_sample(void) {
  PDH_FMT_COUNTERVALUE value;
  if (!g_pdh_ready) return;
  if (PdhCollectQueryData(g_query) != ERROR_SUCCESS) return;
  if (PdhGetFormattedCounterValue(g_counter, PDH_FMT_DOUBLE, NULL, &value) != ERROR_SUCCESS) return;
  if (value.CStatus != ERROR_SUCCESS) return;
  if (value.doubleValue < 0 || value.doubleValue > 100) return;
  EnterCriticalSection(&g_lock);
  g_load_v = value.doubleValue;
  LeaveCriticalSection(&g_lock);
}

/* ---- AMD / NVIDIA temperature, off the window thread ---- */

typedef int(__cdecl *adl_create_fn)(void *(__cdecl *)(int), int, void **);
typedef int(__cdecl *adl_count_fn)(void *, int *);
typedef int(__cdecl *adl_info_fn)(void *, void *, int);
typedef int(__cdecl *adl_pmlog_fn)(void *, int, void *);
typedef int(__cdecl *adl_od8_fn)(void *, int, int, int *);

static void * __cdecl adl_alloc(int n) { return malloc(n > 0 ? n : 1); }

typedef struct { int supported; int value; } AdlSensor;
typedef struct { int size; AdlSensor sensors[256]; } AdlPmLog;

static int sensor_c(const AdlPmLog *log, int id) {
  int v;
  if (id < 0 || id >= 256) return -1;
  if (!log->sensors[id].supported) return -1;
  v = log->sensors[id].value;
  if (v > 200) v /= 1000;
  if (v <= 0 || v > 125) return -1;
  return v;
}

static void publish_gpu(const wchar_t *name, double temp, double load, const wchar_t *source) {
  EnterCriticalSection(&g_lock);
  if (name && name[0]) {
    wcsncpy(g_gpu_name_s, name, 219);
    g_gpu_name_s[219] = 0;
  }
  if (temp >= 1 && temp <= 125) g_gpu_c = temp;
  if (load >= 0 && load <= 100) g_gpu_load = load;
  wcsncpy(g_gpu_src, source, 47);
  g_gpu_src[47] = 0;
  LeaveCriticalSection(&g_lock);
}

static int amd_open(HMODULE *dll_out, void **ctx_out, int *index_out, wchar_t *name_out) {
  HMODULE dll;
  adl_create_fn create;
  adl_count_fn count_fn;
  adl_info_fn info_fn;
  void *ctx = NULL;
  int count = 0, i, chosen = -1;
  unsigned char *buf;
  name_out[0] = 0;
  dll = LoadLibraryW(L"atiadlxx.dll");
  if (!dll) return 0;
  create = (adl_create_fn)GetProcAddress(dll, "ADL2_Main_Control_Create");
  count_fn = (adl_count_fn)GetProcAddress(dll, "ADL2_Adapter_NumberOfAdapters_Get");
  info_fn = (adl_info_fn)GetProcAddress(dll, "ADL2_Adapter_AdapterInfo_Get");
  if (!create || !count_fn || !info_fn) return 0;
  if (create(adl_alloc, 0, &ctx) < 0 || !ctx) return 0;
  if (count_fn(ctx, &count) < 0 || count <= 0) return 0;
  if (count > 16) count = 16;
  buf = (unsigned char *)calloc((size_t)count, 1568);
  if (!buf) return 0;
  if (info_fn(ctx, buf, count * 1568) < 0) { free(buf); return 0; }
  for (i = 0; i < count; i++) {
    unsigned char *raw = buf + i * 1568;
    int vendor = *(int *)(raw + 276);
    int index = *(int *)(raw + 4);
    int present = *(int *)(raw + 792);
    wchar_t name[220];
    if (vendor != 0x1002 || present == 0) continue;
    MultiByteToWideChar(CP_ACP, 0, (char *)(raw + 280), -1, name, 220);
    if (weak_name(name)) continue;
    chosen = index;
    wcsncpy(name_out, name[0] ? name : L"AMD Radeon", 219);
    name_out[219] = 0;
    if (contains_ci(name, L"radeon")) break;
  }
  free(buf);
  if (chosen < 0) return 0;
  *dll_out = dll;
  *ctx_out = ctx;
  *index_out = chosen;
  return 1;
}

static int amd_sample(void *ctx, int index, const wchar_t *name) {
  HMODULE dll = GetModuleHandleW(L"atiadlxx.dll");
  adl_pmlog_fn pmlog;
  adl_od8_fn od8;
  AdlPmLog log;
  int edge, gfx, hot, temp, load, n;
  if (!dll) return 0;
  pmlog = (adl_pmlog_fn)GetProcAddress(dll, "ADL2_New_QueryPMLogData_Get");
  od8 = (adl_od8_fn)GetProcAddress(dll, "ADL2_Overdrive8_Temperature_Get");
  if (!pmlog) return 0;
  ZeroMemory(&log, sizeof(log));
  log.size = (int)sizeof(log);
  temp = -1;
  load = -1;
  if (pmlog(ctx, index, &log) >= 0) {
    edge = sensor_c(&log, 8);
    gfx = sensor_c(&log, 28);
    hot = sensor_c(&log, 27);
    temp = edge > 0 ? edge : (gfx > 0 ? gfx : hot);
    if (log.sensors[19].supported) {
      int activity = log.sensors[19].value;
      if (activity >= 0 && activity <= 100) load = activity;
    }
  }
  if (temp <= 0 && od8 && od8(ctx, index, 1, &n) >= 0 && n > 0) {
    if (n > 200) n /= 1000;
    if (n > 0 && n <= 125) temp = n;
  }
  if (temp <= 0) return 0;
  publish_gpu(name, temp, load, L"AMD driver");
  return 1;
}

static int poll_nvidia_pipe(void) {
  wchar_t cmd[700];
  wchar_t exe[MAX_PATH];
  const wchar_t *root = _wgetenv(L"SystemRoot");
  SECURITY_ATTRIBUTES sa;
  HANDLE read_h = NULL, write_h = NULL;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  char raw[800];
  DWORD got = 0, chunk = 0;
  char *c1, *c2, *line;
  double temp, load;
  wchar_t name[220];
  if (!root) root = L"C:\\Windows";
  _snwprintf(exe, MAX_PATH, L"%s\\System32\\nvidia-smi.exe", root);
  if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return 0;
  _snwprintf(cmd, 700, L"\"%s\" --query-gpu=name,temperature.gpu,utilization.gpu --format=csv,noheader,nounits", exe);
  ZeroMemory(&sa, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  if (!CreatePipe(&read_h, &write_h, &sa, 0)) return 0;
  SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write_h;
  si.hStdError = write_h;
  ZeroMemory(&pi, sizeof(pi));
  if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    CloseHandle(read_h);
    CloseHandle(write_h);
    return 0;
  }
  CloseHandle(write_h);
  if (WaitForSingleObject(pi.hProcess, 2500) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  raw[0] = 0;
  while (got < sizeof(raw) - 1 && ReadFile(read_h, raw + got, (DWORD)(sizeof(raw) - 1 - got), &chunk, NULL) && chunk) got += chunk;
  raw[got] = 0;
  CloseHandle(read_h);
  line = raw;
  while (*line == ' ' || *line == '\r' || *line == '\n') line++;
  c1 = strchr(line, ',');
  if (!c1) return 0;
  *c1 = 0;
  c2 = strchr(c1 + 1, ',');
  if (!c2) return 0;
  *c2 = 0;
  temp = atof(c1 + 1);
  load = atof(c2 + 1);
  if (temp < 1 || temp > 125) return 0;
  MultiByteToWideChar(CP_ACP, 0, line, -1, name, 220);
  publish_gpu(name, temp, load, L"NVIDIA driver");
  return 1;
}

static DWORD WINAPI gpu_thread(LPVOID unused) {
  HMODULE dll = NULL;
  void *ctx = NULL;
  int index = -1, amd = 0;
  wchar_t name[220];
  (void)unused;
  name[0] = 0;
  amd = amd_open(&dll, &ctx, &index, name);
  if (amd) {
    int misses = 0;
    for (;;) {
      if (amd_sample(ctx, index, name)) misses = 0;
      else if (++misses == 3) {
        EnterCriticalSection(&g_lock);
        if (g_gpu_c < 0) {
          wcscpy(g_gpu_src, L"no sensor");
          if (!g_note[0]) wcscpy(g_note, L"The AMD driver is installed, but it did not return a GPU temperature.");
        }
        LeaveCriticalSection(&g_lock);
      }
      Sleep(2000);
    }
  }
  if (!poll_nvidia_pipe()) {
    EnterCriticalSection(&g_lock);
    if (g_gpu_c < 0) {
      wcscpy(g_gpu_src, L"no sensor");
      if (!g_note[0]) wcscpy(g_note, L"No GPU temperature from the graphics driver. The card name can still be right.");
    }
    LeaveCriticalSection(&g_lock);
    return 0;
  }
  for (;;) {
    poll_nvidia_pipe();
    Sleep(2000);
  }
}

/* ---- board temperature, its own process, killed if it stalls ---- */

static void write_temp_script(const wchar_t *dir) {
  wchar_t path[MAX_PATH];
  HANDLE file;
  DWORD wrote = 0;
  static const char body[] =
    "$ErrorActionPreference = 'SilentlyContinue'\r\n"
    "$best = $null\r\n"
    "Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature | ForEach-Object {\r\n"
    "  $c = [math]::Round(($_.CurrentTemperature / 10) - 273.15, 1)\r\n"
    "  if ($c -ge 15 -and $c -le 115) { if ($null -eq $best -or $c -gt $best) { $best = $c } }\r\n"
    "}\r\n"
    "if ($null -ne $best) { [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'acpi.txt'), ([string]$best)) }\r\n";
  _snwprintf(path, MAX_PATH, L"%s\\board-temp.ps1", dir);
  file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return;
  WriteFile(file, body, (DWORD)(sizeof(body) - 1), &wrote, NULL);
  CloseHandle(file);
}

static DWORD WINAPI cpu_temp_thread(LPVOID unused) {
  wchar_t dir[MAX_PATH], script[MAX_PATH], outp[MAX_PATH], cmd[1100];
  (void)unused;
  app_dir(dir, MAX_PATH);
  _snwprintf(script, MAX_PATH, L"%s\\board-temp.ps1", dir);
  _snwprintf(outp, MAX_PATH, L"%s\\acpi.txt", dir);
  write_temp_script(dir);
  for (;;) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    const wchar_t *root = _wgetenv(L"SystemRoot");
    if (!root) root = L"C:\\Windows";
    _snwprintf(cmd, 1100, L"\"%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"%s\"", root, script);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
      if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      {
        FILE *f = _wfopen(outp, L"r");
        if (f) {
          char line[64];
          double c;
          if (fgets(line, sizeof(line), f)) {
            c = atof(line);
            if (c >= 15 && c <= 115) {
              EnterCriticalSection(&g_lock);
              g_cpu_c = c;
              wcscpy(g_cpu_src, L"board sensor");
              LeaveCriticalSection(&g_lock);
            }
          }
          fclose(f);
        }
      }
    }
    Sleep(5000);
  }
}

/* ---- event log ---- */

static const Rule *find_rule(int id, const wchar_t *provider) {
  size_t i;
  const Rule *loose = NULL;
  for (i = 0; i < sizeof(g_rules) / sizeof(g_rules[0]); i++) {
    if (g_rules[i].id != id) continue;
    if (!g_rules[i].provider) return &g_rules[i];
    if (provider && contains_ci(provider, g_rules[i].provider)) return &g_rules[i];
    if (!loose) loose = &g_rules[i];
  }
  if (id == 41 || id == 1001 || id == 4101 || id == 10016) return loose;
  return NULL;
}

static int xml_event_id(const wchar_t *xml) {
  const wchar_t *p = wcsstr(xml, L"<EventID");
  if (!p) return 0;
  p = wcschr(p, L'>');
  if (!p) return 0;
  return _wtoi(p + 1);
}

static void xml_provider(const wchar_t *xml, wchar_t *out, size_t cap) {
  const wchar_t *p = wcsstr(xml, L"<Provider ");
  const wchar_t *q;
  size_t n;
  out[0] = 0;
  if (!p) return;
  p = wcsstr(p, L"Name=\"");
  if (!p) return;
  p += 6;
  q = wcschr(p, L'"');
  if (!q) return;
  n = (size_t)(q - p);
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n * sizeof(wchar_t));
  out[n] = 0;
}

static void scan_channel(const wchar_t *channel, int *counts, int *best_rank) {
  const size_t rules = sizeof(g_rules) / sizeof(g_rules[0]);
  EVT_HANDLE query;
  wchar_t xml[8192];
  DWORD used = 0, props = 0;
  int seen = 0;
  query = EvtQuery(NULL, channel, L"*[System[(Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= 604800000]]]", EvtQueryChannelPath | EvtQueryReverseDirection);
  if (!query) return;
  for (;;) {
    EVT_HANDLE ev = NULL;
    DWORD got = 0;
    if (seen++ > 250) break;
    if (!EvtNext(query, 1, &ev, 1500, 0, &got) || got == 0) break;
    used = 0;
    if (EvtRender(NULL, ev, EvtRenderEventXml, sizeof(xml), xml, &used, &props)) {
      int id = xml_event_id(xml);
      wchar_t provider[180];
      const Rule *rule;
      xml_provider(xml, provider, 180);
      rule = find_rule(id, provider);
      if (rule) {
        size_t idx = (size_t)(rule - g_rules);
        if (idx < rules) {
          counts[idx] += 1;
          if (rule->rank > *best_rank) *best_rank = rule->rank;
        }
      }
    }
    EvtClose(ev);
  }
  EvtClose(query);
}

static DWORD WINAPI diagnose_thread2(LPVOID param) {
  HWND hwnd = (HWND)param;
  const size_t rules = sizeof(g_rules) / sizeof(g_rules[0]);
  int *counts = (int *)calloc(rules, sizeof(int));
  int *used = (int *)calloc(rules, sizeof(int));
  int best = 0, noise = 0, guard;
  size_t i;
  wchar_t *report = (wchar_t *)calloc(14000, sizeof(wchar_t));
  wchar_t *fixes = (wchar_t *)calloc(8000, sizeof(wchar_t));
  if (!counts || !used || !report || !fixes) {
    free(counts); free(used); free(report); free(fixes);
    PostMessageW(hwnd, WM_APP + 1, 0, (LPARAM)_wcsdup(L"Could not read the log."));
    return 0;
  }
  scan_channel(L"System", counts, &best);
  scan_channel(L"Application", counts, &best);
  wcscpy(report, L"WHAT IS GOING ON\r\n\r\n");
  wcscpy(fixes, L"");
  for (i = 0; i < rules; i++) if (counts[i] > 0 && g_rules[i].rank <= 5) noise += counts[i];
  if (best <= 5) {
    wcscat(report, L"No serious errors in the System or Application logs for the last 7 days.");
    if (noise) wcscat(report, L" The rest is ordinary Windows noise and is not why the PC is hot or restarting.");
    wcscat(report, L"\r\n\r\nCOMMON SOLUTIONS\r\n\r\nNothing in the log needs a fix right now. If the PC misbehaves, watch the graphs while you do the thing that causes it.");
  } else {
    for (guard = 0; guard < 4; guard++) {
      int top = -1;
      size_t t;
      wchar_t line[1000];
      for (t = 0; t < rules; t++) {
        if (!counts[t] || used[t] || g_rules[t].rank <= 5) continue;
        if (top < 0 || g_rules[t].rank > g_rules[top].rank) top = (int)t;
      }
      if (top < 0) break;
      used[top] = 1;
      _snwprintf(line, 1000, L"%s — event %d, seen %d time%s\r\n%s\r\n\r\n", g_rules[top].title, g_rules[top].id, counts[top], counts[top] == 1 ? L"" : L"s", g_rules[top].what);
      line[999] = 0;
      if (wcslen(report) + wcslen(line) < 8000) wcscat(report, line);
      _snwprintf(line, 1000, L"• %s\r\n", g_rules[top].fix);
      line[999] = 0;
      if (wcslen(fixes) + wcslen(line) < 7000) wcscat(fixes, line);
    }
    wcscat(report, L"COMMON SOLUTIONS\r\n\r\n");
    wcscat(report, fixes);
  }
  free(counts);
  free(used);
  free(fixes);
  PostMessageW(hwnd, WM_APP + 1, 0, (LPARAM)report);
  return 0;
}

static void push_hist(void) {
  g_cpu_hist[g_hist_i] = g_cpu_c;
  g_gpu_hist[g_hist_i] = g_gpu_c;
  g_load_hist[g_hist_i] = g_load_v;
  g_cpu_ok[g_hist_i] = g_cpu_c >= 0;
  g_gpu_ok[g_hist_i] = g_gpu_c >= 0;
  g_load_ok[g_hist_i] = g_load_v >= 0;
  g_hist_i = (g_hist_i + 1) % HIST;
  if (g_hist_n < HIST) g_hist_n++;
}

static void paint_chart(HDC dc, RECT rc, int which) {
  double *series = which == 1 ? g_gpu_hist : which == 2 ? g_load_hist : g_cpu_hist;
  unsigned char *ok = which == 1 ? g_gpu_ok : which == 2 ? g_load_ok : g_cpu_ok;
  HBRUSH brush = CreateSolidBrush(COL_CARD);
  HPEN axis = CreatePen(PS_SOLID, 1, COL_LINE);
  HPEN line = CreatePen(PS_SOLID, 2, COL_AMBER);
  int i, count = 0, x0, y0, w, h;
  double lo, hi, peak = -1;
  wchar_t label[32];
  FillRect(dc, &rc, brush);
  DeleteObject(brush);
  x0 = rc.left + 8;
  y0 = rc.top + 8;
  w = rc.right - rc.left - 16;
  h = rc.bottom - rc.top - 22;
  if (w < 10 || h < 10) { DeleteObject(axis); DeleteObject(line); return; }
  SelectObject(dc, axis);
  MoveToEx(dc, x0, y0 + h, NULL);
  LineTo(dc, x0 + w, y0 + h);
  if (which == 2) { lo = 0; hi = 100; }
  else { lo = 20; hi = 100; }
  for (i = 0; i < g_hist_n; i++) {
    int idx = (g_hist_i - g_hist_n + i + HIST) % HIST;
    if (!ok[idx]) continue;
    if (series[idx] > peak) peak = series[idx];
    count++;
  }
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, COL_MUTED);
  SelectObject(dc, g_font_small);
  if (!count) {
    TextOutW(dc, x0, y0 + 8, L"waiting", 7);
    DeleteObject(axis);
    DeleteObject(line);
    return;
  }
  if (which == 2) _snwprintf(label, 32, L"peak %.0f%%", peak);
  else _snwprintf(label, 32, L"peak %.0f°F", peak * 9.0 / 5.0 + 32.0);
  TextOutW(dc, x0, rc.top + 2, label, (int)wcslen(label));
  SelectObject(dc, line);
  {
    int started = 0, n = 0;
    for (i = 0; i < g_hist_n; i++) {
      int idx = (g_hist_i - g_hist_n + i + HIST) % HIST;
      int x, y;
      double v, t;
      if (!ok[idx]) { started = 0; continue; }
      v = series[idx];
      if (which != 2 && v > hi) hi = v;
      t = (v - lo) / (hi - lo);
      if (t < 0) t = 0;
      if (t > 1) t = 1;
      x = x0 + (g_hist_n <= 1 ? 0 : (i * w) / (g_hist_n - 1));
      y = y0 + h - (int)(t * h);
      if (!started) { MoveToEx(dc, x, y, NULL); started = 1; }
      else LineTo(dc, x, y);
      n++;
    }
    (void)n;
  }
  DeleteObject(axis);
  DeleteObject(line);
}

static LRESULT CALLBACK chart_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    paint_chart(dc, rc, (int)(INT_PTR)GetPropW(hwnd, L"series"));
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND add_text(HWND parent, const wchar_t *text, HFONT font, DWORD style) {
  HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, NULL, NULL, NULL);
  SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
  return hwnd;
}

static void layout(HWND hwnd) {
  RECT rc;
  int width, col, gap = 18, x = 24, y, x2, x3;
  GetClientRect(hwnd, &rc);
  width = rc.right - rc.left;
  if (width < 640) width = 640;
  col = (width - 48 - gap * 2) / 3;
  x2 = x + col + gap;
  x3 = x2 + col + gap;
  MoveWindow(g_status, width - 140, 18, 110, 22, TRUE);
  MoveWindow(g_cpu_lbl, x, 78, col, 16, TRUE);
  MoveWindow(g_gpu_lbl, x2, 78, col, 16, TRUE);
  MoveWindow(g_load_lbl, x3, 78, col, 16, TRUE);
  MoveWindow(g_cpu_temp, x, 96, col, 42, TRUE);
  MoveWindow(g_gpu_temp, x2, 96, col, 42, TRUE);
  MoveWindow(g_load, x3, 96, col, 42, TRUE);
  MoveWindow(g_cpu_hint, x, 140, col, 18, TRUE);
  MoveWindow(g_gpu_hint, x2, 140, col, 18, TRUE);
  MoveWindow(g_load_hint, x3, 140, col, 18, TRUE);
  MoveWindow(g_cpu_chart, x, 162, col, 96, TRUE);
  MoveWindow(g_gpu_chart, x2, 162, col, 96, TRUE);
  MoveWindow(g_load_chart, x3, 162, col, 96, TRUE);
  MoveWindow(g_cpu_name, x, 262, col, 36, TRUE);
  MoveWindow(g_gpu_name, x2, 262, col, 36, TRUE);
  MoveWindow(g_notes, x, 306, width - 48, 32, TRUE);
  MoveWindow(g_run, x, 344, 180, 36, TRUE);
  y = 392;
  MoveWindow(g_log, x, y, width - 48, rc.bottom - y - 16, TRUE);
}

static void refresh(HWND hwnd) {
  wchar_t line[64], cpu_src[48], gpu_src[48], note[400], cpu_name[220], gpu_name[220];
  double cpu, gpu, load, gload;
  pdh_sample();
  EnterCriticalSection(&g_lock);
  cpu = g_cpu_c;
  gpu = g_gpu_c;
  load = g_load_v;
  gload = g_gpu_load;
  wcscpy(cpu_src, g_cpu_src);
  wcscpy(gpu_src, g_gpu_src);
  wcscpy(note, g_note);
  wcscpy(cpu_name, g_cpu_name_s);
  wcscpy(gpu_name, g_gpu_name_s);
  LeaveCriticalSection(&g_lock);
  format_f(line, 64, cpu);
  set_text(g_cpu_temp, line);
  format_f(line, 64, gpu);
  set_text(g_gpu_temp, line);
  if (load < 0) wcscpy(line, L"—");
  else _snwprintf(line, 64, L"%.0f%%", load);
  set_text(g_load, line);
  g_cpu_tone = tone_of_c(cpu);
  g_gpu_tone = tone_of_c(gpu);
  g_load_tone = tone_of_load(load);
  set_text(g_cpu_hint, cpu_src[0] ? cpu_src : L"reading");
  if (gpu_src[0] && gload >= 0) {
    wchar_t hint[80];
    _snwprintf(hint, 80, L"%s · %.0f%%", gpu_src, gload);
    set_text(g_gpu_hint, hint);
  } else set_text(g_gpu_hint, gpu_src[0] ? gpu_src : L"reading");
  set_text(g_load_hint, load < 0 ? L"reading" : L"this PC");
  set_text(g_cpu_name, cpu_name);
  set_text(g_gpu_name, gpu_name[0] ? gpu_name : L"Looking for the graphics card");
  set_text(g_notes, note);
  set_text(g_status, L"Live");
  push_hist();
  InvalidateRect(g_cpu_chart, NULL, FALSE);
  InvalidateRect(g_gpu_chart, NULL, FALSE);
  InvalidateRect(g_load_chart, NULL, FALSE);
  InvalidateRect(hwnd, NULL, FALSE);
}

static COLORREF tone_color(int tone) {
  if (tone == 3) return COL_HOT;
  if (tone == 2) return COL_WARM;
  if (tone == 1) return COL_OK;
  return COL_MUTED;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CREATE) return 0;
  if (msg == WM_SIZE) { layout(hwnd); return 0; }
  if (msg == WM_TIMER) { refresh(hwnd); return 0; }
  if (msg == WM_COMMAND && (HWND)lp == g_run && !g_diagnosing) {
    HANDLE thread;
    g_diagnosing = 1;
    EnableWindow(g_run, FALSE);
    set_text(g_log, L"Reading the System and Application logs…");
    thread = CreateThread(NULL, 0, diagnose_thread2, hwnd, 0, NULL);
    if (thread) CloseHandle(thread);
    else { g_diagnosing = 0; EnableWindow(g_run, TRUE); }
    return 0;
  }
  if (msg == WM_APP + 1) {
    wchar_t *text = (wchar_t *)lp;
    set_text(g_log, text ? text : L"");
    free(text);
    g_diagnosing = 0;
    EnableWindow(g_run, TRUE);
    return 0;
  }
  if (msg == WM_CTLCOLORSTATIC) {
    HDC dc = (HDC)wp;
    HWND child = (HWND)lp;
    SetBkColor(dc, COL_BG);
    SetTextColor(dc, COL_TEXT);
    if (child == g_status) SetTextColor(dc, COL_AMBER);
    else if (child == g_cpu_temp) SetTextColor(dc, tone_color(g_cpu_tone));
    else if (child == g_gpu_temp) SetTextColor(dc, tone_color(g_gpu_tone));
    else if (child == g_load) SetTextColor(dc, tone_color(g_load_tone));
    else if (child == g_cpu_lbl || child == g_gpu_lbl || child == g_load_lbl) SetTextColor(dc, COL_AMBER);
    else if (child == g_cpu_hint || child == g_gpu_hint || child == g_load_hint || child == g_notes || child == g_cpu_name || child == g_gpu_name) SetTextColor(dc, COL_MUTED);
    return (LRESULT)g_bg;
  }
  if (msg == WM_CTLCOLOREDIT) {
    HDC dc = (HDC)wp;
    SetBkColor(dc, COL_BG);
    SetTextColor(dc, COL_TEXT);
    return (LRESULT)g_bg;
  }
  if (msg == WM_ERASEBKGND) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect((HDC)wp, &rc, g_bg);
    return 1;
  }
  if (msg == WM_DESTROY) { KillTimer(hwnd, 1); PostQuitMessage(0); return 0; }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  WNDCLASSW wc, chart;
  MSG msg;
  HWND host;
  HANDLE mutex;
  (void)prev; (void)cmd;
  if (!is_admin()) { relaunch_admin(); return 0; }
  mutex = CreateMutexW(NULL, FALSE, L"Local\\DiagnosticChickenSingle");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  InitializeCriticalSection(&g_lock);
  wcscpy(g_cpu_src, L"reading");
  wcscpy(g_gpu_src, L"reading");
  read_cpu_name();
  read_gpu_name_fallback();
  pdh_open();
  CreateThread(NULL, 0, gpu_thread, NULL, 0, NULL);
  CreateThread(NULL, 0, cpu_temp_thread, NULL, 0, NULL);
  g_bg = CreateSolidBrush(COL_BG);
  g_card = CreateSolidBrush(COL_CARD);
  g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  g_font_big = CreateFontW(-34, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  g_font_small = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  ZeroMemory(&chart, sizeof(chart));
  chart.lpfnWndProc = chart_proc;
  chart.hInstance = inst;
  chart.lpszClassName = L"ChickenChart";
  chart.hbrBackground = g_card;
  RegisterClassW(&chart);
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.lpszClassName = L"DiagnosticChicken";
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = g_bg;
  RegisterClassW(&wc);
  g_hwnd = CreateWindowExW(0, L"DiagnosticChicken", L"Diagnostic Chicken", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 720, NULL, NULL, inst, NULL);
  host = add_text(g_hwnd, L"DIAGNOSTIC CHICKEN", g_font_small, SS_LEFT);
  set_text(host, L"DIAGNOSTIC CHICKEN");
  SendMessageW(host, WM_SETFONT, (WPARAM)g_font, TRUE);
  {
    wchar_t title[260];
    wchar_t computer[128];
    DWORD n = 128;
    computer[0] = 0;
    GetComputerNameW(computer, &n);
    _snwprintf(title, 260, L"%s", computer);
    host = add_text(g_hwnd, title, g_font_big, SS_LEFT | SS_NOPREFIX);
    MoveWindow(add_text(g_hwnd, L"DIAGNOSTIC CHICKEN", g_font_small, SS_LEFT), 24, 16, 400, 16, TRUE);
    /* host handle was overwritten. Place the computer name we just created — it is `host`. */
    MoveWindow(host, 24, 34, 700, 36, TRUE);
  }
  g_status = add_text(g_hwnd, L"Live", g_font, SS_RIGHT);
  g_cpu_lbl = add_text(g_hwnd, L"CPU", g_font_small, SS_LEFT);
  g_gpu_lbl = add_text(g_hwnd, L"GPU", g_font_small, SS_LEFT);
  g_load_lbl = add_text(g_hwnd, L"CPU LOAD", g_font_small, SS_LEFT);
  g_cpu_temp = add_text(g_hwnd, L"—", g_font_big, SS_LEFT);
  g_gpu_temp = add_text(g_hwnd, L"—", g_font_big, SS_LEFT);
  g_load = add_text(g_hwnd, L"—", g_font_big, SS_LEFT);
  g_cpu_hint = add_text(g_hwnd, L"reading", g_font_small, SS_LEFT);
  g_gpu_hint = add_text(g_hwnd, L"reading", g_font_small, SS_LEFT);
  g_load_hint = add_text(g_hwnd, L"reading", g_font_small, SS_LEFT);
  g_cpu_name = add_text(g_hwnd, g_cpu_name_s, g_font_small, SS_LEFT | SS_NOPREFIX);
  g_gpu_name = add_text(g_hwnd, L"", g_font_small, SS_LEFT | SS_NOPREFIX);
  g_notes = add_text(g_hwnd, L"", g_font_small, SS_LEFT);
  g_cpu_chart = CreateWindowW(L"ChickenChart", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_hwnd, NULL, inst, NULL);
  g_gpu_chart = CreateWindowW(L"ChickenChart", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_hwnd, NULL, inst, NULL);
  g_load_chart = CreateWindowW(L"ChickenChart", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_hwnd, NULL, inst, NULL);
  SetPropW(g_cpu_chart, L"series", (HANDLE)(INT_PTR)0);
  SetPropW(g_gpu_chart, L"series", (HANDLE)(INT_PTR)1);
  SetPropW(g_load_chart, L"series", (HANDLE)(INT_PTR)2);
  g_run = CreateWindowW(L"BUTTON", L"Run diagnostic", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_hwnd, NULL, inst, NULL);
  SendMessageW(g_run, WM_SETFONT, (WPARAM)g_font, TRUE);
  g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
    L"This window is Diagnostic Chicken. Close it and it stops. It does not start with Windows.\r\n\r\nCPU load is read in this program. GPU temperature comes from the graphics driver already on the PC. The CPU temperature is the board sensor and shows up a few seconds after the window opens.\r\n\r\nRun diagnostic reads the System and Application logs, then says what is going on and the usual fixes.",
    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 0, 0, 10, 10, g_hwnd, NULL, inst, NULL);
  SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);
  layout(g_hwnd);
  ShowWindow(g_hwnd, show);
  SetTimer(g_hwnd, 1, 1000, NULL);
  refresh(g_hwnd);
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (mutex) CloseHandle(mutex);
  return 0;
}
