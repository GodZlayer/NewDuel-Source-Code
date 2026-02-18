# elu_ani_to_glb

Conversor offline de source assets: `ELU/ANI/XML -> GLB` (fonte aberta).

## Uso recomendado (minset atual)

```powershell
node .\gunz-nakama-client\tools\elu_ani_to_glb\elu_ani_to_glb.js `
  --output-root .\OpenGunZ-Client\system\rs3\open_assets `
  --graph .\docs\model_asset_graph_v1.json `
  --minset .\OpenGunZ-Client\system\rs3\item_minset_v1.json `
  --allow-missing
```

## Saidas

- `OpenGunZ-Client/system/rs3/open_assets/**/model.glb`
- `OpenGunZ-Client/system/rs3/open_assets/**/source_meta.json`
- `OpenGunZ-Client/system/rs3/open_assets/open_assets_manifest_v1.json`
- `OpenGunZ-Client/system/rs3/open_assets/conversion_report_open_assets_v1.md`
