import type { SensorSnapshot, WinEvent } from "./types";

export type SampleCase = {
  id: string;
  label: string;
  detail: string;
  sensors: SensorSnapshot;
  events: WinEvent[];
};

let seq = 0;

function event(partial: Omit<WinEvent, "id">): WinEvent {
  seq += 1;
  return { ...partial, id: `sample-${seq}` };
}

function sim(over: Partial<SensorSnapshot> = {}): SensorSnapshot {
  return {
    origin: "simulated",
    host: "SAMPLE-PC",
    collectedAt: "2026-10-31T20:14:00Z",
    cpuName: "Sample 8-core",
    cpuLoad: 8,
    cpuTemp: 51,
    cpuTempSource: "simulated",
    gpuName: "Sample GPU",
    gpuLoad: 2,
    gpuTemp: 44,
    gpuTempSource: "simulated",
    notes: [],
    ...over,
  };
}

export const CASES: SampleCase[] = [
  {
    id: "power",
    label: "Dirty shutdown",
    detail: "Restart with no bugcheck",
    sensors: sim({ cpuLoad: 6, cpuTemp: 54, gpuTemp: 47 }),
    events: [
      event({
        log: "System",
        time: "2026-10-31T20:14:02Z",
        level: "Critical",
        source: "Microsoft-Windows-Kernel-Power",
        eventId: 41,
        message:
          "The system has rebooted without cleanly shutting down first. BugcheckCode: 0. PowerButtonTimestamp: 0. No dump file was written.",
      }),
      event({
        log: "System",
        time: "2026-10-31T20:14:08Z",
        level: "Error",
        source: "EventLog",
        eventId: 6008,
        message: "The previous system shutdown at 8:11:02 PM on 10/31/2026 was unexpected.",
      }),
    ],
  },
  {
    id: "disk",
    label: "Disk timeouts",
    detail: "Retried reads on Disk 0",
    sensors: sim({ cpuLoad: 22, cpuTemp: 58, gpuTemp: 45 }),
    events: [
      event({
        log: "System",
        time: "2026-10-31T18:02:11Z",
        level: "Warning",
        source: "disk",
        eventId: 153,
        message:
          "The IO operation at logical block address 0x2a11c0 for Disk 0 (PDO name: \\Device\\0000003a) was retried.",
      }),
      event({
        log: "System",
        time: "2026-10-31T18:02:12Z",
        level: "Warning",
        source: "stornvme",
        eventId: 129,
        message: "Reset to device \\Device\\RaidPort1 was issued.",
      }),
      event({
        log: "System",
        time: "2026-10-31T18:06:40Z",
        level: "Warning",
        source: "disk",
        eventId: 153,
        message: "The IO operation at logical block address 0x2a2400 for Disk 0 was retried.",
      }),
    ],
  },
  {
    id: "gpu",
    label: "GPU reset",
    detail: "Driver timeout under load",
    sensors: sim({ cpuLoad: 64, cpuTemp: 78, gpuLoad: 99, gpuTemp: 88 }),
    events: [
      event({
        log: "System",
        time: "2026-10-31T21:40:18Z",
        level: "Warning",
        source: "Display",
        eventId: 4101,
        message:
          "Display driver nvlddmkm stopped responding and has successfully recovered.",
      }),
      event({
        log: "System",
        time: "2026-10-31T21:40:18Z",
        level: "Error",
        source: "nvlddmkm",
        eventId: 14,
        message: "NVRM: Xid (PCI:0000:01:00): 79, GPU has fallen off the bus.",
      }),
    ],
  },
  {
    id: "whea",
    label: "Machine check",
    detail: "Fatal WHEA error",
    sensors: sim({ cpuLoad: 40, cpuTemp: 71, gpuTemp: 52 }),
    events: [
      event({
        log: "System",
        time: "2026-10-31T07:16:44Z",
        level: "Critical",
        source: "Microsoft-Windows-WHEA-Logger",
        eventId: 18,
        message:
          "A fatal hardware error has occurred. Component: Memory. Error Source: Machine Check Exception. The details section names a corrected-to-fatal memory error.",
      }),
    ],
  },
  {
    id: "heat",
    label: "Heat cutoff",
    detail: "Died while rendering",
    sensors: sim({ cpuLoad: 100, cpuTemp: 98, gpuLoad: 96, gpuTemp: 93 }),
    events: [
      event({
        log: "System",
        time: "2026-10-31T22:05:01Z",
        level: "Critical",
        source: "Microsoft-Windows-Kernel-Power",
        eventId: 41,
        message:
          "The system has rebooted without cleanly shutting down first. BugcheckCode: 0. The machine was rendering when the display went black and the fans stayed at full speed until the restart.",
      }),
    ],
  },
  {
    id: "service",
    label: "Crashing service",
    detail: "Helper loop, plus noise",
    sensors: sim({ cpuLoad: 18, cpuTemp: 49, gpuTemp: 42 }),
    events: [
      event({
        log: "System",
        time: "2026-10-31T11:12:09Z",
        level: "Error",
        source: "Service Control Manager",
        eventId: 7031,
        message:
          "The LampTray service terminated unexpectedly. It has done this 5 time(s). The following corrective action will be taken in 60000 milliseconds: Restart the service.",
      }),
      event({
        log: "Application",
        time: "2026-10-31T11:12:08Z",
        level: "Error",
        source: "Application Error",
        eventId: 1000,
        message:
          "Faulting application name: LampTray.exe, version: 2.4.1.0. Faulting module name: LampTray.exe. Exception code: 0xc0000005.",
      }),
      event({
        log: "System",
        time: "2026-10-31T11:13:22Z",
        level: "Warning",
        source: "Microsoft-Windows-DistributedCOM",
        eventId: 10016,
        message:
          "The application-specific permission settings do not grant Local Activation permission for the COM Server application with CLSID {C2F03A33-21F5-47FA-B4BB-156362A2F239}.",
      }),
    ],
  },
];
