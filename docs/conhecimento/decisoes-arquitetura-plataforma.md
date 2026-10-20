---
titulo: Decisões de plataforma, stack, emulador e transporte
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: CMakeLists.txt, core/gbah_core.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:08:47Z
parecer: Conferi as afirmações no código. O CMakeLists.txt embute o libmgba como lib estática via submódulo (third_party/mgba, que é MPL-2.0) e monta o app desktop com SDL3 (third_party/SDL). O header do core diz que o core é compartilhado entre o desktop (SDL3) e o Android (NDK/JNI). O .gitignore ignora `roms/` com o comentário 'ROMs comerciais do usuário'. O tests/roms/README.md confirma que a única ROM de teste é homebrew CC0. Nada no código contradiz o documento.; Ainda não há código Android/Kotlin nem de transporte (adb reverse) no repositório. Esses itens ficam como decisões tomadas, sem afirmar que já estão implementados.; Duplicação: o documento aprovado contrato-handoff-estado.md já diz que o handoff leva state + .sav e que a ROM é identificada por SHA-1. Reduzi esses itens ao que é novo: a recusa do streaming de vídeo e a transferência da ROM quando o destino não tem.; Troquei o caminho absoluto `/workspace/roms/` por `roms/` na raiz do repositório, que é o padrão do .gitignore e não depende da máquina.; Mantive os motivos e as alternativas descartadas, que são o valor do documento. O núcleo próprio e o libretro não tinham motivo. Escrevi um motivo curto inferido do contexto: no caso do libretro, o frontend teria que ser próprio de qualquer jeito. O autor pode corrigir se o motivo real foi outro.; Está conciso e não tem segredos nem dados pessoais.
---

# Decisões de plataforma, stack, emulador e transporte

Contexto: app que migra uma partida de GBA entre PC e celular ("handoff").

- **Plataformas**: Android + Windows. iOS descartado (restrições a emuladores e a USB).
- **Stack nativa**: C/C++ + SDL3 no PC; Kotlin + NDK no Android, com o mesmo `core/` em C nos dois.
  Motivo: o PC alvo é fraco. Electron e Flutter descartados pelo consumo de recursos.
- **Emulador**: libmgba embutida como lib estática (MPL-2.0, open source, sem engenharia reversa).
  Núcleo próprio descartado (custo e precisão); libretro descartado (camada extra sem ganho, já que o
  frontend é próprio de qualquer jeito).
- **Transporte (MVP)**: `adb reverse` + TCP; exige Depuração USB ativada no celular.
  AOA e tethering USB descartados (complexidade; o tethering mexe na rede do PC).
- **O que migra**: o estado da emulação (ver contrato-handoff-estado), não streaming de vídeo.
  Se o destino não tiver a ROM (comparada por SHA-1), a ROM é transferida.
- **Conflito** (as duas pontas têm progresso diferente): o usuário escolhe a versão; a outra vira backup.
- **ROMs**: as comerciais do usuário ficam em `roms/` na raiz (gitignored, nunca versionar). Os testes
  automáticos usam só a ROM homebrew CC0 de `tests/roms/`.
