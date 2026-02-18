# rs3_scene_v1

Formato nativo de runtime para cenas estaticas RS3 no client DX11x64.

## Objetivo

- Eliminar parser RS2 (`.RS/.BSP/.COL/.LM`) do caminho de runtime.
- Carregar apenas pacote convertido offline.
- Empacotar texturas locais em PNG com alpha preservado.
- Para modelos 3D de personagem/arma/NPC, ver contrato separado em `docs/rs3_model_v1.md`.

## Layout do pacote

Diretorio alvo por cena:

- `scene.json`
- `world.bin`
- `collision.bin`
- `textures/*.png`
- `texture_manifest_v1.json`
- `conversion_report.md`
- `conversion_report_textures_v1.md`

## `scene.json` (metadados)

Campos obrigatorios:

- `version`: `rs3_scene_v1`
- `sceneId`
- `coordinateSystem`
- `cameraPos01` (`position`, `direction`)
- `cameraPos02` (`position`, `direction`)
- `charSpawn` (`position`, `direction`)
- `fog` (`enabled`, `min`, `max`, `color`)
- `lights`
- `worldChunk`: `world.bin`
- `collisionChunk`: `collision.bin`
- `textureManifest`: `texture_manifest_v1.json`
- `materials[]`
- `usedMaterialIndices[]`

`materials[]`:

- `materialIndex`
- `name`
- `flags`
- `sourceDiffuseMap`
- `packageTexture` (`textures/*.png`)

Campos de auditoria:

- `source`
- `stats`
- `hashes`

## `world.bin` (runtime)

Little-endian.

1. Header
- `char[8] magic = "RS3SCN1\0"`
- `u32 version = 1`
- `u32 vertexCount`
- `u32 indexCount`
- `u32 materialCount`
- `u32 sectionCount`
- `u32 lightCount`

2. Scene metadata
- `float3 cameraPos01`
- `float3 cameraDir01`
- `float3 cameraPos02`
- `float3 cameraDir02`
- `float3 spawnPos`
- `float3 spawnDir`
- `float fogMin`
- `float fogMax`
- `float3 fogColor`
- `u32 fogEnabled`
- `float3 boundsMin`
- `float3 boundsMax`

3. Materials (`materialCount`)
- `u32 flags`
- `u32 diffuseMapLen`
- `bytes[diffuseMapLen] diffuseMapUtf8`

4. Lights (`lightCount`)
- `float3 position`
- `float3 color`
- `float intensity`
- `float attenuationStart`
- `float attenuationEnd`

5. Sections (`sectionCount`)
- `u32 materialIndex`
- `u32 indexStart`
- `u32 indexCount`

6. Vertices (`vertexCount`)
- `float3 pos`
- `float3 normal`
- `float2 uv`

7. Indices (`indexCount`)
- `u32 index`

Observacao:

- Depois do `rs3_texture_converter`, `diffuseMap` de materiais usados deve apontar somente para `textures/*.png`.

## `collision.bin` (runtime)

Little-endian.

1. Header
- `char[8] magic = "RS3COL1\0"`
- `u32 version = 1`
- `u32 nodeCount`
- `i32 rootIndex`

2. Nodes (`nodeCount`)
- `float4 plane`
- `u8 solid`
- `i32 posChild`
- `i32 negChild`

## Pipeline oficial de geracao

1. Converter RS2 para `rs3_scene_v1`:

```powershell
node .\gunz-nakama-client\tools\rs2_scene_converter\rs2_scene_converter.js `
  --input .\OpenGunZ-Client\ui\Char-Creation-Select `
  --output .\OpenGunZ-Client\system\rs3\scenes\char_creation_select `
  --scene-id char_creation_select
```

2. Converter texturas usadas para PNG:

```powershell
node .\gunz-nakama-client\tools\rs3_texture_converter\rs3_texture_converter.js `
  --scene-dir .\OpenGunZ-Client\system\rs3\scenes\char_creation_select `
  --source-map-dir .\OpenGunZ-Client\ui\Char-Creation-Select `
  --scene-id char_creation_select
```

## Runtime

- Loader: `gunz-nakama-client/src/RealSpace3/Source/ScenePackageLoader.cpp`
- Integracao: `gunz-nakama-client/src/RealSpace3/Source/RScene.cpp` (`LoadCharSelectPackage`)
- Fallback: `LoadLobbyBasic()` quando pacote ausente/invalido
- Sem parser RS2 no runtime
- Material flags usados no draw pass:
- `RM_FLAG_USEALPHATEST`
- `RM_FLAG_USEOPACITY`
- `RM_FLAG_ADDITIVE`
- `RM_FLAG_TWOSIDED`
