import type { SensorSnapshot, TempSource } from "./types";

export const AGENT_URL = "http://127.0.0.1:47321";

const TEMP_SOURCES: readonly TempSource[] = [
  "simulated",
  "librehardwaremonitor",
  "nvidia-smi",
  "acpi-thermal-zone",
  "unavailable",
  "unknown",
];

export async function fetchLive(): Promise<SensorSnapshot | null> {
  try {
    const response = await fetch(`${AGENT_URL}/live`, {
      cache: "no-store",
      signal: AbortSignal.timeout(2000),
    });
    if (!response.ok) return null;
    return toSnapshot(await response.json());
  } catch {
    return null;
  }
}

export async function fetchDiagnostic(): Promise<string> {
  const response = await fetch(`${AGENT_URL}/diagnose`, {
    cache: "no-store",
    signal: AbortSignal.timeout(60_000),
  });
  if (!response.ok) throw new Error("The agent could not read the logs.");
  return response.text();
}

function toSnapshot(body: unknown): SensorSnapshot | null {
  const record = asRecord(body);
  if (!record || record.kind !== "sentinel-agent") return null;
  const cpu = asRecord(record.cpu);
  const gpus = asArray(record.gpus)
    .map(asRecord)
    .filter((item): item is Record<string, unknown> => item !== null);
  const ranked = [...gpus].sort((a, b) => (temp(b.tempC) ?? -1) - (temp(a.tempC) ?? -1));
  const gpu = ranked[0];
  const notes = strList(record.notes);
  return {
    origin: "live",
    host: str(record.host),
    collectedAt: str(record.collectedAt),
    cpuName: str(cpu?.name),
    cpuCores: whole(cpu?.cores),
    cpuMaxMhz: whole(cpu?.maxMhz),
    cpuLoad: load(cpu?.loadPercent),
    cpuTemp: temp(cpu?.tempC),
    cpuTempSource: tempSource(cpu?.tempSource, temp(cpu?.tempC) == null ? "unavailable" : "unknown"),
    gpuName: str(gpu?.name),
    gpuDriver: str(gpu?.driver),
    gpuLoad: load(gpu?.loadPercent),
    gpuTemp: temp(gpu?.tempC),
    gpuTempSource: tempSource(gpu?.tempSource, temp(gpu?.tempC) == null ? "unavailable" : "unknown"),
    notes,
  };
}

function asRecord(value: unknown): Record<string, unknown> | null {
  if (value && typeof value === "object" && !Array.isArray(value)) return value as Record<string, unknown>;
  return null;
}

function asArray(value: unknown): unknown[] {
  if (Array.isArray(value)) return value;
  if (value == null) return [];
  return [value];
}

function str(value: unknown): string | undefined {
  if (typeof value === "string" && value.trim()) return value.trim();
  return undefined;
}

function strList(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .map((item) => (typeof item === "string" ? item.trim() : ""))
    .filter(Boolean)
    .slice(0, 8);
}

function num(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim() && Number.isFinite(Number(value))) return Number(value);
  return null;
}

function whole(value: unknown): number | null {
  const valueNumber = num(value);
  if (valueNumber == null || valueNumber <= 0) return null;
  return Math.round(valueNumber);
}

function temp(value: unknown): number | null {
  const valueNumber = num(value);
  if (valueNumber == null || valueNumber < 0 || valueNumber > 125) return null;
  return Math.round(valueNumber * 10) / 10;
}

function load(value: unknown): number | null {
  const valueNumber = num(value);
  if (valueNumber == null || valueNumber < 0 || valueNumber > 100) return null;
  return Math.round(valueNumber);
}

function tempSource(value: unknown, fallback: TempSource): TempSource {
  if (typeof value === "string" && (TEMP_SOURCES as readonly string[]).includes(value)) return value as TempSource;
  return fallback;
}
