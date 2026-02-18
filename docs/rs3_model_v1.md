# rs3_model_v1

Formato nativo de runtime para personagens/armas/NPC no client DX11x64.

## Objetivo

- Remover parser de `.elu/.ani/.xml` do runtime.
- Consumir apenas pacote convertido offline.
- Manter fonte aberta de edicao em `GLB`.

## Pipeline oficial

1. Auditoria de catalogos:

```powershell
node .\gunz-nakama-client\tools\model_audit\export_asset_graph.js
```

2. Conversao source (`ELU/ANI/XML -> GLB`):

```powershell
node .\gunz-nakama-client\tools\elu_ani_to_glb\elu_ani_to_glb.js `
  --output-root .\OpenGunZ-Client\system\rs3\open_assets `
  --graph .\docs\model_asset_graph_v1.json `
  --minset .\OpenGunZ-Client\system\rs3\item_minset_v1.json `
  --allow-missing
```

3. Conversao runtime (`GLB -> rs3_model_v1`):

```powershell
node .\gunz-nakama-client\tools\glb_to_rs3_model\glb_to_rs3_model.js `
  --input-root .\OpenGunZ-Client\system\rs3\open_assets `
  --output-root .\OpenGunZ-Client\system\rs3\models `
  --allow-missing
```

## Layout do pacote runtime

Diretorio por `modelId` em `OpenGunZ-Client/system/rs3/models/<modelId>/`:

- `model.json`
- `mesh.bin`
- `skeleton.bin`
- `anim.bin`
- `materials.bin`
- `attachments.json`

## Contrato dos bins

### `mesh.bin`

- `char[8] magic = "RS3MSH1\0"`
- `u32 version = 1`
- `u32 vertexCount`
- `u32 indexCount`
- `u32 submeshCount`
- `u32 hasSkin`

Vertices (`vertexCount`):

- `float3 pos`
- `float3 normal`
- `float2 uv`
- `u16 joints[4]`
- `float weights[4]`

Indices (`indexCount`):

- `u32 index`

Submeshes (`submeshCount`):

- `u32 materialIndex`
- `u32 nodeIndex`
- `u32 indexStart`
- `u32 indexCount`

### `skeleton.bin`

- `char[8] magic = "RS3SKN1\0"`
- `u32 version = 1`
- `u32 boneCount`

Bones (`boneCount`):

- `i32 parentBone`
- `string name`
- `float4x4 bind`
- `float4x4 inverseBind`

### `anim.bin`

- `char[8] magic = "RS3ANI1\0"`
- `u32 version = 1`
- `u32 clipCount`

Clips (`clipCount`):

- `string clipName`
- `u32 channelCount`

Channels (`channelCount`):

- `i32 boneIndex`
- `u32 posKeyCount`
- `posKey`: `float time` + `float3 value`
- `u32 rotKeyCount`
- `rotKey`: `float time` + `float4 value`

### `materials.bin`

- `char[8] magic = "RS3MAT1\0"`
- `u32 version = 1`
- `u32 materialCount`

Materiais (`materialCount`):

- `u32 legacyFlags`
- `u32 alphaMode`
- `float metallic`
- `float roughness`
- `string baseColorTexture`
- `string normalTexture`
- `string ormTexture`
- `string emissiveTexture`
- `string opacityTexture`

## Runtime APIs (C++)

- `bool LoadModelPackage(const std::string& modelId, RS3ModelPackage& out, std::string* err);`
- `bool BuildCharacterVisual(const CharacterVisualRequest& req, CharacterVisualInstance& out, std::string* err);`
- `bool SetAnimationClipByName(const std::string& clipName, float blendSeconds);`

Implementacoes:

- `src/RealSpace3/Source/Model/ModelPackageLoader.cpp`
- `src/RealSpace3/Source/Model/CharacterAssembler.cpp`
- `src/RealSpace3/Source/Model/SkeletonPlayer.cpp`
- `src/RealSpace3/Source/Model/PbrMaterialSystem.cpp`

## Contrato server-driven (proximo passo)

Contrato alvo para bootstrap do client:

- `assetCatalogV2.meshMap[]`
- `meshName`
- `modelId`
- `motionType`
- `rigId`
- `clipSetId`

Com esse contrato, o runtime deixa de depender de `parts_index.xml`/`weapon.xml` em execucao e passa a resolver visual apenas por `modelId`.
