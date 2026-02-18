# CHANGELOG_CLIENT_LOCAL_v1

## Repo
- `NewDuel-Source-Code` (`gunz-nakama-client`)

## Entrega (Model Pipeline Open Format - GLB -> rs3_model_v1)
- Adicionado `tools/model_audit/export_asset_graph.js` e `tools/model_audit/README.md`.
- Grafo de assets gerado em `docs/model_asset_graph_v1.json` e `docs/model_asset_graph_v1.md`.
- Adicionado conversor `tools/elu_ani_to_glb/elu_ani_to_glb.js` + `README.md`.
- Saida gerada em `OpenGunZ-Client/system/rs3/open_assets/**`:
- `open_assets_manifest_v1.json`
- `conversion_report_open_assets_v1.md`
- Adicionado conversor `tools/glb_to_rs3_model/glb_to_rs3_model.js` + `README.md`.
- Saida gerada em `OpenGunZ-Client/system/rs3/models/**`:
- `rs3_model_manifest_v1.json`
- `conversion_report_rs3_model_v1.md`
- Novo contrato documentado em `docs/rs3_model_v1.md`.
- Runtime RS3 (C++) atualizado com:
- `ModelPackageLoader` (`LoadModelPackage`)
- `CharacterAssembler` (`BuildCharacterVisual`)
- `SkeletonPlayer` (`SetAnimationClipByName`)
- `PbrMaterialSystem`
- Integracao inicial no `RScene::SetCreationPreview` para carregar pacote convertido de personagem.

## Entrega (Texturas PNG + Alpha - char_creation_select)
- Adicionado `tools/rs3_texture_converter/rs3_texture_converter.js` para converter texturas usadas para PNG no pacote RS3.
- Adicionado `tools/rs3_texture_converter/README.md` com comando oficial.
- `tools/rs2_scene_converter/rs2_scene_converter.js` atualizado para gravar:
- `scene.json.materials[]` com `materialIndex`, `flags`, `sourceDiffuseMap`, `packageTexture`.
- `scene.json.usedMaterialIndices[]`.
- `scene.json.textureManifest`.
- `RScene` atualizado para pipeline de materiais com alpha:
- 4 passes de draw: `Opaque`, `AlphaTest`, `AlphaBlend`, `Additive`.
- uso de `materialFlags` por secao.
- shader passa a respeitar `sample.a` + `clip(alpha - alphaRef)` para alphatest.
- blend/depth states dedicados para alpha blend e additive.
- Contrato/documentacao atualizados em:
- `docs/rs3_scene_v1.md`
- `../docs/RealSpace3-char-select-pipeline.md`

## Resultado tecnico desta leva
- Conversao de texturas executada para `char_creation_select`.
- `usedMaterialCount=27`, `convertedPngCount=27`, `missingUsedTextures=0`.
- `world.bin` e `scene.json` atualizados para referenciar `textures/*.png`.
- Build validado:
- `cmake --build gunz-nakama-client/build --config MinSizeRel --target GunzNakama` concluido com sucesso.

## Entrega
- `NakamaManager`:
- adicionados `getBootstrapV2(...)`, `getGameData(...)`, `sendClientReady(recipeHash, contentHash)`.
- `sendClientReady` envia RT opcode `4108` com envelope `v/t/payload`.
- `UIManager` bridge JS:
- adicionados `fetch_bootstrap_v2()`, `fetch_game_data(key)`, `send_client_ready(recipeHash, contentHash)`.
- novos callbacks para UI: `onBootstrapV2`, `onGameDataResult`, `onRtProtocolError`.
- `ui/lobby.html`:
- removido hardcode de mapas/modos em runtime (agora bootstrap-driven).
- adicionado seletor `modeProfile` no create stage (default `classic`).
- `create_stage` agora envia `modeProfile`.
- painel técnico em stage com `seed`, `recipeHash`, `contentVersion`.
- handshake de integridade por stage: envio único de `send_client_ready(...)` por entrada na sala.
- tratamento de `S_MATCH_ERROR` (`1099`) com `CONTENT_HASH_MISMATCH` bloqueando `READY/START`.
- `ui/character_selection.html`:
- `sexo/face/preset(costume)` preenchidos por `bootstrap_v2.characterCreate`.
- mantido fluxo de criar/deletar/selecionar personagem.

## Checklist de validação
- [x] Compilação C++ debug concluída (`GunzNakama.exe` gerado).
- [x] Fluxo de lobby usa bootstrap v2 para opções de mapa/modo.
- [x] Create stage envia `modeProfile`.
- [x] Join stage envia `C_CLIENT_READY` com hashes do estado da sala.
- [x] Cliente trata `CONTENT_HASH_MISMATCH` com bloqueio visual de ready/start.
- [ ] `build-debug.bat` completo até o fim.
- Observação: passo 6 do batch falhou no script de cópia de UI (`. foi inesperado neste momento.`), apesar da compilação e cópia do executável concluírem.

## Entrega (Char-Creation-Select RS3 Scene Package L1)
- Adicionado `tools/rs2_scene_converter/rs2_scene_converter.js` para conversao offline RS2 -> `rs3_scene_v1`.
- Adicionado `tools/rs2_scene_converter/README.md` com uso CLI.
- Adicionado contrato de formato em `docs/rs3_scene_v1.md`.
- Adicionado loader nativo de pacote em runtime:
- `src/RealSpace3/Include/ScenePackageLoader.h`
- `src/RealSpace3/Source/ScenePackageLoader.cpp`
- `RScene` atualizado para:
- tentar `LoadCharSelectPackage("char_creation_select")` antes do fallback.
- renderizar geometria estatica via pipeline DX11 (VB/IB/VS/PS + textura/fog/luz).
- usar `camera_pos 02` como camera padrao de char select.
- expor spawn convertido em `GetSpawnPos()`.
- fallback automatico para `LoadLobbyBasic()` quando pacote estiver ausente/invalido.
- Documento global de pipeline atualizado: `docs/RealSpace3-char-select-pipeline.md`.

## Checklist de validacao (Char-Creation-Select)
- [x] Conversor offline gera `scene.json`, `world.bin`, `collision.bin`, `conversion_report.md`.
- [x] Runtime carrega apenas pacote nativo no fluxo de char select.
- [x] Fallback para cena basica quando pacote falha.
- [x] `GetPreferredCamera()` usa camera convertida (camera_pos 02 por padrao).
- [x] `GetSpawnPos()` retorna dummy convertido quando pacote carregado.
- [ ] Validacao completa de FPS local (60 alvo) em run manual.
- [ ] Validacao manual final de regressao de UI resize/click.
