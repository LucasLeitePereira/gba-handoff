---
titulo: Editor de controles virtuais do Android: decisões
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: android/app/src/main/java/io/gbahandoff/PadLayout.kt, android/app/src/main/java/io/gbahandoff/PadView.kt, android/app/src/main/java/io/gbahandoff/PadHost.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:58:31Z
parecer: Conferi tudo no código e está correto: UNIT_FRAC 0.1, scale 0,5..2, alpha 0,1..1 e DEFAULT_ALPHA 0,5 (PadLayout.kt), as chaves pad_port/pad_land e as SharedPreferences 'pad_layout' (PadHost.kt:24), o decode tolerante, o KvStore, o toque longo de 700 ms na engrenagem (PadView.kt:134), a pausa via onEditing e o Voltar chamando exitEdit (GameActivity.kt:103). O comentário 'cerca de 35% menor que a primeira versão' também está no código.; Não repete nenhum documento aprovado. O android-decisoes-app.md não fala dos controles virtuais.; Boa parte do texto só descreve o que o código faz, e o KDoc de PadLayout.kt e PadView.kt já diz isso: posição em fração, unidade U, faixas de scale e alpha, o formato 'i,cx,cy,scale,alpha', o PadLayout sem android.* e a engrenagem com 0,7 s. Cortei essa parte e deixei só o porquê e a armadilha: por que há um layout por orientação, por que a entrada é por toque longo e de onde veio o padrão compacto.; Também deixei mais clara uma consequência que não está no código: mudar DEF_*, BASE_* ou DEFAULT_ALPHA só afeta quem nunca salvou um layout. Quem já salvou não vê a mudança, porque save() grava os 7 controles inteiros.
---

# Editor de controles virtuais do Android: decisões

- **Um layout por orientação** (`pad_port` / `pad_land`): em paisagem os controles ficam em lugares muito diferentes dos de retrato, então não dá para derivar um do outro. A posição e o tamanho são relativos à tela (fração e menor dimensão) para o layout funcionar em telas e densidades diferentes.
- **O editor só abre com toque longo (0,7 s) na engrenagem.** O toque simples foi descartado para o editor não abrir sem querer no meio do jogo. Durante a edição a emulação fica pausada, e o Voltar fecha o editor antes de sair da tela.
- **O padrão é compacto e semitransparente** (alpha 0,5, tamanhos cerca de 35% menores que a primeira versão). Isso veio de quem testou no celular: os botões originais pareciam grandes demais.
- **Armadilha:** o layout é salvo inteiro (os 7 controles). Por isso, mudar os padrões (`DEF_*`, `BASE_*`, `DEFAULT_ALPHA`) só afeta quem nunca editou o layout. Para levar um padrão novo a quem já salvou, é preciso migrar ou apagar as chaves salvas, ou o usuário tem que usar a opção de restaurar.
