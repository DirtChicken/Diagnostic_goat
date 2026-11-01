#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps1.h"
#include "html.h"
#include "books.h"
#include "sensors.h"

#define COL_BG RGB(18, 20, 16)
#define COL_FG RGB(232, 230, 220)
#define COL_MUTED RGB(167, 171, 159)
#define COL_ACCENT RGB(224, 161, 6)
#define COL_ACCENT_FG RGB(28, 21, 3)
#define COL_OK RGB(142, 174, 114)
#define COL_BAD RGB(212, 101, 74)
#define ID_RUN 1001
#define ID_TIMER 1
#define HIST 180

static HANDLE g_child = NULL;
static HANDLE g_job = NULL;
static HWND g_hwnd, g_kicker, g_host, g_status;
static HWND g_cpu_lbl, g_gpu_lbl, g_load_lbl;
static HWND g_cpu_temp, g_gpu_temp, g_load;
static HWND g_cpu_hint, g_gpu_hint, g_load_hint;
static HWND g_cpu_name, g_gpu_name, g_notes, g_log, g_run;
static HWND g_cpu_chart, g_gpu_chart;
static HFONT g_font, g_font_big, g_font_small, g_font_mono;
static HBRUSH g_bg;
static int g_busy = 0;
static int g_stay = 0;
static int g_cpu_tone = 0;
static int g_gpu_tone = 0;
static int g_load_tone = 0;
static double g_cpu_series[HIST];
static double g_gpu_series[HIST];
static unsigned char g_cpu_ok[HIST];
static unsigned char g_gpu_ok[HIST];
static int g_series_n = 0;
static int g_series_at = 0;

static int weak_gpu(const char *name);
static void push_series(int cpu_ok, double cpu, int gpu_ok, double gpu);

typedef struct {
  char title[96];
  char source[96];
  char severity[16];
  char meaning[640];
  int ids[8];
  int id_count;
  char causes[4][240];
  int cause_count;
  char checks[4][240];
  int check_count;
} Book;

static Book g_books[24];
static int g_book_count = 0;

static void fail(const char *message) {
  MessageBoxA(NULL, message, "Diagnostic Chicken", MB_OK | MB_ICONERROR);
}

static int write_file(const char *path, const unsigned char *data, size_t len) {
  FILE *file = fopen(path, "wb");
  if (!file) return 0;
  if (fwrite(data, 1, len, file) != len) {
    fclose(file);
    return 0;
  }
  fclose(file);
  return 1;
}

static void powershell_path(char *out, size_t cap) {
  const char *root = getenv("SystemRoot");
  if (!root || !root[0]) root = "C:\\Windows";
  snprintf(out, cap, "%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", root);
}

static void ps_quote(const char *in, char *out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 2 < cap; i++) {
    if (in[i] == '\'') out[j++] = '\'';
    out[j++] = in[i];
  }
  out[j] = 0;
}

static int is_admin(void) {
  BOOL member = FALSE;
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID sid = NULL;
  if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) {
    return 0;
  }
  CheckTokenMembership(NULL, sid, &member);
  FreeSid(sid);
  return member ? 1 : 0;
}

static int relaunch_elevated(const char *exe) {
  SHELLEXECUTEINFOA info;
  ZeroMemory(&info, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = "runas";
  info.lpFile = exe;
  info.lpParameters = "--stay";
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExA(&info)) return 0;
  if (info.hProcess) CloseHandle(info.hProcess);
  return 1;
}

static int port_open(void) {
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) return 0;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(47321);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  int ok = connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  closesocket(sock);
  return ok;
}

static void pin_start_menu(const char *exe, const char *dir) {
  char script[MAX_PATH];
  snprintf(script, MAX_PATH, "%s\\pin.ps1", dir);
  FILE *file = fopen(script, "wb");
  if (!file) return;
  char qexe[MAX_PATH * 2];
  char qdir[MAX_PATH * 2];
  ps_quote(exe, qexe, sizeof(qexe));
  ps_quote(dir, qdir, sizeof(qdir));
  fprintf(file,
          "$shell = New-Object -ComObject WScript.Shell\r\n"
          "$lnk = Join-Path $env:APPDATA 'Microsoft\\Windows\\Start Menu\\Programs\\Diagnostic Chicken.lnk'\r\n"
          "$shortcut = $shell.CreateShortcut($lnk)\r\n"
          "$shortcut.TargetPath = '%s'\r\n"
          "$shortcut.WorkingDirectory = '%s'\r\n"
          "$shortcut.WindowStyle = 1\r\n"
          "$shortcut.Description = 'Diagnostic Chicken. Runs only when opened.'\r\n"
          "$shortcut.Save()\r\n",
          qexe, qdir);
  fclose(file);

  char ps[MAX_PATH];
  powershell_path(ps, sizeof(ps));
  char cmd[2048];
  snprintf(cmd, sizeof(cmd), "\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps, script);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));
  if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return;
  WaitForSingleObject(pi.hProcess, 8000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}

static int install_and_start(void) {
  const char *local = getenv("LOCALAPPDATA");
  if (!local || !local[0]) {
    fail("Could not find the local app folder.");
    return 0;
  }

  char dir[MAX_PATH];
  snprintf(dir, MAX_PATH, "%s\\Diagnostic Chicken", local);
  CreateDirectoryA(dir, NULL);

  char running[MAX_PATH];
  GetModuleFileNameA(NULL, running, MAX_PATH);
  char installed[MAX_PATH];
  snprintf(installed, MAX_PATH, "%s\\Diagnostic Chicken.exe", dir);
  if (_stricmp(running, installed) != 0) CopyFileA(running, installed, FALSE);

  char ps1[MAX_PATH], html[MAX_PATH], books_path[MAX_PATH], zip_path[MAX_PATH];
  snprintf(ps1, MAX_PATH, "%s\\sentinel-agent.ps1", dir);
  snprintf(html, MAX_PATH, "%s\\desk.html", dir);
  snprintf(books_path, MAX_PATH, "%s\\playbooks.json", dir);
  snprintf(zip_path, MAX_PATH, "%s\\sensors.zip", dir);
  if (!write_file(ps1, ps1_bytes, ps1_bytes_len) || !write_file(html, html_bytes, html_bytes_len) ||
      !write_file(books_path, books_bytes, books_bytes_len) || !write_file(zip_path, sensors_bytes, sensors_bytes_len)) {
    fail("Could not write Diagnostic Chicken's files.");
    return 0;
  }

  pin_start_menu(installed, dir);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fail("Could not start networking.");
    return 0;
  }
  if (port_open()) {
    HWND existing = FindWindowW(L"DiagnosticChicken", NULL);
    if (existing) {
      ShowWindow(existing, SW_RESTORE);
      SetForegroundWindow(existing);
    } else {
      fail("Diagnostic Chicken is already running. Close that window first.");
    }
    return 0;
  }

  if (!g_stay && !is_admin() && relaunch_elevated(installed)) return 0;

  char ps[MAX_PATH];
  powershell_path(ps, sizeof(ps));
  char cmd[2048];
  snprintf(cmd, sizeof(cmd), "\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps, ps1);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));
  if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
    fail("Could not start Windows PowerShell.");
    return 0;
  }

  g_child = pi.hProcess;
  g_job = CreateJobObjectA(NULL, NULL);
  if (g_job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    if (!AssignProcessToJobObject(g_job, pi.hProcess)) {
      CloseHandle(g_job);
      g_job = NULL;
    }
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  for (int i = 0; i < 50; i++) {
    if (port_open()) return 1;
    Sleep(200);
  }
  TerminateProcess(g_child, 1);
  fail("Diagnostic Chicken did not start. Close any other Diagnostic Chicken window and try again.");
  return 0;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}

static const char *span_value(const char *p, const char **out_end) {
  p = skip_ws(p);
  if (*p == '"') {
    const char *q = p + 1;
    while (*q) {
      if (*q == '\\' && q[1]) {
        q += 2;
        continue;
      }
      if (*q == '"') {
        *out_end = q + 1;
        return p;
      }
      q++;
    }
    *out_end = q;
    return p;
  }
  if (*p == '{' || *p == '[') {
    int brace = 0;
    int bracket = 0;
    int in_str = 0;
    for (const char *q = p; *q; q++) {
      if (in_str) {
        if (*q == '\\' && q[1]) {
          q++;
          continue;
        }
        if (*q == '"') in_str = 0;
        continue;
      }
      if (*q == '"') {
        in_str = 1;
        continue;
      }
      if (*q == '{') brace++;
      else if (*q == '}') brace--;
      else if (*q == '[') bracket++;
      else if (*q == ']') bracket--;
      if (brace == 0 && bracket == 0) {
        *out_end = q + 1;
        return p;
      }
    }
  }
  const char *q = p;
  while (*q && *q != ',' && *q != '}' && *q != ']' && *q != ' ' && *q != '\n' && *q != '\r') q++;
  *out_end = q;
  return p;
}

static const char *find_key(const char *s, const char *e, const char *key) {
  size_t kn = strlen(key);
  int brace = 0;
  int bracket = 0;
  int in_str = 0;
  for (const char *p = s; p < e; p++) {
    if (in_str) {
      if (*p == '\\' && p + 1 < e) {
        p++;
        continue;
      }
      if (*p == '"') in_str = 0;
      continue;
    }
    if (*p == '"') {
      if (brace == 1 && bracket == 0 && (size_t)(e - (p + 1)) > kn && memcmp(p + 1, key, kn) == 0 && p[1 + kn] == '"') {
        const char *q = skip_ws(p + kn + 2);
        if (*q == ':') return skip_ws(q + 1);
      }
      in_str = 1;
      continue;
    }
    if (*p == '{') brace++;
    else if (*p == '}') brace--;
    else if (*p == '[') bracket++;
    else if (*p == ']') bracket--;
  }
  return NULL;
}

static int json_string(const char *p, char *out, size_t cap) {
  p = skip_ws(p);
  if (*p != '"') {
    if (cap) out[0] = 0;
    return 0;
  }
  p++;
  size_t j = 0;
  while (*p && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    if (c == '\\' && *p) {
      char e = *p++;
      if (e == 'n') c = '\n';
      else if (e == 'r') c = '\r';
      else if (e == 't') c = '\t';
      else if (e == 'u') {
        for (int i = 0; i < 4 && *p; i++) p++;
        c = '?';
      } else c = (unsigned char)e;
    }
    if (c == '\r') continue;
    if (j + 1 < cap) out[j++] = (char)c;
  }
  if (cap) out[j] = 0;
  return 1;
}

static int json_num(const char *p, double *out) {
  p = skip_ws(p);
  if (*p == 'n' || *p == '"') return 0;
  char *stop = NULL;
  double value = strtod(p, &stop);
  if (stop == p) return 0;
  *out = value;
  return 1;
}

static void copy_string_array(const char *value, char out[][240], int *count, int max_count) {
  *count = 0;
  if (!value || *skip_ws(value) != '[') return;
  const char *end = NULL;
  span_value(value, &end);
  const char *p = skip_ws(value) + 1;
  while (p < end && *count < max_count) {
    p = skip_ws(p);
    if (*p == ']') break;
    if (*p == '"') {
      json_string(p, out[*count], 240);
      (*count)++;
      const char *ve = NULL;
      span_value(p, &ve);
      p = ve;
    } else break;
    p = skip_ws(p);
    if (*p == ',') p++;
  }
}

static void load_books(void) {
  char *json = (char *)malloc(books_bytes_len + 1);
  if (!json) return;
  memcpy(json, books_bytes, books_bytes_len);
  json[books_bytes_len] = 0;
  const char *p = skip_ws(json);
  if (*p == '[') p++;
  while (*p && g_book_count < 24) {
    p = skip_ws(p);
    if (*p == ']') break;
    if (*p != '{') break;
    const char *end = NULL;
    span_value(p, &end);
    Book *book = &g_books[g_book_count];
    memset(book, 0, sizeof(*book));
    const char *title = find_key(p, end, "title");
    const char *source = find_key(p, end, "source");
    const char *severity = find_key(p, end, "severity");
    const char *meaning = find_key(p, end, "meaning");
    if (title) json_string(title, book->title, sizeof(book->title));
    if (source) json_string(source, book->source, sizeof(book->source));
    if (severity) json_string(severity, book->severity, sizeof(book->severity));
    if (meaning) json_string(meaning, book->meaning, sizeof(book->meaning));
    const char *ids = find_key(p, end, "ids");
    if (ids && *skip_ws(ids) == '[') {
      const char *ip = skip_ws(ids) + 1;
      while (book->id_count < 8) {
        ip = skip_ws(ip);
        if (*ip == ']') break;
        char *stop = NULL;
        long value = strtol(ip, &stop, 10);
        if (stop == ip) break;
        book->ids[book->id_count++] = (int)value;
        ip = skip_ws(stop);
        if (*ip == ',') ip++;
      }
    }
    copy_string_array(find_key(p, end, "causes"), book->causes, &book->cause_count, 4);
    copy_string_array(find_key(p, end, "checks"), book->checks, &book->check_count, 4);
    if (book->title[0] && book->id_count) g_book_count++;
    p = end;
    p = skip_ws(p);
    if (*p == ',') p++;
  }
  free(json);
}

static int http_get(const char *path, char **body, DWORD timeout_ms) {
  *body = NULL;
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) return 0;
  DWORD timeout = timeout_ms ? timeout_ms : 60000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(47321);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(sock);
    return 0;
  }
  char req[256];
  snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
  send(sock, req, (int)strlen(req), 0);
  size_t cap = 65536;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    closesocket(sock);
    return 0;
  }
  size_t n = 0;
  for (;;) {
    if (n + 4096 + 1 > cap) {
      if (cap > 1500000) break;
      cap *= 2;
      char *next = (char *)realloc(buf, cap);
      if (!next) break;
      buf = next;
    }
    int got = recv(sock, buf + n, 4096, 0);
    if (got <= 0) break;
    n += (size_t)got;
  }
  closesocket(sock);
  buf[n] = 0;
  if (strncmp(buf, "HTTP/1.1 200", 12) != 0 && strncmp(buf, "HTTP/1.0 200", 12) != 0) {
    free(buf);
    return 0;
  }
  char *sep = strstr(buf, "\r\n\r\n");
  if (!sep) {
    free(buf);
    return 0;
  }
  char *payload = sep + 4;
  size_t len = n - (size_t)(payload - buf);
  memmove(buf, payload, len);
  buf[len] = 0;
  *body = buf;
  return 1;
}

static void set_utf8(HWND hwnd, const char *text) {
  if (!text) text = "";
  int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (count <= 0) {
    SetWindowTextW(hwnd, L"");
    return;
  }
  wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
  if (!wide) return;
  MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count);
  SetWindowTextW(hwnd, wide);
  free(wide);
}

static int tone_of(int present, double value) {
  if (!present) return 0;
  if (value >= 90) return 3;
  if (value >= 80) return 2;
  return 1;
}

static const char *source_hint(const char *source) {
  if (!source || !source[0]) return "";
  if (strcmp(source, "nvidia-smi") == 0) return "nvidia-smi";
  if (strcmp(source, "hwinfo") == 0) return "HWiNFO";
  if (strcmp(source, "librehardwaremonitor") == 0) return "built in";
  if (strcmp(source, "acpi-thermal-zone") == 0) return "ACPI zone, not the CPU die";
  if (strcmp(source, "unavailable") == 0) return "no sensor";
  return "live";
}

static void format_temp(char *out, size_t cap, int present, double celsius) {
  if (!present) snprintf(out, cap, "—");
  else snprintf(out, cap, "%.0f\xC2\xB0" "F", celsius * 9.0 / 5.0 + 32.0);
}

static void refresh_live(void) {
  char *body = NULL;
  if (!http_get("/live", &body, 8000)) {
    set_utf8(g_status, "Connecting");
    free(body);
    return;
  }
  const char *end = body + strlen(body);
  const char *cpu_at = find_key(body, end, "cpu");
  const char *cpu_end = end;
  if (cpu_at) span_value(cpu_at, &cpu_end);
  char host[128] = "THIS PC";
  const char *host_at = find_key(body, end, "host");
  if (host_at) json_string(host_at, host, sizeof(host));
  set_utf8(g_host, host);

  double temp = 0;
  double load = 0;
  const char *temp_key = cpu_at ? find_key(cpu_at, cpu_end, "tempC") : NULL;
  const char *load_key = cpu_at ? find_key(cpu_at, cpu_end, "loadPercent") : NULL;
  int has_temp = temp_key && json_num(temp_key, &temp);
  int has_load = load_key && json_num(load_key, &load);
  char line[64];
  format_temp(line, sizeof(line), has_temp, temp);
  set_utf8(g_cpu_temp, line);
  g_cpu_tone = tone_of(has_temp, temp);
  if (has_load) snprintf(line, sizeof(line), "%.0f%%", load);
  else snprintf(line, sizeof(line), "—");
  set_utf8(g_load, line);
  g_load_tone = has_load ? 1 : 0;

  char source[64] = "";
  const char *source_at = cpu_at ? find_key(cpu_at, cpu_end, "tempSource") : NULL;
  if (source_at) json_string(source_at, source, sizeof(source));
  set_utf8(g_cpu_hint, source_hint(source));

  char cpu_bits[320] = "";
  char name[180] = "";
  const char *name_at = cpu_at ? find_key(cpu_at, cpu_end, "name") : NULL;
  if (name_at) json_string(name_at, name, sizeof(name));
  if (name[0]) snprintf(cpu_bits, sizeof(cpu_bits), "%s", name);
  double cores = 0;
  double mhz = 0;
  const char *cores_at = cpu_at ? find_key(cpu_at, cpu_end, "cores") : NULL;
  const char *mhz_at = cpu_at ? find_key(cpu_at, cpu_end, "maxMhz") : NULL;
  if (cores_at && json_num(cores_at, &cores) && cores > 0) {
    char extra[32];
    snprintf(extra, sizeof(extra), "%s%.0f cores", cpu_bits[0] ? " · " : "", cores);
    strncat(cpu_bits, extra, sizeof(cpu_bits) - strlen(cpu_bits) - 1);
  }
  if (mhz_at && json_num(mhz_at, &mhz) && mhz > 0) {
    char extra[32];
    snprintf(extra, sizeof(extra), "%s%.0f MHz", cpu_bits[0] ? " · " : "", mhz);
    strncat(cpu_bits, extra, sizeof(cpu_bits) - strlen(cpu_bits) - 1);
  }
  set_utf8(g_cpu_name, cpu_bits);

  const char *gpus = find_key(body, end, "gpus");
  const char *best_s = NULL;
  const char *best_e = NULL;
  double best_temp = -1e9;
  int best_has = 0;
  int best_weak = 1;
  if (gpus && *skip_ws(gpus) == '[') {
    const char *arr_end = NULL;
    span_value(gpus, &arr_end);
    const char *p = skip_ws(gpus) + 1;
    while (p < arr_end) {
      p = skip_ws(p);
      if (*p == ']') break;
      if (*p != '{') break;
      const char *obj_end = NULL;
      span_value(p, &obj_end);
      double card_temp = 0;
      const char *card_temp_at = find_key(p, obj_end, "tempC");
      int card_has = card_temp_at && json_num(card_temp_at, &card_temp);
      char card_name[160] = "";
      const char *card_name_at = find_key(p, obj_end, "name");
      if (card_name_at) json_string(card_name_at, card_name, sizeof(card_name));
      int card_weak = weak_gpu(card_name);
      int replace = 0;
      if (!best_s) replace = 1;
      else if (best_weak && !card_weak) replace = 1;
      else if (!card_weak && !best_weak && card_has && (!best_has || card_temp >= best_temp)) replace = 1;
      else if (!card_weak && card_has && !best_has) replace = 1;
      if (replace) {
        best_s = p;
        best_e = obj_end;
        best_has = card_has;
        best_weak = card_weak;
        if (card_has) best_temp = card_temp;
      }
      p = obj_end;
      p = skip_ws(p);
      if (*p == ',') p++;
    }
  }
  if (best_s) {
    format_temp(line, sizeof(line), best_has, best_temp);
    set_utf8(g_gpu_temp, line);
    g_gpu_tone = tone_of(best_has, best_temp);
    char gpu_source[64] = "";
    const char *gs = find_key(best_s, best_e, "tempSource");
    if (gs) json_string(gs, gpu_source, sizeof(gpu_source));
    double gpu_load = 0;
    const char *gl = find_key(best_s, best_e, "loadPercent");
    char hint[80];
    if (gl && json_num(gl, &gpu_load)) snprintf(hint, sizeof(hint), "%s · %.0f%%", source_hint(gpu_source), gpu_load);
    else snprintf(hint, sizeof(hint), "%s", source_hint(gpu_source));
    set_utf8(g_gpu_hint, hint);
    char gpu_name[220] = "";
    char gpu_n[160] = "";
    char driver[64] = "";
    const char *gn = find_key(best_s, best_e, "name");
    const char *gd = find_key(best_s, best_e, "driver");
    if (gn) json_string(gn, gpu_n, sizeof(gpu_n));
    if (gd) json_string(gd, driver, sizeof(driver));
    if (gpu_n[0]) snprintf(gpu_name, sizeof(gpu_name), "%s", gpu_n);
    if (driver[0]) {
      char extra[80];
      snprintf(extra, sizeof(extra), "%sdriver %s", gpu_name[0] ? " · " : "", driver);
      strncat(gpu_name, extra, sizeof(gpu_name) - strlen(gpu_name) - 1);
    }
    set_utf8(g_gpu_name, gpu_name);
  }

  char notes[1200] = "";
  const char *note_at = find_key(body, end, "notes");
  if (note_at && *skip_ws(note_at) == '[') {
    const char *note_end = NULL;
    span_value(note_at, &note_end);
    const char *p = skip_ws(note_at) + 1;
    while (p < note_end) {
      p = skip_ws(p);
      if (*p == ']') break;
      if (*p == '"') {
        char note[300];
        json_string(p, note, sizeof(note));
        if (note[0]) {
          if (notes[0]) strncat(notes, "\n", sizeof(notes) - strlen(notes) - 1);
          strncat(notes, note, sizeof(notes) - strlen(notes) - 1);
        }
        const char *ve = NULL;
        span_value(p, &ve);
        p = ve;
      } else break;
      p = skip_ws(p);
      if (*p == ',') p++;
    }
  }
  set_utf8(g_notes, notes);
  set_utf8(g_status, "Live");
  push_series(has_temp, temp, best_has, best_has ? best_temp : 0);
  if (g_cpu_chart) InvalidateRect(g_cpu_chart, NULL, FALSE);
  if (g_gpu_chart) InvalidateRect(g_gpu_chart, NULL, FALSE);
  free(body);
  InvalidateRect(g_hwnd, NULL, TRUE);
}

typedef struct {
  char *data;
  size_t n;
  size_t cap;
} Buf;

static void buf_add(Buf *buf, const char *text) {
  size_t len = strlen(text);
  if (buf->n + len + 1 > buf->cap) {
    size_t cap = buf->cap ? buf->cap * 2 : 4096;
    while (cap < buf->n + len + 1) cap *= 2;
    char *next = (char *)realloc(buf->data, cap);
    if (!next) return;
    buf->data = next;
    buf->cap = cap;
  }
  memcpy(buf->data + buf->n, text, len);
  buf->n += len;
  buf->data[buf->n] = 0;
}

static int contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n || !hay) return 0;
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (needle[i] && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
    if (!needle[i]) return 1;
  }
  return 0;
}

static int weak_gpu(const char *name) {
  if (!name || !name[0]) return 1;
  if (contains_ci(name, "virtual")) return 1;
  if (contains_ci(name, "meta")) return 1;
  if (contains_ci(name, "indirect")) return 1;
  if (contains_ci(name, "remote display")) return 1;
  if (contains_ci(name, "basic display")) return 1;
  if (contains_ci(name, "basic render")) return 1;
  if (contains_ci(name, "oculus")) return 1;
  if (contains_ci(name, "quest")) return 1;
  return 0;
}

static void push_series(int cpu_ok, double cpu, int gpu_ok, double gpu) {
  g_cpu_series[g_series_at] = cpu;
  g_gpu_series[g_series_at] = gpu;
  g_cpu_ok[g_series_at] = (unsigned char)(cpu_ok ? 1 : 0);
  g_gpu_ok[g_series_at] = (unsigned char)(gpu_ok ? 1 : 0);
  g_series_at = (g_series_at + 1) % HIST;
  if (g_series_n < HIST) g_series_n++;
}

static void paint_chart(HDC dc, RECT rc, int which) {
  HBRUSH panel = CreateSolidBrush(RGB(28, 31, 24));
  FillRect(dc, &rc, panel);
  DeleteObject(panel);
  double *series = which ? g_gpu_series : g_cpu_series;
  unsigned char *ok = which ? g_gpu_ok : g_cpu_ok;
  int start = g_series_at - g_series_n;
  if (start < 0) start += HIST;
  double lo = 1e9, hi = -1e9;
  int any = 0;
  for (int i = 0; i < g_series_n; i++) {
    int idx = (start + i) % HIST;
    if (!ok[idx]) continue;
    any = 1;
    if (series[idx] < lo) lo = series[idx];
    if (series[idx] > hi) hi = series[idx];
  }
  SetBkMode(dc, TRANSPARENT);
  HFONT old = (HFONT)SelectObject(dc, g_font_small);
  SetTextColor(dc, COL_MUTED);
  if (!any) {
    RECT box = rc;
    box.left += 8;
    DrawTextA(dc, "waiting", -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    return;
  }
  if (hi - lo < 8) {
    double mid = (hi + lo) / 2.0;
    lo = mid - 6;
    hi = mid + 6;
  }
  char peak[48];
  snprintf(peak, sizeof(peak), "peak %.0f F", hi * 9.0 / 5.0 + 32.0);
  RECT label = rc;
  label.left += 8;
  label.right -= 8;
  label.top += 3;
  label.bottom = label.top + 16;
  DrawTextA(dc, peak, -1, &label, DT_LEFT | DT_SINGLELINE);
  DrawTextA(dc, g_series_n >= HIST ? "6 min" : "recent", -1, &label, DT_RIGHT | DT_SINGLELINE);
  int left = rc.left + 8;
  int rightx = rc.right - 8;
  int top = rc.top + 22;
  int bottom = rc.bottom - 6;
  if (bottom <= top) bottom = top + 1;
  if (rightx <= left) rightx = left + 1;
  HPEN pen = CreatePen(PS_SOLID, 2, which ? COL_ACCENT : COL_OK);
  HPEN old_pen = (HPEN)SelectObject(dc, pen);
  int drawing = 0;
  int width = rightx - left;
  for (int i = 0; i < g_series_n; i++) {
    int idx = (start + i) % HIST;
    if (!ok[idx]) {
      drawing = 0;
      continue;
    }
    int x = g_series_n > 1 ? left + (i * width) / (g_series_n - 1) : rightx;
    double t = (series[idx] - lo) / (hi - lo);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int y = bottom - (int)(t * (double)(bottom - top));
    if (!drawing) {
      MoveToEx(dc, x, y, NULL);
      drawing = 1;
    } else {
      LineTo(dc, x, y);
    }
  }
  SelectObject(dc, old_pen);
  DeleteObject(pen);
  SelectObject(dc, old);
}

static LRESULT CALLBACK chart_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);
  paint_chart(dc, rc, (int)(INT_PTR)GetPropW(hwnd, L"series"));
  EndPaint(hwnd, &ps);
  return 0;
}

static int source_match(const char *pattern, const char *source) {
  if (!pattern || !pattern[0] || !source || !source[0]) return 1;
  char copy[96];
  snprintf(copy, sizeof(copy), "%s", pattern);
  char *part = copy;
  while (part && *part) {
    char *bar = strchr(part, '|');
    if (bar) *bar = 0;
    if (part[0] && contains_ci(source, part)) return 1;
    part = bar ? bar + 1 : NULL;
  }
  return 0;
}

static int severity_rank(const char *severity) {
  if (strcmp(severity, "urgent") == 0) return 0;
  if (strcmp(severity, "serious") == 0) return 1;
  if (strcmp(severity, "watch") == 0) return 2;
  if (strcmp(severity, "noise") == 0) return 3;
  return 9;
}

static void on_run(void) {
  if (g_busy) return;
  g_busy = 1;
  EnableWindow(g_run, FALSE);
  InvalidateRect(g_run, NULL, TRUE);
  set_utf8(g_log, "Reading the System and Application logs…");
  UpdateWindow(g_hwnd);

  char *body = NULL;
  if (!http_get("/diagnose", &body, 60000)) {
    set_utf8(g_log, "The log check did not finish. Leave this window open and try again.");
    EnableWindow(g_run, TRUE);
    InvalidateRect(g_run, NULL, TRUE);
    g_busy = 0;
    return;
  }
  const char *end = body + strlen(body);
  const char *events = find_key(body, end, "events");
  typedef struct {
    char level[24];
    char source[120];
    char log_name[32];
    char message[720];
    int id;
  } EventRow;
  EventRow *rows = (EventRow *)calloc(40, sizeof(EventRow));
  int row_count = 0;
  if (rows && events && *skip_ws(events) == '[') {
    const char *arr_end = NULL;
    span_value(events, &arr_end);
    const char *p = skip_ws(events) + 1;
    while (p < arr_end && row_count < 40) {
      p = skip_ws(p);
      if (*p == ']') break;
      if (*p != '{') break;
      const char *obj_end = NULL;
      span_value(p, &obj_end);
      EventRow *row = &rows[row_count];
      const char *level = find_key(p, obj_end, "level");
      const char *source = find_key(p, obj_end, "source");
      const char *log_name = find_key(p, obj_end, "log");
      const char *message = find_key(p, obj_end, "message");
      const char *id_at = find_key(p, obj_end, "eventId");
      if (level) json_string(level, row->level, sizeof(row->level));
      if (source) json_string(source, row->source, sizeof(row->source));
      if (log_name) json_string(log_name, row->log_name, sizeof(row->log_name));
      if (message) json_string(message, row->message, sizeof(row->message));
      double id = 0;
      if (id_at && json_num(id_at, &id)) row->id = (int)id;
      row_count++;
      p = obj_end;
      p = skip_ws(p);
      if (*p == ',') p++;
    }
  }

  int hit_book[24];
  int hit_count[24];
  int hits = 0;
  for (int i = 0; i < row_count; i++) {
    for (int b = 0; b < g_book_count; b++) {
      int listed = 0;
      for (int k = 0; k < g_books[b].id_count; k++) {
        if (g_books[b].ids[k] == rows[i].id) listed = 1;
      }
      if (!listed || !source_match(g_books[b].source, rows[i].source)) continue;
      int slot = -1;
      for (int h = 0; h < hits; h++) {
        if (hit_book[h] == b) slot = h;
      }
      if (slot >= 0) hit_count[slot]++;
      else if (hits < 24) {
        hit_book[hits] = b;
        hit_count[hits] = 1;
        hits++;
      }
    }
  }
  for (int i = 0; i < hits; i++) {
    for (int j = i + 1; j < hits; j++) {
      if (severity_rank(g_books[hit_book[j]].severity) < severity_rank(g_books[hit_book[i]].severity)) {
        int tb = hit_book[i];
        int tc = hit_count[i];
        hit_book[i] = hit_book[j];
        hit_count[i] = hit_count[j];
        hit_book[j] = tb;
        hit_count[j] = tc;
      }
    }
  }

  Buf buf = {0};
  int lead = -1;
  for (int h = 0; h < hits; h++) {
    if (severity_rank(g_books[hit_book[h]].severity) < 3) {
      lead = h;
      break;
    }
  }
  if (lead < 0 && hits > 0) lead = 0;

  if (lead < 0 && row_count == 0) {
    buf_add(&buf, "WHAT IS GOING ON\r\n");
    buf_add(&buf, "Nothing in the System or Application logs from the last 2 days looks like a fault. There were no critical errors or warnings.\r\n\r\n");
    buf_add(&buf, "COMMON SOLUTIONS\r\n");
    buf_add(&buf, "• If the PC still misbehaves, note whether it happens at idle, in sleep, or only under load, then run this again right after it happens.\r\n");
  } else if (lead < 0) {
    buf_add(&buf, "WHAT IS GOING ON\r\n");
    buf_add(&buf, "Windows recorded problems, but they do not match a known crash, disk, or hardware pattern. The program name in each message is the useful part, not the event number alone.\r\n\r\n");
    buf_add(&buf, "COMMON SOLUTIONS\r\n");
    buf_add(&buf, "• Update or remove the program named in the message before replacing hardware.\r\n");
    buf_add(&buf, "• One warning is often a blip. The same message repeating is the one to chase.\r\n");
  } else {
    Book *book = &g_books[hit_book[lead]];
    int rank = severity_rank(book->severity);
    buf_add(&buf, "WHAT IS GOING ON\r\n");
    if (rank >= 3) {
      buf_add(&buf, "This is log noise, not the cause of a crash, heat, or a slow PC.\r\n\r\n");
    } else if (rank == 0) {
      buf_add(&buf, "This needs attention before it is written off as a glitch.\r\n\r\n");
    } else if (rank == 1) {
      buf_add(&buf, "This is a real fault, not a routine notice.\r\n\r\n");
    } else {
      buf_add(&buf, "This is worth a look. It is not, by itself, an emergency.\r\n\r\n");
    }
    buf_add(&buf, book->title);
    buf_add(&buf, "\r\n");
    buf_add(&buf, book->meaning);
    buf_add(&buf, "\r\n\r\nCOMMON SOLUTIONS\r\n");
    if (book->check_count) {
      for (int c = 0; c < book->check_count; c++) {
        buf_add(&buf, "• ");
        buf_add(&buf, book->checks[c]);
        buf_add(&buf, "\r\n");
      }
    } else {
      for (int c = 0; c < book->cause_count; c++) {
        buf_add(&buf, "• ");
        buf_add(&buf, book->causes[c]);
        buf_add(&buf, "\r\n");
      }
    }
    if (book->cause_count && book->check_count) {
      buf_add(&buf, "\r\nWhy it happens\r\n");
      for (int c = 0; c < book->cause_count; c++) {
        buf_add(&buf, "• ");
        buf_add(&buf, book->causes[c]);
        buf_add(&buf, "\r\n");
      }
    }
    int extra = 0;
    for (int h = 0; h < hits; h++) {
      if (h == lead) continue;
      if (!extra) buf_add(&buf, "\r\nAlso in the log\r\n");
      extra++;
      Book *other = &g_books[hit_book[h]];
      buf_add(&buf, "• ");
      buf_add(&buf, other->title);
      if (other->check_count) {
        buf_add(&buf, " — ");
        buf_add(&buf, other->checks[0]);
      }
      buf_add(&buf, "\r\n");
    }
  }

  if (row_count) buf_add(&buf, "\r\nEvents behind this\r\n");
  for (int i = 0; i < row_count; i++) {
    char title[180];
    snprintf(title, sizeof(title), "\r\n%s · %s · %d\r\n%s\r\n", rows[i].level[0] ? rows[i].level : "Event", rows[i].source, rows[i].id, rows[i].log_name);
    buf_add(&buf, title);
    buf_add(&buf, rows[i].message);
    buf_add(&buf, "\r\n");
  }
  set_utf8(g_log, buf.data ? buf.data : "No report.");
  free(buf.data);
  free(rows);
  free(body);
  EnableWindow(g_run, TRUE);
  InvalidateRect(g_run, NULL, TRUE);
  g_busy = 0;
}

static HFONT make_font(int size, int weight, const wchar_t *face) {
  return CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static HWND add_text(HWND parent, const wchar_t *text, HFONT font, DWORD style) {
  HWND hwnd = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, NULL, NULL, NULL);
  SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
  return hwnd;
}

static void create_controls(HWND hwnd) {
  g_kicker = add_text(hwnd, L"WINDOWS FAULT DESK", g_font_small, SS_LEFT);
  g_host = add_text(hwnd, L"DETECTING", g_font, SS_LEFT);
  g_status = add_text(hwnd, L"Starting", g_font, SS_RIGHT);
  g_cpu_lbl = add_text(hwnd, L"CPU", g_font_small, SS_LEFT);
  g_gpu_lbl = add_text(hwnd, L"GPU", g_font_small, SS_LEFT);
  g_load_lbl = add_text(hwnd, L"CPU LOAD", g_font_small, SS_LEFT);
  g_cpu_temp = add_text(hwnd, L"—", g_font_big, SS_LEFT);
  g_gpu_temp = add_text(hwnd, L"—", g_font_big, SS_LEFT);
  g_load = add_text(hwnd, L"—", g_font_big, SS_LEFT);
  g_cpu_hint = add_text(hwnd, L"", g_font_small, SS_LEFT);
  g_gpu_hint = add_text(hwnd, L"", g_font_small, SS_LEFT);
  g_load_hint = add_text(hwnd, L"live", g_font_small, SS_LEFT);
  g_cpu_name = add_text(hwnd, L"", g_font_small, SS_LEFT | SS_NOPREFIX);
  g_gpu_name = add_text(hwnd, L"", g_font_small, SS_LEFT | SS_NOPREFIX);
  g_notes = add_text(hwnd, L"", g_font_small, SS_LEFT | SS_NOPREFIX);
  g_cpu_chart = CreateWindowW(L"ChickenChart", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, NULL, NULL, NULL);
  g_gpu_chart = CreateWindowW(L"ChickenChart", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, NULL, NULL, NULL);
  SetPropW(g_cpu_chart, L"series", (HANDLE)0);
  SetPropW(g_gpu_chart, L"series", (HANDLE)(INT_PTR)1);
  g_run = CreateWindowW(L"BUTTON", L"Run diagnostic", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_RUN, NULL, NULL);
  g_log = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0, 10, 10, hwnd, NULL, NULL, NULL);
  SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
  set_utf8(g_log, "This is the Diagnostic Chicken window. Close it to stop. It does not start with Windows.\r\n\r\nTemperatures are read inside this app. Windows may ask for administrator approval so the sensor reader can see the chip. You do not open a second monitor.\r\n\r\nRun diagnostic says what the log points to, then the usual fixes.");
}

static void place(HWND hwnd, int x, int y, int w, int h) {
  MoveWindow(hwnd, x, y, w, h, TRUE);
}

static void layout(int w, int h) {
  int pad = 20;
  int gap = 16;
  int col = (w - pad * 2 - gap * 2) / 3;
  if (col < 100) col = 100;
  int x1 = pad;
  int x2 = pad + col + gap;
  int x3 = pad + (col + gap) * 2;
  place(g_kicker, pad, 14, w - pad * 2, 16);
  place(g_host, pad, 34, w - 160, 26);
  place(g_status, w - 136, 34, 116, 26);
  place(g_cpu_lbl, x1, 68, col, 16);
  place(g_gpu_lbl, x2, 68, col, 16);
  place(g_load_lbl, x3, 68, col, 16);
  place(g_cpu_temp, x1, 86, col, 40);
  place(g_gpu_temp, x2, 86, col, 40);
  place(g_load, x3, 86, col, 40);
  place(g_cpu_hint, x1, 126, col, 16);
  place(g_gpu_hint, x2, 126, col, 16);
  place(g_load_hint, x3, 126, col, 16);
  place(g_cpu_chart, x1, 148, col, 86);
  place(g_gpu_chart, x2, 148, col, 86);
  place(g_cpu_name, x1, 240, col, 36);
  place(g_gpu_name, x2, 240, col, 36);
  place(g_notes, pad, 280, w - pad * 2, 32);
  place(g_run, pad, 318, 168, 36);
  int log_y = 366;
  if (h - log_y < 120) log_y = h - 120;
  if (log_y < 366) log_y = 366;
  place(g_log, pad, log_y, w - pad * 2, h - log_y - 16);
}

static COLORREF tone_color(int tone) {
  if (tone == 3) return COL_BAD;
  if (tone == 2) return COL_ACCENT;
  if (tone == 1) return COL_OK;
  return COL_MUTED;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      create_controls(hwnd);
      return 0;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) layout(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_TIMER:
      if (wp == ID_TIMER && !g_busy) {
        SetTimer(hwnd, ID_TIMER, 2000, NULL);
        refresh_live();
      }
      return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == ID_RUN) on_run();
      return 0;
    case WM_DRAWITEM: {
      DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
      if (item->CtlID != ID_RUN) break;
      int disabled = item->itemState & ODS_DISABLED;
      HBRUSH brush = CreateSolidBrush(disabled ? RGB(58, 56, 42) : COL_ACCENT);
      FillRect(item->hDC, &item->rcItem, brush);
      DeleteObject(brush);
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, disabled ? COL_MUTED : COL_ACCENT_FG);
      HFONT old = (HFONT)SelectObject(item->hDC, g_font);
      DrawTextW(item->hDC, L"Run diagnostic", -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(item->hDC, old);
      return TRUE;
    }
    case WM_ERASEBKGND: {
      RECT rect;
      GetClientRect(hwnd, &rect);
      FillRect((HDC)wp, &rect, g_bg);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)wp;
      HWND child = (HWND)lp;
      SetBkColor(dc, COL_BG);
      SetBkMode(dc, OPAQUE);
      if (child == g_cpu_temp) SetTextColor(dc, tone_color(g_cpu_tone));
      else if (child == g_gpu_temp) SetTextColor(dc, tone_color(g_gpu_tone));
      else if (child == g_load) SetTextColor(dc, tone_color(g_load_tone));
      else if (child == g_kicker || child == g_status) SetTextColor(dc, COL_ACCENT);
      else if (child == g_cpu_hint || child == g_gpu_hint || child == g_load_hint || child == g_notes || child == g_cpu_name || child == g_gpu_name) SetTextColor(dc, COL_MUTED);
      else SetTextColor(dc, COL_FG);
      return (LRESULT)g_bg;
    }
    case WM_GETMINMAXINFO: {
      MINMAXINFO *info = (MINMAXINFO *)lp;
      info->ptMinTrackSize.x = 760;
      info->ptMinTrackSize.y = 680;
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd, ID_TIMER);
      if (g_child) TerminateProcess(g_child, 0);
      if (g_job) CloseHandle(g_job);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;
  if (cmd && strstr(cmd, "--stay")) g_stay = 1;
  SetProcessDPIAware();
  load_books();
  if (!install_and_start()) return g_child ? 1 : 0;

  g_bg = CreateSolidBrush(COL_BG);
  g_font = make_font(-18, FW_NORMAL, L"Segoe UI");
  g_font_big = make_font(-34, FW_SEMIBOLD, L"Segoe UI");
  g_font_small = make_font(-14, FW_NORMAL, L"Segoe UI");
  g_font_mono = make_font(-16, FW_NORMAL, L"Consolas");

  WNDCLASSW chart;
  ZeroMemory(&chart, sizeof(chart));
  chart.lpfnWndProc = chart_proc;
  chart.hInstance = instance;
  chart.lpszClassName = L"ChickenChart";
  chart.hCursor = LoadCursor(NULL, IDC_ARROW);
  chart.hbrBackground = g_bg;
  RegisterClassW(&chart);

  WNDCLASSW klass;
  ZeroMemory(&klass, sizeof(klass));
  klass.lpfnWndProc = wndproc;
  klass.hInstance = instance;
  klass.lpszClassName = L"DiagnosticChicken";
  klass.hCursor = LoadCursor(NULL, IDC_ARROW);
  klass.hbrBackground = g_bg;
  RegisterClassW(&klass);

  g_hwnd = CreateWindowExW(0, L"DiagnosticChicken", L"Diagnostic Chicken", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 980, 860, NULL, NULL, instance, NULL);
  if (!g_hwnd) {
    fail("Could not open the Diagnostic Chicken window.");
    return 1;
  }
  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);
  SetTimer(g_hwnd, ID_TIMER, 700, NULL);

  MSG message;
  while (GetMessageW(&message, NULL, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  DeleteObject(g_font);
  DeleteObject(g_font_big);
  DeleteObject(g_font_small);
  DeleteObject(g_font_mono);
  DeleteObject(g_bg);
  return 0;
}
