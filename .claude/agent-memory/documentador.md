# Memória — documentador (GBA Handoff)

- Prefixo dos documentos: `retro-switch-`. Projeto: GBA Handoff (emulador GBA Android ↔ Windows).
- Vale para o que já existe aprovado: `D:/TI/conhecimento/aprovados/retro-switch/`.

## O que vale documentar aqui
- O porquê de decisões e alternativas descartadas (ex.: por que ADB e não AOA).
- Armadilhas: sintoma, causa real, como verificar.
- Procedimentos: como montar ambiente, buildar, testar, instalar.

## Lições aprendidas
- [Fases 0-1] 5/5 aprovados com ajustes. O revisor cortou: fatos da máquina local (WSL, C: montado), números que mudam por build (tamanho do .exe), caminhos absolutos, itens fora do assunto do doc e frases que só descrevem o código. Evite-os de cara.
- [Fases 0-1] O revisor confere afirmações no código e no mGBA: não copie afirmações do dev sem verificar (ex.: a ordem sav/state estava errada). Afirme como "decisão" o que ainda não está implementado.
- [Fase 2] Ótimo: conferiu no código e corrigiu o material recebido (-O2 vs -O3, build-tools já fixado). 3/3 aprovados. Continue verificando antes de escrever.
- [Fase 3] 4/5 aprovados; 1 rejeitado com razão: relato de teste/validação ("funcionou no celular") não é conhecimento — é status. Só registre resultado de teste se revelar sintoma, causa ou condição de falha. (Erro de instrução do sênior; não repetir.)
- [Fase 3] Ótimo achado: bug real em código (bool retornando -1). Continue relatando bugs que encontrar ao verificar, sem corrigir.
- [Commit] O repo agora tem commits: use `git rev-parse --short HEAD` no campo `commit` (não mais `sem-commit`).
- [Fase 4a] Ótimo: corrigiu o material do dev (PC_STATUS não tem ordem fixa; TIMEOUT 120 só nos _san) e apontou doc antigo desatualizado. Ao documentar uma fase, procure também docs existentes que ela tornou obsoletos e proponha a nova versão na mesma entrega.
- [Fase 4b] 7/7 aprovados (4 sem ajuste). Ótimo: achou 3 docs obsoletos sozinho, recusou números não verificados (<1 ms) e corrigiu a cadência do autosave. Padrão a manter.
- [Dono único] 9/9 aprovados. Ótimo: recusou afirmação do dev que não estava no código ("RETURN espúrio") e delimitou o fallback de 5 s. Continue assim.
- [PC layout/instalação] 4/4 aprovados (3 com ajuste). O revisor pegou generalização ("toda mensagem de falha diz X") que só valia para parte dos casos: confira cada ramo antes de escrever "toda/sempre". Não repita o que outro doc aprovado já cobre; faça remissão.
- [Celular entra no jogo do PC] 3/3 aprovados (1 com ajuste). Ótimo: corrigiu o material do dev (NOTHING além de RECEIVE), recusou documentar falha esporádica não verificada e não criou doc trivial de release.
- O revisor NÃO roda sozinho: após escrever, o sênior roda `docserver revisar`. Não espere o watcher.
