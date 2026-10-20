---
titulo: Armadilhas do sync no celular (ordem do "no PC", .meta, conectar com jogo aberto, push retido)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 50fe505
arquivos: android/app/src/main/cpp/phone_link.c, sync/gbah_sync.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T13:44:13Z
parecer: Conferi no código e as afirmações batem. Em `leave_on_pc` (phone_link.c:632-655), `apply_stored` roda antes de `p->on_pc_has = 0`, e o comentário do código explica por quê. Em `handle_push` (phone_link.c:775-779), o RETURN/HANDOFF só zera `on_pc_has` depois do `H(p)->apply`, com o comentário "only now (core already following the store)". `checkpoint` (phone_link.c:256-263) retorna sem gravar enquanto `on_pc_has` está ligado para o jogo aberto.; `hold_push` (phone_link.c:659-668) faz `memset(pc, 0, ...)` nos dois caminhos em que guarda o push. `handle_push` copia `sha` e `ver` antes de chamar `hold_push` e usa essas cópias em `ask_rom` e no ACK de erro (phone_link.c:690-696). A regra "depois de hold_push bem-sucedido não leia *pc" bate com o contrato no comentário da função.; `gbah_store_commit` (sync/gbah_sync.c:394-415) grava nesta ordem: .state, .sav e por último .meta. `gbah_store_meta` cai em `dev_self` com versão 1 quando falta o .meta (gbah_sync.c:358-390).; Há `checkpoint` ao conectar (phone_link.c:1222) e ao processar a LIBRARY (phone_link.c:1026), com comentários dizendo que é intencional. Quando o frame não mudou, `checkpoint` não grava, então a ressalva "se o jogo mudou" está certa.; Não duplica os docs aprovados. sync-regras-versao só diz como um .state sem .meta é lido. Este doc acrescenta o que é novo: o .meta é escrito por último, e quem espera um espelho deve esperar o .meta, não o .state. Nenhum doc aprovado trata da ordem de limpar o "no PC", do push retido com SHA zerado nem do efeito do checkpoint ao conectar.; Explica porquê, sintoma e regra de cada armadilha em vez de resumir o código. É conciso e não tem segredos nem dados pessoais. Não deu para conferir no código a observação sob ASan, mas ela está apresentada como observação de execução e é plausível.
---

# Armadilhas do sync no celular

- **"No PC" só pode ser limpo DEPOIS do apply do state.** Em `leave_on_pc` (cabo puxado, PC parou) e
  no HANDOFF recebido (RETURN), a marca `on_pc_has` é zerada só depois que o state guardado voltou ao
  core. Enquanto ela está ligada, o jogo fica retido e `checkpoint` não grava. Limpar antes abre uma janela em que o jogo já
  não está retido, mas o core ainda não tem o state: o motor que limpava cedo foi observado sob ASan
  (mais lento, janela maior). Sintoma esperado: autosave/checkpoint do core ainda velho competindo com
  o state guardado. Verificar em `leave_on_pc` e `handle_push` que `on_pc_has = 0` vem após `apply`.
- **Push retido saía com SHA zerado.** `hold_push` guarda o push e zera (`memset`) a struct do
  chamador. Em `handle_push` o ROM_REQUEST era montado depois disso, então pedia a ROM de SHA 0..0 e a
  ROM do jogo nunca chegava. Corrigido copiando o SHA (e a versão, usada no ACK de erro) antes de
  reter. Regra: depois de `hold_push` bem-sucedido, não leia mais `*pc`.
- **`.meta` é escrito por último** (`gbah_store_commit`: state, sav, meta). Um `.state` sem `.meta` é
  lido como progresso do próprio celular `(dev_self, 1)`. Quem espera um espelho recebido do PC (teste
  ou ferramenta) não deve esperar o `.state` aparecer: tem de esperar o `.meta` mostrar um device
  diferente do celular, senão lê o state novo com a versão antiga.
- **Conectar com um jogo aberto grava antes e avança a versão do celular** (`checkpoint` ao receber
  a LIBRARY e ao conectar), para o store descrever o jogo como ele está agora. É intencional. Efeito:
  se o jogo mudou desde a última gravação, o celular passa a ter uma versão nova que o PC não viu;
  contra um PC que também avançou, a regra de base (sync-regras-versao) dá CONFLICT, não RECEIVE.
