export const PROTOCOL_VERSION = 1;
export const FLAG_LAST_OF_FRAME = 1 << 0;

export enum MsgType {
  Unknown = 0,
  Frame = 1,
  Touch = 2,
  FrameStats = 3,
  OpenURL = 4,
  Keepalive = 5,
  CurrentURL = 6
}

export enum Encoding {
  Unknown = 0,
  PNG = 1,
  JPEG = 2,
  RAW565 = 3,
  RAW565_RLE = 4,
  RAW565_LZ4 = 5
}

export enum TouchType {
  Unknown = 0,
  Down = 1,
  Move = 2,
  Up = 3
}

export type QueryOptions = {
  id?: string;
  w: number;
  h: number;
  r?: number;
  ts?: number;
  fftc?: number;
  ffat?: number;
  ffe?: number;
  enf?: number;
  mfi?: number;
  q?: number;
  mbpm?: number;
};

export type FrameInfo = {
  frameId: number;
  encoding: Encoding;
  tileCount: number;
  flags: number;
};

export type TileInfo = {
  x: number;
  y: number;
  w: number;
  h: number;
  dlen: number;
  data: Uint8Array;
};

export type ParsedFrame = {
  header: FrameInfo;
  tiles: TileInfo[];
};

export type CurrentURLPacket = {
  url: string;
};

const FRAME_HEADER_SIZE = 11;
const TILE_HEADER_SIZE = 12;
const CURRENT_URL_HEADER_SIZE = 6;

export function buildWsUri(server: string, opts: QueryOptions): string {
  const serverNormalized = server.startsWith("ws://") || server.startsWith("wss://") ? server : `ws://${server}`;
  const base = new URL(serverNormalized.includes("/") ? serverNormalized : `${serverNormalized}/`);

  const entries: Array<[string, string]> = [
    ["id", opts.id ?? `browser-${crypto.randomUUID().slice(0, 8)}`],
    ["w", String(opts.w)],
    ["h", String(opts.h)]
  ];

  appendOptional(entries, "r", opts.r);
  appendOptional(entries, "ts", opts.ts);
  appendOptional(entries, "fftc", opts.fftc);
  appendOptional(entries, "ffat", opts.ffat);
  appendOptional(entries, "ffe", opts.ffe);
  appendOptional(entries, "enf", opts.enf);
  appendOptional(entries, "mfi", opts.mfi);
  appendOptional(entries, "q", opts.q);
  appendOptional(entries, "mbpm", opts.mbpm);

  for (const [k, v] of entries) {
    base.searchParams.set(k, v);
  }

  return base.toString();
}

function appendOptional(entries: Array<[string, string]>, key: string, value: number | undefined): void {
  if (value === undefined || Number.isNaN(value)) {
    return;
  }
  entries.push([key, String(value)]);
}

export function parseCurrentURLPacket(buffer: ArrayBuffer): CurrentURLPacket | null {
  if (buffer.byteLength < CURRENT_URL_HEADER_SIZE) {
    return null;
  }

  const view = new DataView(buffer);
  const urlLen = view.getUint32(2, true); // Read 4-byte length at offset 2 (little-endian)

  if (buffer.byteLength < CURRENT_URL_HEADER_SIZE + urlLen) {
    return null;
  }

  const urlBytes = new Uint8Array(buffer, CURRENT_URL_HEADER_SIZE, urlLen);
  const url = new TextDecoder().decode(urlBytes);

  return { url };
}

export function parseFramePacket(buffer: ArrayBuffer): ParsedFrame | null {
  if (buffer.byteLength < FRAME_HEADER_SIZE) {
    return null;
  }

  const view = new DataView(buffer);
  const type = view.getUint8(0);
  const version = view.getUint8(1);

  if (type !== MsgType.Frame || version !== PROTOCOL_VERSION) {
    return null;
  }

  const header: FrameInfo = {
    frameId: view.getUint32(2, true),
    encoding: view.getUint8(6),
    tileCount: view.getUint16(7, true),
    flags: view.getUint16(9, true)
  };

  let offset = FRAME_HEADER_SIZE;
  const tiles: TileInfo[] = [];

  for (let i = 0; i < header.tileCount; i += 1) {
    if (offset + TILE_HEADER_SIZE > buffer.byteLength) {
      return null;
    }

    const x = view.getUint16(offset, true);
    const y = view.getUint16(offset + 2, true);
    const w = view.getUint16(offset + 4, true);
    const h = view.getUint16(offset + 6, true);
    const dlen = view.getUint32(offset + 8, true);
    offset += TILE_HEADER_SIZE;

    if (offset + dlen > buffer.byteLength) {
      return null;
    }

    const data = new Uint8Array(buffer, offset, dlen);
    offset += dlen;

    tiles.push({ x, y, w, h, dlen, data });
  }

  return { header, tiles };
}

export function buildTouchPacket(type: TouchType, pointerId: number, x: number, y: number): Uint8Array {
  const out = new Uint8Array(8);
  const view = new DataView(out.buffer);
  view.setUint8(0, MsgType.Touch);
  view.setUint8(1, PROTOCOL_VERSION);
  view.setUint8(2, type);
  view.setUint8(3, pointerId & 0xff);
  view.setUint16(4, clampU16(x), true);
  view.setUint16(6, clampU16(y), true);
  return out;
}

export function buildOpenUrlPacket(url: string, flags = 0): Uint8Array {
  const payload = new TextEncoder().encode(url);
  const out = new Uint8Array(8 + payload.length);
  const view = new DataView(out.buffer);
  view.setUint8(0, MsgType.OpenURL);
  view.setUint8(1, PROTOCOL_VERSION);
  view.setUint16(2, flags, true);
  view.setUint32(4, payload.length, true);
  out.set(payload, 8);
  return out;
}

export function buildKeepalivePacket(): Uint8Array {
  const out = new Uint8Array(2);
  out[0] = MsgType.Keepalive;
  out[1] = PROTOCOL_VERSION;
  return out;
}

function clampU16(v: number): number {
  if (v < 0) {
    return 0;
  }
  if (v > 65535) {
    return 65535;
  }
  return v | 0;
}
