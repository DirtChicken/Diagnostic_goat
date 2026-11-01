import type { SensorSnapshot, WinEvent } from "./types";

export function buildBrief(
  events: WinEvent[],
  sensors: SensorSnapshot | null,
  playbookTitles: string[],
): string {
  const lines: string[] = ["Report for a Windows PC diagnostic."];
  if (!sensors) {
    lines.push("Temperatures: not included.");
  } else if (sensors.origin === "simulated") {
    lines.push(
      "Temperatures below are a built-in sample scenario, not a live machine. Use them only as part of that scenario.",
    );
    lines.push(sensorLine(sensors));
  } else if (sensors.origin === "live") {
    lines.push(
      "Load and temperatures were read live by Diagnostic Chicken on this PC when Ask Grok was pressed. They are not a history.",
    );
    lines.push(sensorLine(sensors));
    if (sensors.notes.length) lines.push(`Agent notes: ${sensors.notes.join(" | ")}`);
  } else {
    lines.push("Temperatures are a snapshot collected on the user's PC, not a live stream.");
    lines.push(sensorLine(sensors));
    if (sensors.notes.length) lines.push(`Collector notes: ${sensors.notes.join(" | ")}`);
  }
  lines.push(`Local playbook matches: ${playbookTitles.join("; ") || "none"}`);
  if (!events.length) lines.push("Events: none in this report.");
  else {
    lines.push(`Events (${events.length}, showing up to 12):`);
    for (const event of events.slice(0, 12)) {
      const message = event.message.replace(/\s+/g, " ").slice(0, 380);
      lines.push(
        `- ${event.level} | ${event.log ?? "log unknown"} | ${event.time ?? "time unknown"} | ${event.source} | ID ${event.eventId} | ${message}`,
      );
    }
  }
  return lines.join("\n").slice(0, 20000);
}

function sensorLine(sensors: SensorSnapshot): string {
  return [
    `host=${sensors.host ?? "unknown"}`,
    `cpu=${sensors.cpuName ?? "unknown"}`,
    `cores=${sensors.cpuCores ?? "n/a"}`,
    `cpuLoad=${sensors.cpuLoad ?? "n/a"}`,
    `cpuTempC=${sensors.cpuTemp ?? "n/a"}`,
    `cpuTempSource=${sensors.cpuTempSource}`,
    `gpu=${sensors.gpuName ?? "unknown"}`,
    `gpuLoad=${sensors.gpuLoad ?? "n/a"}`,
    `gpuTempC=${sensors.gpuTemp ?? "n/a"}`,
    `gpuTempSource=${sensors.gpuTempSource}`,
  ].join(" ");
}
