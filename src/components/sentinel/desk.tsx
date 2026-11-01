import { useEffect, useRef, useState, type RefObject } from "react";
import { useServerFn } from "@tanstack/react-start";
import { Download, ScrollText, Stethoscope, Thermometer } from "lucide-react";
import { fetchDiagnostic, fetchLive } from "@/lib/sentinel/agent";
import { diagnoseReport, type DiagnoseResult } from "@/lib/diagnose.functions";
import { buildBrief } from "@/lib/sentinel/brief";
import { matchPlaybooks, type PlaybookSeverity } from "@/lib/sentinel/catalog";
import { parseReport } from "@/lib/sentinel/parse";
import { CASES } from "@/lib/sentinel/samples";
import type { Diagnosis, Level, SensorSnapshot, Severity, WinEvent } from "@/lib/sentinel/types";

type Tab = "bench" | "events" | "readout";

const TABS: { id: Tab; label: string }[] = [
  { id: "bench", label: "Install" },
  { id: "events", label: "Events" },
  { id: "readout", label: "Readout" },
];

const opening = CASES[0];

const blankReport: SensorSnapshot = {
  origin: "report",
  cpuLoad: null,
  cpuTemp: null,
  cpuTempSource: "unavailable",
  gpuLoad: null,
  gpuTemp: null,
  gpuTempSource: "unavailable",
  notes: ["This paste had events only. Temperatures were not included."],
};

export function Desk() {
  const ask = useServerFn(diagnoseReport);
  const fileRef = useRef<HTMLInputElement>(null);
  const [tab, setTab] = useState<Tab>("readout");
  const [caseId, setCaseId] = useState<string | null>(opening.id);
  const [events, setEvents] = useState<WinEvent[]>(opening.events);
  const [sensors, setSensors] = useState<SensorSnapshot>(opening.sensors);
  const [paste, setPaste] = useState("");
  const [parseError, setParseError] = useState<string | null>(null);
  const [search, setSearch] = useState(true);
  const [pending, setPending] = useState(false);
  const [result, setResult] = useState<DiagnoseResult | null>(null);
  const [tick, setTick] = useState(0);
  const [motion, setMotion] = useState(false);
  const [link, setLink] = useState<"live" | "off">("off");
  const [session, setSession] = useState(0);
  const [diagPending, setDiagPending] = useState(false);
  const [diagError, setDiagError] = useState<string | null>(null);

  useEffect(() => {
    const media = window.matchMedia("(prefers-reduced-motion: reduce)");
    if (media.matches || sensors.origin !== "simulated") {
      setMotion(false);
      return;
    }
    setMotion(true);
    const id = window.setInterval(() => setTick((value) => value + 1), 1400);
    return () => window.clearInterval(id);
  }, [sensors.origin]);

  useEffect(() => {
    if (session === 0 && window.sessionStorage.getItem("sentinel-agent") !== "1") return;
    let stop = false;
    let timer = 0;
    async function poll() {
      const reading = await fetchLive();
      if (stop) return;
      if (!reading) {
        setLink("off");
        window.sessionStorage.removeItem("sentinel-agent");
        return;
      }
      setLink("live");
      setSensors(reading);
      timer = window.setTimeout(() => void poll(), 2000);
    }
    void poll();
    return () => {
      stop = true;
      window.clearTimeout(timer);
    };
  }, [session]);

  const matches = matchPlaybooks(events);
  const activeCase = CASES.find((item) => item.id === caseId) ?? null;
  const cpuTemp = wobble(sensors.cpuTemp, tick, 0, motion);
  const gpuTemp = wobble(sensors.gpuTemp, tick, 1.4, motion);
  const cpuLoad = wobbleLoad(sensors.cpuLoad, tick, motion);

  function selectCase(id: string) {
    const sample = CASES.find((item) => item.id === id);
    if (!sample) return;
    setCaseId(sample.id);
    setEvents(sample.events);
    if (link !== "live") setSensors(sample.sensors);
    setParseError(null);
    setResult(null);
    setTab("readout");
  }

  function applyText(text: string) {
    if (text.includes("\u0000")) {
      setParseError(
        "This looks like a binary .evtx log. Event Viewer can save that view as XML, or run the collector.",
      );
      return;
    }
    const parsed = parseReport(text);
    if (!parsed.ok) {
      setParseError(parsed.error);
      return;
    }
    setCaseId(null);
    setEvents(parsed.events);
    if (link !== "live") setSensors(parsed.sensors ?? blankReport);
    setParseError(null);
    setResult(null);
    setPaste(text.slice(0, 24000));
    setTab(parsed.events.length ? "readout" : "bench");
  }

  function readFile(file: File) {
    const reader = new FileReader();
    reader.onload = () => {
      const text = String(reader.result ?? "");
      if (!text.includes("\u0000")) {
        applyText(text);
        return;
      }
      const retry = new FileReader();
      retry.onload = () => applyText(String(retry.result ?? ""));
      retry.readAsText(file, "utf-16le");
    };
    reader.readAsText(file);
  }

  async function onConnect() {
    setDiagError(null);
    const reading = await fetchLive();
    if (!reading) {
      setLink("off");
      setDiagError("Diagnostic Chicken.exe is not running on this PC. Open it, then try again.");
      return;
    }
    window.sessionStorage.setItem("sentinel-agent", "1");
    setSensors(reading);
    setLink("live");
    setSession((value) => value + 1);
  }

  async function onDiagnose() {
    if (diagPending) return;
    setDiagPending(true);
    setDiagError(null);
    try {
      if (link !== "live") {
        const reading = await fetchLive();
        if (!reading) {
          setDiagError("Diagnostic Chicken.exe is not running on this PC. Open it, then try again.");
          return;
        }
        window.sessionStorage.setItem("sentinel-agent", "1");
        setSensors(reading);
        setLink("live");
        setSession((value) => value + 1);
      }
      const parsed = parseReport(await fetchDiagnostic());
      if (!parsed.ok) {
        setDiagError(parsed.error);
        return;
      }
      setCaseId(null);
      setEvents(parsed.events);
      setResult(null);
      setTab("readout");
    } catch {
      setDiagError("The log check did not finish. Leave the Diagnostic Chicken window open and try again.");
    } finally {
      setDiagPending(false);
    }
  }

  async function onAsk() {
    if (pending) return;
    setPending(true);
    setResult(null);
    try {
      const brief = buildBrief(
        events,
        sensors,
        matches.map((hit) => hit.playbook.title),
      );
      const next = await ask({ data: { brief, search } });
      setResult(next);
    } catch {
      setResult({
        ok: false,
        error: "Could not reach Grok. The local playbook above still applies.",
      });
    } finally {
      setPending(false);
    }
  }

  return (
    <main className="mx-auto min-h-screen w-full max-w-3xl px-4 py-5 pb-16">
      <header className="mb-4 flex items-start justify-between gap-3">
        <div className="min-w-0">
          <p className="text-xs font-medium tracking-wide text-accent">Windows fault desk</p>
          <h1 className="mt-1 text-3xl font-semibold leading-tight text-fg">Diagnostic Chicken</h1>
        </div>
        <Stethoscope className="mt-1 size-6 text-accent" aria-hidden="true" />
      </header>

      <p className="mb-4 text-sm text-muted">
        {link === "live"
          ? "Connected to this PC. Names, load, and temperatures refresh while the Diagnostic Chicken window stays open."
          : "The desktop app reads this PC. It starts only when you open Diagnostic Chicken, and it stops when you close that window. Until then, the numbers here are a sample."}
      </p>

      <section className="panel mb-4 p-4" aria-label="Temperatures">
        <div className="flex items-center justify-between gap-3">
          <p className="font-mono text-xs tracking-wide text-muted">
            {sensors.host ?? "THIS REPORT"}
          </p>
          <OriginBadge origin={sensors.origin} link={link} />
        </div>
        <div className="mt-3 grid grid-cols-3 gap-2">
          <Meter
            label="CPU"
            value={cpuTemp}
            unit="°C"
            tone={tempTone(cpuTemp)}
            hint={sourceHint(sensors.origin, sensors.cpuTempSource)}
          />
          <Meter
            label="GPU"
            value={gpuTemp}
            unit="°C"
            tone={tempTone(gpuTemp)}
            hint={gpuHint(sensors)}
          />
          <Meter
            label="CPU load"
            value={cpuLoad}
            unit="%"
            tone={sensors.cpuLoad == null ? "muted" : "ok"}
            hint={sensors.origin === "live" ? "live" : sensors.origin === "simulated" ? "sample" : "snapshot"}
          />
        </div>
        {sensors.cpuName || sensors.gpuName ? (
          <p className="mt-3 text-xs text-muted">
            {[
              sensors.cpuName,
              sensors.cpuCores ? `${sensors.cpuCores} cores` : null,
              sensors.cpuMaxMhz ? `${sensors.cpuMaxMhz} MHz` : null,
            ]
              .filter(Boolean)
              .join(" · ")}
          </p>
        ) : null}
        {sensors.gpuName ? (
          <p className="text-xs text-muted">
            {[sensors.gpuName, sensors.gpuDriver ? `driver ${sensors.gpuDriver}` : null].filter(Boolean).join(" · ")}
          </p>
        ) : null}
        <div className="mt-4 flex flex-wrap gap-2">
          {link !== "live" ? (
            <a
              href="/Diagnostic%20Chicken.exe"
              download="Diagnostic Chicken.exe"
              className="inline-flex min-h-11 items-center gap-2 rounded-xl bg-accent px-4 text-sm font-medium text-accent-fg"
            >
              <Download className="size-4" aria-hidden="true" />
              Download Diagnostic Chicken.exe
            </a>
          ) : null}
          {link !== "live" ? (
            <button
              type="button"
              onClick={() => void onConnect()}
              className="min-h-11 rounded-xl bg-surface-2 px-4 text-sm font-medium text-fg"
            >
              Connect to this PC
            </button>
          ) : null}
          <button
            type="button"
            onClick={() => void onDiagnose()}
            disabled={diagPending}
            className="min-h-11 rounded-xl bg-accent px-4 text-sm font-medium text-accent-fg disabled:opacity-50"
          >
            {diagPending ? "Reading logs…" : "Run diagnostic"}
          </button>
        </div>
        {diagError ? <p className="mt-3 text-sm text-bad">{diagError}</p> : null}
        {link !== "live" ? (
          <p className="mt-3 text-sm text-muted">
            Run diagnostic reads critical, error, and warning events from the System and Application logs.
          </p>
        ) : null}
      </section>

      <div role="tablist" aria-label="Sections" className="mb-4 grid grid-cols-3 gap-1 rounded-xl bg-surface p-1">
        {TABS.map((item) => (
          <button
            key={item.id}
            type="button"
            role="tab"
            aria-selected={tab === item.id}
            className={`min-h-11 rounded-lg text-sm font-medium ${
              tab === item.id ? "bg-surface-2 text-fg" : "text-muted"
            }`}
            onClick={() => setTab(item.id)}
          >
            {item.label}
          </button>
        ))}
      </div>

      {tab === "bench" ? <Bench notes={sensors.notes} /> : null}
      {tab === "events" ? (
        <Events
          cases={caseId}
          paste={paste}
          error={parseError}
          events={events}
          fileRef={fileRef}
          onPaste={setPaste}
          onCase={selectCase}
          onParse={() => applyText(paste)}
          onFile={(file) => readFile(file)}
        />
      ) : null}
      {tab === "readout" ? (
        <Readout
          title={activeCase ? `Sample: ${activeCase.label}` : "Your report"}
          detail={activeCase?.detail}
          matches={matches}
          events={events}
          pending={pending}
          search={search}
          canAsk={events.length > 0 || sensors.cpuTemp != null || sensors.gpuTemp != null}
          result={result}
          onSearch={setSearch}
          onAsk={() => void onAsk()}
        />
      ) : null}
    </main>
  );
}

function OriginBadge({ origin, link }: { origin: SensorSnapshot["origin"]; link: "live" | "off" }) {
  if (link === "live" || origin === "live") {
    const on = link === "live";
    return (
      <span className={`rounded-full bg-surface-2 px-2 py-1 text-xs font-medium ${on ? "text-ok" : "text-warn"}`}>
        {on ? "Live" : "Agent paused"}
      </span>
    );
  }
  const simulated = origin === "simulated";
  return (
    <span
      className={`rounded-full bg-surface-2 px-2 py-1 text-xs font-medium ${simulated ? "text-warn" : "text-ok"}`}
    >
      {simulated ? "Simulated" : "Snapshot"}
    </span>
  );
}

function Meter({
  label,
  value,
  unit,
  tone,
  hint,
}: {
  label: string;
  value: number | null;
  unit: string;
  tone: "ok" | "warn" | "bad" | "muted";
  hint: string;
}) {
  const width = value == null ? 0 : Math.max(0, Math.min(100, unit === "°C" ? value : value));
  const toneClass = {
    ok: "text-ok",
    warn: "text-warn",
    bad: "text-bad",
    muted: "text-muted",
  }[tone];
  const barClass = {
    ok: "bg-ok",
    warn: "bg-warn",
    bad: "bg-bad",
    muted: "bg-line",
  }[tone];
  return (
    <div className="min-w-0">
      <p className="text-xs text-muted">{label}</p>
      <p className={`mt-1 font-mono text-2xl font-medium tabular-nums ${toneClass}`}>
        {value == null ? "—" : Math.round(value)}
        <span className="text-sm">{value == null ? "" : unit}</span>
      </p>
      <div className="mt-2 h-1.5 overflow-hidden rounded-full bg-surface-2">
        <div className={`h-full ${barClass}`} style={{ width: `${width}%` }} />
      </div>
      <p className="mt-1 truncate text-xs text-muted">{hint}</p>
    </div>
  );
}

function Bench({ notes }: { notes: string[] }) {
  return (
    <div className="space-y-4">
      <section className="panel p-4">
        <div className="flex items-center gap-2">
          <Thermometer className="size-4 text-accent" aria-hidden="true" />
          <h2 className="text-lg font-semibold">What cannot be done from here</h2>
        </div>
        <ul className="mt-3 space-y-3 text-sm text-muted">
          <li>This preview cannot see a PC. Open the Diagnostic Chicken desktop app on the computer you want to check.</li>
          <li>Diagnostic Chicken is a window, not a browser page. It starts only when you open it. Closing the window stops it. It is not a service and it does not start with Windows.</li>
          <li>
            CPU die temperature still needs a sensor driver. NVIDIA temperature comes from nvidia-smi.
            AMD and Intel need LibreHardwareMonitor with WMI enabled.
          </li>
          <li>It only listens on that PC. Other devices cannot connect to it.</li>
        </ul>
      </section>

      <section className="panel p-4">
        <h2 className="text-lg font-semibold">Install on the Windows PC</h2>
        <p className="mt-2 text-sm text-muted">
          Download Diagnostic Chicken.exe and open it. The program is its own window. The first launch adds a Start Menu shortcut.
          Nothing is added to Windows startup. Close the window when you are done.
        </p>
        <ol className="mt-3 list-decimal space-y-2 pl-5 text-sm text-muted">
          <li>This build is for 64-bit Windows.</li>
          <li>If the browser warns about an unsigned download, keep the file and open it.</li>
          <li>The window names the CPU and GPU, keeps load and temperature live, and Run diagnostic reads the System and Application logs.</li>
        </ol>
        <a
          href="/Diagnostic%20Chicken.exe"
          download="Diagnostic Chicken.exe"
          className="mt-4 inline-flex min-h-11 items-center gap-2 rounded-xl bg-accent px-4 text-sm font-medium text-accent-fg"
        >
          <Download className="size-4" aria-hidden="true" />
          Download Diagnostic Chicken.exe
        </a>
      </section>

      {notes.length ? (
        <section className="panel p-4">
          <h2 className="text-lg font-semibold">Notes on this reading</h2>
          <ul className="mt-3 space-y-2 text-sm text-muted">
            {notes.map((note) => (
              <li key={note}>{note}</li>
            ))}
          </ul>
        </section>
      ) : null}
    </div>
  );
}

function Events({
  cases,
  paste,
  error,
  events,
  fileRef,
  onPaste,
  onCase,
  onParse,
  onFile,
}: {
  cases: string | null;
  paste: string;
  error: string | null;
  events: WinEvent[];
  fileRef: RefObject<HTMLInputElement | null>;
  onPaste: (value: string) => void;
  onCase: (id: string) => void;
  onParse: () => void;
  onFile: (file: File) => void;
}) {
  return (
    <div className="space-y-4">
      <section className="panel p-4">
        <h2 className="text-lg font-semibold">Sample incidents</h2>
        <div className="mt-3 grid grid-cols-2 gap-2">
          {CASES.map((item) => (
            <button
              key={item.id}
              type="button"
              onClick={() => onCase(item.id)}
              className={`min-h-11 rounded-xl bg-surface-2 px-3 py-2 text-left ${
                cases === item.id ? "ring-2 ring-accent" : ""
              }`}
            >
              <span className="block text-sm font-medium text-fg">{item.label}</span>
              <span className="block text-xs text-muted">{item.detail}</span>
            </button>
          ))}
        </div>
      </section>

      <section
        className="panel p-4"
        onDragOver={(event) => event.preventDefault()}
        onDrop={(event) => {
          event.preventDefault();
          const file = event.dataTransfer.files[0];
          if (file) onFile(file);
        }}
      >
        <div className="flex items-center gap-2">
          <ScrollText className="size-4 text-accent" aria-hidden="true" />
          <h2 className="text-lg font-semibold">Your log</h2>
        </div>
        <label className="mt-3 block text-sm text-muted" htmlFor="log-paste">
          Paste Event Viewer details, XML, CSV, or the collector JSON.
        </label>
        <textarea
          id="log-paste"
          value={paste}
          onChange={(event) => onPaste(event.target.value)}
          rows={6}
          spellCheck={false}
          className="mt-2 w-full resize-y rounded-xl bg-surface-2 p-3 font-mono text-xs text-fg"
          placeholder="Log Name: System"
        />
        <div className="mt-3 flex flex-wrap gap-2">
          <button
            type="button"
            onClick={onParse}
            className="min-h-11 rounded-xl bg-accent px-4 text-sm font-medium text-accent-fg"
          >
            Read paste
          </button>
          <button
            type="button"
            onClick={() => fileRef.current?.click()}
            className="min-h-11 rounded-xl bg-surface-2 px-4 text-sm font-medium text-fg"
          >
            Open a file
          </button>
          <input
            ref={fileRef}
            type="file"
            accept=".xml,.csv,.json,.txt,.log,.evtx"
            className="sr-only"
            onChange={(event) => {
              const file = event.target.files?.[0];
              if (file) onFile(file);
              event.target.value = "";
            }}
          />
        </div>
        {error ? <p className="mt-3 text-sm text-bad">{error}</p> : null}
      </section>

      <section className="space-y-2" aria-label="Parsed events">
        <h2 className="text-lg font-semibold">{events.length ? `${events.length} events` : "No events yet"}</h2>
        {events.slice(0, 30).map((event) => (
          <article key={event.id} className="panel p-4">
            <div className="flex items-start justify-between gap-3">
              <p className="font-mono text-sm text-fg">
                {event.source} · {event.eventId}
              </p>
              <LevelPill level={event.level} />
            </div>
            <p className="mt-1 font-mono text-xs text-muted">
              {[event.log, shortTime(event.time)].filter(Boolean).join(" · ")}
            </p>
            <p className="mt-2 break-words text-sm text-muted">{event.message}</p>
          </article>
        ))}
        {events.length > 30 ? (
          <p className="text-sm text-muted">Showing the first 30. The readout still uses the newest 12 for Grok.</p>
        ) : null}
      </section>
    </div>
  );
}

function Readout({
  title,
  detail,
  matches,
  events,
  pending,
  search,
  canAsk,
  result,
  onSearch,
  onAsk,
}: {
  title: string;
  detail?: string;
  matches: ReturnType<typeof matchPlaybooks>;
  events: WinEvent[];
  pending: boolean;
  search: boolean;
  canAsk: boolean;
  result: DiagnoseResult | null;
  onSearch: (value: boolean) => void;
  onAsk: () => void;
}) {
  return (
    <div className="space-y-4">
      <div>
        <h2 className="text-lg font-semibold">{title}</h2>
        {detail ? <p className="text-sm text-muted">{detail}</p> : null}
      </div>

      {matches.length === 0 ? (
        <section className="panel p-4">
          <p className="text-sm text-muted">
            {events.length
              ? "No local entry for these IDs. Ask Grok to read the message text."
              : "No errors in this report. You can still ask Grok about the temperatures."}
          </p>
        </section>
      ) : (
        matches.map((hit) => <PlayCard key={hit.playbook.id} hit={hit} />)
      )}

      <section className="panel p-4">
        <h2 className="text-lg font-semibold">Ask Grok</h2>
        <p className="mt-2 text-sm text-muted">
          Sends this trimmed report to xAI. It is not stored here. Web search looks up public
          write-ups for the same errors. Local matches above do not need the network.
        </p>
        <label className="mt-3 flex min-h-11 items-center gap-2 text-sm text-fg">
          <input
            type="checkbox"
            checked={search}
            onChange={(event) => onSearch(event.target.checked)}
            className="size-4"
          />
          Cross-check on the web
        </label>
        <button
          type="button"
          onClick={onAsk}
          disabled={pending || !canAsk}
          className="mt-2 min-h-11 rounded-xl bg-accent px-4 text-sm font-medium text-accent-fg disabled:opacity-50"
        >
          {pending ? "Asking Grok…" : "Ask Grok"}
        </button>
        <div aria-live="polite" className="mt-4">
          {pending ? <p className="text-sm text-muted">Reading the log and, if asked, public write-ups.</p> : null}
          {result && !result.ok ? <p className="text-sm text-bad">{result.error}</p> : null}
          {result && result.ok && result.diagnosis ? <DiagnosisView diagnosis={result.diagnosis} /> : null}
          {result && result.ok && !result.diagnosis ? (
            <p className="break-words text-sm text-muted">{result.raw}</p>
          ) : null}
        </div>
      </section>
    </div>
  );
}

function PlayCard({ hit }: { hit: ReturnType<typeof matchPlaybooks>[number] }) {
  return (
    <article className="panel p-4">
      <div className="flex items-start justify-between gap-3">
        <h3 className="text-base font-semibold text-fg">{hit.playbook.title}</h3>
        <SeverityPill severity={hit.playbook.severity} count={hit.count} />
      </div>
      <p className="mt-2 text-sm text-muted">{hit.playbook.meaning}</p>
      <h4 className="mt-3 text-sm font-medium text-fg">Likely causes</h4>
      <ul className="mt-1 list-disc space-y-1 pl-5 text-sm text-muted">
        {hit.playbook.causes.map((cause) => (
          <li key={cause}>{cause}</li>
        ))}
      </ul>
      <h4 className="mt-3 text-sm font-medium text-fg">Checks</h4>
      <ul className="mt-1 list-disc space-y-1 pl-5 text-sm text-muted">
        {hit.playbook.checks.map((check) => (
          <li key={check}>{check}</li>
        ))}
      </ul>
    </article>
  );
}

function DiagnosisView({ diagnosis }: { diagnosis: Diagnosis }) {
  return (
    <div className="space-y-3">
      <div className="flex items-start justify-between gap-3">
        <h3 className="text-base font-semibold text-fg">{diagnosis.headline}</h3>
        <SeverityWord severity={diagnosis.severity} />
      </div>
      {diagnosis.summary ? <p className="text-sm text-muted">{diagnosis.summary}</p> : null}
      {diagnosis.likelyCauses.length ? (
        <div>
          <h4 className="text-sm font-medium text-fg">Grok’s read</h4>
          <ul className="mt-1 space-y-2">
            {diagnosis.likelyCauses.map((cause) => (
              <li key={cause.title} className="text-sm text-muted">
                <span className="text-fg">{cause.title}.</span> {cause.why}
              </li>
            ))}
          </ul>
        </div>
      ) : null}
      {diagnosis.checks.length ? <Bullet title="Check next" items={diagnosis.checks} /> : null}
      {diagnosis.fixes.length ? <Bullet title="If the check confirms it" items={diagnosis.fixes} /> : null}
      {diagnosis.doNot.length ? <Bullet title="Leave alone" items={diagnosis.doNot} /> : null}
      {diagnosis.urls.length ? (
        <ul className="space-y-1">
          {diagnosis.urls.map((url) => (
            <li key={url}>
              <a href={url} target="_blank" rel="noreferrer" className="break-all text-sm text-accent">
                {url.replace(/^https:\/\//, "")}
              </a>
            </li>
          ))}
        </ul>
      ) : null}
    </div>
  );
}

function Bullet({ title, items }: { title: string; items: string[] }) {
  return (
    <div>
      <h4 className="text-sm font-medium text-fg">{title}</h4>
      <ul className="mt-1 list-disc space-y-1 pl-5 text-sm text-muted">
        {items.map((item) => (
          <li key={item}>{item}</li>
        ))}
      </ul>
    </div>
  );
}

function LevelPill({ level }: { level: Level }) {
  const tone =
    level === "Critical" || level === "Error" ? "text-bad" : level === "Warning" ? "text-warn" : "text-ok";
  return <span className={`shrink-0 text-xs font-medium ${tone}`}>{level}</span>;
}

function SeverityPill({ severity, count }: { severity: PlaybookSeverity; count: number }) {
  const label = { noise: "Noise", watch: "Watch", serious: "Serious", urgent: "Urgent" }[severity];
  const tone = {
    noise: "text-muted",
    watch: "text-warn",
    serious: "text-warn",
    urgent: "text-bad",
  }[severity];
  return (
    <span className={`shrink-0 text-xs font-medium ${tone}`}>
      {label}
      {count > 1 ? ` · ${count}` : ""}
    </span>
  );
}

function SeverityWord({ severity }: { severity: Severity }) {
  const tone = {
    low: "text-ok",
    medium: "text-warn",
    high: "text-warn",
    critical: "text-bad",
  }[severity];
  return <span className={`shrink-0 text-xs font-medium uppercase ${tone}`}>{severity}</span>;
}

function sourceHint(origin: SensorSnapshot["origin"], source: SensorSnapshot["cpuTempSource"]): string {
  if (origin === "simulated") return "sample";
  if (origin === "live" && source === "librehardwaremonitor") return "live · sensor driver";
  if (origin === "live" && source === "nvidia-smi") return "live · nvidia-smi";
  if (origin === "live" && source === "acpi-thermal-zone") return "live · ACPI zone";
  if (origin === "live" && source === "unavailable") return "live · no sensor";
  if (origin === "live") return "live";
  if (source === "librehardwaremonitor") return "sensor driver";
  if (source === "nvidia-smi") return "nvidia-smi";
  if (source === "acpi-thermal-zone") return "ACPI zone";
  if (source === "unavailable") return "not collected";
  return "report";
}

function gpuHint(sensors: SensorSnapshot): string {
  const base = sourceHint(sensors.origin, sensors.gpuTempSource);
  if (sensors.gpuLoad == null) return base;
  return `${base} · ${sensors.gpuLoad}%`;
}

function tempTone(value: number | null): "ok" | "warn" | "bad" | "muted" {
  if (value == null) return "muted";
  if (value >= 90) return "bad";
  if (value >= 80) return "warn";
  return "ok";
}

function wobble(base: number | null, tick: number, phase: number, motion: boolean): number | null {
  if (base == null || !motion) return base;
  return Math.round((base + Math.sin(tick / 2 + phase) * 0.6) * 10) / 10;
}

function wobbleLoad(base: number | null, tick: number, motion: boolean): number | null {
  if (base == null || !motion) return base;
  return Math.max(0, Math.min(100, Math.round(base + Math.sin(tick / 2) * 1.5)));
}

function shortTime(value?: string): string | undefined {
  if (!value) return undefined;
  return value.replace("T", " ").replace(/\.\d+Z$/, " UTC").replace(/Z$/, " UTC");
}
