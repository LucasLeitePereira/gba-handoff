---
titulo: O padrão roms/ do .gitignore escondia as ROMs de teste em tests/roms/
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: d7240e6
arquivos: .gitignore, tests/roms/build.sh, tests/roms/gbah_test.s, tests/roms/README.md, tests/CMakeLists.txt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T17:53:54Z
parecer: O código confirma as afirmações. O .gitignore tem `roms/` e, depois dele, `!tests/roms/` e `tests/roms/*.sav`. `git check-ignore -v tests/roms/gbah_test.gba` não imprime nada (rc=1), e para `x.sav` aponta a regra da linha 12. `git ls-files` lista os dois .gba. O tests/CMakeLists.txt calcula `file(SHA1 ...)` no configure, e o gbah_test.s grava SRAM[0..2] (frame, teclas&0xFF, teclas>>8).; A parte nova e valiosa é a armadilha do padrão `roms/`, que sem barra inicial casa em qualquer profundidade e esconde as ROMs de teste sem nenhum aviso. Ela não está nos documentos aprovados. O decisoes-arquitetura-plataforma só diz que `roms/` é gitignored e que os testes usam `tests/roms/`.; Removi a seção sobre o que a ROM grava na SRAM. Ela descreve o código (gbah_test.s), e o tests/roms/README.md já documenta isso. A correção do 'R/L não verificáveis' cabe ao próprio testes-phone-link-armadilhas, que tem pendente própria.; Encurtei a nota de reconfigurar o CMake e de commitar os .gba. O tests/roms/README.md já cobre os dois pontos, então ficou só o sintoma enganoso (falha que parece sync quebrado), que é o que justifica estar na base.
---

# ROMs de teste escondidas pelo padrão `roms/` do .gitignore

**Sintoma.** Um clone limpo vinha sem `tests/roms/gbah_test.gba` e sem `gbah_test_late.gba`, e os testes que abrem a ROM (core, phone_link, e2e) falhavam. Nada avisava: o `git status` simplesmente não listava os `.gba`.

**Causa.** A regra `roms/` existe para barrar as ROMs comerciais do usuário na raiz. Sem barra inicial, ela casa com qualquer pasta `roms/` em qualquer profundidade, inclusive `tests/roms/`.

**Correção.** `!tests/roms/` reabre a pasta, já que as ROMs de teste são homebrew próprio (CC0). `tests/roms/*.sav` continua ignorado, porque o mGBA cria `gbah_test.gba.sav` ao rodar. A ordem importa: a negação tem de vir depois de `roms/`. Para verificar, `git check-ignore -v tests/roms/gbah_test.gba` não deve imprimir nada, e `git check-ignore -v tests/roms/x.sav` deve apontar a regra do `.sav`.

**Armadilha relacionada.** Depois de regravar a ROM (`tests/roms/build.sh`), rode `cmake .` no build. O SHA-1 esperado é calculado no configure; sem reconfigurar, os testes comparam com o hash da ROM antiga e falham como se o sync estivesse quebrado. Commite os `.gba` regravados.
