---
titulo: Editor de controles virtuais do Android: decisões
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 428ea59
arquivos: android/app/src/main/java/io/gbahandoff/PadLayout.kt, android/app/src/main/java/io/gbahandoff/PadView.kt, android/app/src/main/java/io/gbahandoff/PadHost.kt, android/app/src/main/java/io/gbahandoff/GameActivity.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:31:19Z
parecer: Conferi as afirmações no código e estão corretas. O item EDIT("Editar controles") existe em GameMenu.kt:15. O PadView não tem mais engrenagem nem toque longo. Em GameActivity.kt:57, a edição chama Native.stop() e, ao terminar, Native.start(). Em GameActivity.kt:211, o menuBtn fica GONE durante a edição. Em GameActivity.kt:267, o Voltar chama padHost.exitEdit(). PadLayout.occupiedRects (PadLayout.kt:49) é chamado por PadView.occupiedRects (PadView.kt:94, com folga de 8 dp) e por placeFloating (GameActivity.kt:208), que usa o resultado para o botão de menu e para os chips.; Substitui bem a versão aprovada, que falava do toque longo de 0,7 s na engrenagem. Esse trecho ficou desatualizado depois do commit 428ea59.; Ajuste: o segundo item repetia o android-menu-jogo-decisoes.md, já aprovado: a engrenagem removida, o botão como View separado e o fato de o PadView consumir todo toque. Reduzi o item ao que é próprio do editor, que é por onde se entra, a pausa, o botão que some e o Voltar, e deixei uma referência ao outro documento.; Mantive o item sobre occupiedRects como fonte única porque ele explica uma consequência: mudar um controle faz o botão e os chips desviarem sem outro ajuste. Mantive também a armadilha dos padrões salvos inteiros, que continua valendo.; Não há segredos nem dados pessoais.
---

# Editor de controles virtuais do Android: decisões

- **Um layout por orientação** (`pad_port` / `pad_land`): em paisagem os controles ficam em lugares muito diferentes dos de retrato, então não dá para derivar um do outro. A posição e o tamanho são relativos à tela (fração e menor dimensão) para o layout funcionar em telas e densidades diferentes.
- **O editor abre pela entrada "Editar controles" do menu do jogo.** Antes abria com um toque longo de 0,7 s numa engrenagem dentro do pad. O motivo da troca está em android-menu-jogo-decisoes. Durante a edição a emulação fica pausada (o mesmo `Native.stop/start` do menu), o botão de menu some e o Voltar fecha o editor antes de sair da tela.
- **`PadLayout.occupiedRects` é a fonte única das áreas ocupadas pelos controles** (com folga de toque). Ela serve ao `PadView` e ao posicionamento do botão de menu e dos chips. Se um controle muda de lugar ou de tamanho, o botão e os chips desviam sem outro ajuste.
- **O padrão é compacto e semitransparente** (alpha 0,5, tamanhos cerca de 35% menores que a primeira versão). Isso veio de quem testou no celular: os botões originais pareciam grandes demais.
- **Armadilha:** o layout é salvo inteiro (os 7 controles). Por isso, mudar os padrões (`DEF_*`, `BASE_*`, `DEFAULT_ALPHA`) só afeta quem nunca editou o layout. Para levar um padrão novo a quem já salvou, é preciso migrar ou apagar as chaves salvas, ou o usuário tem que usar a opção de restaurar.
