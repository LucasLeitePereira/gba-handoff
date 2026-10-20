---
titulo: Regras de versão do sync (base) e por que "maior counter vence" foi descartado
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: f84dedf
arquivos: sync/gbah_sync.h, sync/gbah_sync.c, tests/sync_test.c, desktop/handoff.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T12:39:18Z
parecer: Conferi as afirmações no código e elas batem. Em sync/gbah_sync.c: `gbah_sync_decide` segue a ordem descrita (sem dados nos dois lados = NOTHING, lado sem dados perde, mesma versão = NOTHING, `l.base == r.version` = SEND, `r.base == l.version` = RECEIVE, senão CONFLICT). Versão com counter 0 nunca casa (`gbah_ver_none`). `gbah_ver_eq` compara o counter e o device_id. `advance` soma 1 ao counter, usa o id do aparelho e mantém a base. `gbah_store_mark_synced` devolve 1 quando a versão em disco não é mais a `v`. Um `.state` sem `.meta` vira `(dev_self, 1)` com base zerada (`memset`). Em desktop/handoff.cpp, `Hub::on_ack` só marca a base com ACK OK e se `g->meta.version == v`, em memória e no disco, seja qual for a flag (o espelho também conta). tests/sync_test.c tem `naive_decide` como controle negativo: devolve NOTHING com counters iguais e nunca indica conflito. O teste `sync_test` existe no ctest.; Uma imprecisão corrigida: no PC, o receptor não chama `adopt` 'depois de guardar'. O `gbah_sync_adopt` roda antes, e a meta já com base = versão recebida vai no mesmo `gbah_store_commit` que grava o state. O ACK OK só sai depois desse commit. Reescrevi o trecho assim.; Acrescentei o segundo controle do teste, que justifica por que só uma transferência confirmada move a base: copiar os dados sem fazer base = version transforma uma reconexão limpa em falso conflito. Também anotei que, no commit f84dedf, só o PC aplica essas regras de ponta a ponta; protocolo-decisoes diz que o celular ainda não implementa sync.; Não repete os documentos aprovados. protocolo-decisoes só cita versão e base e remete a sync-regras-versao. decisoes-arquitetura-plataforma só diz que o usuário escolhe a versão no conflito. handoff-fluxos-pc trata do ACK depois do commit, mas não das regras de versão. O texto explica a decisão, a alternativa descartada e as armadilhas com os sintomas, é conciso e não tem segredos.
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

No commit f84dedf, só o PC aplica essas regras de ponta a ponta; o celular ainda não implementa sync (ver protocolo-decisoes). Para verificar: `ctest -R sync_test`.
