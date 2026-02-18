#!/usr/bin/env node
/*
  elu_ani_to_glb.js
  Offline converter (source stage): ELU/ANI/XML catalogs -> open GLB assets + manifest.

  Runtime note:
  - This tool is offline-only.
  - Client runtime must not parse ELU/ANI/XML.
*/

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const EXPORTER_SIG = 0x0107f060;
const EXPORTER_MESH_VER2 = 0x00005001;
const EXPORTER_MESH_VER3 = 0x00005002;
const EXPORTER_MESH_VER4 = 0x00005003;
const EXPORTER_MESH_VER6 = 0x00005005;
const EXPORTER_MESH_VER7 = 0x00005006;
const EXPORTER_MESH_VER8 = 0x00005007;

const EXPORTER_ANI_VER1 = 0x00000012;
const EXPORTER_ANI_VER3 = 0x00001002;

const RM_FLAG_USEOPACITY = 0x01;
const RM_FLAG_USEALPHATEST = 0x02;
const RM_FLAG_ADDITIVE = 0x04;
const RM_FLAG_TWOSIDED = 0x08;

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

function normalizeSlash(v) {
  return String(v || "").replace(/\\/g, "/");
}

function ensureDir(dirPath) {
  fs.mkdirSync(dirPath, { recursive: true });
}

function readText(filePath) {
  const text = fs.readFileSync(filePath, "utf8");
  return text.charCodeAt(0) === 0xfeff ? text.slice(1) : text;
}

function fileExistsSafe(filePath) {
  try {
    return fs.existsSync(filePath) && fs.statSync(filePath).isFile();
  } catch {
    return false;
  }
}

function hashFileSha256(filePath) {
  const h = crypto.createHash("sha256");
  h.update(fs.readFileSync(filePath));
  return h.digest("hex");
}

function toSafePathSegment(v) {
  return String(v || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_./-]+/g, "_")
    .replace(/_+/g, "_")
    .replace(/^_+|_+$/g, "") || "unknown";
}

class Reader {
  constructor(buffer) {
    this.buf = buffer;
    this.off = 0;
  }

  ensure(sz) {
    if (this.off + sz > this.buf.length) {
      throw new Error(`Unexpected EOF at ${this.off}, need ${sz} bytes`);
    }
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

  bytes(sz) {
    this.ensure(sz);
    const out = this.buf.slice(this.off, this.off + sz);
    this.off += sz;
    return out;
  }

  skip(sz) {
    this.ensure(sz);
    this.off += sz;
  }

  fixedString(len) {
    const b = this.bytes(len);
    const end = b.indexOf(0x00);
    const payload = end >= 0 ? b.slice(0, end) : b;
    return payload.toString("latin1").trim();
  }

  vec3() {
    return [this.f32(), this.f32(), this.f32()];
  }

  vec4() {
    return [this.f32(), this.f32(), this.f32(), this.f32()];
  }

  mat4() {
    const m = new Array(16);
    for (let i = 0; i < 16; i++) m[i] = this.f32();
    return m;
  }
}

function clamp(v, a, b) {
  return Math.max(a, Math.min(b, v));
}

function vec3Sub(a, b) {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function vec3Cross(a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  ];
}

function vec3Normalize(v) {
  const len = Math.hypot(v[0], v[1], v[2]);
  if (len < 1e-8) return [0, 1, 0];
  return [v[0] / len, v[1] / len, v[2] / len];
}

function transposeMat4(m) {
  return [
    m[0], m[4], m[8], m[12],
    m[1], m[5], m[9], m[13],
    m[2], m[6], m[10], m[14],
    m[3], m[7], m[11], m[15]
  ];
}

function invertMat4(a) {
  const out = new Array(16);

  const a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
  const a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
  const a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
  const a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];

  const b00 = a00 * a11 - a01 * a10;
  const b01 = a00 * a12 - a02 * a10;
  const b02 = a00 * a13 - a03 * a10;
  const b03 = a01 * a12 - a02 * a11;
  const b04 = a01 * a13 - a03 * a11;
  const b05 = a02 * a13 - a03 * a12;
  const b06 = a20 * a31 - a21 * a30;
  const b07 = a20 * a32 - a22 * a30;
  const b08 = a20 * a33 - a23 * a30;
  const b09 = a21 * a32 - a22 * a31;
  const b10 = a21 * a33 - a23 * a31;
  const b11 = a22 * a33 - a23 * a32;

  let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (Math.abs(det) < 1e-10) {
    return [
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1
    ];
  }
  det = 1.0 / det;

  out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
  out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
  out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
  out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
  out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
  out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
  out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
  out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
  out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
  out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
  out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
  out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
  out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
  out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
  out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
  out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;

  return out;
}

function parseElu(filePath) {
  const buf = fs.readFileSync(filePath);
  const r = new Reader(buf);

  const sig = r.u32();
  const ver = r.u32();
  const mtrlNum = r.i32();
  const meshNum = r.i32();

  if (sig !== EXPORTER_SIG) {
    throw new Error(`ELU signature mismatch: ${path.basename(filePath)} got=0x${sig.toString(16)}`);
  }

  const materials = [];
  for (let i = 0; i < mtrlNum; i++) {
    const mtrlId = r.i32();
    const subMtrlId = r.i32();

    const ambient = [r.f32(), r.f32(), r.f32(), r.f32()];
    const diffuse = [r.f32(), r.f32(), r.f32(), r.f32()];
    const specular = [r.f32(), r.f32(), r.f32(), r.f32()];
    const powerRaw = r.f32();
    const subMtrlNum = r.i32();

    const nameLen = (ver < EXPORTER_MESH_VER7) ? 40 : 256;
    const diffuseName = normalizeSlash(r.fixedString(nameLen));
    const opacityName = normalizeSlash(r.fixedString(nameLen));

    let twoSided = false;
    let additive = false;
    let alphaTest = false;
    let alphaTestValue = 0;

    if (ver > EXPORTER_MESH_VER3) {
      twoSided = (r.i32() !== 0);
    }
    if (ver > EXPORTER_MESH_VER4) {
      additive = (r.i32() !== 0);
    }
    if (ver > EXPORTER_MESH_VER7) {
      alphaTestValue = r.i32();
      alphaTest = alphaTestValue !== 0;
    }

    let flags = 0;
    if (opacityName) flags |= RM_FLAG_USEOPACITY;
    if (alphaTest) flags |= RM_FLAG_USEALPHATEST;
    if (additive) flags |= RM_FLAG_ADDITIVE;
    if (twoSided) flags |= RM_FLAG_TWOSIDED;

    materials.push({
      materialIndex: i,
      mtrlId,
      subMtrlId,
      subMtrlNum,
      ambient,
      diffuse,
      specular,
      powerRaw,
      power: powerRaw * 100.0,
      diffuseName,
      opacityName,
      twoSided,
      additive,
      alphaTest,
      alphaTestValue,
      flags
    });
  }

  const nodes = [];

  for (let i = 0; i < meshNum; i++) {
    const name = r.fixedString(40);
    const parent = r.fixedString(40);
    const matBase = r.mat4();

    let apScale = [1, 1, 1];
    if (ver >= EXPORTER_MESH_VER2) {
      apScale = r.vec3();
    }

    if (ver >= EXPORTER_MESH_VER4) {
      r.vec3(); // axis_rot
      r.f32();  // axis_rot_angle
      r.vec3(); // axis_scale
      r.f32();  // axis_scale_angle
      r.mat4(); // mat_etc
    }

    const pointNum = r.i32();
    const points = new Array(Math.max(0, pointNum));
    for (let p = 0; p < pointNum; p++) {
      points[p] = r.vec3();
    }

    const faceNum = r.i32();
    const faces = [];

    if (faceNum > 0) {
      if (ver >= EXPORTER_MESH_VER6) {
        for (let f = 0; f < faceNum; f++) {
          const pointIndex = [r.i32(), r.i32(), r.i32()];
          const uv3 = [r.vec3(), r.vec3(), r.vec3()];
          const mtrlId = r.i32();
          const sgId = r.i32();
          faces.push({ pointIndex, uv3, mtrlId, sgId, pointNormals: [[0, 1, 0], [0, 1, 0], [0, 1, 0]] });
        }

        for (let f = 0; f < faceNum; f++) {
          const normal = r.vec3();
          const p0 = r.vec3();
          const p1 = r.vec3();
          const p2 = r.vec3();
          if (faces[f]) {
            faces[f].faceNormal = normal;
            faces[f].pointNormals = [p0, p1, p2];
          }
        }
      } else if (ver > EXPORTER_MESH_VER2) {
        for (let f = 0; f < faceNum; f++) {
          const pointIndex = [r.i32(), r.i32(), r.i32()];
          const uv3 = [r.vec3(), r.vec3(), r.vec3()];
          const mtrlId = r.i32();
          const sgId = r.i32();
          faces.push({ pointIndex, uv3, mtrlId, sgId, pointNormals: null });
        }
      } else {
        for (let f = 0; f < faceNum; f++) {
          const pointIndex = [r.i32(), r.i32(), r.i32()];
          const uv3 = [r.vec3(), r.vec3(), r.vec3()];
          const mtrlId = r.i32();
          faces.push({ pointIndex, uv3, mtrlId, sgId: 0, pointNormals: null });
        }
      }
    }

    let pointColorNum = 0;
    if (ver >= EXPORTER_MESH_VER6) {
      pointColorNum = r.i32();
      if (pointColorNum > 0) {
        r.skip(pointColorNum * 3 * 4);
      }
    }

    const mtrlId = r.i32();
    const physiqueNum = r.i32();

    const physique = [];
    for (let p = 0; p < Math.max(0, physiqueNum); p++) {
      const parentNames = [r.fixedString(40), r.fixedString(40), r.fixedString(40), r.fixedString(40)];
      const weights = [r.f32(), r.f32(), r.f32(), r.f32()];
      const parentIds = [r.i32(), r.i32(), r.i32(), r.i32()];
      const num = r.i32();
      const offsets = [r.vec3(), r.vec3(), r.vec3(), r.vec3()];
      physique.push({ parentNames, weights, parentIds, num, offsets });
    }

    nodes.push({
      nodeIndex: i,
      name,
      parent,
      matBase,
      apScale,
      pointNum,
      points,
      faceNum,
      faces,
      pointColorNum,
      mtrlId,
      physiqueNum,
      physique
    });
  }

  return {
    filePath: normalizeSlash(filePath),
    sig,
    ver,
    materialCount: mtrlNum,
    meshCount: meshNum,
    materials,
    nodes
  };
}

function parseAni(filePath) {
  const buf = fs.readFileSync(filePath);
  const r = new Reader(buf);

  const sig = r.u32();
  const ver = r.u32();
  const maxFrame = r.i32();
  const modelNum = r.i32();
  const aniType = r.i32();

  if (sig !== EXPORTER_SIG) {
    throw new Error(`ANI signature mismatch: ${path.basename(filePath)} got=0x${sig.toString(16)}`);
  }

  const nodes = [];

  if (aniType === 2) {
    for (let i = 0; i < modelNum; i++) {
      const name = r.fixedString(40);
      const matBase = r.mat4();

      const posCnt = r.i32();
      const posKeys = [];
      for (let p = 0; p < posCnt; p++) {
        const x = r.f32();
        const y = r.f32();
        const z = r.f32();
        const frame = r.i32();
        posKeys.push({ frame, value: [x, y, z] });
      }

      const rotCnt = r.i32();
      const rotKeys = [];
      for (let q = 0; q < rotCnt; q++) {
        const x = r.f32();
        const y = r.f32();
        const z = r.f32();
        const w = r.f32();
        const frame = r.i32();

        let qx = x;
        let qy = y;
        let qz = z;
        let qw = w;

        // OGZ legacy path: for ANI <= EXPORTER_ANI_VER3, rotation key is angle-axis,
        // then converted to quaternion by RRot2Quat.
        if (ver <= EXPORTER_ANI_VER3) {
          const half = w * 0.5;
          const s = Math.sin(half);
          qx = x * s;
          qy = y * s;
          qz = z * s;
          qw = Math.cos(half);
        }

        rotKeys.push({ frame, value: [qx, qy, qz, qw] });
      }

      let visKeys = [];
      if (ver > EXPORTER_ANI_VER1) {
        const visCnt = r.u32();
        visKeys = new Array(visCnt);
        for (let v = 0; v < visCnt; v++) {
          visKeys[v] = { v: r.f32(), frame: r.i32() };
        }
      }

      nodes.push({ name, matBase, posKeys, rotKeys, visKeys });
    }
  } else if (aniType === 3) {
    // TM animation - parse minimally for diagnostics.
    for (let i = 0; i < modelNum; i++) {
      const name = r.fixedString(40);
      const matCnt = r.i32();
      r.skip(Math.max(0, matCnt) * (16 * 4 + 4));
      let visCnt = 0;
      if (ver > EXPORTER_ANI_VER1) {
        visCnt = r.u32();
        r.skip(visCnt * (4 + 4));
      }
      nodes.push({ name, tmKeys: matCnt, visKeys: visCnt });
    }
  } else if (aniType === 1) {
    // Vertex animation - parse minimally for diagnostics.
    for (let i = 0; i < modelNum; i++) {
      const name = r.fixedString(40);
      const vertexCnt = r.i32();
      const vtxArrayCount = r.i32();
      r.skip(Math.max(0, vertexCnt) * 4);
      for (let v = 0; v < Math.max(0, vertexCnt); v++) {
        r.skip(Math.max(0, vtxArrayCount) * (3 * 4));
      }
      let visCnt = 0;
      if (ver > EXPORTER_ANI_VER1) {
        visCnt = r.u32();
        r.skip(visCnt * (4 + 4));
      }
      nodes.push({ name, vertexCnt, vtxArrayCount, visKeys: visCnt });
    }
  }

  return {
    filePath: normalizeSlash(filePath),
    sig,
    ver,
    maxFrame,
    modelNum,
    aniType,
    nodes
  };
}

class GltfBuilder {
  constructor() {
    this.gltf = {
      asset: { version: "2.0", generator: "ndg-elu-ani-to-glb" },
      scene: 0,
      scenes: [{ nodes: [] }],
      nodes: [],
      meshes: [],
      materials: [],
      textures: [],
      images: [],
      samplers: [{ magFilter: 9729, minFilter: 9987, wrapS: 10497, wrapT: 10497 }],
      skins: [],
      animations: [],
      accessors: [],
      bufferViews: [],
      buffers: [{ byteLength: 0 }]
    };

    this._binParts = [];
    this._byteLength = 0;
    this._textureByUri = new Map();
  }

  _align4() {
    const mod = this._byteLength % 4;
    if (mod === 0) return;
    const pad = 4 - mod;
    this._binParts.push(Buffer.alloc(pad));
    this._byteLength += pad;
  }

  _appendBytes(buf) {
    this._align4();
    const byteOffset = this._byteLength;
    this._binParts.push(buf);
    this._byteLength += buf.length;
    return { byteOffset, byteLength: buf.length };
  }

  addBufferView(buf, target) {
    const loc = this._appendBytes(buf);
    const view = {
      buffer: 0,
      byteOffset: loc.byteOffset,
      byteLength: loc.byteLength
    };
    if (target != null) view.target = target;
    const idx = this.gltf.bufferViews.length;
    this.gltf.bufferViews.push(view);
    this.gltf.buffers[0].byteLength = this._byteLength;
    return idx;
  }

  addAccessor(typedArray, componentType, type, target, minmax) {
    const buf = Buffer.from(typedArray.buffer, typedArray.byteOffset, typedArray.byteLength);
    const viewIdx = this.addBufferView(buf, target);

    const accessor = {
      bufferView: viewIdx,
      componentType,
      count: typedArray.length / componentCountForType(type),
      type
    };

    if (minmax && minmax.min && minmax.max) {
      accessor.min = minmax.min;
      accessor.max = minmax.max;
    }

    const idx = this.gltf.accessors.length;
    this.gltf.accessors.push(accessor);
    return idx;
  }

  addTextureUri(uri) {
    const key = normalizeSlash(uri || "");
    if (!key) return null;

    if (this._textureByUri.has(key)) {
      return this._textureByUri.get(key);
    }

    const imageIndex = this.gltf.images.length;
    this.gltf.images.push({ uri: key });

    const texIndex = this.gltf.textures.length;
    this.gltf.textures.push({ sampler: 0, source: imageIndex });
    this._textureByUri.set(key, texIndex);
    return texIndex;
  }

  toGlbBuffer() {
    const jsonText = JSON.stringify(this.gltf);
    let jsonChunk = Buffer.from(jsonText, "utf8");
    const jsonPad = (4 - (jsonChunk.length % 4)) % 4;
    if (jsonPad > 0) {
      jsonChunk = Buffer.concat([jsonChunk, Buffer.alloc(jsonPad, 0x20)]);
    }

    let binChunk = Buffer.concat(this._binParts);
    const binPad = (4 - (binChunk.length % 4)) % 4;
    if (binPad > 0) {
      binChunk = Buffer.concat([binChunk, Buffer.alloc(binPad)]);
    }

    const totalLen = 12 + 8 + jsonChunk.length + 8 + binChunk.length;

    const header = Buffer.alloc(12);
    header.writeUInt32LE(0x46546c67, 0); // glTF
    header.writeUInt32LE(2, 4);
    header.writeUInt32LE(totalLen, 8);

    const jsonHeader = Buffer.alloc(8);
    jsonHeader.writeUInt32LE(jsonChunk.length, 0);
    jsonHeader.writeUInt32LE(0x4e4f534a, 4); // JSON

    const binHeader = Buffer.alloc(8);
    binHeader.writeUInt32LE(binChunk.length, 0);
    binHeader.writeUInt32LE(0x004e4942, 4); // BIN

    return Buffer.concat([header, jsonHeader, jsonChunk, binHeader, binChunk]);
  }
}

function componentCountForType(type) {
  switch (type) {
    case "SCALAR": return 1;
    case "VEC2": return 2;
    case "VEC3": return 3;
    case "VEC4": return 4;
    case "MAT4": return 16;
    default: throw new Error(`Unsupported accessor type: ${type}`);
  }
}

function computeMinMaxVec3(flatArray) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < flatArray.length; i += 3) {
    for (let c = 0; c < 3; c++) {
      const v = flatArray[i + c];
      if (v < min[c]) min[c] = v;
      if (v > max[c]) max[c] = v;
    }
  }
  return { min, max };
}

function computeMinMaxScalar(flatArray) {
  let min = Infinity;
  let max = -Infinity;
  for (let i = 0; i < flatArray.length; i++) {
    const v = flatArray[i];
    if (v < min) min = v;
    if (v > max) max = v;
  }
  return { min: [min], max: [max] };
}

function resolveTexturePath(textureName, eluDir, clientRoot) {
  const raw = normalizeSlash(textureName || "").trim();
  if (!raw) return "";

  const lower = raw.toLowerCase();
  const ext = path.extname(lower);

  const candidates = [];

  function addCandidate(p) {
    if (!p) return;
    candidates.push(path.resolve(p));
  }

  const asModelRelative = lower.startsWith("model/") || lower.startsWith("system/") || lower.startsWith("ui/");
  if (asModelRelative) {
    addCandidate(path.join(clientRoot, raw));
  } else {
    addCandidate(path.join(eluDir, raw));
    addCandidate(path.join(clientRoot, raw));
  }

  if (ext === ".tga" || ext === ".bmp" || ext === ".jpg" || ext === ".jpeg" || ext === ".png") {
    if (asModelRelative) {
      addCandidate(path.join(clientRoot, raw + ".dds"));
      addCandidate(path.join(clientRoot, raw.replace(ext, ".dds")));
    } else {
      addCandidate(path.join(eluDir, raw + ".dds"));
      addCandidate(path.join(eluDir, raw.replace(ext, ".dds")));
      addCandidate(path.join(clientRoot, raw + ".dds"));
    }
  }

  if (!ext) {
    const exts = [".dds", ".png", ".bmp", ".tga", ".jpg", ".jpeg"];
    for (const e of exts) {
      if (asModelRelative) {
        addCandidate(path.join(clientRoot, raw + e));
      } else {
        addCandidate(path.join(eluDir, raw + e));
        addCandidate(path.join(clientRoot, raw + e));
      }
    }
  }

  const seen = new Set();
  for (const c of candidates) {
    const n = normalizeSlash(c);
    if (seen.has(n)) continue;
    seen.add(n);
    if (fileExistsSafe(c)) return c;
  }

  return "";
}

function roughnessFromLegacyPower(power) {
  const normalized = clamp((Number(power) || 0) / 120.0, 0.0, 1.0);
  return clamp(1.0 - normalized, 0.04, 1.0);
}

function buildGeometryFromNode(node, boneIndexByName, boneIndexByNameLower, resolveMaterialIndex) {
  if (!node || !node.points || !node.faces || !node.points.length || !node.faces.length) {
    return [];
  }

  const groups = new Map();

  function ensureGroup(materialIndex) {
    const key = Number.isFinite(materialIndex) ? materialIndex : -1;
    if (!groups.has(key)) {
      groups.set(key, {
        materialIndex: key,
        positions: [],
        normals: [],
        uvs: [],
        joints: [],
        weights: [],
        indices: []
      });
    }
    return groups.get(key);
  }

  const fallbackBone = boneIndexByName.get(node.name) ?? 0;

  for (const face of node.faces) {
    const ids = face.pointIndex || [0, 0, 0];
    const p0 = node.points[ids[0]] || [0, 0, 0];
    const p1 = node.points[ids[1]] || [0, 0, 0];
    const p2 = node.points[ids[2]] || [0, 0, 0];

    const baseN = vec3Normalize(vec3Cross(vec3Sub(p1, p0), vec3Sub(p2, p0)));

    const resolvedMatIndex = resolveMaterialIndex
      ? resolveMaterialIndex(node.mtrlId, face.mtrlId)
      : 0;
    const g = ensureGroup(resolvedMatIndex);

    for (let c = 0; c < 3; c++) {
      const pointIndex = ids[c] ?? 0;
      const pos = node.points[pointIndex] || [0, 0, 0];
      const uv3 = (face.uv3 && face.uv3[c]) ? face.uv3[c] : [0, 0, 0];
      const normal = (face.pointNormals && face.pointNormals[c]) ? face.pointNormals[c] : baseN;

      let joints = [fallbackBone, 0, 0, 0];
      let weights = [1, 0, 0, 0];

      if (node.physique && node.physique.length > pointIndex) {
        const ph = node.physique[pointIndex];
        const infl = [];
        const maxInfluences = Math.min(4, Math.max(0, Number(ph.num || 0)));

        for (let k = 0; k < maxInfluences; k++) {
          const bnameRaw = String(ph.parentNames[k] || "");
          const bname = bnameRaw.trim();
          let boneIdx = boneIndexByName.get(bname);
          if (boneIdx == null && bname) {
            boneIdx = boneIndexByNameLower ? boneIndexByNameLower.get(bname.toLowerCase()) : undefined;
          }
          if (boneIdx == null) {
            const pid = Number((ph.parentIds && ph.parentIds[k]) != null ? ph.parentIds[k] : -1);
            if (Number.isInteger(pid) && pid >= 0 && pid < boneIndexByName.size) {
              boneIdx = pid;
            }
          }
          const weight = Number(ph.weights[k] || 0);
          if (boneIdx == null || weight <= 0) continue;
          infl.push({ boneIdx, weight });
        }

        if (infl.length > 0) {
          while (infl.length < 4) infl.push({ boneIdx: fallbackBone, weight: 0 });
          const sum = infl.reduce((acc, x) => acc + x.weight, 0);
          const div = sum > 1e-8 ? sum : 1;
          joints = infl.slice(0, 4).map((x) => x.boneIdx);
          weights = infl.slice(0, 4).map((x) => x.weight / div);
        }
      }

      const vertexIndex = g.positions.length / 3;
      g.positions.push(pos[0], pos[1], pos[2]);
      g.normals.push(normal[0], normal[1], normal[2]);
      g.uvs.push(uv3[0], 1.0 - uv3[1]);
      g.joints.push(joints[0], joints[1], joints[2], joints[3]);
      g.weights.push(weights[0], weights[1], weights[2], weights[3]);
      g.indices.push(vertexIndex);
    }
  }

  return Array.from(groups.values());
}

function addAnimationClip(builder, aniClip, nodeIndexByName, fps, uniqueNamesSet) {
  if (!aniClip || !aniClip.ani || aniClip.ani.aniType !== 2) {
    return { added: false, reason: "ANI_NOT_BONE" };
  }

  const channels = [];
  const samplers = [];

  function addChannel(nodeIdx, pathName, timeValues, valueValues, valueType) {
    if (!timeValues.length || !valueValues.length) return;

    const inputAccessor = builder.addAccessor(
      new Float32Array(timeValues),
      5126,
      "SCALAR",
      undefined,
      computeMinMaxScalar(timeValues)
    );

    const outputAccessor = builder.addAccessor(
      new Float32Array(valueValues),
      5126,
      valueType,
      undefined,
      undefined
    );

    const samplerIndex = samplers.length;
    samplers.push({ input: inputAccessor, output: outputAccessor, interpolation: "LINEAR" });
    channels.push({ sampler: samplerIndex, target: { node: nodeIdx, path: pathName } });
  }

  for (const n of aniClip.ani.nodes || []) {
    const nodeIdx = nodeIndexByName.get(n.name);
    if (nodeIdx == null) continue;

    if (Array.isArray(n.posKeys) && n.posKeys.length > 0) {
      const times = [];
      const values = [];
      for (const k of n.posKeys) {
        times.push((Number(k.frame || 0) / fps));
        values.push(k.value[0], k.value[1], k.value[2]);
      }
      addChannel(nodeIdx, "translation", times, values, "VEC3");
    }

    if (Array.isArray(n.rotKeys) && n.rotKeys.length > 0) {
      const times = [];
      const values = [];
      for (const k of n.rotKeys) {
        times.push((Number(k.frame || 0) / fps));
        values.push(k.value[0], k.value[1], k.value[2], k.value[3]);
      }
      addChannel(nodeIdx, "rotation", times, values, "VEC4");
    }
  }

  if (!channels.length) {
    return { added: false, reason: "NO_CHANNELS" };
  }

  const originalName = String(aniClip.clipName || "clip");
  const motionType = Number(aniClip.motionType || 0);
  let animationName = originalName;

  if (uniqueNamesSet.has(animationName)) {
    animationName = `${originalName}#m${motionType}`;
    if (uniqueNamesSet.has(animationName)) {
      let n = 2;
      while (uniqueNamesSet.has(`${animationName}_${n}`)) n++;
      animationName = `${animationName}_${n}`;
    }
  }
  uniqueNamesSet.add(animationName);

  builder.gltf.animations.push({
    name: animationName,
    samplers,
    channels,
    extras: {
      originalName,
      motionType,
      sourceFile: aniClip.sourceAni || ""
    }
  });

  return { added: true, name: animationName };
}

function buildGlbForTarget(target, options) {
  const elu = parseElu(target.sourceElu);

  const builder = new GltfBuilder();

  const nodeIndexByName = new Map();
  const nodeIndexByNameLower = new Map();
  const parentNameByIndex = new Map();

  for (const n of elu.nodes) {
    const idx = builder.gltf.nodes.length;
    nodeIndexByName.set(n.name, idx);
    nodeIndexByNameLower.set(String(n.name || "").toLowerCase(), idx);
    parentNameByIndex.set(idx, n.parent || "");
    builder.gltf.nodes.push({
      name: n.name,
      matrix: transposeMat4(n.matBase)
    });
  }

  const rootNodes = [];
  for (let i = 0; i < builder.gltf.nodes.length; i++) {
    const parentName = parentNameByIndex.get(i);
    const parentIdx = nodeIndexByName.get(parentName);
    if (parentIdx == null) {
      rootNodes.push(i);
      continue;
    }
    const pnode = builder.gltf.nodes[parentIdx];
    if (!pnode.children) pnode.children = [];
    pnode.children.push(i);
  }
  builder.gltf.scenes[0].nodes = rootNodes;

  const modelDir = path.dirname(target.outputGlbPath);
  const eluDir = path.dirname(target.sourceElu);

  const mtrlGltfIndexByMtrlId = new Map();
  const mtrlGltfIndexByMtrlSub = new Map(); // key: `${mtrlId}:${subMtrlId}`
  const mtrlBaseIndexByMtrlId = new Map();  // mtrlId -> base material (sub=-1 preferred)
  const mtrlSubCountByMtrlId = new Map();   // mtrlId -> declared sub material count
  const mtrlHasSubByMtrlId = new Map();     // mtrlId -> true if any sub material exists

  for (const m of elu.materials) {
    let alphaMode = "OPAQUE";
    if ((m.flags & RM_FLAG_ADDITIVE) !== 0) alphaMode = "BLEND";
    else if ((m.flags & RM_FLAG_USEALPHATEST) !== 0) alphaMode = "MASK";
    else if ((m.flags & RM_FLAG_USEOPACITY) !== 0) alphaMode = "BLEND";

    const mtrl = {
      name: `mtrl_${m.mtrlId}`,
      pbrMetallicRoughness: {
        baseColorFactor: [1, 1, 1, 1],
        metallicFactor: 0.0,
        roughnessFactor: roughnessFromLegacyPower(m.power)
      },
      alphaMode,
      doubleSided: !!m.twoSided,
      extras: {
        legacyMtrlId: m.mtrlId,
        legacySubMtrlId: m.subMtrlId,
        legacyFlags: m.flags,
        sourceDiffuseMap: m.diffuseName,
        sourceOpacityMap: m.opacityName,
        legacyPower: m.power
      }
    };

    if (alphaMode === "MASK") {
      const cutoff = clamp((Number(m.alphaTestValue || 128) / 255.0), 0.01, 1.0);
      mtrl.alphaCutoff = cutoff;
    }

    const diffuseTextureAbs = resolveTexturePath(m.diffuseName, eluDir, options.clientRoot);
    if (diffuseTextureAbs) {
      const uri = normalizeSlash(path.relative(modelDir, diffuseTextureAbs));
      const texIdx = builder.addTextureUri(uri);
      if (texIdx != null) {
        mtrl.pbrMetallicRoughness.baseColorTexture = { index: texIdx };
      }
    }

    const opacityTextureAbs = resolveTexturePath(m.opacityName, eluDir, options.clientRoot);
    if (opacityTextureAbs) {
      mtrl.extras.opacityTexture = normalizeSlash(path.relative(modelDir, opacityTextureAbs));
    }

    const gidx = builder.gltf.materials.length;
    builder.gltf.materials.push(mtrl);
    mtrlGltfIndexByMtrlId.set(m.mtrlId, gidx);
    mtrlGltfIndexByMtrlSub.set(`${m.mtrlId}:${m.subMtrlId}`, gidx);
    if (!mtrlBaseIndexByMtrlId.has(m.mtrlId) || m.subMtrlId === -1) {
      mtrlBaseIndexByMtrlId.set(m.mtrlId, gidx);
    }
    if (m.subMtrlId >= 0) {
      mtrlHasSubByMtrlId.set(m.mtrlId, true);
    }
    if (m.subMtrlId === -1 && Number.isFinite(m.subMtrlNum) && m.subMtrlNum > 0) {
      mtrlSubCountByMtrlId.set(m.mtrlId, m.subMtrlNum);
    }
  }

  function resolveGltfMaterialIndex(nodeMtrlId, faceSubMtrlId) {
    const nodeMtrl = Number(nodeMtrlId);
    const baseIdx = mtrlBaseIndexByMtrlId.get(nodeMtrl);

    // OGZ path for multi-sub material meshes:
    // material = Get_s(node.m_mtrl_id, sub_mtrl), where sub_mtrl comes from face and is wrapped.
    const declaredSubCount = Number(mtrlSubCountByMtrlId.get(nodeMtrl) || 0);
    const hasAnySub = !!mtrlHasSubByMtrlId.get(nodeMtrl);
    if (hasAnySub) {
      let sub = Number(faceSubMtrlId);
      if (!Number.isFinite(sub)) sub = 0;
      const subCount = declaredSubCount > 0 ? declaredSubCount : 0;
      if (subCount > 0) {
        sub = ((sub % subCount) + subCount) % subCount;
      }

      const keyed = mtrlGltfIndexByMtrlSub.get(`${nodeMtrl}:${sub}`);
      if (keyed != null) {
        return keyed;
      }
    }

    if (baseIdx != null) {
      return baseIdx;
    }

    const legacyFallback = mtrlGltfIndexByMtrlId.get(nodeMtrl);
    if (legacyFallback != null) {
      return legacyFallback;
    }

    return 0;
  }

  const allJoints = [];
  for (let i = 0; i < builder.gltf.nodes.length; i++) allJoints.push(i);

  const hasAnySkin = elu.nodes.some((n) => n.physiqueNum > 0);
  let skinIndex = null;

  if (hasAnySkin) {
    const invBind = [];
    for (const n of elu.nodes) {
      const localM = transposeMat4(n.matBase);
      const inv = invertMat4(localM);
      invBind.push(...inv);
    }

    const accessor = builder.addAccessor(
      new Float32Array(invBind),
      5126,
      "MAT4",
      undefined,
      undefined
    );

    skinIndex = builder.gltf.skins.length;
    builder.gltf.skins.push({
      joints: allJoints,
      inverseBindMatrices: accessor,
      skeleton: allJoints[0] || 0
    });
  }

  let totalVertex = 0;
  let totalIndex = 0;
  let totalPrimitive = 0;

  for (const n of elu.nodes) {
    const nodeIndex = nodeIndexByName.get(n.name);
    const groups = buildGeometryFromNode(n, nodeIndexByName, nodeIndexByNameLower, resolveGltfMaterialIndex);
    if (!groups.length) continue;

    const primitives = [];

    for (const g of groups) {
      const positions = new Float32Array(g.positions);
      const normals = new Float32Array(g.normals);
      const uvs = new Float32Array(g.uvs);
      const joints = new Uint16Array(g.joints);
      const weights = new Float32Array(g.weights);

      let indices;
      let indexComponentType;
      const maxIndex = Math.max(0, ...g.indices);
      if (maxIndex <= 65535) {
        indices = new Uint16Array(g.indices);
        indexComponentType = 5123;
      } else {
        indices = new Uint32Array(g.indices);
        indexComponentType = 5125;
      }

      const posAccessor = builder.addAccessor(
        positions,
        5126,
        "VEC3",
        34962,
        computeMinMaxVec3(g.positions)
      );

      const nrmAccessor = builder.addAccessor(normals, 5126, "VEC3", 34962, undefined);
      const uvAccessor = builder.addAccessor(uvs, 5126, "VEC2", 34962, undefined);
      const jAccessor = builder.addAccessor(joints, 5123, "VEC4", 34962, undefined);
      const wAccessor = builder.addAccessor(weights, 5126, "VEC4", 34962, undefined);
      const iAccessor = builder.addAccessor(indices, indexComponentType, "SCALAR", 34963, undefined);

      const prim = {
        mode: 4,
        attributes: {
          POSITION: posAccessor,
          NORMAL: nrmAccessor,
          TEXCOORD_0: uvAccessor
        },
        indices: iAccessor
      };

      if (hasAnySkin) {
        prim.attributes.JOINTS_0 = jAccessor;
        prim.attributes.WEIGHTS_0 = wAccessor;
      }

      if (Number.isFinite(g.materialIndex) && g.materialIndex >= 0) {
        prim.material = g.materialIndex;
      }

      primitives.push(prim);

      totalVertex += (positions.length / 3);
      totalIndex += indices.length;
      totalPrimitive += 1;
    }

    if (!primitives.length) continue;

    const meshIndex = builder.gltf.meshes.length;
    builder.gltf.meshes.push({ name: `${n.name}_mesh`, primitives });

    builder.gltf.nodes[nodeIndex].mesh = meshIndex;
    if (skinIndex != null) {
      builder.gltf.nodes[nodeIndex].skin = skinIndex;
    }
  }

  const animationResults = [];
  const uniqueAnimationNames = new Set();

  for (const clip of target.clipRefs) {
    if (!clip.exists || !clip.sourceAni) {
      animationResults.push({
        clipName: clip.clipName,
        motionType: clip.motionType,
        sourceAni: clip.sourceAni || "",
        status: "missing"
      });
      continue;
    }

    let ani;
    try {
      ani = parseAni(clip.sourceAni);
    } catch (err) {
      animationResults.push({
        clipName: clip.clipName,
        motionType: clip.motionType,
        sourceAni: clip.sourceAni,
        status: "error",
        error: err.message
      });
      continue;
    }

    const addRet = addAnimationClip(
      builder,
      {
        clipName: clip.clipName,
        motionType: clip.motionType,
        sourceAni: normalizeSlash(clip.sourceAni),
        ani
      },
      nodeIndexByName,
      options.fps,
      uniqueAnimationNames
    );

    animationResults.push({
      clipName: clip.clipName,
      motionType: clip.motionType,
      sourceAni: normalizeSlash(clip.sourceAni),
      aniType: ani.aniType,
      status: addRet.added ? "added" : "skipped",
      reason: addRet.reason || "",
      outputName: addRet.name || ""
    });
  }

  const glb = builder.toGlbBuffer();

  return {
    glb,
    summary: {
      materialCount: elu.materials.length,
      meshNodeCount: elu.nodes.length,
      primitiveCount: totalPrimitive,
      vertexCount: totalVertex,
      indexCount: totalIndex,
      animationCount: builder.gltf.animations.length
    },
    animationResults,
    eluVersion: elu.ver,
    eluSig: elu.sig
  };
}

function buildTargets(graph, options) {
  const targets = [];

  const minsetMeshes = new Set();
  if (options.minset && fileExistsSafe(options.minset)) {
    const minsetJson = JSON.parse(readText(options.minset));
    const resolved = minsetJson.resolved || {};
    for (const e of resolved.combat || []) {
      const mesh = String(e.resolvedMesh || e.meshName || "").trim();
      if (mesh) minsetMeshes.add(mesh);
    }
    for (const e of resolved.equip || []) {
      const mesh = String(e.meshName || "").trim();
      if (mesh) minsetMeshes.add(mesh);
    }
  }

  const meshFilterEnabled = minsetMeshes.size > 0;

  const seen = new Set();

  function addTarget(target) {
    if (!target || !target.modelId) return;
    if (seen.has(target.modelId)) return;
    seen.add(target.modelId);
    targets.push(target);
  }

  const clientRoot = options.clientRoot;

  if (options.includeCharacter) {
    for (const ch of graph.characterModels || []) {
      const base = (ch.baseModels || [])[0];
      if (!base || !base.resolvedPath) continue;
      addTarget({
        modelId: `character/${toSafePathSegment(ch.name)}`,
        modelType: "character",
        sourceElu: normalizeSlash(base.resolvedPath),
        sourceXml: normalizeSlash(ch.sourceXml || ""),
        clipRefs: (ch.animations || []).map((a) => ({
          clipName: a.name || "clip",
          motionType: Number(a.motionType || 0),
          sourceAni: normalizeSlash(a.resolvedPath || ""),
          exists: fileExistsSafe(a.resolvedPath || "")
        }))
      });
    }
  }

  if (options.includeNpc) {
    for (const npc of graph.npcModels || []) {
      const base = (npc.baseModels || [])[0];
      if (!base || !base.resolvedPath) continue;
      addTarget({
        modelId: `npc/${toSafePathSegment(npc.name)}`,
        modelType: "npc",
        sourceElu: normalizeSlash(base.resolvedPath),
        sourceXml: normalizeSlash(npc.sourceXml || ""),
        clipRefs: (npc.animations || []).map((a) => ({
          clipName: a.name || "clip",
          motionType: Number(a.motionType || 0),
          sourceAni: normalizeSlash(a.resolvedPath || ""),
          exists: fileExistsSafe(a.resolvedPath || "")
        }))
      });
    }
  }

  const weaponByName = new Map();
  for (const w of graph.weaponModels || []) {
    if (w && w.name) weaponByName.set(w.name, w);
  }

  const partByName = new Map();
  for (const p of graph.partsEntries || []) {
    const resolved = normalizeSlash(path.resolve(clientRoot, normalizeSlash(p.file || "")));
    for (const part of p.parts || []) {
      if (!partByName.has(part)) {
        partByName.set(part, {
          file: normalizeSlash(p.file || ""),
          resolvedPath: resolved
        });
      }
    }
  }

  for (const meshRow of graph.meshes || []) {
    const meshName = String(meshRow.meshName || "").trim();
    if (!meshName) continue;

    if (meshFilterEnabled && !minsetMeshes.has(meshName)) {
      continue;
    }

    if (meshRow.sourceKind === "weapon.xml") {
      const w = weaponByName.get(meshName);
      if (!w) continue;
      const base = (w.baseModels || [])[0];
      if (!base || !base.resolvedPath) continue;

      addTarget({
        modelId: meshRow.targetModelId || `weapon/${toSafePathSegment(meshName)}`,
        modelType: "weapon",
        sourceElu: normalizeSlash(base.resolvedPath),
        sourceXml: normalizeSlash(w.sourceXml || ""),
        meshName,
        clipRefs: (w.animations || []).map((a) => ({
          clipName: a.name || "clip",
          motionType: Number(a.motionType || w.weaponMotionType || 0),
          sourceAni: normalizeSlash(a.resolvedPath || ""),
          exists: fileExistsSafe(a.resolvedPath || "")
        }))
      });
    } else if (meshRow.sourceKind === "parts_index.xml") {
      const p = partByName.get(meshName);
      if (!p) continue;
      addTarget({
        modelId: meshRow.targetModelId || `parts/${toSafePathSegment(meshName)}`,
        modelType: "parts",
        sourceElu: normalizeSlash(p.resolvedPath),
        sourceXml: normalizeSlash(meshRow.sourceXml || ""),
        meshName,
        clipRefs: []
      });
    }
  }

  targets.sort((a, b) => String(a.modelId).localeCompare(String(b.modelId)));
  return targets;
}

function printUsage() {
  console.log("Usage: node elu_ani_to_glb.js --output-root <OpenGunZ-Client/system/rs3/open_assets> [--client-root <OpenGunZ-Client>] [--graph <docs/model_asset_graph_v1.json>] [--manifest <.../open_assets_manifest_v1.json>] [--minset <item_minset_v1.json>] [--allow-missing] [--include-npc]");
}

function main() {
  const args = parseArgs(process.argv);
  const root = path.resolve(__dirname, "..", "..", "..");

  const clientRoot = path.resolve(args["client-root"] || path.join(root, "OpenGunZ-Client"));
  const outputRoot = path.resolve(args["output-root"] || path.join(clientRoot, "system", "rs3", "open_assets"));
  const graphPath = path.resolve(args["graph"] || path.join(root, "docs", "model_asset_graph_v1.json"));
  const manifestPath = path.resolve(args["manifest"] || path.join(outputRoot, "open_assets_manifest_v1.json"));
  const minsetPath = args["minset"] ? path.resolve(args["minset"]) : path.join(clientRoot, "system", "rs3", "item_minset_v1.json");

  if (!fileExistsSafe(graphPath)) {
    throw new Error(`asset graph json not found: ${graphPath}`);
  }

  ensureDir(outputRoot);

  const graph = JSON.parse(readText(graphPath));

  const options = {
    clientRoot,
    fps: Number(args.fps || 30),
    includeCharacter: args["include-character"] == null ? true : !!args["include-character"],
    includeNpc: !!args["include-npc"],
    minset: minsetPath
  };

  const strict = !args["allow-missing"];

  const targets = buildTargets(graph, options);
  if (!targets.length) {
    throw new Error("no conversion targets selected");
  }

  const results = [];
  const missing = [];

  for (const target of targets) {
    const targetDir = path.join(outputRoot, normalizeSlash(target.modelId));
    ensureDir(targetDir);
    target.outputGlbPath = path.join(targetDir, "model.glb");

    const sourceMissing = !fileExistsSafe(target.sourceElu);

    const missingAnis = (target.clipRefs || []).filter((c) => !c.exists).map((c) => c.sourceAni);

    if (sourceMissing) {
      missing.push({ modelId: target.modelId, type: "source_elu", file: target.sourceElu });
    }

    for (const m of missingAnis) {
      missing.push({ modelId: target.modelId, type: "source_ani", file: m });
    }

    if (sourceMissing) {
      results.push({
        modelId: target.modelId,
        modelType: target.modelType,
        status: "missing_source",
        sourceElu: target.sourceElu,
        sourceXml: target.sourceXml,
        clipCount: target.clipRefs.length,
        missingAniCount: missingAnis.length
      });
      continue;
    }

    let build;
    try {
      build = buildGlbForTarget(target, options);
    } catch (err) {
      results.push({
        modelId: target.modelId,
        modelType: target.modelType,
        status: "error",
        sourceElu: target.sourceElu,
        sourceXml: target.sourceXml,
        error: err.message,
        clipCount: target.clipRefs.length,
        missingAniCount: missingAnis.length
      });
      continue;
    }

    fs.writeFileSync(target.outputGlbPath, build.glb);

    const sidecarPath = path.join(path.dirname(target.outputGlbPath), "source_meta.json");
    const sidecar = {
      modelId: target.modelId,
      modelType: target.modelType,
      sourceElu: normalizeSlash(target.sourceElu),
      sourceXml: normalizeSlash(target.sourceXml || ""),
      clipRefs: target.clipRefs,
      animationResults: build.animationResults,
      summary: build.summary,
      hashes: {
        glbSha256: hashFileSha256(target.outputGlbPath)
      }
    };
    fs.writeFileSync(sidecarPath, JSON.stringify(sidecar, null, 2), "utf8");

    results.push({
      modelId: target.modelId,
      modelType: target.modelType,
      status: "ok",
      sourceElu: normalizeSlash(target.sourceElu),
      sourceXml: normalizeSlash(target.sourceXml || ""),
      outputGlb: normalizeSlash(target.outputGlbPath),
      outputMeta: normalizeSlash(sidecarPath),
      summary: build.summary,
      clipCount: target.clipRefs.length,
      missingAniCount: missingAnis.length,
      animationCount: build.summary.animationCount,
      glbSha256: sidecar.hashes.glbSha256
    });
  }

  const okCount = results.filter((r) => r.status === "ok").length;
  const errorCount = results.filter((r) => r.status === "error").length;
  const missingSourceCount = results.filter((r) => r.status === "missing_source").length;

  const manifest = {
    version: "open_assets_manifest_v1",
    generatedAt: new Date().toISOString(),
    strict,
    inputs: {
      graphPath: normalizeSlash(graphPath),
      clientRoot: normalizeSlash(clientRoot),
      outputRoot: normalizeSlash(outputRoot),
      minset: fileExistsSafe(minsetPath) ? normalizeSlash(minsetPath) : ""
    },
    stats: {
      targetCount: targets.length,
      okCount,
      errorCount,
      missingSourceCount,
      missingDependencyCount: missing.length
    },
    missing,
    entries: results
  };

  ensureDir(path.dirname(manifestPath));
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2), "utf8");

  const reportLines = [];
  reportLines.push("# open_assets conversion report");
  reportLines.push("");
  reportLines.push(`Generated: ${manifest.generatedAt}`);
  reportLines.push("");
  reportLines.push("## Summary");
  reportLines.push("");
  reportLines.push(`- targets: **${targets.length}**`);
  reportLines.push(`- ok: **${okCount}**`);
  reportLines.push(`- error: **${errorCount}**`);
  reportLines.push(`- missing_source: **${missingSourceCount}**`);
  reportLines.push(`- missing dependencies: **${missing.length}**`);
  reportLines.push("");
  reportLines.push("## Entries");
  reportLines.push("");
  reportLines.push("| model_id | type | status | vertices | indices | animations | glb |" );
  reportLines.push("|---|---|---|---:|---:|---:|---|");
  for (const e of results) {
    const sv = e.summary || {};
    reportLines.push(`| ${e.modelId} | ${e.modelType} | ${e.status} | ${sv.vertexCount || 0} | ${sv.indexCount || 0} | ${sv.animationCount || 0} | ${e.outputGlb || "-"} |`);
  }

  reportLines.push("");
  reportLines.push("## Missing Dependencies");
  reportLines.push("");
  if (!missing.length) {
    reportLines.push("- none");
  } else {
    for (const m of missing) {
      reportLines.push(`- ${m.modelId} :: ${m.type} :: ${m.file}`);
    }
  }

  const reportPath = path.join(outputRoot, "conversion_report_open_assets_v1.md");
  fs.writeFileSync(reportPath, reportLines.join("\n"), "utf8");

  console.log(`[elu_ani_to_glb] targets=${targets.length} ok=${okCount} error=${errorCount} missing_source=${missingSourceCount}`);
  console.log(`[elu_ani_to_glb] manifest=${normalizeSlash(manifestPath)}`);
  console.log(`[elu_ani_to_glb] report=${normalizeSlash(reportPath)}`);

  if (strict && (missing.length > 0 || errorCount > 0 || missingSourceCount > 0)) {
    throw new Error(`strict mode failed: missing=${missing.length} error=${errorCount} missing_source=${missingSourceCount}`);
  }
}

if (process.argv.includes("--help") || process.argv.includes("-h")) {
  printUsage();
  process.exit(0);
}

try {
  main();
} catch (err) {
  console.error(`[elu_ani_to_glb] fatal: ${err && err.message ? err.message : err}`);
  process.exit(1);
}
