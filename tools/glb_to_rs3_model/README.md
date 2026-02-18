# glb_to_rs3_model

Conversor offline de runtime assets: `GLB -> rs3_model_v1`.

## Uso

```powershell
node .\gunz-nakama-client\tools\glb_to_rs3_model\glb_to_rs3_model.js `
  --input-root .\OpenGunZ-Client\system\rs3\open_assets `
  --output-root .\OpenGunZ-Client\system\rs3\models `
  --allow-missing
```

## Saidas

Para cada `modelId`:

- `model.json`
- `mesh.bin`
- `skeleton.bin`
- `anim.bin`
- `materials.bin`
- `attachments.json`

Manifestos:

- `OpenGunZ-Client/system/rs3/models/rs3_model_manifest_v1.json`
- `OpenGunZ-Client/system/rs3/models/conversion_report_rs3_model_v1.md`
