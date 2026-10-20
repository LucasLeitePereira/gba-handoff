---
titulo: Armadilhas dos testes do handoff (órfãos, ASan no WSL2, mensagens fora de ordem, teclas remotas)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 74f70ce
arquivos: tests/handoff_e2e_test.c, tests/run_san.sh, tests/CMakeLists.txt, desktop/handoff.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T17:56:34Z
parecer: Os três primeiros itens são iguais aos da versão aprovada (f84dedf), já conferida. O código não mudou nesses pontos.; O quarto item é novo e confere com o código. desktop/handoff.cpp:97 zera as teclas em set_mode ao sair de Playing, e handoff.cpp:286 zera de novo em enter_playing. O PLACE_PC passa por place_pc -> open_game -> enter_playing. O cenário G (tests/handoff_e2e_test.c:962-1001) manda 0x301, força o conflito, manda PLACE_PC, espera 0/0 e depois manda 0x201 para mostrar que a medição funciona. Nesse fluxo qualquer um dos dois zeramentos basta para o teste passar, então a armadilha é real.; Ajuste: o texto chamava os dois zeramentos de 'dupla proteção deliberada', mas eles cobrem casos diferentes. Em link.cpp:526, o INPUT é aceito em qualquer modo. Por isso o zeramento em enter_playing apaga um INPUT atrasado que chegou durante a pausa, depois que set_mode já tinha zerado. O zeramento em set_mode solta o que estava preso ao sair de Playing. Escrevi o porquê de cada um e tirei a redação longa do negrito.; Ajuste: título atualizado para incluir o novo assunto.; Não repete outros documentos: testes-phone-link-armadilhas fala do byte SRAM[1] no cenário H, não do zeramento de teclas. Não tem segredos nem dados pessoais.
---

# Armadilhas dos testes do handoff

- **Processos `gbahandoff` órfãos.** O e2e sobe vários PCs reais (fork+exec). Se o teste morre
  (falha, SIGTERM do ctest, timeout), os filhos continuam vivos segurando as portas TCP, e a rodada
  seguinte pode encontrá-las ocupadas. Defesas no código: `prctl(PR_SET_PDEATHSIG, SIGKILL)` no
  filho (só Linux), com checagem de `getppid` para cobrir a janela entre o fork e o prctl;
  `kill_all_pcs` depois de cada cenário, em `atexit` e nos handlers de SIGTERM/SIGINT/SIGALRM; e
  `alarm(170)`, para o teste se limpar antes do timeout de 180 s do ctest. Processos `<defunct>`
  com pai PID 1 que sobram são zumbis inofensivos de execuções interrompidas.
- **ASan às vezes trava na largada no WSL2 (kernel 6.6).** A execução entra num loop sem fim de
  `AddressSanitizer:DEADLYSIGNAL` sem relatório (mapa de sombra x ASLR) e o ctest fica pendurado.
  `setarch -R` não é permitido neste ambiente. `tests/run_san.sh` repete o teste só quando a saída
  tem DEADLYSIGNAL e não tem `ERROR: AddressSanitizer` nem `runtime error`. Achado real ou falha
  de teste passa direto. Cuidado: cada tentativa tem `timeout 90` e os testes `*_san` têm
  `TIMEOUT 120` no ctest, então depois de uma tentativa travada só cabe uma repetição. Dois
  glitches seguidos aparecem como timeout do ctest, não como falha. Se o ASan falhar, olhe a saída
  antes de culpar o código: sem relatório, é glitch de largada.
- **O PC manda CONTROL(PC_STATUS) sem ser pedido** (ao conectar e a cada mudança de modo), então
  ele pode chegar antes do PONG ou de qualquer resposta esperada. Clientes de teste devem ler em
  laço e pular mensagens até chegar a esperada (ver `wait_type` no e2e). Um teste que assume "a
  próxima mensagem é o PONG" falha de forma intermitente.
- **O cenário G não prova cada zeramento de teclas remotas isoladamente.** As teclas do celular
  são zeradas em dois pontos de `desktop/handoff.cpp`, com papéis diferentes. `set_mode`, ao sair
  de Playing, solta o que estava preso. `enter_playing` apaga um INPUT atrasado, que o link aceita
  em qualquer modo e que pode chegar durante a pausa, depois do primeiro zeramento. O G prende
  A+L+R, força conflito, manda PLACE_PC e exige `SRAM[1..2] == 0`. No fluxo dele qualquer um dos
  dois basta, então remover um deles não quebra o teste, mas reabre o caso que só ele cobre. Não
  trate isso como redundância. O G aperta uma tecla de novo no fim para mostrar que o "0" não é
  vácuo.
