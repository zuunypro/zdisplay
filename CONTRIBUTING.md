# Contribuindo com o Zdisplay

Obrigado pelo interesse. Este documento descreve como compilar, o que a base de
código espera de uma mudança e como enviar uma contribuição.

## Compilar

Basta um compilador C++17 para Windows (MinGW-w64). Se você não tiver nenhum, o
script baixa um portátil sozinho, sem instalação e sem privilégio de
administrador:

```bash
build.bat
```

Com `make` e a toolchain já no `PATH`:

```bash
make check
```

`make check` compila o aplicativo **e** roda a suíte. É o alvo a usar antes de
abrir um pull request — `make` sozinho não roda os testes.

## Testes

```bash
build.bat test
```

A suíte não depende de hardware: roda igual em qualquer máquina, inclusive em CI
sem monitor, sem GPU dedicada e sem monitor externo. Toda mudança de
comportamento precisa vir com teste.

Se você está corrigindo um defeito, escreva primeiro o teste que falha. A seção
de regressões da suíte existe para que um defeito corrigido não volte.

## O que a base de código espera

O Zdisplay controla a tela inteira do usuário. Se um ajuste sair errado, a pessoa
pode ficar sem enxergar o suficiente para desfazê-lo. As regras abaixo vêm daí e
não são negociáveis:

- **O piso de luz é absoluto.** Nenhum caminho de entrada — arquivo editado à
  mão, linha de comando, perfil importado — pode produzir menos de 8% de luz
  estimada. Toda entrada é saneada na leitura.
- **Nada lança exceção.** O binário é compilado com `-fno-exceptions`. Não use
  `std::sto*` nem `.at()`. Erro é valor de retorno.
- **Todo backend é opcional.** O programa precisa rodar em máquina sem GPU
  dedicada, sem monitor externo e sem DDC/CI. Um backend ausente sai de cena em
  silêncio; nunca é pré-requisito.
- **DLL sempre de System32.** Carregamento em tempo de execução usa
  `LOAD_LIBRARY_SEARCH_SYSTEM32`. O programa é portátil e costuma rodar de
  Downloads ou de um pendrive.
- **DDC/CI grava na EEPROM do monitor.** Comandos passam pela fila existente,
  com intervalo mínimo e teto por minuto. Não envie comando direto ao painel.
- **"Detectei" não é "funciona".** Um backend que aceita o comando e devolve
  sucesso sem mudar nada precisa ser reportado como indisponível, não como ativo.

## Estilo

- C++17, Win32 puro. Sem dependência externa, sem runtime, sem framework.
- Comentários em **inglês**. Texto de interface e mensagens de log em
  **português** — é o idioma do usuário do programa.
- Comente o **porquê**, não o quê. Se o comentário só repete o que o código diz,
  ele não deve existir. Restrição escondida, invariante sutil e comportamento
  surpreendente de API do Windows merecem uma linha; o resto não.
- Escreva no presente e de forma impessoal. Comentário não é diário de
  desenvolvimento: não relate defeito passado nem compare com outros programas.
- Flags de compilação vivem em `flags.mk`, que é lido tanto pelo `Makefile`
  quanto pelo `build.bat`. Nunca duplique uma flag em um dos dois.
- Número de versão vive em `src/version.h`, que é a fonte única.

## Pull requests

1. Abra uma issue antes de mudança grande, para alinhar a direção sem trabalho
   perdido.
2. Um assunto por pull request. Refatoração e correção em PRs separados.
3. `make check` precisa passar. O CI roda o mesmo alvo no Windows.
4. Descreva o comportamento observável que mudou e como testar à mão, quando o
   teste automatizado não alcança (o que é comum em código de UI e de hardware).

## Relatando um problema

Use os modelos de issue. Para um problema de hardware — monitor que não responde,
backend que não aparece — anexe a saída de:

```bash
zdisplay.exe --diag
```

Ela lista, por monitor, qual backend cuida de quê e o motivo quando um controle
não está disponível. É a informação que resolve a maior parte desses relatos.

## Falha de segurança

**Não abra issue pública.** Veja [SECURITY.md](SECURITY.md) para o canal privado
e para a fronteira de segurança que o projeto se compromete a defender.

## Licença

Ao contribuir, você concorda em licenciar sua contribuição sob a
[GPL-3.0-or-later](LICENSE), a mesma licença do projeto.
