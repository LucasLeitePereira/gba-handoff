---
titulo: SDL3 no PC - viewport com escala desloca o jogo; sem logical presentation
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 8b8c4de
arquivos: desktop/main.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:23:29Z
parecer: Conferi as afirmações em desktop/main.cpp e todas batem com o código. O comentário nas linhas 108-110 confirma que SDL_SetRenderViewport não é usado porque o SDL3 guarda o viewport em unidades escaladas. begin_view (linha 179) só chama SDL_SetRenderScale e calcula g_ox e g_oy em unidades virtuais. text e fill somam g_ox e g_oy. SDL_SetRenderLogicalPresentation não é usado em nenhum lugar. SDL_RenderClear limpa o alvo inteiro, e por isso as bordas ficam na cor do menu (linha 911). kBarH vale 28. game_view usa escala inteira e, abaixo de 1x, encolhe.; O assunto não está duplicado. Nenhum documento aprovado (android-decisoes-app, android-build-armadilhas, editor-controles-android, handoff-fluxos-pc, pc-*) trata do viewport e da escala do SDL3 no PC.; Registra uma armadilha de verdade, com sintoma, causa, a alternativa descartada (viewport ou logical presentation), a regra para quem criar um novo helper de desenho e um jeito de verificar. Não é só um resumo do código.; É conciso e não tem segredos nem dados pessoais.
---

# SDL3 no PC: viewport + escala e bordas

- **Sintoma:** o jogo (ou o menu) aparecia deslocado para baixo/direita ao centralizar com letterbox.
- **Causa:** `SDL_SetRenderViewport` combinado com `SDL_SetRenderScale` guarda o viewport em unidades já escaladas, então a posição do viewport é multiplicada pela escala. Não dá para colocá-lo num pixel arbitrário.
- **Solução adotada:** sem viewport e sem `SDL_SetRenderLogicalPresentation`. `begin_view` só define a escala e calcula uma origem (`g_ox`, `g_oy`, em unidades virtuais); todos os helpers de desenho (`text`, `fill`, ...) somam essa origem. Qualquer novo helper de desenho precisa somar também, senão sai fora do lugar.
- **Bordas:** sem logical presentation, `SDL_RenderClear` limpa o alvo inteiro, então toda frame limpa com a cor do menu e as bordas ficam dessa cor (é intencional). Jogo: barra de 28 px no topo e 240x160 em escala inteira centralizada abaixo (abaixo de 1x encolhe).
- **Verificar:** redimensionar a janela e usar F11; o conteúdo deve ficar centrado, sem faixa deslocada.
