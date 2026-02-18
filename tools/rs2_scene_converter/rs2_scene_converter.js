#!/usr/bin/env node
/*
  rs2_scene_converter.js
  Offline converter for RS2 map assets (.RS/.RS.xml/.RS.col) into rs3_scene_v1 package.
*/

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const RS_ID = 0x12345678;
const RS_VERSION = 7;
const RBSP_ID = 0x35849298;
const RBSP_VERSION = 2;
const R_COL_ID = 0x5050178f;
const R_COL_VERSION = 0;

const RM_FLAG_ADDITIVE = 0x04;
const RM_FLAG_TWOSIDED = 0x08;
const RM_FLAG_HIDE = 0x10;

function printUsage() {
  console.log("Usage: node rs2_scene_converter.js --input <map_dir> --output <scene_dir> --scene-id <scene_id>");
}

function parseArgs(argv) {
  const out = {};
  for (let i = 2; i < argv.length; i++) {
    const token = argv[i];
    if (!token.startsWith("--")) continue;
    const key = token.slice(2);
    const next = argv[i + 1];
    if (next && !next.startsWith("--")) {
      out[key] = next;
      i++;
    } else {
      out[key] = true;
    }
  }
  return out;
}

class Reader {
  constructor(buffer) {
    this.buf = buffer;
    this.off = 0;
  }

  ensure(bytes) {
    if (this.off + bytes > this.buf.length) {
      throw new Error(`Unexpected EOF at ${this.off}, need ${bytes} bytes`);
    }
  }

  u8() {
    this.ensure(1);
    const v = this.buf.readUInt8(this.off);
    this.off += 1;
    return v;
  }

  bool() {
    return this.u8() !== 0;
  }

  i32() {
    this.ensure(4);
    const v = this.buf.readInt32LE(this.off);
    this.off += 4;
    return v;
  }

  u32() {
    this.ensure(4);
    const v = this.buf.readUInt32LE(this.off);
    this.off += 4;
    return v;
  }

  f32() {
    this.ensure(4);
    const v = this.buf.readFloatLE(this.off);
    this.off += 4;
    return v;
  }

  skip(bytes) {
    this.ensure(bytes);
    this.off += bytes;
  }

  strz(maxLen = 4096) {
    const start = this.off;
    let end = start;
    while (end < this.buf.length && this.buf[end] !== 0x00) end++;
    if (end >= this.buf.length) {
      throw new Error(`Missing NUL terminator for string at ${start}`);
    }
    if (end - start >= maxLen) {
      throw new Error(`String too long at ${start}`);
    }
    this.off = end + 1;
    return this.buf.toString("utf8", start, end);
  }
}

class Writer {
  constructor() {
    this.parts = [];
  }

  push(buf) {
    this.parts.push(buf);
  }

  u8(v) {
    const b = Buffer.allocUnsafe(1);
    b.writeUInt8(v >>> 0, 0);
    this.push(b);
  }

  i32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeInt32LE(v | 0, 0);
    this.push(b);
  }

  u32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeUInt32LE(v >>> 0, 0);
    this.push(b);
  }

  f32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeFloatLE(Number(v), 0);
    this.push(b);
  }

  vec3(v) {
    this.f32(v[0]);
    this.f32(v[1]);
    this.f32(v[2]);
  }

  vec4(v) {
    this.f32(v[0]);
    this.f32(v[1]);
    this.f32(v[2]);
    this.f32(v[3]);
  }

  str(s) {
    const payload = Buffer.from(String(s || ""), "utf8");
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

function readText(filePath) {
  return fs.readFileSync(filePath, "utf8");
}

function normalizeSlash(v) {
  return String(v || "").replace(/\\/g, "/");
}

function safeNum(v, fallback = 0) {
  const n = Number(v);
  return Number.isFinite(n) ? n : fallback;
}

function parseVec3(text, fallback = [0, 0, 0]) {
  if (!text) return [...fallback];
  const parts = text.trim().split(/\s+/g).map((x) => Number(x));
  if (parts.length < 3) return [...fallback];
  const out = [safeNum(parts[0], fallback[0]), safeNum(parts[1], fallback[1]), safeNum(parts[2], fallback[2])];
  return out;
}

function parseTagText(block, tagName) {
  const re = new RegExp(`<${tagName}>([\\s\\S]*?)<\\/${tagName}>`, "i");
  const m = block.match(re);
  return m ? m[1].trim() : "";
}

function parseAttr(attrs, attrName) {
  const re = new RegExp(`${attrName}\\s*=\\s*\"([^\"]*)\"`, "i");
  const m = attrs.match(re);
  return m ? m[1] : "";
}

function parseMaterials(xmlText) {
  const list = [{ name: "__default__", diffuseMap: "", flags: 0 }];
  const re = /<MATERIAL\b([^>]*)>([\s\S]*?)<\/MATERIAL>/gi;
  let m;
  while ((m = re.exec(xmlText)) !== null) {
    const attrs = m[1] || "";
    const body = m[2] || "";
    const name = parseAttr(attrs, "name") || `material_${list.length}`;
    const diffuseMap = parseTagText(body, "DIFFUSEMAP");
    let flags = 0;
    if (/<ADDITIVE\s*\/>/i.test(body)) flags |= RM_FLAG_ADDITIVE;
    if (/<TWOSIDED\s*\/>/i.test(body)) flags |= RM_FLAG_TWOSIDED;
    if (/<USEOPACITY\s*\/>/i.test(body)) flags |= 0x01;
    if (/<USEALPHATEST\s*\/>/i.test(body)) flags |= 0x02;
    if (/<HIDE\s*\/>/i.test(body)) flags |= RM_FLAG_HIDE;

    list.push({
      name,
      diffuseMap: normalizeSlash(diffuseMap),
      flags
    });
  }
  return list;
}

function parseLights(xmlText) {
  const lights = [];
  const re = /<LIGHT\b([^>]*)>([\s\S]*?)<\/LIGHT>/gi;
  let m;
  while ((m = re.exec(xmlText)) !== null) {
    const attrs = m[1] || "";
    const body = m[2] || "";
    lights.push({
      name: parseAttr(attrs, "name") || `light_${lights.length}`,
      position: parseVec3(parseTagText(body, "POSITION"), [0, 0, 0]),
      color: parseVec3(parseTagText(body, "COLOR"), [1, 1, 1]),
      intensity: safeNum(parseTagText(body, "INTENSITY"), 1.0),
      attenuationStart: safeNum(parseTagText(body, "ATTENUATIONSTART"), 0.0),
      attenuationEnd: safeNum(parseTagText(body, "ATTENUATIONEND"), 1000.0)
    });
  }
  return lights;
}

function parseDummies(xmlText) {
  const dummies = {};
  const re = /<DUMMY\b([^>]*)>([\s\S]*?)<\/DUMMY>/gi;
  let m;
  while ((m = re.exec(xmlText)) !== null) {
    const attrs = m[1] || "";
    const body = m[2] || "";
    const rawName = parseAttr(attrs, "name") || "";
    const key = rawName.trim().toLowerCase();
    if (!key) continue;
    dummies[key] = {
      name: rawName,
      position: parseVec3(parseTagText(body, "POSITION"), [0, 0, 0]),
      direction: parseVec3(parseTagText(body, "DIRECTION"), [0, 1, 0])
    };
  }
  return dummies;
}

function parseFog(xmlText) {
  const m = xmlText.match(/<FOG\b([^>]*)>([\s\S]*?)<\/FOG>/i);
  if (!m) {
    return {
      enabled: false,
      min: 0,
      max: 0,
      color: [0.0, 0.0, 0.0]
    };
  }

  const attrs = m[1] || "";
  const body = m[2] || "";
  const min = safeNum(parseAttr(attrs, "min"), 0.0);
  const max = safeNum(parseAttr(attrs, "max"), 0.0);
  const r = safeNum(parseTagText(body, "R"), 0.0);
  const g = safeNum(parseTagText(body, "G"), 0.0);
  const b = safeNum(parseTagText(body, "B"), 0.0);

  return {
    enabled: true,
    min,
    max,
    color: [r / 255.0, g / 255.0, b / 255.0]
  };
}

function parseRs(rsPath) {
  const buf = fs.readFileSync(rsPath);
  const r = new Reader(buf);

  const id = r.u32();
  const version = r.u32();
  if (id !== RS_ID || version !== RS_VERSION) {
    throw new Error(`Invalid RS header in ${rsPath} (id=0x${id.toString(16)}, version=${version})`);
  }

  const materialNameCount = r.i32();
  const rsMaterialNames = [];
  for (let i = 0; i < materialNameCount; i++) {
    rsMaterialNames.push(r.strz(256));
  }

  const numConvexPolygons = r.i32();
  const numConvexVertices = r.i32();
  for (let i = 0; i < numConvexPolygons; i++) {
    r.skip(4 + 4 + 16 + 4);
    const nVertices = r.i32();
    r.skip(nVertices * 2 * 12);
  }

  const counts = {
    nodes: r.i32(),
    polygons: r.i32(),
    vertices: r.i32(),
    indices: r.i32()
  };

  const allocCounts = {
    nodeCount: r.i32(),
    polygonCount: r.i32(),
    numVertices: r.i32(),
    numIndices: r.i32()
  };

  const vertices = [];
  const polygons = [];
  let polygonId = 0;

  function readNode() {
    const bbMin = [r.f32(), r.f32(), r.f32()];
    const bbMax = [r.f32(), r.f32(), r.f32()];
    const plane = [r.f32(), r.f32(), r.f32(), r.f32()];

    const hasPos = r.bool();
    if (hasPos) readNode();
    const hasNeg = r.bool();
    if (hasNeg) readNode();

    const nPolygon = r.i32();
    for (let i = 0; i < nPolygon; i++) {
      const mat = r.i32();
      const nConvexPolygon = r.i32();
      const flags = r.u32();
      const nVertices = r.i32();

      const baseVertex = vertices.length;
      for (let j = 0; j < nVertices; j++) {
        const pos = [r.f32(), r.f32(), r.f32()];
        const normal = [r.f32(), r.f32(), r.f32()];
        const tu1 = r.f32();
        const tv1 = r.f32();
        const tu2 = r.f32();
        const tv2 = r.f32();
        vertices.push({ pos, normal, uv1: [tu1, tv1], uv2: [tu2, tv2] });
      }

      const polyNormal = [r.f32(), r.f32(), r.f32()];
      const hidden = (flags & RM_FLAG_HIDE) !== 0;
      let materialIndex = mat + 1;
      if (materialIndex < 0) materialIndex = 0;

      polygons.push({
        polygonId,
        materialIndex,
        hidden,
        nConvexPolygon,
        flags,
        baseVertex,
        vertexCount: nVertices,
        polygonNormal: polyNormal,
        nodePlane: plane,
        nodeBoundsMin: bbMin,
        nodeBoundsMax: bbMax
      });
      polygonId++;
    }
  }

  readNode();

  const perMaterialIndices = new Map();
  for (const poly of polygons) {
    if (poly.vertexCount < 3) continue;
    if (!perMaterialIndices.has(poly.materialIndex)) {
      perMaterialIndices.set(poly.materialIndex, []);
    }
    const arr = perMaterialIndices.get(poly.materialIndex);
    for (let j = 1; j < poly.vertexCount - 1; j++) {
      arr.push(poly.baseVertex + 0, poly.baseVertex + j, poly.baseVertex + j + 1);
    }
  }

  const sections = [];
  const indices = [];
  const sortedMaterials = [...perMaterialIndices.keys()].sort((a, b) => a - b);
  for (const matId of sortedMaterials) {
    const arr = perMaterialIndices.get(matId);
    if (!arr || arr.length === 0) continue;
    const indexStart = indices.length;
    indices.push(...arr);
    sections.push({
      materialIndex: matId,
      indexStart,
      indexCount: arr.length
    });
  }

  const bounds = {
    min: [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY],
    max: [Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY]
  };
  for (const v of vertices) {
    for (let i = 0; i < 3; i++) {
      bounds.min[i] = Math.min(bounds.min[i], v.pos[i]);
      bounds.max[i] = Math.max(bounds.max[i], v.pos[i]);
    }
  }
  if (!Number.isFinite(bounds.min[0])) {
    bounds.min = [0, 0, 0];
    bounds.max = [0, 0, 0];
  }

  return {
    rsMaterialNames,
    counts,
    allocCounts,
    numConvexPolygons,
    numConvexVertices,
    vertices,
    polygons,
    sections,
    indices,
    bounds
  };
}

function parseBsp(bspPath, materialCountHint) {
  const buf = fs.readFileSync(bspPath);
  const r = new Reader(buf);

  const id = r.u32();
  const version = r.u32();
  if (id !== RBSP_ID || version !== RBSP_VERSION) {
    throw new Error(`Invalid BSP header in ${bspPath} (id=0x${id.toString(16)}, version=${version})`);
  }

  const counts = {
    nodes: r.i32(),
    polygons: r.i32(),
    vertices: r.i32(),
    indices: r.i32()
  };

  const vertices = [];
  const polygons = [];
  let polygonId = 0;

  function readNode() {
    const bbMin = [r.f32(), r.f32(), r.f32()];
    const bbMax = [r.f32(), r.f32(), r.f32()];
    const plane = [r.f32(), r.f32(), r.f32(), r.f32()];

    const hasPos = r.bool();
    if (hasPos) readNode();
    const hasNeg = r.bool();
    if (hasNeg) readNode();

    const nPolygon = r.i32();
    for (let i = 0; i < nPolygon; i++) {
      const mat = r.i32();
      const nConvexPolygon = r.i32();
      const flags = r.u32();
      const nVertices = r.i32();

      const baseVertex = vertices.length;
      for (let j = 0; j < nVertices; j++) {
        const pos = [r.f32(), r.f32(), r.f32()];
        const normal = [r.f32(), r.f32(), r.f32()];
        const tu1 = r.f32();
        const tv1 = r.f32();
        const tu2 = r.f32();
        const tv2 = r.f32();
        vertices.push({ pos, normal, uv1: [tu1, tv1], uv2: [tu2, tv2] });
      }

      const polyNormal = [r.f32(), r.f32(), r.f32()];
      let materialIndex = mat + 1;
      if (materialIndex < 0) materialIndex = 0;
      if (materialIndex >= materialCountHint) materialIndex = 0;

      polygons.push({
        polygonId,
        materialIndex,
        hidden: (flags & RM_FLAG_HIDE) !== 0,
        nConvexPolygon,
        flags,
        baseVertex,
        vertexCount: nVertices,
        polygonNormal: polyNormal,
        nodePlane: plane,
        nodeBoundsMin: bbMin,
        nodeBoundsMax: bbMax
      });
      polygonId++;
    }
  }

  readNode();

  const perMaterialIndices = new Map();
  for (const poly of polygons) {
    if (poly.vertexCount < 3) continue;
    if (!perMaterialIndices.has(poly.materialIndex)) {
      perMaterialIndices.set(poly.materialIndex, []);
    }
    const arr = perMaterialIndices.get(poly.materialIndex);
    for (let j = 1; j < poly.vertexCount - 1; j++) {
      arr.push(poly.baseVertex + 0, poly.baseVertex + j, poly.baseVertex + j + 1);
    }
  }

  const sections = [];
  const indices = [];
  const sortedMaterials = [...perMaterialIndices.keys()].sort((a, b) => a - b);
  for (const matId of sortedMaterials) {
    const arr = perMaterialIndices.get(matId);
    if (!arr || arr.length === 0) continue;
    const indexStart = indices.length;
    indices.push(...arr);
    sections.push({
      materialIndex: matId,
      indexStart,
      indexCount: arr.length
    });
  }

  const bounds = {
    min: [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY],
    max: [Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY]
  };
  for (const v of vertices) {
    for (let i = 0; i < 3; i++) {
      bounds.min[i] = Math.min(bounds.min[i], v.pos[i]);
      bounds.max[i] = Math.max(bounds.max[i], v.pos[i]);
    }
  }
  if (!Number.isFinite(bounds.min[0])) {
    bounds.min = [0, 0, 0];
    bounds.max = [0, 0, 0];
  }

  return {
    counts,
    vertices,
    polygons,
    sections,
    indices,
    bounds
  };
}

function parseCol(colPath) {
  const buf = fs.readFileSync(colPath);
  const r = new Reader(buf);

  const id = r.u32();
  const version = r.u32();
  if (id !== R_COL_ID || version !== R_COL_VERSION) {
    throw new Error(`Invalid COL header in ${colPath} (id=0x${id.toString(16)}, version=${version})`);
  }

  const nodeCount = r.i32();
  const polygonCount = r.i32();

  const nodes = [];

  function parseNode() {
    const idx = nodes.length;
    const node = {
      plane: [r.f32(), r.f32(), r.f32(), r.f32()],
      solid: r.bool(),
      pos: -1,
      neg: -1
    };
    nodes.push(node);

    const hasPos = r.bool();
    if (hasPos) node.pos = parseNode();

    const hasNeg = r.bool();
    if (hasNeg) node.neg = parseNode();

    const nPolygon = r.i32();
    if (nPolygon > 0) {
      r.skip(nPolygon * 4 * 3 * 4);
    }

    return idx;
  }

  const rootIndex = parseNode();

  return {
    nodeCount,
    polygonCount,
    nodes,
    rootIndex
  };
}

function findMapFiles(inputDir) {
  const files = fs.readdirSync(inputDir);
  const lower = files.map((f) => f.toLowerCase());

  const rsCandidates = files.filter((f) => /\.rs$/i.test(f));
  if (rsCandidates.length === 0) {
    throw new Error(`No .RS file found under ${inputDir}`);
  }
  const rsFile = rsCandidates[0];

  const baseLower = rsFile.toLowerCase();
  const rsXml = files.find((f) => f.toLowerCase() === `${baseLower}.xml`) || files.find((f) => /\.rs\.xml$/i.test(f));
  const rsCol = files.find((f) => f.toLowerCase() === `${baseLower}.col`) || files.find((f) => /\.rs\.col$/i.test(f));
  const rsBsp = files.find((f) => f.toLowerCase() === `${baseLower}.bsp`) || files.find((f) => /\.rs\.bsp$/i.test(f));
  const rsLm = files.find((f) => f.toLowerCase() === `${baseLower}.lm`) || files.find((f) => /\.rs\.lm$/i.test(f));

  if (!rsXml) throw new Error(`No .RS.xml found under ${inputDir}`);
  if (!rsCol) throw new Error(`No .RS.col found under ${inputDir}`);

  return {
    rs: path.join(inputDir, rsFile),
    rsXml: path.join(inputDir, rsXml),
    rsCol: path.join(inputDir, rsCol),
    rsBsp: rsBsp ? path.join(inputDir, rsBsp) : "",
    rsLm: rsLm ? path.join(inputDir, rsLm) : ""
  };
}

function toSha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex");
}

function writeWorldBin(filePath, payload) {
  const w = new Writer();

  w.bytes(Buffer.from([0x52, 0x53, 0x33, 0x53, 0x43, 0x4e, 0x31, 0x00])); // RS3SCN1\0
  w.u32(1);

  w.u32(payload.vertices.length);
  w.u32(payload.indices.length);
  w.u32(payload.materials.length);
  w.u32(payload.sections.length);
  w.u32(payload.lights.length);

  w.vec3(payload.cameraPos01.position);
  w.vec3(payload.cameraPos01.direction);
  w.vec3(payload.cameraPos02.position);
  w.vec3(payload.cameraPos02.direction);
  w.vec3(payload.spawn.position);
  w.vec3(payload.spawn.direction);

  w.f32(payload.fog.min);
  w.f32(payload.fog.max);
  w.vec3(payload.fog.color);
  w.u32(payload.fog.enabled ? 1 : 0);

  w.vec3(payload.bounds.min);
  w.vec3(payload.bounds.max);

  for (const mat of payload.materials) {
    w.u32(mat.flags >>> 0);
    w.str(mat.diffuseMap || "");
  }

  for (const light of payload.lights) {
    w.vec3(light.position);
    w.vec3(light.color);
    w.f32(light.intensity);
    w.f32(light.attenuationStart);
    w.f32(light.attenuationEnd);
  }

  for (const sec of payload.sections) {
    w.u32(sec.materialIndex >>> 0);
    w.u32(sec.indexStart >>> 0);
    w.u32(sec.indexCount >>> 0);
  }

  for (const v of payload.vertices) {
    w.vec3(v.pos);
    w.vec3(v.normal);
    w.f32(v.uv1[0]);
    w.f32(v.uv1[1]);
  }

  for (const idx of payload.indices) {
    w.u32(idx >>> 0);
  }

  const bin = w.finish();
  fs.writeFileSync(filePath, bin);
  return bin;
}

function writeCollisionBin(filePath, col) {
  const w = new Writer();
  w.bytes(Buffer.from([0x52, 0x53, 0x33, 0x43, 0x4f, 0x4c, 0x31, 0x00])); // RS3COL1\0
  w.u32(1);
  w.u32(col.nodes.length);
  w.i32(col.rootIndex);

  for (const node of col.nodes) {
    w.vec4(node.plane);
    w.u8(node.solid ? 1 : 0);
    w.i32(node.pos);
    w.i32(node.neg);
  }

  const bin = w.finish();
  fs.writeFileSync(filePath, bin);
  return bin;
}

function resolveDummy(dummies, key, fallbackPos, fallbackDir) {
  const found = dummies[key.toLowerCase()];
  if (!found) {
    return {
      name: key,
      position: [...fallbackPos],
      direction: [...fallbackDir],
      found: false
    };
  }
  return {
    name: found.name,
    position: found.position,
    direction: found.direction,
    found: true
  };
}

function generateReportMarkdown(meta) {
  const lines = [];
  lines.push("# rs2_scene_converter report");
  lines.push("");
  lines.push(`- sceneId: ${meta.sceneId}`);
  lines.push(`- input: ${meta.inputDir}`);
  lines.push(`- output: ${meta.outputDir}`);
  lines.push(`- generatedAtUtc: ${meta.generatedAtUtc}`);
  lines.push("");
  lines.push("## Source files");
  lines.push(`- RS: ${meta.source.rs}`);
  lines.push(`- RS.xml: ${meta.source.rsXml}`);
  lines.push(`- RS.col: ${meta.source.rsCol}`);
  lines.push(`- RS.bsp: ${meta.source.rsBsp || "(missing)"}`);
  lines.push(`- RS.lm: ${meta.source.rsLm || "(missing)"}`);
  lines.push("");
  lines.push("## Geometry");
  lines.push(`- vertices: ${meta.stats.vertices}`);
  lines.push(`- indices: ${meta.stats.indices}`);
  lines.push(`- triangles: ${meta.stats.triangles}`);
  lines.push(`- sections: ${meta.stats.sections}`);
  lines.push(`- materials: ${meta.stats.materials}`);
  lines.push("");
  lines.push("## Scene Metadata");
  lines.push(`- lights: ${meta.stats.lights}`);
  lines.push(`- collisionNodes: ${meta.stats.collisionNodes}`);
  lines.push(`- camera_pos 01 found: ${meta.audit.camera01Found}`);
  lines.push(`- camera_pos 02 found: ${meta.audit.camera02Found}`);
  lines.push(`- spawn_solo_101 found: ${meta.audit.spawnFound}`);
  lines.push("");
  lines.push("## Hashes");
  lines.push(`- world.bin sha256: ${meta.hashes.worldBinSha256}`);
  lines.push(`- collision.bin sha256: ${meta.hashes.collisionBinSha256}`);
  lines.push(`- scene.json sha256: ${meta.hashes.sceneJsonSha256}`);
  lines.push("");
  lines.push("## Notes");
  lines.push("- This pass is L1 static parity: only static world geometry/materials/lights/fog/dummies.");
  lines.push("- OBJECTLIST animated props are intentionally excluded from runtime draw in this phase.");
  return lines.join("\n");
}

function main() {
  const args = parseArgs(process.argv);
  const inputDir = args.input ? path.resolve(args.input) : "";
  const outputDir = args.output ? path.resolve(args.output) : "";
  const sceneId = args["scene-id"] ? String(args["scene-id"]).trim() : "";

  if (!inputDir || !outputDir || !sceneId) {
    printUsage();
    process.exit(1);
  }
  if (!fs.existsSync(inputDir) || !fs.statSync(inputDir).isDirectory()) {
    throw new Error(`Input directory not found: ${inputDir}`);
  }

  fs.mkdirSync(outputDir, { recursive: true });

  const files = findMapFiles(inputDir);
  const xmlText = readText(files.rsXml);

  const materials = parseMaterials(xmlText);
  const lights = parseLights(xmlText);
  const dummies = parseDummies(xmlText);
  const fog = parseFog(xmlText);
  const rs = parseRs(files.rs);
  const bsp = files.rsBsp ? parseBsp(files.rsBsp, materials.length) : null;
  const geometry = bsp || rs;
  const col = parseCol(files.rsCol);

  const camera01 = resolveDummy(dummies, "camera_pos 01", [-2606.4, -508.1, 1750.0], [-0.7, 0.0, -0.7]);
  const camera02 = resolveDummy(dummies, "camera_pos 02", [1313.8, 648.9, 910.6], [1.0, -0.1, -0.1]);
  const spawn = resolveDummy(dummies, "spawn_solo_101", [1635.4, 550.4, 750.0], [-1.0, 0.1, 0.0]);

  const usedMaterialIndices = [...new Set(geometry.sections.map((s) => Number(s.materialIndex)))].sort((a, b) => a - b);
  const sceneMaterials = materials.map((mat, index) => ({
    materialIndex: index,
    name: String(mat.name || `material_${index}`),
    flags: Number(mat.flags || 0),
    sourceDiffuseMap: String(mat.diffuseMap || ""),
    packageTexture: ""
  }));

  const worldPayload = {
    materials,
    lights,
    fog,
    cameraPos01: { position: camera01.position, direction: camera01.direction },
    cameraPos02: { position: camera02.position, direction: camera02.direction },
    spawn: { position: spawn.position, direction: spawn.direction },
    vertices: geometry.vertices,
    indices: geometry.indices,
    sections: geometry.sections,
    bounds: geometry.bounds
  };

  const worldBinPath = path.join(outputDir, "world.bin");
  const collisionBinPath = path.join(outputDir, "collision.bin");
  const sceneJsonPath = path.join(outputDir, "scene.json");
  const reportPath = path.join(outputDir, "conversion_report.md");

  const worldBin = writeWorldBin(worldBinPath, worldPayload);
  const collisionBin = writeCollisionBin(collisionBinPath, col);

  const sceneJson = {
    version: "rs3_scene_v1",
    sceneId,
    coordinateSystem: "x-right,y-forward,z-up,left-handed",
    cameraPos01: {
      name: camera01.name,
      position: camera01.position,
      direction: camera01.direction
    },
    cameraPos02: {
      name: camera02.name,
      position: camera02.position,
      direction: camera02.direction
    },
    charSpawn: {
      name: spawn.name,
      position: spawn.position,
      direction: spawn.direction
    },
    fog: {
      enabled: !!fog.enabled,
      min: fog.min,
      max: fog.max,
      color: fog.color
    },
    lights,
    worldChunk: "world.bin",
    collisionChunk: "collision.bin",
    textureManifest: "texture_manifest_v1.json",
    lightmaps: [],
    materials: sceneMaterials,
    usedMaterialIndices,
    source: {
      inputDir: normalizeSlash(inputDir),
      rs: normalizeSlash(files.rs),
      rsXml: normalizeSlash(files.rsXml),
      rsCol: normalizeSlash(files.rsCol),
      rsBsp: normalizeSlash(files.rsBsp || ""),
      rsLm: normalizeSlash(files.rsLm || "")
    },
    stats: {
      vertices: geometry.vertices.length,
      indices: geometry.indices.length,
      triangles: Math.floor(geometry.indices.length / 3),
      sections: geometry.sections.length,
      materials: sceneMaterials.length,
      lights: lights.length,
      collisionNodes: col.nodes.length,
      rsCounts: rs.counts,
      rsAllocCounts: rs.allocCounts,
      rsMaterialNameCount: rs.rsMaterialNames.length,
      rsConvexPolygonCount: rs.numConvexPolygons,
      rsConvexVertexCount: rs.numConvexVertices,
      geometrySource: bsp ? "bsp" : "rs",
      bspCounts: bsp ? bsp.counts : null
    },
    hashes: {
      worldBinSha256: toSha256(worldBin),
      collisionBinSha256: toSha256(collisionBin)
    }
  };

  const sceneJsonBuffer = Buffer.from(JSON.stringify(sceneJson, null, 2), "utf8");
  fs.writeFileSync(sceneJsonPath, sceneJsonBuffer);

  const meta = {
    sceneId,
    inputDir: normalizeSlash(inputDir),
    outputDir: normalizeSlash(outputDir),
    generatedAtUtc: new Date().toISOString(),
    source: {
      rs: normalizeSlash(files.rs),
      rsXml: normalizeSlash(files.rsXml),
      rsCol: normalizeSlash(files.rsCol),
      rsBsp: normalizeSlash(files.rsBsp || ""),
      rsLm: normalizeSlash(files.rsLm || "")
    },
    stats: sceneJson.stats,
    audit: {
      camera01Found: camera01.found,
      camera02Found: camera02.found,
      spawnFound: spawn.found
    },
    hashes: {
      worldBinSha256: sceneJson.hashes.worldBinSha256,
      collisionBinSha256: sceneJson.hashes.collisionBinSha256,
      sceneJsonSha256: toSha256(sceneJsonBuffer)
    }
  };

  fs.writeFileSync(reportPath, generateReportMarkdown(meta), "utf8");

  console.log(`Converted scene '${sceneId}'`);
  console.log(`- world.bin: ${worldBin.length} bytes`);
  console.log(`- collision.bin: ${collisionBin.length} bytes`);
  console.log(`- scene.json: ${sceneJsonBuffer.length} bytes`);
  console.log(`- report: ${reportPath}`);
}

try {
  main();
} catch (err) {
  console.error("[rs2_scene_converter] ERROR:", err && err.message ? err.message : err);
  process.exit(1);
}
