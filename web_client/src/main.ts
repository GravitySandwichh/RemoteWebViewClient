import "./app.css";
import { CanvasRenderer } from "./canvasRenderer";
import { TouchType } from "./protocol";
import { RemoteWebViewBrowserClient } from "./wsClient";

type UiSettings = {
  server: string;
  width: number;
  height: number;
  fullFrameTileCount: number;
  scalePercent: number;
  url: string;
  quality: number;
  maxBytesPerMsg: number;
};

const STORAGE_KEY = "rwv-browser-client-settings";
const SCALE_OPTIONS = [50, 75, 100, 125, 150, 200];

const defaults: UiSettings = {
  server: "127.0.0.1:8081",
  width: 480,
  height: 480,
  fullFrameTileCount: 1,
  scalePercent: 100,
  url: "http://127.0.0.1:8123/",
  quality: 85,
  maxBytesPerMsg: 61440
};

const root = document.querySelector<HTMLDivElement>("#app");
if (!root) {
  throw new Error("Root container not found");
}

root.innerHTML = `
  <main class="layout">
    <section class="panel controls">
      <h1>Remote WebView Browser Client</h1>
      <label>Server
        <input id="server" type="text" placeholder="172.16.0.252:8081" />
      </label>
      <div class="row2">
        <label>Width
          <input id="width" type="number" min="1" step="1" />
        </label>
        <label>Height
          <input id="height" type="number" min="1" step="1" />
        </label>
      </div>
      <div class="row2">
        <label>JPEG q
          <input id="quality" type="number" min="1" max="100" />
        </label>
        <label>full_frame_tile_count
          <input id="fftc" type="number" min="1" step="1" />
        </label>
      </div>
      <div class="row2">
        <label>Max bytes/msg
          <input id="mbpm" type="number" min="1024" step="1024" />
        </label>
      </div>
      <div class="actions">
        <button id="connect">Connect</button>
        <button id="disconnect" class="ghost">Disconnect</button>
      </div>
      <label>Open URL
        <input id="url" type="text" placeholder="http://..." />
      </label>
      <div class="actions">
        <button id="openUrl">Send open_url</button>
      </div>
    </section>

    <section class="panel viewer">
      <canvas id="screen"></canvas>
      <div class="viewer-footer">
        <div class="metrics" id="metrics">status: idle</div>
        <select id="scale" class="scale-compact" aria-label="Scale"></select>
      </div>
      <div class="current-url-display" id="currentUrlDisplay">Current URL: —</div>
    </section>
  </main>
`;

const elServer = byId<HTMLInputElement>("server");
const elWidth = byId<HTMLInputElement>("width");
const elHeight = byId<HTMLInputElement>("height");
const elQuality = byId<HTMLInputElement>("quality");
const elFftc = byId<HTMLInputElement>("fftc");
const elMbpm = byId<HTMLInputElement>("mbpm");
const elScale = byId<HTMLSelectElement>("scale");
const elUrl = byId<HTMLInputElement>("url");
const elConnect = byId<HTMLButtonElement>("connect");
const elDisconnect = byId<HTMLButtonElement>("disconnect");
const elOpenUrl = byId<HTMLButtonElement>("openUrl");
const elCanvas = byId<HTMLCanvasElement>("screen");
const elMetrics = byId<HTMLDivElement>("metrics");
const elCurrentUrlDisplay = byId<HTMLDivElement>("currentUrlDisplay");

const settings = loadSettings();

elScale.innerHTML = SCALE_OPTIONS.map((value) => `<option value="${value}">${value}%</option>`).join("");

elServer.value = settings.server;
elWidth.value = String(settings.width);
elHeight.value = String(settings.height);
elQuality.value = String(settings.quality);
elFftc.value = String(settings.fullFrameTileCount);
elMbpm.value = String(settings.maxBytesPerMsg);
elScale.value = String(settings.scalePercent);
elUrl.value = settings.url;

const renderer = new CanvasRenderer(elCanvas, settings.width, settings.height);
applyDisplayScale(elCanvas, settings.width, settings.height, settings.scalePercent);

const client = new RemoteWebViewBrowserClient(renderer, {
  onMetrics(metrics) {
    elMetrics.textContent = `status: ${metrics.status} | frame: ${metrics.lastFrameId} | frames: ${metrics.frames} | bytes: ${metrics.bytes} | err: ${metrics.lastError}`;
  },
  onURL(url: string) {
    elCurrentUrlDisplay.textContent = `Current URL: ${url}`;
  }
});

elConnect.addEventListener("click", () => {
  const s = getSettingsFromUi();
  persistSettings(s);
  renderer.resize(s.width, s.height);
  applyDisplayScale(elCanvas, s.width, s.height, s.scalePercent);
  client.connect(s.server, {
    w: s.width,
    h: s.height,
    fftc: s.fullFrameTileCount,
    q: s.quality,
    mbpm: s.maxBytesPerMsg
  });
});

elScale.addEventListener("change", () => {
  const s = getSettingsFromUi();
  applyDisplayScale(elCanvas, s.width, s.height, s.scalePercent);
  persistSettings(s);
});

elDisconnect.addEventListener("click", () => {
  client.disconnect();
  elMetrics.textContent = "status: disconnected";
});

elOpenUrl.addEventListener("click", () => {
  const url = elUrl.value.trim();
  if (!url) {
    return;
  }

  if (!client.sendOpenUrl(url)) {
    elMetrics.textContent = "status: open_url failed (not connected)";
    return;
  }

  const s = getSettingsFromUi();
  s.url = url;
  persistSettings(s);
});

attachTouchHandlers(elCanvas, client);

function attachTouchHandlers(canvas: HTMLCanvasElement, remoteClient: RemoteWebViewBrowserClient): void {
  const map = (event: PointerEvent): { x: number; y: number } => {
    const rect = canvas.getBoundingClientRect();
    const x = ((event.clientX - rect.left) * canvas.width) / rect.width;
    const y = ((event.clientY - rect.top) * canvas.height) / rect.height;
    return { x: Math.round(x), y: Math.round(y) };
  };

  const onDown = (event: PointerEvent): void => {
    canvas.setPointerCapture(event.pointerId);
    const p = map(event);
    remoteClient.sendTouch(TouchType.Down, event.pointerId, p.x, p.y);
  };

  const onMove = (event: PointerEvent): void => {
    const p = map(event);
    remoteClient.sendTouch(TouchType.Move, event.pointerId, p.x, p.y);
  };

  const onUp = (event: PointerEvent): void => {
    const p = map(event);
    remoteClient.sendTouch(TouchType.Up, event.pointerId, p.x, p.y);
    if (canvas.hasPointerCapture(event.pointerId)) {
      canvas.releasePointerCapture(event.pointerId);
    }
  };

  canvas.addEventListener("pointerdown", onDown);
  canvas.addEventListener("pointermove", onMove);
  canvas.addEventListener("pointerup", onUp);
  canvas.addEventListener("pointercancel", onUp);
}

function getSettingsFromUi(): UiSettings {
  return {
    server: elServer.value.trim() || defaults.server,
    width: readPositiveInt(elWidth.value, defaults.width),
    height: readPositiveInt(elHeight.value, defaults.height),
    fullFrameTileCount: readPositiveInt(elFftc.value, defaults.fullFrameTileCount),
    scalePercent: readScalePercent(elScale.value, defaults.scalePercent),
    quality: readPositiveInt(elQuality.value, defaults.quality),
    maxBytesPerMsg: readPositiveInt(elMbpm.value, defaults.maxBytesPerMsg),
    url: elUrl.value.trim() || defaults.url
  };
}

function loadSettings(): UiSettings {
  try {
    const text = localStorage.getItem(STORAGE_KEY);
    if (!text) {
      return { ...defaults };
    }

    const parsed = JSON.parse(text) as Partial<UiSettings>;
    return {
      server: parsed.server ?? defaults.server,
      width: parsed.width ?? defaults.width,
      height: parsed.height ?? defaults.height,
      fullFrameTileCount: readPositiveInt(String(parsed.fullFrameTileCount ?? ""), defaults.fullFrameTileCount),
      scalePercent: readScalePercent(parsed.scalePercent, defaults.scalePercent),
      url: parsed.url ?? defaults.url,
      quality: parsed.quality ?? defaults.quality,
      maxBytesPerMsg: parsed.maxBytesPerMsg ?? defaults.maxBytesPerMsg
    };
  } catch {
    return { ...defaults };
  }
}

function persistSettings(settingsToStore: UiSettings): void {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(settingsToStore));
}

function readPositiveInt(value: string, fallback: number): number {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= 0) {
    return fallback;
  }
  return Math.round(n);
}

function readScalePercent(value: unknown, fallback: number): number {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= 0) {
    return fallback;
  }
  return Math.min(Math.max(Math.round(n), 10), 400);
}

function applyDisplayScale(canvas: HTMLCanvasElement, width: number, height: number, scalePercent: number): void {
  const scale = scalePercent / 100;
  canvas.style.width = `${Math.max(1, Math.round(width * scale))}px`;
  canvas.style.height = `${Math.max(1, Math.round(height * scale))}px`;
}

function byId<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) {
    throw new Error(`Missing element: ${id}`);
  }
  return node as T;
}
