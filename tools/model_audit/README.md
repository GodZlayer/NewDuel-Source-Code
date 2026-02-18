# model_audit

Gera o grafo auditavel de assets para conversao OGZ -> NDG.

## Uso

```powershell
node .\gunz-nakama-client\tools\model_audit\export_asset_graph.js
```

## Saidas padrao

- `docs/model_asset_graph_v1.json`
- `docs/model_asset_graph_v1.md`

## Opcional

```powershell
node .\gunz-nakama-client\tools\model_audit\export_asset_graph.js `
  --client-root .\OpenGunZ-Client `
  --zitem .\newduel-server\data\game\zitem.json `
  --output-doc .\docs\model_asset_graph_v1.md `
  --output-json .\docs\model_asset_graph_v1.json
```
