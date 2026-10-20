---
titulo: Regras de versão do sync (base), "maior counter vence" descartado e o desempate "mais recente"
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: d7240e6
arquivos: sync/gbah_sync.h, sync/gbah_sync.c, tests/sync_test.c, desktop/handoff.cpp, android/app/src/main/cpp/phone_link.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T17:55:00Z
parecer: Conferi no código do commit d7240e6 as afirmações novas sobre "sempre manter a mais recente". Em `sync/gbah_sync.c`, `gbah_sync_newest` desempata nesta ordem: has_data, timestamp_ms, counter e `memcmp` do device_id. O `sync_test` (`test_newest`) cobre a simetria `newest(a,b) == -newest(b,a)` e o caso em que o counter menor é mais novo e mesmo assim vence. Ali também está o controle negativo `naive_decide`, que mostra por que decidir só pelo counter foi descartado.; `gbah_sync_decide` não mudou: a opção só age em conflitos já abertos. Em `phone_link.c`, quem decide é o celular, em `auto_resolve`. Ele só roda com `prefer_newest` ligado no celular e com `pc_pref_known && pc_pref`, ou seja, com os dois lados ligados. O padrão é desligado (`library.h`: `prefer_newest = false`). O perdedor vai a backup dos dois lados: `gbah_store_backup` em `handle_push` (phone_link.c:804) e em `desktop/handoff.cpp:619`.; O resto é a versão aprovada condensada e continua certo diante do código. Conferi `decide` (com counter 0 nunca casando, via `gbah_ver_none`), `advance`, `adopt` (base = versão recebida), `gbah_store_mark_synced` (devolve 1 quando a versão em disco mudou) e a checagem `gbah_ver_eq` em `Hub::on_ack`. Não há mudança local não commitada em sync/, tests/, desktop/ nem no cpp do Android.; Não duplica nada: substitui o próprio sync-regras-versao. O desempate "mais recente" e o risco aceito dos relógios desajustados são conhecimento novo. Saiu a frase sobre o espelho MIRROR, mas ela continua coberta em protocolo-decisoes (STATE_PUSH com flags).; Explica as decisões, as alternativas descartadas e o risco aceito, em vez de descrever o código. É conciso e não tem segredos nem dados pessoais.
---

# Regras de versão do sync (base)

Cada jogo (chave = SHA-1 da ROM) guarda, por aparelho, `version = (device_id, counter)` e `base`, a última versão em que PC e celular estavam comprovadamente iguais. Salvar progresso novo chama `gbah_sync_advance`: counter+1, id do próprio aparelho, base intocada. `gbah_sync_decide(local, remote)`:

- mesma versão: nada; lado sem dados: perde;
- `local.base == remote.version`: SEND; `remote.base == local.version`: RECEIVE;
- qualquer outro caso (inclusive sem base): CONFLICT.

**Descartado: "maior counter vence".** Se os dois avançam da mesma versão, podem chegar ao mesmo counter e essa regra acha que estão iguais ou escolhe um lado em silêncio: o progresso do outro se perde. Por isso a versão carrega o id do aparelho e a base distingue "só um avançou" de "divergiram" (controle negativo `naive_decide` no `sync_test`).

**A base só muda numa transferência confirmada.** O remetente faz `mark_synced` no STATE_ACK OK; o receptor grava a meta com base = versão recebida no mesmo commit do state e só então dá o ACK. Copiar dados sem base = version transforma a reconexão seguinte em falso CONFLICT. `gbah_store_mark_synced` só grava se a versão em disco ainda é a confirmada (o jogo pode ter avançado entre o envio e o ACK); `Hub::on_ack` checa o mesmo em memória. Counter 0 nunca casa; `.state` antigo sem `.meta` vale `(dev_self, 1)` sem base, logo CONFLICT contra quem tiver dados.

**"Sempre manter a mais recente" (opcional, desligado por padrão) só escolhe o lado do CONFLICT; não muda `decide`.** `gbah_sync_newest(a, b)` desempata por: quem tem dados, `timestamp_ms`, counter, `device_id` (memcmp). É simétrica (`newest(a,b) == -newest(b,a)`), então os dois lados chegam à mesma resposta. Quem decide é o celular (`auto_resolve`), só quando os DOIS aparelhos têm a opção ligada; o perdedor sempre vai a backup. Descartado: decidir só por counter (o que tem counter menor mas é mais novo perderia).
**Risco aceito:** `timestamp_ms` vem do relógio de cada aparelho; relógios desajustados escolhem o lado "errado". Por isso o perdedor nunca é apagado, só vira backup.

Verificar: `ctest -R sync_test` (regras e `newest`), `ctest -R phone_link_test` (celular contra o PC real).
