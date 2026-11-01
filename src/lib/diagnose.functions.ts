import { createServerFn } from "@tanstack/react-start";
import type { Diagnosis, Severity } from "./sentinel/types";

export type DiagnoseResult =
  | { ok: true; diagnosis: Diagnosis }
  | { ok: true; diagnosis: null; raw: string }
  | { ok: false; error: string };

const SYSTEM = [
  "You are Diagnostic Chicken, a careful Windows desktop technician.",
  "The user pasted logs into a web desk that cannot see their computer.",
  "Use only the events and readings in the report.",
  "If temperatures are labeled simulated or missing, do not invent hardware temperatures.",
  "Say what is likely, what is uncertain, and what to check next.",
  "Prefer Microsoft documentation when you search.",
  "Do not tell the user to disable security features or to rewrite DCOM permissions for event 10016.",
  "Return one JSON object with keys:",
  "headline (string), severity (low|medium|high|critical), summary (string, max 80 words),",
  "likelyCauses (array of {title, why}, max 4), checks (string array, max 5),",
  "fixes (string array, max 5), doNot (string array, max 3), urls (https URL strings, max 4).",
  "No markdown.",
].join(" ");

export const diagnoseReport = createServerFn({ method: "POST" })
  .validator((input: unknown) => {
    if (!input || typeof input !== "object") throw new Error("Invalid request");
    const brief = (input as { brief?: unknown }).brief;
    const search = (input as { search?: unknown }).search;
    if (typeof brief !== "string" || brief.trim().length < 8) {
      throw new Error("Nothing to diagnose yet.");
    }
    if (brief.length > 24000) throw new Error("Report is too long. Keep the newest errors.");
    return { brief, search: search !== false };
  })
  .handler(async ({ data }): Promise<DiagnoseResult> => {
    const apiKey = process.env.XAI_API_KEY;
    if (!apiKey) {
      return {
        ok: false,
        error: "Grok is not available here. The local playbook above still applies.",
      };
    }

    const body: Record<string, unknown> = {
      model: "grok-4.5",
      temperature: 0.2,
      reasoning: { effort: "low" },
      max_output_tokens: 2200,
      input: [
        { role: "system", content: SYSTEM },
        { role: "user", content: data.brief },
      ],
      text: { format: { type: "json_object" } },
    };
    if (data.search) {
      body.tools = [{ type: "web_search" }];
      body.max_tool_calls = 2;
    }

    try {
      const response = await fetch("https://api.x.ai/v1/responses", {
        method: "POST",
        signal: AbortSignal.timeout(55_000),
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${apiKey}`,
        },
        body: JSON.stringify(body),
      });
      if (!response.ok) {
        const detail = (await response.text()).slice(0, 180);
        return { ok: false, error: `Grok could not read this report (${response.status}). ${detail}` };
      }
      const payload = (await response.json()) as unknown;
      const { text, urls } = extractText(payload);
      if (!text) return { ok: false, error: "Grok returned an empty read. Try again." };
      const diagnosis = parseDiagnosis(text, urls);
      if (!diagnosis) return { ok: true, diagnosis: null, raw: text };
      return { ok: true, diagnosis };
    } catch (error) {
      if (error instanceof Error && (error.name === "TimeoutError" || error.name === "AbortError")) {
        return { ok: false, error: "Grok took too long. Try again, or turn off web search." };
      }
      return { ok: false, error: "Could not reach Grok. The local playbook still applies." };
    }
  });

function extractText(payload: unknown): { text: string; urls: string[] } {
  const record = asRecord(payload);
  const output = Array.isArray(record?.output) ? record.output : [];
  const parts: string[] = [];
  const urls: string[] = [];
  for (const item of output) {
    const message = asRecord(item);
    if (message?.type !== "message" || !Array.isArray(message.content)) continue;
    for (const chunk of message.content) {
      const block = asRecord(chunk);
      if (typeof block?.text === "string") parts.push(block.text);
      if (!Array.isArray(block?.annotations)) continue;
      for (const note of block.annotations) {
        const url = asRecord(note)?.url;
        if (typeof url === "string") urls.push(url);
      }
    }
  }
  return { text: parts.join("\n").trim(), urls };
}

function parseDiagnosis(text: string, extraUrls: string[]): Diagnosis | null {
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start < 0 || end <= start) return null;
  try {
    return normalize(JSON.parse(text.slice(start, end + 1)) as unknown, extraUrls);
  } catch {
    return null;
  }
}

function normalize(value: unknown, extraUrls: string[]): Diagnosis | null {
  const record = asRecord(value);
  if (!record) return null;
  const summary = typeof record.summary === "string" ? record.summary.trim() : "";
  const headline =
    typeof record.headline === "string" && record.headline.trim()
      ? record.headline.trim()
      : summary.slice(0, 80);
  if (!headline && !summary) return null;
  const causes = Array.isArray(record.likelyCauses) ? record.likelyCauses : [];
  return {
    headline: headline || "Reading",
    severity: severity(record.severity),
    summary,
    likelyCauses: causes
      .map((item) => {
        const cause = asRecord(item);
        const title = typeof cause?.title === "string" ? cause.title.trim() : "";
        const why = typeof cause?.why === "string" ? cause.why.trim() : "";
        return title ? { title, why } : null;
      })
      .filter((item): item is { title: string; why: string } => item !== null)
      .slice(0, 4),
    checks: strings(record.checks, 5),
    fixes: strings(record.fixes, 5),
    doNot: strings(record.doNot, 3),
    urls: uniqueUrls([...strings(record.urls, 6), ...extraUrls]).slice(0, 4),
  };
}

function severity(value: unknown): Severity {
  if (value === "low" || value === "medium" || value === "high" || value === "critical") return value;
  return "medium";
}

function strings(value: unknown, max: number): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .map((item) => (typeof item === "string" ? item.trim() : ""))
    .filter(Boolean)
    .slice(0, max);
}

function uniqueUrls(values: string[]): string[] {
  const seen = new Set<string>();
  const urls: string[] = [];
  for (const value of values) {
    if (!/^https:\/\//i.test(value) || value.length > 300 || seen.has(value)) continue;
    seen.add(value);
    urls.push(value);
  }
  return urls;
}

function asRecord(value: unknown): Record<string, unknown> | null {
  if (value && typeof value === "object" && !Array.isArray(value)) return value as Record<string, unknown>;
  return null;
}
