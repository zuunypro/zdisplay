## O que muda

<!-- O comportamento observável que mudou, em uma ou duas frases. -->

## Por quê

<!-- Issue relacionada, se houver: Closes #123 -->

## Como testar

<!--
Passos para verificar à mão. Importante em código de UI e de hardware, onde o
teste automatizado não alcança.
-->

## Checklist

- [ ] `make check` passa (compila **e** roda a suíte)
- [ ] Mudança de comportamento tem teste; correção de defeito tem teste que
      falhava antes
- [ ] Comentários novos estão em inglês; texto de interface e log, em português
- [ ] Nenhuma flag de compilação foi duplicada fora de `flags.mk`
- [ ] Nenhum caminho novo consegue furar o piso de luz
