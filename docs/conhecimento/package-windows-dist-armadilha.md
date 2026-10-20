---
titulo: package-windows.sh não pode apagar dist/GBAHandoff-windows
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 428ea59
arquivos: tools/package-windows.sh
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:31:46Z
parecer: Bate com o código atual: tools/package-windows.sh monta em STAGE=$(mktemp -d) com trap 'rm -rf "$STAGE"' EXIT, apaga só o zip (mkdir -p "$DIST"; rm -f "$ZIP"), roda com set -euo pipefail e tem GBAH_DIST como override para testes. O diff do 428ea59 mostra o comportamento antigo (rm -rf "$DIST/GBAHandoff-windows" "$ZIP" e montagem em $DIST/GBAHandoff-windows/GBAHandoff), e o conteúdo do zip continua o mesmo.; Não está duplicado: link-operacao, operacao-teste-manual-windows e pc-instalacao-app-adb citam o script, mas nenhum fala da pasta de uso travada pelo Windows nem do Input/output error.; Explica o porquê: dá o sintoma (Input/output error abortando por causa do set -e), a causa (arquivos abertos no Windows ficam travados e o WSL não consegue apagar), a decisão (montar num diretório temporário) e uma regra para não repetir o erro. O comentário no script cobre a regra, mas não o sintoma.; É conciso e não tem segredos nem dados pessoais. O passo 'Verificar' precisa de um arquivo aberto no Windows e não pôde ser rodado aqui, mas a lógica do script o confirma: nada é apagado dentro de <dir>/GBAHandoff-windows/.
---

# package-windows.sh não pode apagar dist/GBAHandoff-windows

Sintoma (relatado pelo dev): ao reempacotar, o `rm -rf dist/GBAHandoff-windows` falhava com "Input/output error". Essa pasta é onde o pacote anterior foi descompactado e está rodando no Windows; com o `gbahandoff.exe` ou o `adb.exe` abertos, o Windows trava os arquivos e o `rm` feito pelo lado Linux/WSL não consegue removê-los. Como o script usa `set -e`, o empacotamento abortava.

Decisão: o script monta a árvore `GBAHandoff/` num diretório temporário (`mktemp -d`, removido por `trap` ao sair) e só grava `dist/GBAHandoff-windows.zip` (apagando apenas o zip antigo). O conteúdo do zip é o mesmo; só mudou onde é montado.

Regra: nunca apague nem monte coisas dentro de `dist/GBAHandoff-windows/`; é a pasta de uso do usuário.

Verificar: `GBAH_DIST=<dir> tools/package-windows.sh` (a variável existe para testes) com um arquivo aberto no Windows em `<dir>/GBAHandoff-windows/` deve terminar com "Created ...zip".
