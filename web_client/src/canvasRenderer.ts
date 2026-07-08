export class CanvasRenderer {
  private readonly canvas: HTMLCanvasElement;
  private readonly ctx: CanvasRenderingContext2D;

  constructor(canvas: HTMLCanvasElement, width: number, height: number) {
    this.canvas = canvas;
    this.canvas.width = width;
    this.canvas.height = height;

    const ctx = this.canvas.getContext("2d", { alpha: false });
    if (!ctx) {
      throw new Error("Cannot initialize 2D context");
    }

    this.ctx = ctx;
    this.clear();
  }

  resize(width: number, height: number): void {
    this.canvas.width = width;
    this.canvas.height = height;
    this.clear();
  }

  clear(): void {
    this.ctx.fillStyle = "#111";
    this.ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
  }

  async drawJpegTile(tileData: Uint8Array, x: number, y: number, w: number, h: number): Promise<void> {
    const bytes = tileData.slice();
    const blob = new Blob([bytes.buffer], { type: "image/jpeg" });
    const bitmap = await createImageBitmap(blob);
    this.ctx.drawImage(bitmap, 0, 0, bitmap.width, bitmap.height, x, y, w, h);
    bitmap.close();
  }
}
