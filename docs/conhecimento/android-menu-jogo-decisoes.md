---
titulo: Menu do jogo no Android (botão separado, pausa, engrenagem na tela inicial)
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: a4c2c43
arquivos: android/app/src/main/java/io/gbahandoff/GameMenu.kt, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/PadLayout.kt, android/app/src/main/java/io/gbahandoff/Placement.kt, android/app/src/main/java/io/gbahandoff/MainActivity.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:30:56Z
parecer: Conferi todas as afirmações no código e estão corretas. O MenuButton é um View próprio e um toque simples abre o menu. buttonPos chama Placement.pick sobre a tela inteira com PREFER_TOP_RIGHT. Em placeFloating o botão é posicionado primeiro, entra em `blocked` e some durante a edição. As regras de items() batem com o documento e com os casos de GameMenuTest. openMenu chama Native.stop() e zera as teclas, e resumeFromMenu retoma o jogo. EDIT libera menuPaused e o editor passa a manter a pausa. onResume respeita menuPaused. A ordem do Voltar é configurações, depois a folha, depois o editor. A engrenagem com toque longo não existe mais no PadView. Na tela inicial, o ícone tem alvo de 48 dp, fica no cabeçalho e usa contentDescription.; Não é duplicado: editor-controles-android.md (toque longo de 0,7 s na engrenagem) e android-decisoes-app.md (chips evitando a engrenagem) descrevem o estado anterior, que este documento substitui.; Ajuste 1: o item 'Posição' descrevia chamadas de função. Reduzi ao porquê: o botão é posto primeiro e na tela inteira, e os chips desviam dele.; Ajuste 2: incluí duas armadilhas confirmadas no código. O parâmetro `here` de GameMenuLogic.items significa 'o jogo está no PC' (vem de `h in st.onPc`), o contrário do que o nome sugere. Ao escolher uma entrada, a folha fecha com hide(false), que não chama onDismiss, para não retomar o jogo antes de o dono decidir.; Sem segredos nem dados pessoais.
---

# Menu do jogo no Android

Em jogo, editar controles, configurações, enviar ao PC e sair ficam num botão de menu (três traços) sobre a imagem. A engrenagem que ficava dentro do `PadView` (toque longo de 0,7 s) foi removida.

- **O botão é um `View` separado do `PadView`.** O `PadView` consome todo toque, então um botão desenhado nele geraria tecla GBA ou exigiria um truque como o toque longo. Como `View` próprio, o toque simples basta e nunca vira entrada do jogo. Alternativa descartada: o toque longo na engrenagem do pad.
- **Posição:** o botão é o primeiro a ser posto e escolhe um canto livre da tela inteira (não só da imagem), de preferência o superior direito, longe dos controles. A pílula "Sair do controle" e o chip "Enviar ao PC" desviam dele. O botão some durante a edição de controles.
- **As entradas variam pela situação:** "Editar controles" só aparece com o jogo no celular ou com o celular como controle do PC (fora disso o pad está oculto). "Enviar ao PC" só aparece com cabo conectado e o jogo no celular. "Configurações" e "Sair do jogo" aparecem sempre. Armadilha: em `GameMenuLogic.items`, o parâmetro `here` significa "o jogo está no PC" (vem de `sha1 in onPc`), o contrário do que o nome sugere.
- **A folha pausa o jogo** (`Native.stop()`, teclas zeradas) e retoma ao fechar (`menuPaused`/`resumeFromMenu`). Ao escolher uma entrada, a folha fecha com `hide(false)`, sem chamar o callback de retomar, e quem decide é `onMenuPick`. No caso de "Editar controles", `menuPaused` é liberado e o editor passa a manter a pausa. Armadilha: qualquer caminho novo que feche a folha ou as configurações precisa passar por `resumeFromMenu`, senão o jogo fica parado. `onResume` também respeita `menuPaused`. O Voltar fecha primeiro as configurações, depois a folha, depois o editor.
- **Tela inicial:** "⚙ Configurações" virou só o ícone, com alvo de 48 dp, no cabeçalho fora da lista rolável, para ficar sempre visível. O texto ficou no `contentDescription`.

Como verificar: o teste unitário `GameMenuTest` cobre a regra das entradas e a posição do botão. O resto é teste manual no aparelho.
