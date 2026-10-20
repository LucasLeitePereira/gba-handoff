---
titulo: Armadilhas dos testes do handoff (órfãos, ASan no WSL2, mensagens fora de ordem)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: f84dedf
arquivos: tests/handoff_e2e_test.c, tests/run_san.sh, tests/CMakeLists.txt, desktop/handoff.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T12:39:52Z
parecer: Conferi no código e quase tudo bate. Em tests/handoff_e2e_test.c: PR_SET_PDEATHSIG(SIGKILL) com a checagem getppid na linha 133-134, kill_all_pcs depois de cada cenário (940-950), atexit e handlers de SIGTERM/SIGINT/SIGALRM (929-930) e alarm(170) abaixo do TIMEOUT 180 do ctest (CMakeLists:54). Em tests/run_san.sh, a regra de repetir só quando há DEADLYSIGNAL sem 'ERROR: AddressSanitizer'/'runtime error' está certa. Os testes *_san têm TIMEOUT 120 e o CMakeLists registra que setarch -R não é permitido. Em desktop/handoff.cpp, send_pc_status é chamado em 7 lugares sem pedido do celular, e wait_type descarta o que não é o tipo esperado.; Correção: 'repete até 6x' engana. Cada tentativa roda com `timeout 90` e o glitch é um loop sem fim, então uma tentativa travada gasta os 90 s inteiros. Com o TIMEOUT 120 do ctest só cabe uma repetição depois de um travamento, e um segundo glitch seguido faz o ctest matar o teste por timeout. Acrescentei isso porque é o sintoma que a pessoa vai ver.; Tirei 'cerca de 1 em 6 execuções': nada no código ou nos comentários confirma essa frequência.; Não repete os documentos aprovados: ambiente-build-teste e link-armadilhas não falam de órfãos do e2e, ASan no WSL2 nem PC_STATUS fora de ordem. protocolo-decisoes só define o PC_STATUS. O texto explica sintoma e causa, é curto e não tem segredos nem dados pessoais.
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
