import {
  buildKeepalivePacket,
  buildOpenUrlPacket,
  buildTouchPacket,
  buildWsUri,
  Encoding,
  FLAG_LAST_OF_FRAME,
  MsgType,
  parseFramePacket,
  parseCurrentURLPacket,
  QueryOptions,
  TouchType
} from "./protocol";
import { CanvasRenderer } from "./canvasRenderer";

const KEEPALIVE_INTERVAL_MS = 60_000;
const MOVE_INTERVAL_MS = 1000 / 60;

type Metrics = {
  status: string;
  frames: number;
  bytes: number;
  lastFrameId: number;
  lastError: string;
};

type Handlers = {
  onMetrics: (metrics: Metrics) => void;
  onURL: (url: string) => void;
};

export class RemoteWebViewBrowserClient {
  private ws: WebSocket | null = null;
  private readonly renderer: CanvasRenderer;
  private readonly handlers: Handlers;

  private readonly inboundQueue: ArrayBuffer[] = [];
  private processing = false;
  private reconnectDelayMs = 1000;

  private maxBytesPerMsg = 64 * 1024;
  private keepaliveId: number | null = null;
  private reconnectId: number | null = null;

  private frames = 0;
  private bytes = 0;
  private lastFrameId = -1;
  private lastError = "-";

  private lastMoveAt = 0;

  constructor(renderer: CanvasRenderer, handlers: Handlers) {
    this.renderer = renderer;
    this.handlers = handlers;
  }

  connect(server: string, options: QueryOptions): void {
    this.disconnect();

    if (options.mbpm && options.mbpm > 0) {
      this.maxBytesPerMsg = options.mbpm;
    }

    const uri = buildWsUri(server, options);
    const ws = new WebSocket(uri);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      this.ws = ws;
      this.reconnectDelayMs = 1000;
      this.startKeepalive();
      this.pushMetrics("connected");
    };

    ws.onclose = () => {
      this.stopKeepalive();
      this.pushMetrics("disconnected");
      this.scheduleReconnect(server, options);
    };

    ws.onerror = () => {
      this.lastError = "websocket error";
      this.pushMetrics("error");
    };

    ws.onmessage = (event) => {
      if (!(event.data instanceof ArrayBuffer)) {
        return;
      }

      if (event.data.byteLength > this.maxBytesPerMsg) {
        this.lastError = `message too large: ${event.data.byteLength}`;
        this.pushMetrics("warning");
        return;
      }

      this.bytes += event.data.byteLength;
      this.inboundQueue.push(event.data);
      this.drainQueue();
    };
  }

  disconnect(): void {
    if (this.reconnectId !== null) {
      clearTimeout(this.reconnectId);
      this.reconnectId = null;
    }

    this.stopKeepalive();

    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
      this.ws = null;
    }

    this.inboundQueue.length = 0;
    this.processing = false;
  }

  sendOpenUrl(url: string): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return false;
    }

    const payload = buildOpenUrlPacket(url);
    this.ws.send(payload);
    return true;
  }

  sendTouch(type: TouchType, pointerId: number, x: number, y: number): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return;
    }

    const now = performance.now();
    if (type === TouchType.Move && now - this.lastMoveAt < MOVE_INTERVAL_MS) {
      return;
    }

    if (type === TouchType.Move) {
      this.lastMoveAt = now;
    }

    const payload = buildTouchPacket(type, pointerId, x, y);
    this.ws.send(payload);
  }

  private scheduleReconnect(server: string, options: QueryOptions): void {
    if (this.reconnectId !== null) {
      clearTimeout(this.reconnectId);
    }

    this.reconnectId = window.setTimeout(() => {
      this.connect(server, options);
    }, this.reconnectDelayMs);

    this.reconnectDelayMs = Math.min(this.reconnectDelayMs * 2, 15_000);
  }

  private startKeepalive(): void {
    this.stopKeepalive();
    this.keepaliveId = window.setInterval(() => {
      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(buildKeepalivePacket());
      }
    }, KEEPALIVE_INTERVAL_MS);
  }

  private stopKeepalive(): void {
    if (this.keepaliveId !== null) {
      clearInterval(this.keepaliveId);
      this.keepaliveId = null;
    }
  }

  private async drainQueue(): Promise<void> {
    if (this.processing) {
      return;
    }

    this.processing = true;
    try {
      while (this.inboundQueue.length > 0) {
        const buffer = this.inboundQueue.shift() as ArrayBuffer;
        await this.processPacket(buffer);
      }
    } finally {
      this.processing = false;
    }
  }

  private async processPacket(buffer: ArrayBuffer): Promise<void> {
    const view = new DataView(buffer);
    const type = view.getUint8(0);

    if (type === MsgType.Frame) {
      const parsed = parseFramePacket(buffer);
      if (!parsed) {
        this.lastError = "bad frame packet";
        this.pushMetrics("warning");
        return;
      }

      if (parsed.header.encoding !== Encoding.JPEG) {
        this.lastError = `unsupported encoding: ${parsed.header.encoding}`;
        this.pushMetrics("warning");
        return;
      }

      for (const tile of parsed.tiles) {
        if (tile.w === 0 || tile.h === 0) {
          continue;
        }
        await this.renderer.drawJpegTile(tile.data, tile.x, tile.y, tile.w, tile.h);
      }

      if (parsed.header.flags & FLAG_LAST_OF_FRAME) {
        this.frames += 1;
      }
      this.lastFrameId = parsed.header.frameId;
      this.pushMetrics("connected");
      return;
    }

    if (type === MsgType.FrameStats) {
      this.pushMetrics("connected");
      return;
    }

    if (type === MsgType.CurrentURL) {
      const parsed = parseCurrentURLPacket(buffer);
      if (!parsed) {
        this.lastError = "bad current URL packet";
        this.pushMetrics("warning");
        return;
      }
      this.handlers.onURL(parsed.url);
      return;
    }

    this.lastError = `unknown packet type: ${type}`;
    this.pushMetrics("warning");
  }

  private pushMetrics(status: string): void {
    this.handlers.onMetrics({
      status,
      frames: this.frames,
      bytes: this.bytes,
      lastFrameId: this.lastFrameId,
      lastError: this.lastError
    });
  }
}
