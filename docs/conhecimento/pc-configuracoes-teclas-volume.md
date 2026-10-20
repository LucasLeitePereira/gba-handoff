---
titulo: Configurações do PC: teclas e volume são locais; teclas reservadas e a regra de troca
tipo: convencao
gerado_por: claude-sonnet-5-5
commit: 2199055
arquivos: desktop/library.cpp, desktop/library.h, desktop/handoff.cpp, desktop/main.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:22:42Z
parecer: Conferi as afirmações no código e elas estão corretas: library.h:53-62, library.cpp:170-229 (key_reserved, settings_from_text com descarte total quando há duplicata), handoff.cpp:798-838 (send_settings só manda prefer_newest; set_local_settings só grava), main.cpp:638-689 e 810-820 (captura, 'Tecla reservada', 'Trocou com', subir o volume tira o mudo). O teste settings_test existe em tests/CMakeLists.txt:114-117.; Uma imprecisão: a tecla reservada '+' é o scancode EQUALS (a tecla '=/+' do teclado principal). Quem procurar 'PLUS' no código não acha. Corrigi isso no texto.; Os documentos aprovados não cobrem este assunto: protocolo-decisoes trata do SETTINGS do protocolo, não do que fica só no PC. Não há duplicação.; Tirei a parte que só descreve o código (formato chave=valor, passos do volume, onde a tela abre). Acrescentei o porquê que faltava: as teclas são reservadas porque são atalhos globais, e o ini com duplicata é descartado inteiro porque deixaria um botão sem tecla.; Não há segredos nem dados pessoais.
---

# Configurações do PC: o que viaja ao celular e o que não

- **Só `prefer_newest` é trocado com o celular** (CONTROL SETTINGS, comparado a cada conexão). As teclas e o volume/mudo ficam no `settings.ini` e valem só para este PC: `Hub::set_local_settings` grava e não envia nada. Motivo: o teclado e o volume do PC não fazem sentido no celular. Se você acrescentar uma configuração, decida de propósito em qual dos dois grupos ela entra.
- **Teclas reservadas** (Esc, F2, F5, F8, F11, M, `-`, `=`/`+` do teclado principal (scancode EQUALS), `-` e `+` do teclado numérico, UNKNOWN) não podem ser ligadas a um botão, porque são atalhos globais (menu, entrega, salvar/recarregar, tela cheia, mudo, volume). Ligar uma delas a um botão faria a tecla apertar o botão e disparar o atalho ao mesmo tempo. A tela de remapeamento mostra "Tecla reservada: X" e continua esperando outra tecla. No ini, uma tecla reservada é ignorada e o padrão fica. Ao criar um atalho novo, inclua a tecla em `key_reserved`.
- **Nunca duas ações na mesma tecla**, porque uma delas ficaria inalcançável. Por isso a tela troca as duas teclas ("Trocou com B") em vez de gravar a duplicata. Se um ini editado à mão tiver duplicata, ele é descartado inteiro e todas as teclas voltam ao padrão. Não se tenta adivinhar qual das duas vale.
- **Durante a captura de tecla nada mais reage** (só o Esc, que cancela). Sem isso, a tecla nova também dispararia o atalho dela.
- **Verificar:** `ctest -R settings_test` (ida e volta do texto, duplicatas, reservadas).
