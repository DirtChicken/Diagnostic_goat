import type { Level, SensorSnapshot, TempSource, WinEvent } from "./types";

export type ParseResult =
  | { ok: true; events: WinEvent[]; sensors: SensorSnapshot | null }
  | { ok: false; error: string };

const TEMP_SOURCES: readonly TempSource[] = [
  "simulated",
  "librehardwaremonitor",
  "nvidia-smi",
  "acpi-thermal-zone",
  "unavailable",
  "unknown",
];

export function parseReport(raw: string): ParseResult {
  const text = raw.replace(/^\uFEFF/, "").trim();
  if (!text) return { ok: false, error: "Paste a log, or open a file, first." };
  if (text.includes("\u0000")) {
    return {
      ok: false,
      error:
        "This looks like a binary .evtx log. Event Viewer can save that view as XML, or run the collector script.",
    };
  }
  if (text.length > 400_000) {
    return {
      ok: false,
      error: "That paste is too large. Keep the newest errors, or use the collector. It already trims the log.",
    };
  }

  if (/^[\[{]/.test(text)) {
    try {
      return fromData(JSON.parse(text) as unknown);
    } catch {
      return { ok: false, error: "That looks like JSON, but it is cut off or invalid." };
    }
  }

  if (/<Event[\s>]/i.test(text) && /EventID/i.test(text)) {
    const events = parseXml(text);
    if (events.length) return finish(events, null);
    return { ok: false, error: "Found XML, but no Event ID fields inside it." };
  }

  if (looksLikeCsv(text)) {
    const events = parseCsvEvents(text);
    if (events.length) return finish(events, null);
  }

  const blocks = parseBlocks(text);
  if (blocks.length) return finish(blocks, null);

  return {
    ok: false,
    error:
      "No Windows events in that paste. Use Event Viewer details, an XML export, CSV, or the collector JSON.",
  };
}

function finish(events: Omit<WinEvent, "id">[], sensors: SensorSnapshot | null): ParseResult {
  if (!events.length && !sensors) {
    return { ok: false, error: "Nothing usable in that report." };
  }
  return {
    ok: true,
    events: events.map((event, index) => ({ ...event, id: `${index}-${event.eventId}-${event.source}` })),
    sensors,
  };
}

function fromData(data: unknown): ParseResult {
  const records = extractEventRecords(data);
  const events = records
    .map(eventFromUnknown)
    .filter((event): event is Omit<WinEvent, "id"> => event !== null);
  const sensors = sensorsFrom(asRecord(data));
  if (!events.length && !sensors) {
    return { ok: false, error: "JSON parsed, but it has no events and no sensor fields." };
  }
  return finish(events, sensors);
}

function extractEventRecords(data: unknown): unknown[] {
  if (Array.isArray(data)) return data;
  const record = asRecord(data);
  if (!record) return [];
  if ("events" in record) return asArray(record.events);
  if ("Events" in record) return asArray(record.Events);
  if (looksLikeEvent(record)) return [record];
  return [];
}

function looksLikeEvent(record: Record<string, unknown>): boolean {
  return "eventId" in record || "EventID" in record || "EventId" in record || "Id" in record;
}

function eventFromUnknown(raw: unknown): Omit<WinEvent, "id"> | null {
  const record = asRecord(raw);
  if (!record) return null;
  const eventId = num(record.eventId ?? record.EventID ?? record.EventId ?? record.Id ?? record.id);
  if (eventId == null) return null;
  const source = str(record.source ?? record.Source ?? record.ProviderName ?? record.Provider) || "Unknown";
  const message = str(record.message ?? record.Message ?? record.Description) || "(no message text)";
  return {
    log: str(record.log ?? record.LogName ?? record.Channel),
    time: parseTime(record.time ?? record.TimeCreated ?? record.Date ?? record.SystemTime),
    level: normLevel(record.level ?? record.LevelDisplayName ?? record.Level),
    source,
    eventId: Math.round(eventId),
    message,
  };
}

function sensorsFrom(record: Record<string, unknown> | null): SensorSnapshot | null {
  if (!record) return null;
  const cpu = asRecord(record.cpu);
  const gpuRecords = asArray(record.gpus ?? record.gpu)
    .map(asRecord)
    .filter((item): item is Record<string, unknown> => item !== null);
  const zones = asArray(record.thermalZones)
    .map(asRecord)
    .filter((item): item is Record<string, unknown> => item !== null);
  const notes = strList(record.notes, 8);
  const tagged = record.kind === "sentinel-report" || record.kind === "sentinel";
  if (!cpu && !gpuRecords.length && !zones.length && !notes.length && !tagged) return null;

  let cpuTemp = temp(cpu?.tempC ?? cpu?.celsius ?? cpu?.temperature);
  let cpuTempSource = tempSource(cpu?.tempSource, cpuTemp == null ? "unavailable" : "unknown");
  if (cpuTemp == null && zones.length) {
    let best: { name: string; value: number } | null = null;
    for (const zone of zones) {
      const value = temp(zone.celsius ?? zone.tempC ?? zone.CurrentTemperature);
      const name = str(zone.name) || "ACPI zone";
      if (value == null) continue;
      if (!best || value > best.value) best = { name, value };
    }
    if (best) {
      cpuTemp = best.value;
      cpuTempSource = "acpi-thermal-zone";
      notes.push(`CPU figure is ACPI zone “${best.name}”, not the CPU die.`);
    }
  }

  const ranked = gpuRecords
    .map((gpu) => ({
      name: str(gpu.name),
      tempC: temp(gpu.tempC ?? gpu.temperature ?? gpu.celsius),
      load: load(gpu.loadPercent ?? gpu.utilPercent ?? gpu.utilization),
      source: tempSource(gpu.tempSource, "unknown"),
    }))
    .sort((a, b) => (b.tempC ?? -1) - (a.tempC ?? -1));
  const gpu = ranked[0];

  return {
    origin: "report",
    host: str(record.host),
    collectedAt: parseTime(record.collectedAt) ?? str(record.collectedAt),
    cpuName: str(cpu?.name),
    cpuLoad: load(cpu?.loadPercent ?? cpu?.load),
    cpuTemp,
    cpuTempSource,
    gpuName: gpu?.name,
    gpuLoad: gpu?.load ?? null,
    gpuTemp: gpu?.tempC ?? null,
    gpuTempSource: gpu ? (gpu.tempC == null ? "unavailable" : gpu.source) : "unavailable",
    notes: notes.slice(0, 8),
  };
}

function parseXml(text: string): Omit<WinEvent, "id">[] {
  const blocks = text.match(/<Event\b[\s\S]*?<\/Event>/gi) ?? [];
  const events: Omit<WinEvent, "id">[] = [];
  for (const block of blocks) {
    const eventId = num(attr(block, /<EventID\b[^>]*>(\d+)<\/EventID>/i));
    if (eventId == null) continue;
    const message =
      decodeXml(attr(block, /<(?:RenderingInfo[\s\S]*?)?<Message>([\s\S]*?)<\/Message>/i) ?? "") ||
      "(no message text)";
    events.push({
      log: decodeXml(attr(block, /<Channel>([\s\S]*?)<\/Channel>/i) ?? ""),
      time: attr(block, /<TimeCreated\b[^>]*SystemTime="([^"]+)"/i),
      level: normLevel(attr(block, /<Level>(\d+)<\/Level>/i)),
      source: decodeXml(attr(block, /<Provider\b[^>]*\bName="([^"]+)"/i) ?? "") || "Unknown",
      eventId: Math.round(eventId),
      message,
    });
  }
  return events;
}

function looksLikeCsv(text: string): boolean {
  const header = text.split(/\r?\n/).find((line) => line.trim());
  if (!header || !header.includes(",")) return false;
  return /event\s*id/i.test(header) && /source|provider|level/i.test(header);
}

function parseCsvEvents(text: string): Omit<WinEvent, "id">[] {
  const rows = parseCsv(text);
  if (rows.length < 2) return [];
  const header = rows[0].map((cell) => cell.trim().toLowerCase());
  const col = (pattern: RegExp) => header.findIndex((cell) => pattern.test(cell));
  const idCol = col(/event\s*id|^id$/);
  if (idCol < 0) return [];
  const sourceCol = col(/source|provider/);
  const levelCol = col(/level/);
  const timeCol = col(/date|time/);
  const logCol = col(/log/);
  const messageCol = col(/message|description/);
  const events: Omit<WinEvent, "id">[] = [];
  for (const row of rows.slice(1)) {
    const eventId = num(row[idCol]);
    if (eventId == null) continue;
    events.push({
      log: logCol >= 0 ? row[logCol]?.trim() : undefined,
      time: timeCol >= 0 ? row[timeCol]?.trim() : undefined,
      level: normLevel(levelCol >= 0 ? row[levelCol] : undefined),
      source: (sourceCol >= 0 ? row[sourceCol]?.trim() : "") || "Unknown",
      eventId: Math.round(eventId),
      message: (messageCol >= 0 ? row[messageCol]?.trim() : "") || "(no message text)",
    });
  }
  return events;
}

function parseCsv(text: string): string[][] {
  const rows: string[][] = [];
  let row: string[] = [];
  let cell = "";
  let quoted = false;
  for (let i = 0; i < text.length; i += 1) {
    const char = text[i];
    if (quoted) {
      if (char === '"') {
        if (text[i + 1] === '"') {
          cell += '"';
          i += 1;
        } else quoted = false;
      } else cell += char;
    } else if (char === '"') quoted = true;
    else if (char === ",") {
      row.push(cell);
      cell = "";
    } else if (char === "\n") {
      row.push(cell);
      rows.push(row);
      row = [];
      cell = "";
    } else if (char !== "\r") cell += char;
  }
  if (cell.length || row.length) {
    row.push(cell);
    rows.push(row);
  }
  return rows.filter((item) => item.some((value) => value.trim()));
}

function parseBlocks(text: string): Omit<WinEvent, "id">[] {
  const chunks = text
    .split(/\n(?=(?:Log Name|ProviderName|Source)\s*:)/i)
    .map((chunk) => chunk.trim())
    .filter(Boolean);
  const events: Omit<WinEvent, "id">[] = [];
  for (const chunk of chunks) {
    const eventId = num(field(chunk, "Event ID") ?? field(chunk, "Id") ?? field(chunk, "EventId"));
    if (eventId == null) continue;
    const source =
      field(chunk, "Source") ?? field(chunk, "ProviderName") ?? field(chunk, "Provider") ?? "Unknown";
    const description = chunk.split(/\n(?:Description|Message)\s*:\s*/i).slice(1).join("\n").trim();
    events.push({
      log: field(chunk, "Log Name") ?? field(chunk, "LogName"),
      time: field(chunk, "Date") ?? field(chunk, "TimeCreated") ?? field(chunk, "Date and Time"),
      level: normLevel(field(chunk, "Level") ?? field(chunk, "LevelDisplayName")),
      source,
      eventId: Math.round(eventId),
      message: description || "(no message text)",
    });
  }
  return events;
}

function field(block: string, name: string): string | undefined {
  const match = new RegExp(`^${name}\\s*:\\s*(.*)$`, "im").exec(block);
  const value = match?.[1]?.trim();
  return value || undefined;
}

function attr(block: string, pattern: RegExp): string | undefined {
  return pattern.exec(block)?.[1]?.trim() || undefined;
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

function num(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim() && Number.isFinite(Number(value))) return Number(value);
  return null;
}

function str(value: unknown): string | undefined {
  if (typeof value === "string" && value.trim()) return value.trim();
  if (typeof value === "number" && Number.isFinite(value)) return String(value);
  return undefined;
}

function strList(value: unknown, max: number): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .map((item) => (typeof item === "string" ? item.trim() : ""))
    .filter(Boolean)
    .slice(0, max);
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
  if (typeof value === "string" && (TEMP_SOURCES as readonly string[]).includes(value)) {
    return value as TempSource;
  }
  return fallback;
}

function normLevel(value: unknown): Level {
  const text = String(value ?? "").toLowerCase();
  if (text === "1" || text.includes("crit")) return "Critical";
  if (text === "2" || text.includes("err")) return "Error";
  if (text === "3" || text.includes("warn")) return "Warning";
  return "Information";
}

function parseTime(value: unknown): string | undefined {
  if (typeof value === "string") {
    const stamped = /\/Date\((-?\d+)\)\//.exec(value);
    if (stamped) {
      const date = new Date(Number(stamped[1]));
      if (!Number.isNaN(date.getTime())) return date.toISOString();
    }
    return value.trim() || undefined;
  }
  const record = asRecord(value);
  if (!record) return undefined;
  return parseTime(record.DateTime ?? record.value ?? record.Value);
}

function decodeXml(value: string): string {
  return value
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
    .replace(/&#x([0-9a-f]+);/gi, (_, hex: string) => safeCode(parseInt(hex, 16)))
    .replace(/&#(\d+);/g, (_, digits: string) => safeCode(Number(digits)))
    .replace(/</g, "<")
    .replace(/>/g, ">")
    .replace(/"/g, '"')
    .replace(/'/g, "'")
    .replace(/&/g, "&")
    .trim();
}

function safeCode(code: number): string {
  try {
    if (!Number.isFinite(code) || code < 0) return "";
    return String.fromCodePoint(code);
  } catch {
    return "";
  }
}
