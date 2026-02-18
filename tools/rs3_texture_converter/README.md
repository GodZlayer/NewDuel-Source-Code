# rs3_texture_converter

Converte texturas usadas por uma cena `rs3_scene_v1` para PNG dentro do proprio pacote.

## Uso

```powershell
node .\gunz-nakama-client\tools\rs3_texture_converter\rs3_texture_converter.js `
  --scene-dir .\OpenGunZ-Client\system\rs3\scenes\char_creation_select `
  --source-map-dir .\OpenGunZ-Client\ui\Char-Creation-Select `
  --scene-id char_creation_select
```

## Saidas

- `textures/*.png`
- `texture_manifest_v1.json`
- `conversion_report_textures_v1.md`

## Regras

- Limpa `textures/` antes de gerar novamente.
- Converte apenas materiais realmente usados pelas `sections` do `world.bin`.
- Se faltar textura usada, falha explicitamente.
- Atualiza `world.bin` para apontar `diffuseMap` para `textures/*.png`.
- Atualiza `scene.json` (`materials[].packageTexture`, `usedMaterialIndices`, `textureManifest`).
