#!/usr/bin/env node
/*
  glb_to_rs3_model.js
  Offline converter (runtime stage): GLB open assets -> rs3_model_v1 package.
*/

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

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

class Reader {
  constructor(buffer) {
    this.buf = buffer;
    this.off = 0;
  }

  ensure(sz) {
    if (this.off + sz > this.buf.length) throw new Error(`Unexpected EOF at ${this.off}, need ${sz}`);
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

  bytes(sz) {
    this.ensure(sz);
    const out = this.buf.slice(this.off, this.off + sz);
    this.off += sz;
    return out;
  }
}

class BinWriter {
  constructor() {
    this.parts = [];
  }

  bytes(buf) {
    this.parts.push(buf);
  }

  u8(v) {
    const b = Buffer.allocUnsafe(1);
    b.writeUInt8(v >>> 0, 0);
    this.parts.push(b);
  }

  u16(v) {
    const b = Buffer.allocUnsafe(2);
    b.writeUInt16LE(v >>> 0, 0);
    this.parts.push(b);
  }

  i32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeInt32LE(v | 0, 0);
    this.parts.push(b);
  }

  u32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeUInt32LE(v >>> 0, 0);
    this.parts.push(b);
  }

  f32(v) {
    const b = Buffer.allocUnsafe(4);
    b.writeFloatLE(Number(v), 0);
    this.parts.push(b);
  }

  str(v) {
    const s = Buffer.from(String(v || ""), "utf8");
    this.u32(s.length);
    if (s.length > 0) this.parts.push(s);
  }

  finish() {
    return Buffer.concat(this.parts);
  }
}

function mat4Identity() {
  return [
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
  ];
}

function transposeMat4(m) {
  return [
    m[0], m[4], m[8], m[12],
    m[1], m[5], m[9], m[13],
    m[2], m[6], m[10], m[14],
    m[3], m[7], m[11], m[15]
  ];
}

function quatToMat4(qx, qy, qz, qw) {
  const x2 = qx + qx;
  const y2 = qy + qy;
  const z2 = qz + qz;
  const xx = qx * x2;
  const xy = qx * y2;
  const xz = qx * z2;
  const yy = qy * y2;
  const yz = qy * z2;
  const zz = qz * z2;
  const wx = qw * x2;
  const wy = qw * y2;
  const wz = qw * z2;

  return [
    1 - (yy + zz), xy + wz, xz - wy, 0,
    xy - wz, 1 - (xx + zz), yz + wx, 0,
    xz + wy, yz - wx, 1 - (xx + yy), 0,
    0, 0, 0, 1
  ];
}

function mat4Multiply(a, b) {
  const out = new Array(16).fill(0);
  for (let r = 0; r < 4; r++) {
    for (let c = 0; c < 4; c++) {
      out[r * 4 + c] =
        a[r * 4 + 0] * b[0 * 4 + c] +
        a[r * 4 + 1] * b[1 * 4 + c] +
        a[r * 4 + 2] * b[2 * 4 + c] +
        a[r * 4 + 3] * b[3 * 4 + c];
    }
  }
  return out;
}

function mat4FromTRS(node) {
  const t = node.translation || [0, 0, 0];
  const r = node.rotation || [0, 0, 0, 1];
  const s = node.scale || [1, 1, 1];

  const rm = quatToMat4(r[0], r[1], r[2], r[3]);
  const sm = [
    s[0], 0, 0, 0,
    0, s[1], 0, 0,
    0, 0, s[2], 0,
    0, 0, 0, 1
  ];
  const tm = [
    1, 0, 0, t[0],
    0, 1, 0, t[1],
    0, 0, 1, t[2],
    0, 0, 0, 1
  ];
  // Build in glTF-style column-major then transpose to runtime row-major.
  return transposeMat4(mat4Multiply(tm, mat4Multiply(rm, sm)));
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
    return mat4Identity();
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

function parseGlb(glbPath) {
  const buf = fs.readFileSync(glbPath);
  const r = new Reader(buf);

  const magic = r.u32();
  const version = r.u32();
  const length = r.u32();

  if (magic !== 0x46546c67) throw new Error("GLB magic mismatch");
  if (version !== 2) throw new Error(`GLB version unsupported: ${version}`);
  if (length !== buf.length) throw new Error("GLB length mismatch");

  let json = null;
  let bin = Buffer.alloc(0);

  while (r.off < buf.length) {
    const chunkLength = r.u32();
    const chunkType = r.u32();
    const chunkData = r.bytes(chunkLength);

    if (chunkType === 0x4e4f534a) {
      json = JSON.parse(chunkData.toString("utf8"));
    } else if (chunkType === 0x004e4942) {
      bin = chunkData;
    }
  }

  if (!json) throw new Error("GLB JSON chunk not found");

  return { json, bin };
}

function componentTypeSize(componentType) {
  switch (componentType) {
    case 5120:
    case 5121:
      return 1;
    case 5122:
    case 5123:
      return 2;
    case 5125:
    case 5126:
      return 4;
    default:
      throw new Error(`Unsupported componentType ${componentType}`);
  }
}

function typeComponentCount(type) {
  switch (type) {
    case "SCALAR": return 1;
    case "VEC2": return 2;
    case "VEC3": return 3;
    case "VEC4": return 4;
    case "MAT4": return 16;
    default: throw new Error(`Unsupported accessor type ${type}`);
  }
}

function readAccessor(gltf, bin, accessorIndex) {
  if (accessorIndex == null) return null;
  const accessor = gltf.accessors[accessorIndex];
  if (!accessor) throw new Error(`Accessor ${accessorIndex} not found`);

  const view = gltf.bufferViews[accessor.bufferView];
  if (!view) throw new Error(`BufferView ${accessor.bufferView} not found`);

  if (accessor.sparse) throw new Error("Sparse accessor is not supported");

  const itemCount = accessor.count;
  const compCount = typeComponentCount(accessor.type);
  const compSize = componentTypeSize(accessor.componentType);

  const viewOffset = view.byteOffset || 0;
  const accOffset = accessor.byteOffset || 0;
  const stride = view.byteStride || (compCount * compSize);

  const out = [];

  for (let i = 0; i < itemCount; i++) {
    const base = viewOffset + accOffset + i * stride;
    const item = [];
    for (let c = 0; c < compCount; c++) {
      const off = base + c * compSize;
      let v;
      switch (accessor.componentType) {
        case 5126: v = bin.readFloatLE(off); break;
        case 5125: v = bin.readUInt32LE(off); break;
        case 5123: v = bin.readUInt16LE(off); break;
        case 5121: v = bin.readUInt8(off); break;
        case 5122: v = bin.readInt16LE(off); break;
        case 5120: v = bin.readInt8(off); break;
        default: throw new Error(`Unsupported accessor componentType ${accessor.componentType}`);
      }
      item.push(v);
    }
    out.push(compCount === 1 ? item[0] : item);
  }

  return {
    accessor,
    values: out
  };
}

function nodeMatrix(node) {
  if (Array.isArray(node.matrix) && node.matrix.length === 16) {
    // glTF stores MAT4 in column-major order.
    return transposeMat4(node.matrix.slice(0, 16));
  }
  return mat4FromTRS(node || {});
}

function alphaModeToEnum(m) {
  if (!m) return 0;
  const mode = String(m.alphaMode || "OPAQUE").toUpperCase();
  if (mode === "MASK") return 1;
  if (mode === "BLEND") return 2;
  return 0;
}

function collectParentIndices(gltf) {
  const parentByNode = new Array((gltf.nodes || []).length).fill(-1);
  (gltf.nodes || []).forEach((n, idx) => {
    for (const c of n.children || []) {
      if (c >= 0 && c < parentByNode.length) parentByNode[c] = idx;
    }
  });
  return parentByNode;
}

function extractModelFromGlb(gltf, bin) {
  const parentByNode = collectParentIndices(gltf);

  const meshVertices = [];
  const meshIndices = [];
  const submeshes = [];

  const nodeToMeshPairs = [];
  (gltf.nodes || []).forEach((n, idx) => {
    if (n.mesh != null) nodeToMeshPairs.push({ nodeIndex: idx, meshIndex: n.mesh, skinIndex: n.skin != null ? n.skin : null });
  });

  for (const pair of nodeToMeshPairs) {
    const mesh = (gltf.meshes || [])[pair.meshIndex];
    if (!mesh) continue;
    const meshNode = (gltf.nodes || [])[pair.nodeIndex] || {};
    const meshNodeMatrix = nodeMatrix(meshNode);

    for (const prim of mesh.primitives || []) {
      const posAcc = readAccessor(gltf, bin, prim.attributes ? prim.attributes.POSITION : null);
      if (!posAcc) continue;

      const nrmAcc = readAccessor(gltf, bin, prim.attributes ? prim.attributes.NORMAL : null);
      const uvAcc = readAccessor(gltf, bin, prim.attributes ? prim.attributes.TEXCOORD_0 : null);
      const jAcc = readAccessor(gltf, bin, prim.attributes ? prim.attributes.JOINTS_0 : null);
      const wAcc = readAccessor(gltf, bin, prim.attributes ? prim.attributes.WEIGHTS_0 : null);

      const idxAcc = readAccessor(gltf, bin, prim.indices);

      const vertexBase = meshVertices.length;
      const positions = posAcc.values;

      for (let i = 0; i < positions.length; i++) {
        const p = positions[i] || [0, 0, 0];
        const n = (nrmAcc && nrmAcc.values[i]) || [0, 1, 0];
        const uv = (uvAcc && uvAcc.values[i]) || [0, 0];
        const j = (jAcc && jAcc.values[i]) || [0, 0, 0, 0];
        const w = (wAcc && wAcc.values[i]) || [1, 0, 0, 0];

        meshVertices.push({
          pos: [Number(p[0]) || 0, Number(p[1]) || 0, Number(p[2]) || 0],
          normal: [Number(n[0]) || 0, Number(n[1]) || 1, Number(n[2]) || 0],
          // Open assets GLB are authored with OpenGL-style V orientation.
          // Runtime RS3 (DX11) expects D3D-style V, so normalize in offline conversion.
          uv: [Number(uv[0]) || 0, 1.0 - (Number(uv[1]) || 0)],
          joints: [
            Number(j[0]) || 0,
            Number(j[1]) || 0,
            Number(j[2]) || 0,
            Number(j[3]) || 0
          ],
          weights: [
            Number(w[0]) || 0,
            Number(w[1]) || 0,
            Number(w[2]) || 0,
            Number(w[3]) || 0
          ]
        });
      }

      const indexStart = meshIndices.length;
      if (idxAcc) {
        for (const idx of idxAcc.values) {
          meshIndices.push(vertexBase + Number(idx || 0));
        }
      } else {
        for (let i = 0; i < positions.length; i++) {
          meshIndices.push(vertexBase + i);
        }
      }

      submeshes.push({
        materialIndex: prim.material != null ? prim.material : 0,
        nodeIndex: pair.nodeIndex,
        indexStart,
        indexCount: meshIndices.length - indexStart,
        nodeTransform: meshNodeMatrix.slice(0, 16)
      });
    }
  }

  const skin = (gltf.skins || [])[0] || null;
  const joints = skin ? (skin.joints || []) : [];

  const invBindAccessor = skin && skin.inverseBindMatrices != null ? readAccessor(gltf, bin, skin.inverseBindMatrices) : null;

  const jointToBoneIndex = new Map();
  const bones = [];

  if (joints.length > 0) {
    const nodeToJointIndex = new Map();
    joints.forEach((nodeIdx, idx) => nodeToJointIndex.set(nodeIdx, idx));

    for (let i = 0; i < joints.length; i++) {
      const nodeIdx = joints[i];
      const node = (gltf.nodes || [])[nodeIdx] || {};
      const nodeParent = parentByNode[nodeIdx];
      const parentJoint = nodeToJointIndex.has(nodeParent) ? nodeToJointIndex.get(nodeParent) : -1;

      const bind = nodeMatrix(node);
      let invBind = invertMat4(bind);
      if (invBindAccessor && invBindAccessor.values && invBindAccessor.values[i]) {
        invBind = transposeMat4(invBindAccessor.values[i].slice(0, 16));
      }

      jointToBoneIndex.set(nodeIdx, i);
      bones.push({
        name: node.name || `bone_${i}`,
        nodeIndex: nodeIdx,
        parentBone: parentJoint,
        bind,
        invBind
      });
    }
  } else {
    for (let i = 0; i < (gltf.nodes || []).length; i++) {
      const node = gltf.nodes[i];
      const parent = parentByNode[i];
      const bind = nodeMatrix(node);
      const invBind = invertMat4(bind);
      jointToBoneIndex.set(i, i);
      bones.push({
        name: node.name || `bone_${i}`,
        nodeIndex: i,
        parentBone: parent,
        bind,
        invBind
      });
    }
  }

  const materials = (gltf.materials || []).map((m, idx) => {
    const pbr = (m && m.pbrMetallicRoughness) || {};
    const baseColorTextureIndex = pbr.baseColorTexture ? pbr.baseColorTexture.index : null;
    const normalTextureIndex = m && m.normalTexture ? m.normalTexture.index : null;
    const emissiveTextureIndex = m && m.emissiveTexture ? m.emissiveTexture.index : null;

    const baseColorTexture = textureUriFromIndex(gltf, baseColorTextureIndex);
    const normalTexture = textureUriFromIndex(gltf, normalTextureIndex);
    const emissiveTexture = textureUriFromIndex(gltf, emissiveTextureIndex);

    return {
      materialIndex: idx,
      name: m && m.name ? m.name : `material_${idx}`,
      alphaMode: alphaModeToEnum(m),
      legacyFlags: Number(m && m.extras && m.extras.legacyFlags) || 0,
      metallic: Number(pbr.metallicFactor) || 0,
      roughness: Number(pbr.roughnessFactor) || 1,
      baseColorTexture,
      normalTexture,
      ormTexture: "",
      emissiveTexture,
      opacityTexture: (m && m.extras && m.extras.opacityTexture) ? String(m.extras.opacityTexture) : ""
    };
  });

  const boneIndexByNode = new Map();
  bones.forEach((b, i) => boneIndexByNode.set(b.nodeIndex, i));

  const clips = [];
  for (const anim of gltf.animations || []) {
    const clip = {
      name: anim.name || "clip",
      channels: []
    };

    const channelMap = new Map();

    for (const ch of anim.channels || []) {
      const sampler = (anim.samplers || [])[ch.sampler];
      if (!sampler || !ch.target) continue;

      const nodeIdx = ch.target.node;
      const boneIndex = boneIndexByNode.has(nodeIdx) ? boneIndexByNode.get(nodeIdx) : -1;
      if (boneIndex < 0) continue;

      const input = readAccessor(gltf, bin, sampler.input);
      const output = readAccessor(gltf, bin, sampler.output);
      if (!input || !output) continue;

      if (!channelMap.has(boneIndex)) {
        channelMap.set(boneIndex, {
          boneIndex,
          posKeys: [],
          rotKeys: []
        });
      }

      const dst = channelMap.get(boneIndex);

      if (ch.target.path === "translation") {
        for (let i = 0; i < input.values.length; i++) {
          const time = Number(input.values[i]) || 0;
          const v = output.values[i] || [0, 0, 0];
          dst.posKeys.push({ time, value: [Number(v[0]) || 0, Number(v[1]) || 0, Number(v[2]) || 0] });
        }
      } else if (ch.target.path === "rotation") {
        for (let i = 0; i < input.values.length; i++) {
          const time = Number(input.values[i]) || 0;
          const v = output.values[i] || [0, 0, 0, 1];
          dst.rotKeys.push({ time, value: [Number(v[0]) || 0, Number(v[1]) || 0, Number(v[2]) || 0, Number(v[3]) || 1] });
        }
      }
    }

    clip.channels = Array.from(channelMap.values()).sort((a, b) => a.boneIndex - b.boneIndex);
    clips.push(clip);
  }

  const attachments = {
    sockets: [],
    byName: {}
  };

  (gltf.nodes || []).forEach((n, idx) => {
    const name = String(n.name || "");
    const lname = name.toLowerCase();
    if (!name) return;

    if (lname.includes("muzzle_flash") || lname.includes("empty_cartridge01") || lname.includes("empty_cartridge02") || lname.startsWith("eq_w") || lname.startsWith("dummy")) {
      attachments.sockets.push({ name, nodeIndex: idx });
      attachments.byName[name] = idx;
    }
  });

  return {
    vertices: meshVertices,
    indices: meshIndices,
    submeshes,
    bones,
    clips,
    materials,
    attachments,
    stats: {
      vertexCount: meshVertices.length,
      indexCount: meshIndices.length,
      submeshCount: submeshes.length,
      boneCount: bones.length,
      clipCount: clips.length,
      materialCount: materials.length
    }
  };
}

function textureUriFromIndex(gltf, textureIndex) {
  if (textureIndex == null) return "";
  const tex = (gltf.textures || [])[textureIndex];
  if (!tex) return "";
  const imageIndex = Number(tex.source);
  const img = (gltf.images || [])[imageIndex];
  if (!img) return "";
  if (img.uri) {
    return normalizeSlash(String(img.uri));
  }
  if (img.bufferView != null) {
    return `__embedded_image_${imageIndex}__`;
  }
  return "";
}

function extFromMime(mimeType) {
  const lower = String(mimeType || "").toLowerCase();
  if (lower.includes("jpeg") || lower.includes("jpg")) return "jpg";
  if (lower.includes("png")) return "png";
  if (lower.includes("webp")) return "webp";
  if (lower.includes("bmp")) return "bmp";
  if (lower.includes("tga")) return "tga";
  return "bin";
}

function decodeDataUri(uri) {
  if (!uri || !String(uri).startsWith("data:")) return Buffer.alloc(0);
  const text = String(uri);
  const comma = text.indexOf(",");
  if (comma < 0) return Buffer.alloc(0);
  const meta = text.slice(0, comma).toLowerCase();
  const payload = text.slice(comma + 1);
  if (meta.endsWith(";base64")) {
    return Buffer.from(payload, "base64");
  }
  return Buffer.from(decodeURIComponent(payload), "utf8");
}

function materializeEmbeddedTextures(gltf, bin, modelDir, materials) {
  const images = gltf.images || [];
  const views = gltf.bufferViews || [];
  const written = new Map();

  function writeImage(imageIndex) {
    if (written.has(imageIndex)) {
      return written.get(imageIndex);
    }

    const img = images[imageIndex];
    if (!img) return "";

    if (img.uri && !String(img.uri).startsWith("data:")) {
      const rel = normalizeSlash(String(img.uri));
      written.set(imageIndex, rel);
      return rel;
    }

    let bytes = Buffer.alloc(0);
    if (img.bufferView != null) {
      const bv = views[img.bufferView];
      if (bv) {
        const byteOffset = Number(bv.byteOffset) || 0;
        const byteLength = Number(bv.byteLength) || 0;
        const end = byteOffset + byteLength;
        if (byteLength > 0 && end <= bin.length) {
          bytes = bin.slice(byteOffset, end);
        }
      }
    } else if (img.uri && String(img.uri).startsWith("data:")) {
      bytes = decodeDataUri(img.uri);
    }

    if (!bytes.length) return "";

    const texturesDir = path.join(modelDir, "textures");
    ensureDir(texturesDir);
    const ext = extFromMime(img.mimeType);
    const fileName = `embedded_${imageIndex}.${ext}`;
    const outPath = path.join(texturesDir, fileName);
    fs.writeFileSync(outPath, bytes);
    const rel = normalizeSlash(path.join("textures", fileName));
    written.set(imageIndex, rel);
    return rel;
  }

  function resolveTextureRef(v) {
    const token = String(v || "");
    const match = /^__embedded_image_(\d+)__$/.exec(token);
    if (!match) return token;
    const imageIndex = Number(match[1]);
    return writeImage(imageIndex);
  }

  for (const m of materials) {
    m.baseColorTexture = resolveTextureRef(m.baseColorTexture);
    m.normalTexture = resolveTextureRef(m.normalTexture);
    m.ormTexture = resolveTextureRef(m.ormTexture);
    m.emissiveTexture = resolveTextureRef(m.emissiveTexture);
    m.opacityTexture = resolveTextureRef(m.opacityTexture);
  }
}

function writeMeshBin(model, outPath) {
  const w = new BinWriter();
  w.bytes(Buffer.from([0x52, 0x53, 0x33, 0x4d, 0x53, 0x48, 0x31, 0x00])); // RS3MSH1\0
  w.u32(2);
  w.u32(model.vertices.length);
  w.u32(model.indices.length);
  w.u32(model.submeshes.length);
  w.u32(model.bones.length > 0 ? 1 : 0);

  for (const v of model.vertices) {
    w.f32(v.pos[0]); w.f32(v.pos[1]); w.f32(v.pos[2]);
    w.f32(v.normal[0]); w.f32(v.normal[1]); w.f32(v.normal[2]);
    w.f32(v.uv[0]); w.f32(v.uv[1]);
    w.u16(v.joints[0]); w.u16(v.joints[1]); w.u16(v.joints[2]); w.u16(v.joints[3]);
    w.f32(v.weights[0]); w.f32(v.weights[1]); w.f32(v.weights[2]); w.f32(v.weights[3]);
  }

  for (const i of model.indices) {
    w.u32(i >>> 0);
  }

  for (const s of model.submeshes) {
    w.u32(s.materialIndex >>> 0);
    w.u32(s.nodeIndex >>> 0);
    w.u32(s.indexStart >>> 0);
    w.u32(s.indexCount >>> 0);
    const nodeTransform = Array.isArray(s.nodeTransform) && s.nodeTransform.length === 16 ? s.nodeTransform : mat4Identity();
    for (let i = 0; i < 16; i++) {
      w.f32(Number(nodeTransform[i]) || 0);
    }
  }

  fs.writeFileSync(outPath, w.finish());
}

function writeSkeletonBin(model, outPath) {
  const w = new BinWriter();
  w.bytes(Buffer.from([0x52, 0x53, 0x33, 0x53, 0x4b, 0x4e, 0x31, 0x00])); // RS3SKN1\0
  w.u32(1);
  w.u32(model.bones.length);

  for (const b of model.bones) {
    w.i32(b.parentBone);
    w.str(b.name);
    for (const v of b.bind) w.f32(v);
    for (const v of b.invBind) w.f32(v);
  }

  fs.writeFileSync(outPath, w.finish());
}

function writeAnimBin(model, outPath) {
  const w = new BinWriter();
  w.bytes(Buffer.from([0x52, 0x53, 0x33, 0x41, 0x4e, 0x49, 0x31, 0x00])); // RS3ANI1\0
  w.u32(1);
  w.u32(model.clips.length);

  for (const clip of model.clips) {
    w.str(clip.name);
    w.u32(clip.channels.length);

    for (const ch of clip.channels) {
      w.i32(ch.boneIndex);

      w.u32(ch.posKeys.length);
      for (const k of ch.posKeys) {
        w.f32(k.time);
        w.f32(k.value[0]); w.f32(k.value[1]); w.f32(k.value[2]);
      }

      w.u32(ch.rotKeys.length);
      for (const k of ch.rotKeys) {
        w.f32(k.time);
        w.f32(k.value[0]); w.f32(k.value[1]); w.f32(k.value[2]); w.f32(k.value[3]);
      }
    }
  }

  fs.writeFileSync(outPath, w.finish());
}

function writeMaterialsBin(model, outPath) {
  const w = new BinWriter();
  w.bytes(Buffer.from([0x52, 0x53, 0x33, 0x4d, 0x41, 0x54, 0x31, 0x00])); // RS3MAT1\0
  w.u32(1);
  w.u32(model.materials.length);

  for (const m of model.materials) {
    w.u32(m.legacyFlags >>> 0);
    w.u32(m.alphaMode >>> 0);
    w.f32(m.metallic);
    w.f32(m.roughness);
    w.str(m.baseColorTexture || "");
    w.str(m.normalTexture || "");
    w.str(m.ormTexture || "");
    w.str(m.emissiveTexture || "");
    w.str(m.opacityTexture || "");
  }

  fs.writeFileSync(outPath, w.finish());
}

function writeModelJson(modelId, sourceGlb, model, outPath) {
  const modelJson = {
    version: "rs3_model_v1",
    modelId,
    sourceGlb: normalizeSlash(sourceGlb),
    files: {
      mesh: "mesh.bin",
      skeleton: "skeleton.bin",
      animation: "anim.bin",
      materials: "materials.bin",
      attachments: "attachments.json"
    },
    stats: model.stats,
    rigId: model.bones.length > 0 ? model.bones[0].name : "",
    clipSetId: `${modelId}_clips`,
    materialPolicy: "pbr_v1",
    generatedAt: new Date().toISOString()
  };

  fs.writeFileSync(outPath, JSON.stringify(modelJson, null, 2), "utf8");
}

function loadInputEntries(args, inputRoot) {
  const manifestPath = args.manifest ? path.resolve(args.manifest) : path.join(inputRoot, "open_assets_manifest_v1.json");

  if (fileExistsSafe(manifestPath)) {
    const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
    const entries = (manifest.entries || [])
      .filter((e) => e.status === "ok" && e.outputGlb)
      .map((e) => ({ modelId: e.modelId, glbPath: path.resolve(e.outputGlb) }));
    return { entries, sourceManifestPath: manifestPath };
  }

  const entries = [];
  walkFiles(inputRoot, (f) => {
    if (path.basename(f).toLowerCase() === "model.glb") {
      const rel = normalizeSlash(path.relative(inputRoot, path.dirname(f)));
      entries.push({ modelId: rel, glbPath: f });
    }
  });

  return { entries, sourceManifestPath: "" };
}

function walkFiles(root, visitor) {
  const stack = [root];
  while (stack.length > 0) {
    const cur = stack.pop();
    const list = fs.readdirSync(cur, { withFileTypes: true });
    for (const d of list) {
      const fp = path.join(cur, d.name);
      if (d.isDirectory()) stack.push(fp);
      else if (d.isFile()) visitor(fp);
    }
  }
}

function main() {
  const args = parseArgs(process.argv);
  const root = path.resolve(__dirname, "..", "..", "..");

  const inputRoot = path.resolve(args["input-root"] || path.join(root, "OpenGunZ-Client", "system", "rs3", "open_assets"));
  const outputRoot = path.resolve(args["output-root"] || path.join(root, "OpenGunZ-Client", "system", "rs3", "models"));
  const outputManifestPath = path.resolve(args["out-manifest"] || path.join(outputRoot, "rs3_model_manifest_v1.json"));

  if (!fs.existsSync(inputRoot) || !fs.statSync(inputRoot).isDirectory()) {
    throw new Error(`input-root not found: ${inputRoot}`);
  }

  ensureDir(outputRoot);

  const strict = !args["allow-missing"];

  const input = loadInputEntries(args, inputRoot);
  const entries = input.entries;
  if (!entries.length) {
    throw new Error("No GLB entries found to convert");
  }

  const results = [];
  const missing = [];

  for (const e of entries) {
    if (!fileExistsSafe(e.glbPath)) {
      missing.push({ modelId: e.modelId, glbPath: e.glbPath, reason: "GLB_NOT_FOUND" });
      results.push({ modelId: e.modelId, status: "missing_glb", sourceGlb: normalizeSlash(e.glbPath) });
      continue;
    }

    let parsed;
    let extracted;
    try {
      parsed = parseGlb(e.glbPath);
      extracted = extractModelFromGlb(parsed.json, parsed.bin);
    } catch (err) {
      results.push({
        modelId: e.modelId,
        status: "error",
        sourceGlb: normalizeSlash(e.glbPath),
        error: err.message
      });
      continue;
    }

    const modelDir = path.join(outputRoot, normalizeSlash(e.modelId));
    ensureDir(modelDir);
    materializeEmbeddedTextures(parsed.json, parsed.bin, modelDir, extracted.materials);

    const meshPath = path.join(modelDir, "mesh.bin");
    const skeletonPath = path.join(modelDir, "skeleton.bin");
    const animPath = path.join(modelDir, "anim.bin");
    const materialsPath = path.join(modelDir, "materials.bin");
    const attachmentsPath = path.join(modelDir, "attachments.json");
    const modelJsonPath = path.join(modelDir, "model.json");

    writeMeshBin(extracted, meshPath);
    writeSkeletonBin(extracted, skeletonPath);
    writeAnimBin(extracted, animPath);
    writeMaterialsBin(extracted, materialsPath);
    fs.writeFileSync(attachmentsPath, JSON.stringify(extracted.attachments, null, 2), "utf8");
    writeModelJson(e.modelId, e.glbPath, extracted, modelJsonPath);

    const outEntry = {
      modelId: e.modelId,
      status: "ok",
      sourceGlb: normalizeSlash(e.glbPath),
      outputDir: normalizeSlash(modelDir),
      stats: extracted.stats,
      hashes: {
        mesh: hashFileSha256(meshPath),
        skeleton: hashFileSha256(skeletonPath),
        animation: hashFileSha256(animPath),
        materials: hashFileSha256(materialsPath),
        modelJson: hashFileSha256(modelJsonPath)
      }
    };

    results.push(outEntry);
  }

  const okCount = results.filter((r) => r.status === "ok").length;
  const errorCount = results.filter((r) => r.status === "error").length;
  const missingCount = results.filter((r) => r.status === "missing_glb").length;

  const manifest = {
    version: "rs3_model_manifest_v1",
    generatedAt: new Date().toISOString(),
    strict,
    inputs: {
      inputRoot: normalizeSlash(inputRoot),
      sourceManifestPath: normalizeSlash(input.sourceManifestPath || "")
    },
    outputRoot: normalizeSlash(outputRoot),
    stats: {
      totalEntries: entries.length,
      okCount,
      errorCount,
      missingCount,
      missingDependencyCount: missing.length
    },
    missing,
    entries: results
  };

  ensureDir(path.dirname(outputManifestPath));
  fs.writeFileSync(outputManifestPath, JSON.stringify(manifest, null, 2), "utf8");

  const reportPath = path.join(outputRoot, "conversion_report_rs3_model_v1.md");
  const report = [];
  report.push("# rs3_model_v1 conversion report");
  report.push("");
  report.push(`Generated: ${manifest.generatedAt}`);
  report.push("");
  report.push("## Summary");
  report.push("");
  report.push(`- entries: **${entries.length}**`);
  report.push(`- ok: **${okCount}**`);
  report.push(`- error: **${errorCount}**`);
  report.push(`- missing_glb: **${missingCount}**`);
  report.push("");
  report.push("## Entries");
  report.push("");
  report.push("| model_id | status | vertices | indices | bones | clips | output_dir |" );
  report.push("|---|---|---:|---:|---:|---:|---|");
  for (const e of results) {
    const st = e.stats || {};
    report.push(`| ${e.modelId} | ${e.status} | ${st.vertexCount || 0} | ${st.indexCount || 0} | ${st.boneCount || 0} | ${st.clipCount || 0} | ${e.outputDir || "-"} |`);
  }

  fs.writeFileSync(reportPath, report.join("\n"), "utf8");

  console.log(`[glb_to_rs3_model] entries=${entries.length} ok=${okCount} error=${errorCount} missing=${missingCount}`);
  console.log(`[glb_to_rs3_model] manifest=${normalizeSlash(outputManifestPath)}`);
  console.log(`[glb_to_rs3_model] report=${normalizeSlash(reportPath)}`);

  if (strict && (errorCount > 0 || missingCount > 0 || missing.length > 0)) {
    throw new Error(`strict mode failed: error=${errorCount} missing_glb=${missingCount} missing_dep=${missing.length}`);
  }
}

function printUsage() {
  console.log("Usage: node glb_to_rs3_model.js --input-root <.../open_assets> --output-root <.../models> [--manifest <open_assets_manifest_v1.json>] [--out-manifest <...>] [--allow-missing]");
}

if (process.argv.includes("--help") || process.argv.includes("-h")) {
  printUsage();
  process.exit(0);
}

try {
  main();
} catch (err) {
  console.error(`[glb_to_rs3_model] fatal: ${err && err.message ? err.message : err}`);
  process.exit(1);
}
