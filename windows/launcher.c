#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps1.h"
#include "html.h"
#include "books.h"

static HANDLE g_child = NULL;

static BOOL WINAPI on_ctrl(DWORD type) {
  (void)type;
  if (g_child) TerminateProcess(g_child, 0);
  return FALSE;
}

static void hold_error(const char *message) {
  fprintf(stderr, "%s\nPress Enter to close.\n", message);
  fflush(stderr);
  getchar();
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

static void open_desk(void) {
  ShellExecuteA(NULL, "open", "http://127.0.0.1:47321/", NULL, NULL, SW_SHOWNORMAL);
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

int main(void) {
  SetConsoleTitleA("Diagnostic Chicken");
  SetConsoleCtrlHandler(on_ctrl, TRUE);

  const char *local = getenv("LOCALAPPDATA");
  if (!local || !local[0]) {
    hold_error("Could not find the local app folder.");
    return 1;
  }

  char dir[MAX_PATH];
  snprintf(dir, MAX_PATH, "%s\\Diagnostic Chicken", local);
  CreateDirectoryA(dir, NULL);

  char running[MAX_PATH];
  GetModuleFileNameA(NULL, running, MAX_PATH);
  char installed[MAX_PATH];
  snprintf(installed, MAX_PATH, "%s\\Diagnostic Chicken.exe", dir);
  if (_stricmp(running, installed) != 0) CopyFileA(running, installed, FALSE);

  char ps1[MAX_PATH], html[MAX_PATH], books[MAX_PATH];
  snprintf(ps1, MAX_PATH, "%s\\sentinel-agent.ps1", dir);
  snprintf(html, MAX_PATH, "%s\\desk.html", dir);
  snprintf(books, MAX_PATH, "%s\\playbooks.json", dir);
  if (!write_file(ps1, ps1_bytes, ps1_bytes_len) || !write_file(html, html_bytes, html_bytes_len) ||
      !write_file(books, books_bytes, books_bytes_len)) {
    hold_error("Could not write Diagnostic Chicken's files.");
    return 1;
  }

  pin_start_menu(installed, dir);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    hold_error("Could not start networking.");
    return 1;
  }
  if (port_open()) {
    printf("Diagnostic Chicken is already running. Opening the desk.\n");
    printf("The other Diagnostic Chicken window is the one to close when you are done.\n");
    fflush(stdout);
    open_desk();
    for (;;) Sleep(1000);
  }

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
    hold_error("Could not start Windows PowerShell.");
    return 1;
  }

  g_child = pi.hProcess;
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
      CloseHandle(job);
      job = NULL;
    }
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  int up = 0;
  for (int i = 0; i < 50; i++) {
    if (port_open()) {
      up = 1;
      break;
    }
    Sleep(200);
  }
  if (!up) {
    TerminateProcess(g_child, 1);
    hold_error("Diagnostic Chicken did not start. Close any other Diagnostic Chicken window and try again.");
    return 1;
  }

  open_desk();
  printf("Diagnostic Chicken is running.\n");
  printf("Close this window to stop it.\n");
  printf("It does not start with Windows and it is not a background service.\n");
  printf("A Start Menu shortcut named Diagnostic Chicken was added.\n");
  fflush(stdout);
  WaitForSingleObject(g_child, INFINITE);
  if (job) CloseHandle(job);
  printf("Diagnostic Chicken stopped.\n");
  return 0;
}
