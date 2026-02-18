# rs2_scene_converter

Conversor offline RS2 -> `rs3_scene_v1` para cenas estaticas DX11x64.

## Uso

```powershell
node .\gunz-nakama-client\tools\rs2_scene_converter\rs2_scene_converter.js `
  --input .\OpenGunZ-Client\ui\Char-Creation-Select `
  --output .\OpenGunZ-Client\system\rs3\scenes\char_creation_select `
  --scene-id char_creation_select
```

## Entradas esperadas

No diretorio `--input`:
- `*.RS`
- `*.RS.xml`
- `*.RS.col`
- `*.RS.bsp` (opcional para auditoria)
- `*.RS.lm` (opcional para auditoria)

## Saidas

No diretorio `--output`:
- `scene.json`
- `world.bin`
- `collision.bin`
- `conversion_report.md`

## Observacoes

- O runtime do client nao le `.RS/.BSP/.COL/.LM`.
- Esta fase (L1) converte e renderiza apenas mundo estatico + metadata (camera/spawn/fog/lights).
- `OBJECTLIST` animado permanece para marco seguinte (L2).
