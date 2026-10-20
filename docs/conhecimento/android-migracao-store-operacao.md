---
titulo: Migração do store legado e como instalar o APK sem perder o save
tipo: operacao
gerado_por: claude-sonnet-5-5
commit: 0b51017
arquivos: android/app/src/main/cpp/phone_link.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt, tests/phone_migrate_test.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T13:42:14Z
parecer: Conferi as afirmações no código e estão corretas. `phone_migrate_store` (phone_link.c:1608) só migra os `.state` que não têm `.meta` decodificável. Primeiro copia `.state` e `.sav` para o backup com `copy_if_absent`, que nunca sobrescreve, e só depois grava o `.meta` de forma atômica, com versão (dev, 1), base 0 e timestamp igual ao mtime do `.state` em ms. Os arquivos originais não são alterados.; `LinkClient.boot` é `@Synchronized` e roda uma vez por processo (retorna cedo se `roms` já existe). Ele chama `nativeMigrate` antes de `nativeInit`, ou seja, antes de criar o motor. O `applicationId` é `io.gbahandoff`, igual ao do comando `run-as`.; `tests/phone_migrate_test.c` cobre o mtime antigo, a cópia de backup e a idempotência: uma segunda execução e uma gravação posterior não re-migram nem sobrescrevem o backup. Também cobre o fallback `(dev_self, 1)` com timestamp 0, que o documento cita via sync-regras-versao.; Não repete os docs aprovados: sync-regras-versao só diz como um `.state` sem `.meta` é lido, e android-decisoes-app não fala de migração nem de instalação do APK.; Explica o porquê: a ordem backup → meta garante que uma falha no meio se recupera no próximo boot. Também registra o que não fazer: desinstalar apaga `files/`, e trocar a keystore dá `INSTALL_FAILED_UPDATE_INCOMPATIBLE`. É um documento de operação útil, curto e sem segredos.
---

# Migração do store legado e instalação do APK

**Contexto.** O app anterior ao sync gravava `files/games/<sha1>.state` e `.sav` sem `.meta`.
`LinkClient.boot` roda `phone_migrate_store` uma vez por processo, antes de criar o motor e de
qualquer outra coisa tocar no store.
- Para cada `.state` sem `.meta` legível: copia `.state`/`.sav` para `files/legacy-backup/` (nunca
  sobrescreve cópia existente; a primeira cópia fica para sempre), depois escreve o `.meta` com
  versão `(device_id deste celular, 1)`, sem base, e timestamp = mtime do `.state`. Os originais não
  são tocados. Idempotente: com `.meta` válido não faz nada, e uma gravação posterior não é
  re-migrada nem apaga o backup.
- Ordem importa: backup primeiro, `.meta` depois. Se falhar no meio, o próximo boot refaz sem perda.
- Sem a migração, o `.state` seria lido como `(dev_self, 1)` sem timestamp (ver sync-regras-versao);
  com ela o jogo antigo aparece na lista com a data certa.

**Instalar APK novo sem perder o progresso.**
- Use `adb install -r`. NUNCA desinstale (nem `adb uninstall`): apaga `files/` e o save com ele.
- O `install -r` só aceita se a assinatura bater: o `debug.keystore` tem de ser o mesmo da
  instalação anterior. Outra chave dá falha de assinatura (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) e a
  única saída seria desinstalar, que apaga o save. Em caso de dúvida, copie antes
  `files/games` com `adb shell run-as`.
- Conferir a migração: `adb shell run-as io.gbahandoff ls -l files/games files/legacy-backup`
  (cada `.state` antigo com `.meta` ao lado, e a mesma lista em `legacy-backup`). Só funciona em
  build debuggable.
