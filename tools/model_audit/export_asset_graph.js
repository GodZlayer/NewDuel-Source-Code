#!/usr/bin/env node
/*
  export_asset_graph.js
  Audita catalogos OGZ e gera grafo de conversao para pipeline aberto (GLB -> rs3_model_v1).
*/

const fs = require("fs");
const path = require("path");

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

function readText(filePath) {
  return fs.readFileSync(filePath, "utf8");
}

function ensureDir(filePath) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
}

function parseAttributes(attrText) {
  const attrs = {};
  if (!attrText) return attrs;
  const re = /([A-Za-z0-9_:-]+)\s*=\s*"([^"]*)"/g;
  let m;
  while ((m = re.exec(attrText)) !== null) {
    attrs[m[1]] = m[2];
  }
  return attrs;
}

function parseStartTags(xmlText, tagName) {
  const out = [];
  const re = new RegExp(`<${tagName}\\b([^>]*)>`, "gi");
  let m;
  while ((m = re.exec(xmlText)) !== null) {
    out.push(parseAttributes(m[1] || ""));
  }
  return out;
}

function parsePartsIndex(xmlText) {
  const result = [];
  let currentMesh = "";

  const tokenRe = /<partslisting\b([^>]*)>|<parts\b([^>]*)\/?>|<\/partslisting>/gi;
  let m;
  while ((m = tokenRe.exec(xmlText)) !== null) {
    if (m[1] !== undefined) {
      const attrs = parseAttributes(m[1]);
      currentMesh = attrs.mesh || "";
      continue;
    }
    if (m[0].toLowerCase() === "</partslisting>") {
      currentMesh = "";
      continue;
    }
    const attrsText = m[2] || "";
    const fileAttrs = parseAttributes(attrsText);
    const parts = [];
    const partRe = /part\s*=\s*"([^"]*)"/gi;
    let pm;
    while ((pm = partRe.exec(attrsText)) !== null) {
      parts.push(pm[1]);
    }
    if (!parts.length) continue;
    result.push({
      mesh: currentMesh,
      file: normalizeSlash(fileAttrs.file || ""),
      parts
    });
  }
  return result;
}

function resolveAssetPath(clientRoot, ownerXmlPath, filenameValue) {
  const raw = normalizeSlash(filenameValue || "").trim();
  if (!raw) return "";

  const ownerDir = path.dirname(ownerXmlPath);

  if (/^[A-Za-z]:\//.test(raw)) return raw;

  const lower = raw.toLowerCase();
  if (lower.startsWith("model/") || lower.startsWith("system/") || lower.startsWith("interface/") || lower.startsWith("ui/")) {
    return normalizeSlash(path.resolve(clientRoot, raw));
  }

  return normalizeSlash(path.resolve(ownerDir, raw));
}

function parseModelXml(xmlPath, modelType, modelName, clientRoot) {
  const xmlText = readText(xmlPath);

  const baseModels = parseStartTags(xmlText, "AddBaseModel").map((attrs) => ({
    name: attrs.name || "",
    filename: normalizeSlash(attrs.filename || ""),
    resolvedPath: resolveAssetPath(clientRoot, xmlPath, attrs.filename || "")
  }));

  const parts = parseStartTags(xmlText, "AddParts").map((attrs) => ({
    name: attrs.name || "",
    filename: normalizeSlash(attrs.filename || ""),
    resolvedPath: resolveAssetPath(clientRoot, xmlPath, attrs.filename || "")
  }));

  const animations = parseStartTags(xmlText, "AddAnimation").map((attrs) => ({
    name: attrs.name || "",
    filename: normalizeSlash(attrs.filename || ""),
    motionType: Number(attrs.motion_type || 0),
    motionLoopType: attrs.motion_loop_type || "",
    resolvedPath: resolveAssetPath(clientRoot, xmlPath, attrs.filename || "")
  }));

  return {
    type: modelType,
    name: modelName,
    sourceXml: normalizeSlash(xmlPath),
    baseModels,
    parts,
    animations
  };
}

function parseWeaponCatalog(weaponXmlPath, clientRoot) {
  const xmlText = readText(weaponXmlPath);
  const blocks = [];

  const blockRe = /<AddWeaponElu\b([^>]*)>([\s\S]*?)<\/AddWeaponElu>/gi;
  let m;
  while ((m = blockRe.exec(xmlText)) !== null) {
    const attrs = parseAttributes(m[1] || "");
    const body = m[2] || "";

    const baseModels = parseStartTags(body, "AddBaseModel").map((bm) => ({
      name: bm.name || "",
      filename: normalizeSlash(bm.filename || ""),
      resolvedPath: resolveAssetPath(clientRoot, weaponXmlPath, bm.filename || "")
    }));

    const animations = parseStartTags(body, "AddAnimation").map((a) => ({
      name: a.name || "",
      filename: normalizeSlash(a.filename || ""),
      motionType: Number(a.motion_type || attrs.weapon_motion_type || 0),
      motionLoopType: a.motion_loop_type || "",
      resolvedPath: resolveAssetPath(clientRoot, weaponXmlPath, a.filename || "")
    }));

    blocks.push({
      type: "weapon",
      name: attrs.name || "",
      weaponMotionType: Number(attrs.weapon_motion_type || 0),
      weaponType: Number(attrs.weapon_type || 0),
      sourceXml: normalizeSlash(weaponXmlPath),
      baseModels,
      animations
    });
  }

  return blocks;
}

function fileExistsSafe(filePath) {
  try {
    return fs.existsSync(filePath) && fs.statSync(filePath).isFile();
  } catch {
    return false;
  }
}

function pickFirst(values) {
  for (const v of values || []) {
    if (v) return v;
  }
  return "";
}

function uniqSorted(arr) {
  return Array.from(new Set(arr || [])).sort((a, b) => String(a).localeCompare(String(b)));
}

function toModelId(prefix, meshName) {
  const slug = String(meshName || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_./-]+/g, "_")
    .replace(/_+/g, "_")
    .replace(/^_+|_+$/g, "");
  return `${prefix}/${slug || "unknown"}`;
}

function main() {
  const args = parseArgs(process.argv);
  const root = path.resolve(__dirname, "..", "..", "..");

  const clientRoot = path.resolve(args["client-root"] || path.join(root, "OpenGunZ-Client"));
  const zitemPath = path.resolve(args["zitem"] || path.join(root, "newduel-server", "data", "game", "zitem.json"));
  const outputDoc = path.resolve(args["output-doc"] || path.join(root, "docs", "model_asset_graph_v1.md"));
  const outputJson = path.resolve(args["output-json"] || path.join(root, "docs", "model_asset_graph_v1.json"));

  const characterCatalogPath = path.join(clientRoot, "Model", "character.xml");
  const npcCatalogPath = path.join(clientRoot, "Model", "NPC.xml");
  const weaponCatalogPath = path.join(clientRoot, "Model", "weapon.xml");
  const partsIndexPath = path.join(clientRoot, "system", "parts_index.xml");

  if (!fileExistsSafe(characterCatalogPath)) throw new Error(`character.xml not found: ${characterCatalogPath}`);
  if (!fileExistsSafe(npcCatalogPath)) throw new Error(`NPC.xml not found: ${npcCatalogPath}`);
  if (!fileExistsSafe(weaponCatalogPath)) throw new Error(`weapon.xml not found: ${weaponCatalogPath}`);
  if (!fileExistsSafe(partsIndexPath)) throw new Error(`parts_index.xml not found: ${partsIndexPath}`);
  if (!fileExistsSafe(zitemPath)) throw new Error(`zitem.json not found: ${zitemPath}`);

  const characterCatalogXml = readText(characterCatalogPath);
  const npcCatalogXml = readText(npcCatalogPath);

  const characterRefs = parseStartTags(characterCatalogXml, "AddXml").map((x) => ({
    name: x.name || "",
    filename: normalizeSlash(x.filename || ""),
    resolvedPath: resolveAssetPath(clientRoot, characterCatalogPath, x.filename || "")
  }));

  const npcRefs = parseStartTags(npcCatalogXml, "AddXml").map((x) => ({
    name: x.name || "",
    filename: normalizeSlash(x.filename || ""),
    resolvedPath: resolveAssetPath(clientRoot, npcCatalogPath, x.filename || "")
  }));

  const characterModels = [];
  for (const ref of characterRefs) {
    if (!fileExistsSafe(ref.resolvedPath)) continue;
    characterModels.push(parseModelXml(ref.resolvedPath, "character", ref.name, clientRoot));
  }

  const npcModels = [];
  for (const ref of npcRefs) {
    if (!fileExistsSafe(ref.resolvedPath)) continue;
    npcModels.push(parseModelXml(ref.resolvedPath, "npc", ref.name, clientRoot));
  }

  const weaponModels = parseWeaponCatalog(weaponCatalogPath, clientRoot);

  const partsIndexXml = readText(partsIndexPath);
  const partsEntries = parsePartsIndex(partsIndexXml);

  const partByName = new Map();
  for (const e of partsEntries) {
    for (const p of e.parts) {
      if (!partByName.has(p)) {
        partByName.set(p, {
          part: p,
          file: normalizeSlash(e.file),
          mesh: e.mesh || "",
          resolvedPath: resolveAssetPath(clientRoot, partsIndexPath, e.file)
        });
      }
    }
  }

  const weaponByName = new Map();
  for (const w of weaponModels) {
    if (!w.name) continue;
    if (!weaponByName.has(w.name)) weaponByName.set(w.name, w);
  }

  const zitem = JSON.parse(readText(zitemPath));
  const items = Array.isArray(zitem.items) ? zitem.items : [];

  const meshGraph = new Map();

  for (const item of items) {
    const meshName = String(item.mesh_name || "").trim();
    if (!meshName) continue;

    if (!meshGraph.has(meshName)) {
      meshGraph.set(meshName, {
        meshName,
        itemIds: [],
        slots: new Set(),
        types: new Set(),
        sourceKind: "unknown",
        sourceFile: "",
        sourceXml: "",
        targetModelId: toModelId("unknown", meshName),
        weaponMotionType: null,
        existsOnDisk: false
      });
    }

    const rec = meshGraph.get(meshName);
    rec.itemIds.push(Number(item.id || 0));
    rec.slots.add(String(item.slot || ""));
    rec.types.add(String(item.type || ""));
  }

  for (const rec of meshGraph.values()) {
    const weapon = weaponByName.get(rec.meshName);
    const part = partByName.get(rec.meshName);

    if (weapon) {
      rec.sourceKind = "weapon.xml";
      rec.sourceXml = weapon.sourceXml;
      rec.sourceFile = pickFirst((weapon.baseModels || []).map((x) => normalizeSlash(x.filename || "")));
      rec.targetModelId = toModelId("weapon", rec.meshName);
      rec.weaponMotionType = weapon.weaponMotionType;
      rec.existsOnDisk = fileExistsSafe(pickFirst((weapon.baseModels || []).map((x) => x.resolvedPath)));
      continue;
    }

    if (part) {
      rec.sourceKind = "parts_index.xml";
      rec.sourceXml = normalizeSlash(partsIndexPath);
      rec.sourceFile = normalizeSlash(part.file || "");
      rec.targetModelId = toModelId("parts", rec.meshName);
      rec.existsOnDisk = fileExistsSafe(part.resolvedPath);
      continue;
    }

    const characterOwner = characterModels.find((m) => m.name === rec.meshName);
    if (characterOwner) {
      rec.sourceKind = "character.xml";
      rec.sourceXml = characterOwner.sourceXml;
      rec.sourceFile = pickFirst((characterOwner.baseModels || []).map((x) => normalizeSlash(x.filename || "")));
      rec.targetModelId = toModelId("character", rec.meshName);
      rec.existsOnDisk = fileExistsSafe(pickFirst((characterOwner.baseModels || []).map((x) => x.resolvedPath)));
      continue;
    }

    const npcOwner = npcModels.find((m) => m.name === rec.meshName);
    if (npcOwner) {
      rec.sourceKind = "npc.xml";
      rec.sourceXml = npcOwner.sourceXml;
      rec.sourceFile = pickFirst((npcOwner.baseModels || []).map((x) => normalizeSlash(x.filename || "")));
      rec.targetModelId = toModelId("npc", rec.meshName);
      rec.existsOnDisk = fileExistsSafe(pickFirst((npcOwner.baseModels || []).map((x) => x.resolvedPath)));
      continue;
    }
  }

  const graphRows = Array.from(meshGraph.values())
    .map((r) => ({
      meshName: r.meshName,
      itemIds: uniqSorted(r.itemIds),
      slots: uniqSorted(Array.from(r.slots)),
      types: uniqSorted(Array.from(r.types)),
      sourceKind: r.sourceKind,
      sourceFile: r.sourceFile,
      sourceXml: r.sourceXml,
      targetModelId: r.targetModelId,
      weaponMotionType: r.weaponMotionType,
      existsOnDisk: !!r.existsOnDisk
    }))
    .sort((a, b) => a.meshName.localeCompare(b.meshName));

  const unresolved = graphRows.filter((r) => r.sourceKind === "unknown");
  const resolved = graphRows.length - unresolved.length;

  const summary = {
    zitemCount: items.length,
    uniqueMeshNameCount: graphRows.length,
    resolvedMeshNameCount: resolved,
    unresolvedMeshNameCount: unresolved.length,
    characterModelCount: characterModels.length,
    npcModelCount: npcModels.length,
    weaponModelCount: weaponModels.length,
    partsIndexPartCount: partByName.size
  };

  const now = new Date().toISOString();

  const jsonOut = {
    generatedAt: now,
    inputs: {
      clientRoot: normalizeSlash(clientRoot),
      zitemPath: normalizeSlash(zitemPath),
      characterCatalogPath: normalizeSlash(characterCatalogPath),
      npcCatalogPath: normalizeSlash(npcCatalogPath),
      weaponCatalogPath: normalizeSlash(weaponCatalogPath),
      partsIndexPath: normalizeSlash(partsIndexPath)
    },
    summary,
    meshes: graphRows,
    unresolvedMeshes: unresolved.map((r) => r.meshName),
    characterModels,
    npcModels,
    weaponModels,
    partsEntries
  };

  ensureDir(outputJson);
  fs.writeFileSync(outputJson, JSON.stringify(jsonOut, null, 2), "utf8");

  const lines = [];
  lines.push("# model_asset_graph_v1");
  lines.push("");
  lines.push(`Gerado em: ${now}`);
  lines.push("");
  lines.push("## Resumo");
  lines.push("");
  lines.push(`- zitem total: **${summary.zitemCount}**`);
  lines.push(`- mesh_name unicos: **${summary.uniqueMeshNameCount}**`);
  lines.push(`- mesh_name resolvidos: **${summary.resolvedMeshNameCount}**`);
  lines.push(`- mesh_name sem origem: **${summary.unresolvedMeshNameCount}**`);
  lines.push(`- character xmls carregados: **${summary.characterModelCount}**`);
  lines.push(`- npc xmls carregados: **${summary.npcModelCount}**`);
  lines.push(`- weapon entries carregadas: **${summary.weaponModelCount}**`);
  lines.push(`- partes mapeadas (parts_index): **${summary.partsIndexPartCount}**`);
  lines.push("");
  lines.push("## Itens para Conversao");
  lines.push("");
  lines.push("| mesh_name | source_kind | source_file | target_model_id | motion_type | item_ids | on_disk |" );
  lines.push("|---|---|---|---|---:|---|---|");
  for (const row of graphRows) {
    lines.push(`| ${row.meshName} | ${row.sourceKind} | ${row.sourceFile || "-"} | ${row.targetModelId} | ${row.weaponMotionType == null ? "-" : row.weaponMotionType} | ${row.itemIds.join(",")} | ${row.existsOnDisk ? "yes" : "no"} |`);
  }

  lines.push("");
  lines.push("## Character Base Catalog");
  lines.push("");
  for (const model of characterModels) {
    const base = pickFirst(model.baseModels.map((x) => x.filename));
    lines.push(`- ${model.name}: ${base || "(sem base model)"} (${model.animations.length} anims, ${model.parts.length} parts)`);
  }

  lines.push("");
  lines.push("## NPC Catalog");
  lines.push("");
  for (const model of npcModels) {
    const base = pickFirst(model.baseModels.map((x) => x.filename));
    lines.push(`- ${model.name}: ${base || "(sem base model)"} (${model.animations.length} anims)`);
  }

  lines.push("");
  lines.push("## Unresolved Meshes");
  lines.push("");
  if (!unresolved.length) {
    lines.push("- none");
  } else {
    for (const row of unresolved) {
      lines.push(`- ${row.meshName}`);
    }
  }

  lines.push("");
  lines.push("## Arquivos Gerados");
  lines.push("");
  lines.push(`- json: ${normalizeSlash(outputJson)}`);
  lines.push(`- markdown: ${normalizeSlash(outputDoc)}`);

  ensureDir(outputDoc);
  fs.writeFileSync(outputDoc, lines.join("\n"), "utf8");

  console.log(`[model_audit] ok uniqueMesh=${summary.uniqueMeshNameCount} resolved=${summary.resolvedMeshNameCount} unresolved=${summary.unresolvedMeshNameCount}`);
  console.log(`[model_audit] json=${normalizeSlash(outputJson)}`);
  console.log(`[model_audit] md=${normalizeSlash(outputDoc)}`);
}

try {
  main();
} catch (err) {
  console.error(`[model_audit] fatal: ${err && err.message ? err.message : err}`);
  process.exit(1);
}
