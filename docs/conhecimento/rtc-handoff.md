---
titulo: RTC no handoff - o jogo segue o relógio do aparelho, sem offset
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: f84dedf
arquivos: core/gbah_core.c, third_party/mgba/src/core/interface.c, third_party/mgba/src/gba/cart/gpio.c, third_party/mgba/src/gba/core.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T12:38:26Z
parecer: Conferi as afirmações no código e elas se confirmam. Em core/interface.c, com RTC_NO_OVERRIDE, `_rtcGenericCallback` devolve `time(0)`, e com RTC_WALLCLOCK_OFFSET devolve `time(0) + rtc->value / 1000LL`. Em gpio.c, `_rtcUpdateClock` chama `localtime_r` a cada latch. `GBAHardwareSerialize` grava só `hw.time` e os registradores do RTC. `_GBACoreSaveState` (gba/core.c) chama só `GBASerialize`, porque o EXTDATA_RTC só é gravado por `mCoreSaveStateNamed` (core/serialize.c:434). O gbah_core.c usa `core->saveState`/`core->loadState` direto, e não há WALLCLOCK nem rtc em core/ ou desktop/.; Corrigi uma imprecisão: o `time(0)` vem de `_rtcGenericCallback` em core/interface.c (o core GBA sempre instala `mRTCGenericSource`), não de gpio.c. O `time(0)` que aparece em gpio.c só roda quando não existe rtcSource.; Acrescentei um fato verificado que reforça a conclusão: `hw->rtc.offset` do RTC GBA só é zerado no init e nunca é serializado nem alterado. Nada do relógio de origem chega ao destino.; O documento não repete os aprovados parecidos (mgba-integracao-armadilhas, contrato-handoff-estado, android-decisoes-app): nenhum deles trata de RTC. Ele explica a decisão, a alternativa descartada e o risco aceito com o sintoma, é conciso e não tem segredos.
---

# RTC no handoff

Jogos com relógio (Pokémon, Boktai) leem o RTC do cartucho. Verificado no mGBA vendorizado: o core GBA
instala uma `mRTCGenericSource`, que com `RTC_NO_OVERRIDE` devolve `time(0)` (`core/interface.c`); a cada
latch, `gpio.c` converte com `localtime_r` (portanto o fuso do aparelho também conta). O state serializa só
o último horário travado (`hw.time`), e `hw->rtc.offset` nunca é salvo. O `saveState` do core GBA
(`GBASerialize`) não grava o `EXTDATA_RTC`, que levaria o modo/offset da fonte de RTC; isso só acontece em
`mCoreSaveStateNamed`, que o `gbah_core.c` não usa. Consequência: depois de restaurar um state, o jogo passa
a seguir o relógio do aparelho onde está rodando.

**Decisão:** o protocolo não leva offset de relógio. Alternativa descartada: enviar a diferença
entre os relógios e aplicá-la com `RTC_WALLCLOCK_OFFSET` (existe no mGBA: `time(0) + value/1000`).
O custo de protocolo e de UI não compensa para um caso raro, e o offset teria de ser recalculado a cada troca.
Risco aceito: com relógios desajustados ou fusos diferentes entre PC e celular, o horário do
jogo "salta" no handoff (eventos por hora do dia, plantas, marés).

Gancho futuro: configurar `RTC_WALLCLOCK_OFFSET` no core após o restore (nada disso existe hoje
em `gbah_core.c`). Para verificar o estado atual: `grep -rn WALLCLOCK core desktop`.
