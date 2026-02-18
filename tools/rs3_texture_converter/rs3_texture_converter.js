#!/usr/bin/env node
/*
  rs3_texture_converter.js
  Converts textures used by an rs3_scene_v1 package to PNG (package-local).
*/

const fs = require("fs");
const path = require("path");
const zlib = require("zlib");
const crypto = require("crypto");
const childProcess = require("child_process");

const DDS_MAGIC = 0x20534444; // "DDS "
const DDPF_ALPHAPIXELS = 0x1;
const DDPF_FOURCC = 0x4;
const DDPF_RGB = 0x40;

const FOURCC_DXT1 = 0x31545844; // DXT1
const FOURCC_DXT3 = 0x33545844; // DXT3
const FOURCC_DXT5 = 0x35545844; // DXT5
const FOURCC_DX10 = 0x30315844; // DX10

function parseArgs(argv) {
  const args = {};
  for (let i = 2; i < argv.length; i++) {
    const token = argv[i];
    if (!token.startsWith("--")) continue;
    const key = token.slice(2);
    const next = argv[i + 1];
    if (next && !next.startsWith("--")) {
      args[key] = next;
      i++;
    } else {
      args[key] = true;
    }
  }
  return args;
}

function ensureDir(dirPath) {
  fs.mkdirSync(dirPath, { recursive: true });
}

function normalizeSlash(v) {
  return String(v || "").replace(/\\/g, "/");
}

function toSha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex");
}

function slugify(name) {
  return String(name || "")
    .normalize("NFKD")
    .replace(/[^a-zA-Z0-9._ -]/g, "")
    .trim()
    .replace(/[\s.]+/g, "_")
    .replace(/_+/g, "_")
    .toLowerCase() || "tex";
}

class Reader {
  constructor(buffer) {
    this.buf = buffer;
    this.off = 0;
  }

  remaining() {
    return this.buf.length - this.off;
  }

  ensure(size) {
    if (this.off + size > this.buf.length) {
      throw new Error(`Unexpected EOF at ${this.off} (need ${size} bytes)`);
    }
  }

  skip(size) {
    this.ensure(size);
    this.off += size;
  }

  u32() {
    this.ensure(4);
    const v = this.buf.readUInt32LE(this.off);
    this.off += 4;
    return v;
  }

  i32() {
    this.ensure(4);
    const v = this.buf.readInt32LE(this.off);
    this.off += 4;
    return v;
  }

  f32() {
    this.ensure(4);
    const v = this.buf.readFloatLE(this.off);
    this.off += 4;
    return v;
  }

  bytes(size) {
    this.ensure(size);
    const out = this.buf.slice(this.off, this.off + size);
    this.off += size;
    return out;
  }

  str() {
    const len = this.u32();
    if (len === 0) return "";
    const payload = this.bytes(len);
    return payload.toString("utf8");
  }
}

class Writer {
  constructor() {
    this.parts = [];
  }

  push(buf) {
    this.parts.push(buf);
  }

  u32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeUInt32LE(v >>> 0, 0);
    this.push(b);
  }

  str(v) {
    const payload = Buffer.from(String(v || ""), "utf8");
    this.u32(payload.length);
    if (payload.length > 0) this.push(payload);
  }

  bytes(buf) {
    this.push(buf);
  }

  finish() {
    return Buffer.concat(this.parts);
  }
}

function parseWorldBin(buffer) {
  const r = new Reader(buffer);

  const magic = r.bytes(8);
  const expectedMagic = Buffer.from([0x52, 0x53, 0x33, 0x53, 0x43, 0x4e, 0x31, 0x00]);
  if (!magic.equals(expectedMagic)) {
    throw new Error("world.bin magic mismatch");
  }

  const version = r.u32();
  if (version !== 1) {
    throw new Error(`world.bin version mismatch: ${version}`);
  }

  const vertexCount = r.u32();
  const indexCount = r.u32();
  const materialCount = r.u32();
  const sectionCount = r.u32();
  const lightCount = r.u32();

  // camera/spawn + fog + bounds
  r.skip((6 * 3 * 4) + (2 * 4) + (3 * 4) + 4 + (2 * 3 * 4));

  const materialStartOffset = r.off;
  const materials = [];
  for (let i = 0; i < materialCount; i++) {
    const flags = r.u32();
    const diffuseMap = r.str();
    materials.push({ flags, diffuseMap });
  }
  const materialEndOffset = r.off;

  // lights (float3 + float3 + 3 floats)
  r.skip(lightCount * ((3 * 4) + (3 * 4) + (3 * 4)));

  const sections = [];
  for (let i = 0; i < sectionCount; i++) {
    const materialIndex = r.u32();
    const indexStart = r.u32();
    const indexCountSec = r.u32();
    sections.push({ materialIndex, indexStart, indexCount: indexCountSec });
  }

  // Tail starts immediately after material list and includes lights+sections+vertices+indices.
  const tailBytes = buffer.slice(materialEndOffset);
  const headerBytes = buffer.slice(0, materialStartOffset);

  return {
    counts: {
      vertexCount,
      indexCount,
      materialCount,
      sectionCount,
      lightCount
    },
    headerBytes,
    tailBytes,
    materials,
    sections
  };
}

function writeWorldBinParsed(parsed) {
  const w = new Writer();
  w.bytes(parsed.headerBytes);
  for (const mat of parsed.materials) {
    w.u32(mat.flags >>> 0);
    w.str(mat.diffuseMap || "");
  }
  w.bytes(parsed.tailBytes);
  return w.finish();
}

function color565ToRgb(c) {
  const r = (c >> 11) & 0x1f;
  const g = (c >> 5) & 0x3f;
  const b = c & 0x1f;
  return [
    Math.round((r * 255) / 31),
    Math.round((g * 255) / 63),
    Math.round((b * 255) / 31)
  ];
}

function writeRgba(dst, width, x, y, rgba) {
  const off = (y * width + x) * 4;
  dst[off + 0] = rgba[0];
  dst[off + 1] = rgba[1];
  dst[off + 2] = rgba[2];
  dst[off + 3] = rgba[3];
}

function decodeBC1ColorBlock(block, forceFourColor) {
  const c0 = block.readUInt16LE(0);
  const c1 = block.readUInt16LE(2);
  const iBits = block.readUInt32LE(4);

  const rgb0 = color565ToRgb(c0);
  const rgb1 = color565ToRgb(c1);

  const palette = [
    [rgb0[0], rgb0[1], rgb0[2], 255],
    [rgb1[0], rgb1[1], rgb1[2], 255],
    [0, 0, 0, 255],
    [0, 0, 0, 255]
  ];

  if (c0 > c1 || forceFourColor) {
    palette[2] = [
      Math.round((2 * rgb0[0] + rgb1[0]) / 3),
      Math.round((2 * rgb0[1] + rgb1[1]) / 3),
      Math.round((2 * rgb0[2] + rgb1[2]) / 3),
      255
    ];
    palette[3] = [
      Math.round((rgb0[0] + 2 * rgb1[0]) / 3),
      Math.round((rgb0[1] + 2 * rgb1[1]) / 3),
      Math.round((rgb0[2] + 2 * rgb1[2]) / 3),
      255
    ];
  } else {
    palette[2] = [
      Math.round((rgb0[0] + rgb1[0]) / 2),
      Math.round((rgb0[1] + rgb1[1]) / 2),
      Math.round((rgb0[2] + rgb1[2]) / 2),
      255
    ];
    palette[3] = [0, 0, 0, 0];
  }

  const out = new Array(16);
  for (let i = 0; i < 16; i++) {
    const idx = (iBits >> (2 * i)) & 0x3;
    out[i] = palette[idx];
  }
  return out;
}

function decodeBC1(width, height, data) {
  const out = Buffer.alloc(width * height * 4, 0);
  const blockWidth = Math.ceil(width / 4);
  const blockHeight = Math.ceil(height / 4);

  let off = 0;
  for (let by = 0; by < blockHeight; by++) {
    for (let bx = 0; bx < blockWidth; bx++) {
      const block = data.slice(off, off + 8);
      off += 8;
      const pixels = decodeBC1ColorBlock(block, false);
      for (let py = 0; py < 4; py++) {
        for (let px = 0; px < 4; px++) {
          const x = bx * 4 + px;
          const y = by * 4 + py;
          if (x >= width || y >= height) continue;
          writeRgba(out, width, x, y, pixels[py * 4 + px]);
        }
      }
    }
  }

  return out;
}

function decodeBC2(width, height, data) {
  const out = Buffer.alloc(width * height * 4, 0);
  const blockWidth = Math.ceil(width / 4);
  const blockHeight = Math.ceil(height / 4);

  let off = 0;
  for (let by = 0; by < blockHeight; by++) {
    for (let bx = 0; bx < blockWidth; bx++) {
      const alpha = data.slice(off, off + 8);
      const colorBlock = data.slice(off + 8, off + 16);
      off += 16;

      const colors = decodeBC1ColorBlock(colorBlock, true);
      for (let i = 0; i < 16; i++) {
        const byte = alpha[Math.floor(i / 2)];
        const nibble = (i % 2 === 0) ? (byte & 0x0f) : ((byte >> 4) & 0x0f);
        colors[i] = [colors[i][0], colors[i][1], colors[i][2], (nibble << 4) | nibble];
      }

      for (let py = 0; py < 4; py++) {
        for (let px = 0; px < 4; px++) {
          const x = bx * 4 + px;
          const y = by * 4 + py;
          if (x >= width || y >= height) continue;
          writeRgba(out, width, x, y, colors[py * 4 + px]);
        }
      }
    }
  }

  return out;
}

function buildAlphaPaletteBC3(a0, a1) {
  const p = new Array(8).fill(0);
  p[0] = a0;
  p[1] = a1;
  if (a0 > a1) {
    p[2] = Math.floor((6 * a0 + 1 * a1 + 3) / 7);
    p[3] = Math.floor((5 * a0 + 2 * a1 + 3) / 7);
    p[4] = Math.floor((4 * a0 + 3 * a1 + 3) / 7);
    p[5] = Math.floor((3 * a0 + 4 * a1 + 3) / 7);
    p[6] = Math.floor((2 * a0 + 5 * a1 + 3) / 7);
    p[7] = Math.floor((1 * a0 + 6 * a1 + 3) / 7);
  } else {
    p[2] = Math.floor((4 * a0 + 1 * a1 + 2) / 5);
    p[3] = Math.floor((3 * a0 + 2 * a1 + 2) / 5);
    p[4] = Math.floor((2 * a0 + 3 * a1 + 2) / 5);
    p[5] = Math.floor((1 * a0 + 4 * a1 + 2) / 5);
    p[6] = 0;
    p[7] = 255;
  }
  return p;
}

function decodeBC3(width, height, data) {
  const out = Buffer.alloc(width * height * 4, 0);
  const blockWidth = Math.ceil(width / 4);
  const blockHeight = Math.ceil(height / 4);

  let off = 0;
  for (let by = 0; by < blockHeight; by++) {
    for (let bx = 0; bx < blockWidth; bx++) {
      const a0 = data[off + 0];
      const a1 = data[off + 1];
      const alphaBits = data.slice(off + 2, off + 8);
      const colorBlock = data.slice(off + 8, off + 16);
      off += 16;

      let bits = 0n;
      for (let i = 0; i < 6; i++) {
        bits |= BigInt(alphaBits[i]) << BigInt(8 * i);
      }

      const alphaPalette = buildAlphaPaletteBC3(a0, a1);
      const colors = decodeBC1ColorBlock(colorBlock, true);

      for (let i = 0; i < 16; i++) {
        const aIdx = Number((bits >> BigInt(3 * i)) & 0x7n);
        const alpha = alphaPalette[aIdx] & 0xff;
        colors[i] = [colors[i][0], colors[i][1], colors[i][2], alpha];
      }

      for (let py = 0; py < 4; py++) {
        for (let px = 0; px < 4; px++) {
          const x = bx * 4 + px;
          const y = by * 4 + py;
          if (x >= width || y >= height) continue;
          writeRgba(out, width, x, y, colors[py * 4 + px]);
        }
      }
    }
  }

  return out;
}

function maskShift(mask) {
  if (mask === 0) return 0;
  let shift = 0;
  while (((mask >>> shift) & 0x1) === 0 && shift < 32) shift++;
  return shift;
}

function maskValueMax(mask, shift) {
  if (mask === 0) return 0;
  return (mask >>> shift);
}

function extractMasked(v, mask) {
  if (!mask) return 0;
  const shift = maskShift(mask >>> 0);
  const max = maskValueMax(mask >>> 0, shift);
  if (!max) return 0;
  const n = (v & mask) >>> shift;
  if (max === 255) return n;
  return Math.round((n * 255) / max);
}

function decodeDDS(fileBuffer) {
  if (fileBuffer.length < 128) {
    throw new Error("DDS file too small");
  }

  const magic = fileBuffer.readUInt32LE(0);
  if (magic !== DDS_MAGIC) {
    throw new Error("Not a DDS file");
  }

  const headerSize = fileBuffer.readUInt32LE(4);
  if (headerSize !== 124) {
    throw new Error(`Unsupported DDS header size: ${headerSize}`);
  }

  const height = fileBuffer.readUInt32LE(12);
  const width = fileBuffer.readUInt32LE(16);
  const pitchOrLinear = fileBuffer.readUInt32LE(20);

  const ddpfFlags = fileBuffer.readUInt32LE(80);
  const ddpfFourCC = fileBuffer.readUInt32LE(84);
  const ddpfRgbBits = fileBuffer.readUInt32LE(88);
  const ddpfRMask = fileBuffer.readUInt32LE(92);
  const ddpfGMask = fileBuffer.readUInt32LE(96);
  const ddpfBMask = fileBuffer.readUInt32LE(100);
  const ddpfAMask = fileBuffer.readUInt32LE(104);

  let dataOffset = 128;
  if ((ddpfFlags & DDPF_FOURCC) !== 0 && ddpfFourCC === FOURCC_DX10) {
    throw new Error("DDS DX10 extension is not supported in this converter");
  }

  const data = fileBuffer.slice(dataOffset);

  if ((ddpfFlags & DDPF_FOURCC) !== 0) {
    if (ddpfFourCC === FOURCC_DXT1) {
      return { width, height, rgba: decodeBC1(width, height, data) };
    }
    if (ddpfFourCC === FOURCC_DXT3) {
      return { width, height, rgba: decodeBC2(width, height, data) };
    }
    if (ddpfFourCC === FOURCC_DXT5) {
      return { width, height, rgba: decodeBC3(width, height, data) };
    }
    const cc = Buffer.allocUnsafe(4);
    cc.writeUInt32LE(ddpfFourCC, 0);
    throw new Error(`Unsupported DDS FourCC: ${cc.toString("ascii")}`);
  }

  if ((ddpfFlags & DDPF_RGB) !== 0 && (ddpfRgbBits === 24 || ddpfRgbBits === 32)) {
    const bytesPerPixel = ddpfRgbBits / 8;
    const packedRowPitch = Math.ceil((width * ddpfRgbBits) / 8);
    let srcPitch = packedRowPitch;
    if (pitchOrLinear > 0 && (pitchOrLinear * height) <= data.length) {
      srcPitch = pitchOrLinear;
    }

    const rgba = Buffer.alloc(width * height * 4, 0);
    for (let y = 0; y < height; y++) {
      const rowStart = y * srcPitch;
      for (let x = 0; x < width; x++) {
        const src = rowStart + x * bytesPerPixel;
        if (src + bytesPerPixel > data.length) continue;

        let pixel = 0;
        if (bytesPerPixel === 4) {
          pixel = data.readUInt32LE(src);
        } else {
          pixel = data[src] | (data[src + 1] << 8) | (data[src + 2] << 16);
        }

        const r = extractMasked(pixel >>> 0, ddpfRMask >>> 0);
        const g = extractMasked(pixel >>> 0, ddpfGMask >>> 0);
        const b = extractMasked(pixel >>> 0, ddpfBMask >>> 0);
        const a = ((ddpfFlags & DDPF_ALPHAPIXELS) !== 0 && ddpfAMask !== 0)
          ? extractMasked(pixel >>> 0, ddpfAMask >>> 0)
          : 255;

        const dst = (y * width + x) * 4;
        rgba[dst + 0] = r;
        rgba[dst + 1] = g;
        rgba[dst + 2] = b;
        rgba[dst + 3] = a;
      }
    }

    return { width, height, rgba };
  }

  throw new Error(`Unsupported DDS format (flags=0x${ddpfFlags.toString(16)}, bits=${ddpfRgbBits})`);
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buffer) {
  let c = 0xffffffff;
  for (let i = 0; i < buffer.length; i++) {
    c = CRC_TABLE[(c ^ buffer[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const typeBuf = Buffer.from(type, "ascii");
  const len = Buffer.allocUnsafe(4);
  len.writeUInt32BE(data.length >>> 0, 0);
  const crcInput = Buffer.concat([typeBuf, data]);
  const crc = Buffer.allocUnsafe(4);
  crc.writeUInt32BE(crc32(crcInput), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

function writePngRGBA(filePath, width, height, rgba) {
  const raw = Buffer.alloc((width * 4 + 1) * height, 0);
  for (let y = 0; y < height; y++) {
    const srcStart = y * width * 4;
    const dstStart = y * (width * 4 + 1);
    raw[dstStart] = 0; // filter type: none
    rgba.copy(raw, dstStart + 1, srcStart, srcStart + width * 4);
  }

  const compressed = zlib.deflateSync(raw, { level: 9 });

  const signature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = Buffer.allocUnsafe(13);
  ihdr.writeUInt32BE(width >>> 0, 0);
  ihdr.writeUInt32BE(height >>> 0, 4);
  ihdr.writeUInt8(8, 8);  // bit depth
  ihdr.writeUInt8(6, 9);  // color type RGBA
  ihdr.writeUInt8(0, 10); // compression
  ihdr.writeUInt8(0, 11); // filter
  ihdr.writeUInt8(0, 12); // interlace

  const png = Buffer.concat([
    signature,
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", compressed),
    pngChunk("IEND", Buffer.alloc(0))
  ]);

  fs.writeFileSync(filePath, png);
  return png;
}

function convertToPngViaWic(inputPath, outputPath) {
  const script = [
    "$ErrorActionPreference = 'Stop'",
    "Add-Type -AssemblyName PresentationCore",
    "$inPath = [System.IO.Path]::GetFullPath($args[0])",
    "$outPath = [System.IO.Path]::GetFullPath($args[1])",
    "$uri = New-Object System.Uri($inPath)",
    "$decoder = [System.Windows.Media.Imaging.BitmapDecoder]::Create($uri, [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat, [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)",
    "$frame = $decoder.Frames[0]",
    "$encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder",
    "$encoder.Frames.Add($frame)",
    "$dir = [System.IO.Path]::GetDirectoryName($outPath)",
    "if (-not [System.IO.Directory]::Exists($dir)) { [System.IO.Directory]::CreateDirectory($dir) | Out-Null }",
    "$stream = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)",
    "$encoder.Save($stream)",
    "$stream.Dispose()"
  ].join("; ");

  const result = childProcess.spawnSync("powershell", ["-NoProfile", "-Command", script, inputPath, outputPath], {
    encoding: "utf8"
  });

  if (result.status !== 0) {
    const detail = `${result.stderr || ""}${result.stdout || ""}`.trim();
    throw new Error(`WIC conversion failed for '${inputPath}': ${detail}`);
  }
}

function convertImageToPng(inputPath, outputPath) {
  const src = fs.readFileSync(inputPath);
  const isDDS = (path.extname(inputPath).toLowerCase() === ".dds") || (src.length >= 4 && src.readUInt32LE(0) === DDS_MAGIC);

  if (isDDS) {
    const decoded = decodeDDS(src);
    return writePngRGBA(outputPath, decoded.width, decoded.height, decoded.rgba);
  }

  convertToPngViaWic(inputPath, outputPath);
  return fs.readFileSync(outputPath);
}

function cleanupDirectory(dirPath) {
  if (!fs.existsSync(dirPath)) {
    fs.mkdirSync(dirPath, { recursive: true });
    return;
  }
  for (const entry of fs.readdirSync(dirPath, { withFileTypes: true })) {
    const full = path.join(dirPath, entry.name);
    if (entry.isDirectory()) {
      fs.rmSync(full, { recursive: true, force: true });
    } else {
      fs.rmSync(full, { force: true });
    }
  }
}

function buildBasenameIndex(rootDir) {
  const index = new Map();

  function walk(current) {
    let entries = [];
    try {
      entries = fs.readdirSync(current, { withFileTypes: true });
    } catch {
      return;
    }

    for (const e of entries) {
      const full = path.join(current, e.name);
      if (e.isDirectory()) {
        walk(full);
      } else if (e.isFile()) {
        const key = e.name.toLowerCase();
        if (!index.has(key)) index.set(key, []);
        index.get(key).push(full);
      }
    }
  }

  walk(rootDir);
  return index;
}

function chooseExistingFile(candidates) {
  for (const c of candidates) {
    if (!c) continue;
    try {
      if (fs.existsSync(c) && fs.statSync(c).isFile()) {
        return path.resolve(c);
      }
    } catch {
      // ignore
    }
  }
  return "";
}

function resolveTextureSource(relPath, sourceMapDir, clientRoot, basenameIndexMaps, basenameIndexLocal) {
  const normalized = normalizeSlash(relPath || "").trim();
  if (!normalized) return "";

  const fileName = path.basename(normalized);
  const candidates = [];

  function addCandidate(basePath) {
    if (!basePath) return;
    const p = path.resolve(basePath);
    candidates.push(p);

    if (!p.toLowerCase().endsWith(".dds")) {
      candidates.push(`${p}.dds`);
    }

    const ext = path.extname(p);
    if (ext && ext.toLowerCase() !== ".dds") {
      candidates.push(`${p}.dds`);
    }
  }

  addCandidate(path.join(sourceMapDir, normalized));
  addCandidate(path.join(sourceMapDir, fileName));

  if (normalized.startsWith("../") || normalized.startsWith("..\\")) {
    const relNoDots = normalized.replace(/^\.\.\//, "");
    addCandidate(path.join(clientRoot, "Maps", relNoDots));
    addCandidate(path.join(clientRoot, "Maps", fileName));
  }

  const direct = chooseExistingFile(candidates);
  if (direct) return direct;

  const nameKeys = [fileName.toLowerCase()];
  if (!fileName.toLowerCase().endsWith(".dds")) {
    nameKeys.push(`${fileName.toLowerCase()}.dds`);
  }

  for (const key of nameKeys) {
    const listLocal = basenameIndexLocal.get(key) || [];
    if (listLocal.length > 0) return path.resolve(listLocal[0]);

    const listMaps = basenameIndexMaps.get(key) || [];
    if (listMaps.length > 0) return path.resolve(listMaps[0]);
  }

  return "";
}

function ensureSceneMaterialList(sceneJson, worldMaterials) {
  if (!Array.isArray(sceneJson.materials)) {
    sceneJson.materials = worldMaterials.map((mat, idx) => ({
      materialIndex: idx,
      name: `material_${idx}`,
      flags: Number(mat.flags || 0),
      sourceDiffuseMap: String(mat.diffuseMap || ""),
      packageTexture: ""
    }));
    return;
  }

  const byIndex = new Map();
  for (const item of sceneJson.materials) {
    const idx = Number(item.materialIndex);
    if (Number.isInteger(idx) && idx >= 0) byIndex.set(idx, item);
  }

  for (let i = 0; i < worldMaterials.length; i++) {
    if (!byIndex.has(i)) {
      sceneJson.materials.push({
        materialIndex: i,
        name: `material_${i}`,
        flags: Number(worldMaterials[i].flags || 0),
        sourceDiffuseMap: String(worldMaterials[i].diffuseMap || ""),
        packageTexture: ""
      });
    }
  }
}

function materialByIndex(sceneJson) {
  const byIndex = new Map();
  for (const item of sceneJson.materials || []) {
    const idx = Number(item.materialIndex);
    if (Number.isInteger(idx) && idx >= 0) {
      byIndex.set(idx, item);
    }
  }
  return byIndex;
}

function writeJson(filePath, obj) {
  fs.writeFileSync(filePath, `${JSON.stringify(obj, null, 2)}\n`, "utf8");
}

function generateReport(reportMeta) {
  const lines = [];
  lines.push("# rs3 texture conversion report");
  lines.push("");
  lines.push(`- sceneId: ${reportMeta.sceneId}`);
  lines.push(`- sceneDir: ${reportMeta.sceneDir}`);
  lines.push(`- sourceMapDir: ${reportMeta.sourceMapDir}`);
  lines.push(`- generatedAtUtc: ${reportMeta.generatedAtUtc}`);
  lines.push("");
  lines.push("## Counts");
  lines.push(`- materialCount(world): ${reportMeta.materialCount}`);
  lines.push(`- usedMaterialCount: ${reportMeta.usedMaterialCount}`);
  lines.push(`- convertedPngCount: ${reportMeta.convertedCount}`);
  lines.push(`- missingUsedTextures: ${reportMeta.missingCount}`);
  lines.push("");
  lines.push("## Hashes");
  lines.push(`- worldBinSha256: ${reportMeta.worldBinSha256}`);
  lines.push(`- sceneJsonSha256: ${reportMeta.sceneJsonSha256}`);
  lines.push(`- textureManifestSha256: ${reportMeta.textureManifestSha256}`);
  lines.push("");
  lines.push("## Notes");
  lines.push("- Converted only materials referenced by world.bin sections.");
  lines.push("- world.bin diffuseMap updated to package-local textures/*.png for converted materials.");
  if (reportMeta.missing.length > 0) {
    lines.push("");
    lines.push("## Missing (hard-fail)");
    for (const m of reportMeta.missing) {
      lines.push(`- materialIndex=${m.materialIndex} source='${m.sourceDiffuseMap}'`);
    }
  }
  return lines.join("\n");
}

function main() {
  const args = parseArgs(process.argv);

  const sceneDir = args["scene-dir"] ? path.resolve(args["scene-dir"]) : "";
  const sourceMapDir = args["source-map-dir"] ? path.resolve(args["source-map-dir"]) : "";
  const sceneId = args["scene-id"] ? String(args["scene-id"]).trim() : "";

  if (!sceneDir || !sourceMapDir || !sceneId) {
    throw new Error("Usage: node rs3_texture_converter.js --scene-dir <dir> --source-map-dir <dir> --scene-id <id>");
  }

  const sceneJsonPath = path.join(sceneDir, "scene.json");
  const worldBinPath = path.join(sceneDir, "world.bin");
  const texturesDir = path.join(sceneDir, "textures");
  const manifestPath = path.join(sceneDir, "texture_manifest_v1.json");
  const reportPath = path.join(sceneDir, "conversion_report_textures_v1.md");

  if (!fs.existsSync(sceneJsonPath)) throw new Error(`scene.json not found: ${sceneJsonPath}`);
  if (!fs.existsSync(worldBinPath)) throw new Error(`world.bin not found: ${worldBinPath}`);
  if (!fs.existsSync(sourceMapDir)) throw new Error(`source map dir not found: ${sourceMapDir}`);

  const sceneJson = JSON.parse(fs.readFileSync(sceneJsonPath, "utf8"));
  if (String(sceneJson.sceneId || "") !== sceneId) {
    throw new Error(`scene-id mismatch: expected '${sceneId}', found '${sceneJson.sceneId || ""}'`);
  }

  const worldBuffer = fs.readFileSync(worldBinPath);
  const parsedWorld = parseWorldBin(worldBuffer);

  ensureSceneMaterialList(sceneJson, parsedWorld.materials);
  const sceneMaterialsByIndex = materialByIndex(sceneJson);

  const usedMaterialIndices = [...new Set(parsedWorld.sections.map((s) => Number(s.materialIndex)))].sort((a, b) => a - b);

  cleanupDirectory(texturesDir);
  ensureDir(texturesDir);

  const clientRoot = path.resolve(sourceMapDir, "..", "..");
  const mapsRoot = path.join(clientRoot, "Maps");
  const basenameIndexMaps = buildBasenameIndex(mapsRoot);
  const basenameIndexLocal = buildBasenameIndex(sourceMapDir);

  const converted = [];
  const missing = [];

  for (const materialIndex of usedMaterialIndices) {
    if (materialIndex < 0 || materialIndex >= parsedWorld.materials.length) {
      missing.push({ materialIndex, sourceDiffuseMap: "<out_of_range_material_index>" });
      continue;
    }

    const worldMat = parsedWorld.materials[materialIndex];
    const sceneMat = sceneMaterialsByIndex.get(materialIndex);

    const sourceDiffuseMap = String((sceneMat && sceneMat.sourceDiffuseMap) || worldMat.diffuseMap || "");
    const resolvedSource = resolveTextureSource(sourceDiffuseMap, sourceMapDir, clientRoot, basenameIndexMaps, basenameIndexLocal);

    if (!resolvedSource) {
      missing.push({ materialIndex, sourceDiffuseMap });
      continue;
    }

    const materialName = sceneMat && sceneMat.name ? sceneMat.name : path.basename(sourceDiffuseMap || `material_${materialIndex}`);
    const pngName = `mat_${materialIndex}_${slugify(materialName)}.png`;
    const outPath = path.join(texturesDir, pngName);

    const pngBytes = convertImageToPng(resolvedSource, outPath);
    const relPkgPath = normalizeSlash(path.join("textures", pngName));

    parsedWorld.materials[materialIndex].diffuseMap = relPkgPath;

    if (sceneMat) {
      sceneMat.flags = Number(worldMat.flags || 0);
      sceneMat.sourceDiffuseMap = sourceDiffuseMap;
      sceneMat.packageTexture = relPkgPath;
    }

    converted.push({
      materialIndex,
      name: sceneMat && sceneMat.name ? sceneMat.name : `material_${materialIndex}`,
      flags: Number(worldMat.flags || 0),
      sourceDiffuseMap,
      resolvedSource: normalizeSlash(resolvedSource),
      packageTexture: relPkgPath,
      width: undefined,
      height: undefined,
      bytes: pngBytes.length,
      sha256: toSha256(pngBytes)
    });
  }

  if (missing.length > 0) {
    const detail = missing.map((m) => `materialIndex=${m.materialIndex} source='${m.sourceDiffuseMap}'`).join(", ");
    throw new Error(`Missing textures for used materials (${missing.length}): ${detail}`);
  }

  sceneJson.textureManifest = "texture_manifest_v1.json";
  sceneJson.usedMaterialIndices = usedMaterialIndices;

  const worldOut = writeWorldBinParsed(parsedWorld);
  fs.writeFileSync(worldBinPath, worldOut);

  writeJson(sceneJsonPath, sceneJson);

  const manifest = {
    version: "rs3_texture_manifest_v1",
    sceneId,
    generatedAtUtc: new Date().toISOString(),
    sourceMapDir: normalizeSlash(sourceMapDir),
    sceneDir: normalizeSlash(sceneDir),
    usedMaterialIndices,
    entries: converted
  };
  writeJson(manifestPath, manifest);

  const sceneJsonBytes = fs.readFileSync(sceneJsonPath);
  const manifestBytes = fs.readFileSync(manifestPath);

  const report = generateReport({
    sceneId,
    sceneDir: normalizeSlash(sceneDir),
    sourceMapDir: normalizeSlash(sourceMapDir),
    generatedAtUtc: manifest.generatedAtUtc,
    materialCount: parsedWorld.materials.length,
    usedMaterialCount: usedMaterialIndices.length,
    convertedCount: converted.length,
    missingCount: 0,
    missing: [],
    worldBinSha256: toSha256(worldOut),
    sceneJsonSha256: toSha256(sceneJsonBytes),
    textureManifestSha256: toSha256(manifestBytes)
  });
  fs.writeFileSync(reportPath, `${report}\n`, "utf8");

  console.log(`Converted ${converted.length} textures to PNG for scene '${sceneId}'.`);
  console.log(`- world.bin updated: ${worldBinPath}`);
  console.log(`- scene.json updated: ${sceneJsonPath}`);
  console.log(`- manifest: ${manifestPath}`);
  console.log(`- report: ${reportPath}`);
}

try {
  main();
} catch (err) {
  console.error("[rs3_texture_converter] ERROR:", err && err.message ? err.message : err);
  process.exit(1);
}
