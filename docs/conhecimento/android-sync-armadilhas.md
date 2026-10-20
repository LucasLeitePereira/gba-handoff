---
titulo: Armadilhas do sync no celular (ordem do "no PC", .meta, conectar com jogo aberto, push retido)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: d7240e6
arquivos: android/app/src/main/cpp/phone_link.c, sync/gbah_sync.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T17:51:59Z
parecer: Conferi a única mudança em relação à versão aprovada e ela está correta. O commit d7240e6 removeu a chamada `leave_on_pc(p, "Cabo desconectado")` do laço de sessão. Hoje `leave_on_pc` só é chamado com "O PC parou de jogar" (phone_link.c:1109, 1127, 1162 e 1374, esta última pelo timeout LEAVE_WAIT_MS em `housekeeping`). Quando o cabo é puxado, o jogo continua retido. Só `phone_cmd_play_here` ("Jogar aqui mesmo assim", phone_link.c:1786-1803) o retoma.; Ajuste: `phone_cmd_play_here` é um terceiro lugar que limpa `on_pc_has`, e segue a mesma ordem (`apply_stored` antes de `p->on_pc_has = 0`, linhas 1797-1800). O doc mandava conferir só `leave_on_pc` e `handle_push`, então quem mexer na retomada manual não ficaria sabendo da regra. Incluí essa função no parêntese e na linha de verificação.; O resto continua batendo com o código. `leave_on_pc` aplica antes de zerar (690-713). `handle_push` só zera depois do `apply` (833-837). `hold_push` faz `memset` nos dois caminhos e `handle_push` copia `sha` e `ver` antes (717-755). A ordem de `gbah_store_commit` e o `(dev_self, 1)` já tinham sido conferidos na versão aprovada e este commit não os mudou.; O comentário acima de `leave_on_pc` (phone_link.c:688) ainda diz "cable pulled", o que está desatualizado. Isso é código, não doc: fica a sugestão de corrigir esse comentário.; Substitui o doc aprovado sem duplicar nada. Continua explicando porquê, sintoma e regra, é conciso e não tem segredos.
---

# Armadilhas do sync no celular

- **"No PC" só pode ser limpo DEPOIS do apply do state.** Em `leave_on_pc` (PC parou de jogar; o cabo
  puxado sozinho não chama mais, o jogo fica retido até "Jogar aqui mesmo assim", `phone_cmd_play_here`),
  nessa própria retomada manual e no HANDOFF recebido (RETURN), a marca `on_pc_has` é zerada só depois que
  o state guardado voltou ao core. Enquanto ela está ligada, o jogo fica retido e `checkpoint` não grava.
  Limpar antes abre uma janela em que o jogo já não está retido, mas o core ainda não tem o state: o motor
  que limpava cedo foi observado sob ASan (mais lento, janela maior). Sintoma esperado: autosave/checkpoint
  do core ainda velho competindo com o state guardado. Verificar em `leave_on_pc`, `phone_cmd_play_here` e
  `handle_push` que `on_pc_has = 0` vem após `apply`.
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
