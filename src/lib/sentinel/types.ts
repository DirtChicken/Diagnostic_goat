export type Level = "Critical" | "Error" | "Warning" | "Information";

export type TempSource =
  | "simulated"
  | "librehardwaremonitor"
  | "nvidia-smi"
  | "acpi-thermal-zone"
  | "unavailable"
  | "unknown";

export type WinEvent = {
  id: string;
  log?: string;
  time?: string;
  level: Level;
  source: string;
  eventId: number;
  message: string;
};

export type SensorSnapshot = {
  origin: "simulated" | "report" | "live";
  host?: string;
  collectedAt?: string;
  cpuName?: string;
  cpuCores?: number | null;
  cpuMaxMhz?: number | null;
  cpuLoad: number | null;
  cpuTemp: number | null;
  cpuTempSource: TempSource;
  gpuName?: string;
  gpuDriver?: string;
  gpuLoad: number | null;
  gpuTemp: number | null;
  gpuTempSource: TempSource;
  notes: string[];
};

export type Severity = "low" | "medium" | "high" | "critical";

export type Diagnosis = {
  headline: string;
  severity: Severity;
  summary: string;
  likelyCauses: { title: string; why: string }[];
  checks: string[];
  fixes: string[];
  doNot: string[];
  urls: string[];
};
