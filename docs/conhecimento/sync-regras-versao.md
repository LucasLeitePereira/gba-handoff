---
titulo: Regras de versão do sync (base) e por que "maior counter vence" foi descartado
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 50fe505
arquivos: sync/gbah_sync.h, sync/gbah_sync.c, tests/sync_test.c, desktop/handoff.cpp, android/app/src/main/cpp/phone_link.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T13:45:21Z
parecer: O documento só muda a versão aprovada (f84dedf) no último parágrafo e no campo `arquivos`, onde entrou `phone_link.c`. Conferi as duas mudanças no código do commit 50fe505.; O celular aplica as mesmas regras com a mesma biblioteca `gbah_sync`. Em `android/app/src/main/cpp/phone_link.c`, `gbah_sync_advance` roda ao salvar e `gbah_sync_decide` roda ao comparar as versões. Ao receber, `gbah_sync_adopt` roda antes de `gbah_store_commit`, e o ACK OK só sai depois do commit (`send_ack(... rc == 0 ? OK : ERROR)`). Em `handle_ack`, com ACK OK o celular chama `gbah_store_mark_synced` para qualquer flag, inclusive a de espelho (`K_MIRROR`). Assim, a frase da versão anterior, que dizia que só o PC implementava o sync, ficou desatualizada, e a correção procede.; Os dois comandos de verificação existem: `tests/CMakeLists.txt` registra o `sync_test` e o `phone_link_test`. O `phone_link_test` roda contra o executável real do desktop (`$<TARGET_FILE:gbahandoff>`) com um `fake_adb.sh`, o que bate com 'celular contra o PC real'.; O documento citado, android-sync-armadilhas, existe em `docs/conhecimento/`.; O resto do texto é igual à versão aprovada, que já tinha sido conferida no código (decide, advance, mark_synced devolvendo 1 quando pula, `.state` sem `.meta`, `Hub::on_ack`). No commit atual não há mudança em `sync/` nem em `desktop/handoff.cpp` além do que está commitado.; O documento não duplica os outros: substitui o próprio sync-regras-versao, e protocolo-decisoes e handoff-fluxos-pc só remetem a ele. Ele explica a decisão, a alternativa descartada e as armadilhas com os sintomas, é conciso e não tem segredos.
---

# Regras de versão do sync (base)

Cada jogo (chave = SHA-1 da ROM) guarda, por aparelho, `version = (device_id, counter)` e `base`, que é a última versão em que PC e celular estavam comprovadamente iguais. Salvar progresso novo chama `gbah_sync_advance`: counter+1, id do próprio aparelho e base intocada. `gbah_sync_decide(local, remote)` decide assim:

- mesma versão: nada;
- lado sem dados: perde;
- `local.base == remote.version`: SEND;
- `remote.base == local.version`: RECEIVE;
- qualquer outro caso (inclusive sem base): CONFLICT.

**Descartado: "maior counter vence".** Se os dois avançam a partir da mesma versão (o PC joga offline e o celular também), os dois podem chegar ao mesmo counter. Aí essa regra acha que estão "iguais" ou escolhe um lado em silêncio, e nunca indica conflito: o progresso do outro lado se perde. Por isso a versão carrega o id do aparelho (dois counter 5 de aparelhos diferentes são versões diferentes), e é a base que distingue "só um avançou" de "divergiram". O controle negativo `naive_decide` do `sync_test` mostra isso.

**A base só muda numa transferência confirmada.** O remetente faz `mark_synced` (base = version) quando chega o STATE_ACK OK. O receptor aplica `adopt` e grava a meta com base = versão recebida no mesmo commit do state, e só então manda o ACK. O espelho periódico (STATE_PUSH com flag MIRROR) também conta: depois de confirmado, a base sobe nos dois lados, e o próximo estado novo de um lado vira um SEND/RECEIVE limpo. Armadilha: copiar os dados sem fazer base = version transforma a reconexão seguinte num falso CONFLICT (é o segundo controle do `sync_test`).

**`gbah_store_mark_synced` só grava se a versão em disco ainda é a `v` confirmada** (retorna 1 quando pula). O jogo pode ter avançado entre o envio e o ACK, e marcar a base na versão nova declararia "sincronizado" algo que o outro lado nunca viu. `Hub::on_ack` faz a mesma checagem em memória.

Counter 0 ("sem versão") nunca casa com nada. Um `.state` antigo sem `.meta` é lido como `(dev_self, 1)` sem base, então dá CONFLICT contra qualquer outro lado que tenha dados.

PC e celular aplicam estas regras (o celular em `phone_link.c`, com o mesmo `gbah_sync`; armadilhas em android-sync-armadilhas). Para verificar: `ctest -R sync_test` (regras) e `ctest -R phone_link_test` (celular contra o PC real).
