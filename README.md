# Zdisplay

[![CI](https://github.com/zuunypro/zdisplay/actions/workflows/ci.yml/badge.svg)](https://github.com/zuunypro/zdisplay/actions/workflows/ci.yml)
[![Licença: GPL-3.0](https://img.shields.io/badge/licen%C3%A7a-GPL--3.0-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/zuunypro/zdisplay)](https://github.com/zuunypro/zdisplay/releases/latest)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011%20(x64)-0078d4)](https://github.com/zuunypro/zdisplay/releases/latest)

Controle de **brilho, contraste, saturação, gamma, temperatura de cor, matiz e
visão nas sombras** para Windows. Roda em segundo plano na bandeja, com perfis que trocam sozinhos conforme
o programa em foco ou o horário, e pode ser alterado de fora por linha de comando.

Feito em **C++ com Win32 puro**: um executável de ~1,6 MB, sem runtime nem
dependência nenhuma, **3,6 MB de memória** em uso e 0% de CPU parado. O zero é
literal: não existe laço de verificação em lugar nenhum — o programa dorme e o
Windows o acorda por `SetWinEventHook` quando a janela em foco muda.

**[Baixar a última versão](https://github.com/zuunypro/zdisplay/releases/latest)**
· [página do projeto](https://zuuny.vercel.app/programas/zdisplay)

---

## Por que ele existe

Hoje, para ter tudo isso, é preciso rodar vários programas ao mesmo tempo — e eles
brigam entre si pela mesma rampa de gamma do Windows:

| Programa | Faz | Não faz |
|---|---|---|
| vibranceGUI | vibrance NVIDIA por jogo | brilho, contraste, gamma, Intel |
| LightBulb / f.lux | temperatura de cor | saturação, hardware do monitor |
| Gammy | brilho adaptativo | saturação, perfis por app |
| Twinkle Tray | brilho DDC/CI (ótimo, mas Electron: 100–250 MB de RAM) | cor |
| Monitorian | brilho DDC/CI leve | cor |
| DimScreen | escurecer além do mínimo | tudo o mais |

O Zdisplay junta as sete funções num programa só, resolve os conflitos entre elas e
escolhe sozinho o melhor caminho disponível em cada máquina.

## Funciona em qualquer PC

Na inicialização o Zdisplay detecta o que a máquina suporta e monta uma pilha de
backends com recuo automático. Nada é obrigatório: se a GPU do fabricante não
existir, o caminho universal assume.

| Backend | Controla | Funciona em | Limites |
|---|---|---|---|
| **Rampa de gamma (GDI)** | brilho, contraste, gamma, temperatura, balanço de branco, bloqueio de luz azul | **qualquer GPU**, por monitor | vale até em jogos de tela cheia exclusiva; o Windows limita a faixa (veja abaixo) |
| **Matriz de cor (Magnification API)** | **saturação**, matiz, inversão | **qualquer PC, até só com vídeo integrado** | efeito global (não por monitor); não alcança tela cheia exclusiva |
| **NVIDIA (NVAPI)** | vibrance (DVC), matiz | só NVIDIA | carregada em tempo de execução |
| **AMD (ADL)** | saturação, matiz | só AMD | carregada em tempo de execução |
| **DDC/CI** | brilho, contraste e **ganho RGB** no hardware do monitor, além de entrada, predefinição de cor e energia | monitores externos compatíveis | lento; fila com intervalo mínimo para poupar a EEPROM |
| **Nível de branco SDR** | **brilho em tela com HDR ligado**, onde a rampa não vale | Windows 10 1803+, por monitor | governa o conteúdo SDR; filme e jogo em HDR mantêm o brilho próprio |
| **Backlight (WMI)** | brilho físico da tela interna | notebooks e all-in-ones | usa os degraus que o painel declara; casa a instância do WMI com o monitor certo |
| **Sobreposição** | escurecer além do mínimo do painel | qualquer PC | lava o contraste; aparece em capturas de tela |

Veja o que a sua máquina suporta com `zdisplay.exe --diag` ou na aba **Diagnóstico**.

A aba lista, **para cada monitor**, quem cuida de quê — e, quando um controle não
funciona ali, o motivo. É a resposta para a pergunta que aparece na prática, *"por
que esta barra não faz nada nesta tela?"*:

```
AOC FTV (DISPLAY1)
    brilho, contraste, gamma, temperatura, sombras: rampa de gamma
    saturação, vibrance, matiz: AMD (ADL) + matriz de cor
    brilho físico: indisponível (este monitor não respondeu a DDC/CI)
```

### Monitor genérico, sem marca, com firmware ruim

A API de alto nível do Windows (`GetMonitorBrightness`/`SetMonitorBrightness`)
valida a string de capacidades do monitor **antes** de falar com ele. Painel
barato costuma trazer essa string truncada, malformada ou ausente — e aí a API
recusa mesmo quando o monitor obedeceria. O Zdisplay tenta o caminho de alto nível
primeiro e, quando ele recusa, **fala VCP direto** com o painel. Um monitor que
não responde nem assim aparece como "sem suporte" em vez de sumir da lista.

O ganho RGB por hardware (VCP `0x16`/`0x18`/`0x1A`) é sondado lendo o próprio
registrador, não perguntando pelas capacidades. Ele entra como caminho de
temperatura de cor quando a rampa de gamma não vale — que é o caso de tela com
**HDR ligado**, onde `SetDeviceGammaRamp` devolve sucesso e não muda um pixel.

### Identidade do monitor

A chave de cada monitor sai do **EDID** (fabricante + modelo + número de série),
não do caminho de dispositivo. O caminho carrega o número da porta em que o cabo
está: trocar de saída da placa fazia o ajuste próprio daquele monitor
desaparecer. Configurações gravadas no formato antigo são migradas sozinhas.

O EDID também dá o nome real do painel em vez de "Generic PnP Monitor", e as
coordenadas de gamut — é o que explica um monitor barato de gamut largo estourar
as cores sem que nada esteja errado no Zdisplay.

### Duas placas de vídeo na mesma máquina

Em notebook híbrido a tela costuma estar pendurada na Intel mesmo havendo uma
NVIDIA na máquina. O Zdisplay descobre qual placa dirige cada tela
(`DISPLAYCONFIG_ADAPTER_NAME`) e só usa o caminho do fabricante onde ele
realmente vale. Antes, a NVAPI aceitava o comando e devolvia sucesso sem mudar
nada — falha silenciosa, a pior espécie.

Intel não expõe vibrance por driver como NVIDIA e AMD; nessas máquinas a
saturação vai pela matriz de cor universal, e o diagnóstico diz isso em vez de
deixar a barra parecer quebrada.

---

## Bloqueio de luz azul

Uma barra na aba **Ajustes**, guardada em cada perfil como qualquer outro ajuste.
Existe separada da temperatura de propósito: a temperatura desloca o branco
inteiro, então quem só quer cortar azul acaba tendo que escolher um Kelvin que
também mexe no vermelho — e depois não sabe dizer quanto de azul sobrou. Aqui o
número da barra é a resposta direta a *quanto de azul estou cortando*.

Corta o azul e, mais de leve, o verde; o vermelho não se mexe. No máximo sobra
15% do azul — zerar o canal apagaria toda distinção entre os tons de azul da
imagem, e o piso de luz teria de desfazer o ajuste logo em seguida.

A barra entra na conta do piso de luz como qualquer outro campo: nenhuma
combinação de bloqueio, brilho e temperatura consegue furá-lo, e isso é testado.

É a versão manual e por perfil do que a aba **Visão** faz sozinha pelo relógio —
as duas convivem, e a camada de visão nunca desfaz o que o perfil pediu.

---

## Aba Visão — conforto para os olhos, em uma chave

Uma chave e quatro números. A tela vai esquentando sozinha conforme o sol se põe e
volta ao normal de manhã, **por cima de qualquer perfil** — não é preciso criar
perfil nem regra de horário para isso.

| Campo | O que faz |
|---|---|
| **Temperatura de dia** | 6500 K é o branco neutro. |
| **Temperatura de noite** | 3400 K é a cor de lâmpada incandescente. Quanto menor, menos azul chegando aos olhos. |
| **Brilho de noite** | % do que o perfil pede. Vale tanto quanto a cor: o que cansa a vista é a tela estar muito mais clara que o ambiente. |
| **Suavidade da passagem** | Minutos. A mudança acontece aos poucos **em torno** do horário, metade antes e metade depois. Uma hora já não dá para perceber. |
| **A noite / o dia começa** | Relógio (`22:00`) ou o próprio sol: `por`, `nascer`, `por-30`, `nascer+45`. |
| **Pausa para os olhos** | A cada N minutos, um aviso discreto na bandeja para olhar 20 segundos para algo a uns 6 metros. |

A camada **nunca vai contra o que você escolheu**: se o perfil ativo já pede algo
mais quente ou mais escuro que o alvo da noite, o perfil manda. Ela só puxa na
direção do conforto.

Os botões *Ver como fica* mostram o resultado enquanto você os segura — escolher a
temperatura da noite sem ver como fica seria adivinhação.

**Sobre o que ajuda de verdade:** a redução de azul à noite tem evidência para
*sono*, não para cansaço visual. O que a literatura clínica realmente recomenda
contra fadiga visual de tela é a pausa 20-20-20 — por isso ela está aqui, e por
isso o campo de brilho noturno existe: a diferença entre a tela e o ambiente pesa
mais que a cor.

---

## Visão nas sombras

Duas barras na aba **Ajustes** para o problema de enxergar o que está no escuro —
o canto sem luz do mapa, a sala mal iluminada do filme — **sem lavar o resto da
imagem**. É o que os monitores de jogo vendem como *Black eQualizer* ou *Shadow
Boost*, feito por software e em qualquer monitor.

| Barra | O que faz |
|---|---|
| **Sombras** (0–100) | levanta o piso do preto com um peso que **morre antes dos tons médios** |
| **Definição** (0–100) | afasta os tons quase pretos uns dos outros, devolvendo o detalhe que o levante achataria |

**Por que duas e não uma.** Subir brilho ou gamma clareia a imagem inteira: o
escuro aparece, mas os médios perdem corpo e os claros estouram. Restringir o
efeito à parte baixa da curva resolve isso — só que quem levanta o piso achata a
inclinação perto do preto, e tons vizinhos viram o mesmo tom. O detalhe some
justamente onde você queria vê-lo. A segunda barra devolve essa inclinação.

Medindo nos 32 tons mais escuros, quantos continuam **distintos** entre si:

| Ajuste | Tons distintos | Tom 0 vira |
|---|---|---|
| neutro | 32 / 32 | 0 |
| Sombras 100 sozinha | 20 / 32 | 40 |
| Sombras 100 + Definição 100 | **29 / 32** | 40 |
| Sombras 78 + Definição 65 | 27 / 32 | 31 |

E o que acontece com o resto da imagem em **Sombras 78 + Definição 65**:

| Tom de entrada | 4 | 10 | 40 | 80 | 128 | 180 | 220 | 255 |
|---|---|---|---|---|---|---|---|---|
| Tom de saída | 35 | **41** | 64 | 89 | 129 | 180 | 220 | 255 |

O tom 10 — invisível na prática — vira 41. O tom 128 anda um ponto. De 180 para
cima **nada muda**: brancos, céu e interface ficam exatamente onde estavam.

Como tudo isso vai pela **rampa de gamma**, vale também dentro de jogos em tela
cheia exclusiva, sem sobreposição, sem custo de CPU e sem aparecer em capturas.

O perfil pronto **Competitivo** já vem com Sombras 78 / Definição 65, e o perfil
**Jogo** ganhou uma dose leve (40 / 30).

### O que estas barras não são

- **Não são nitidez de contorno.** Nitidez de verdade compara cada pixel com os
  vizinhos, e a rampa de gamma é uma tabela de 256 entradas que não enxerga
  vizinhança nenhuma — só o tom. Fazer isso exigiria capturar e reprocessar a tela
  quadro a quadro, o que custa GPU, atrasa a imagem e não funciona em tela cheia
  exclusiva. O que a **Definição** faz é contraste nos tons baixos, que é o que
  realmente traz o detalhe escuro de volta.
- **Cores escuras perdem um pouco de saturação.** A tabela é por canal e trata
  cada um sem saber dos outros, então somar o mesmo piso a R, G e B aproxima os
  três — e aproximar é desbotar. Se incomodar, compense com +5 a +10 em
  **Saturação**.
- **A janela mais forte ainda ganha 3 tons de 32 em fusão**, por causa dos 8 bits
  do sinal. É o preço de esticar a parte baixa da curva; não tem como zerar numa
  tabela de tons.

---

## Compilar

Precisa apenas de um compilador C++ (MinGW-w64). Se você não tiver, o script baixa
um portátil sozinho, sem instalação e sem admin.

```bash
build.bat
```

Ou, com `make` no PATH:

```bash
make
```

O ícone é desenhado pelo próprio programa (`zdisplay.exe --make-icon`) — não há
dependência de nenhuma ferramenta gráfica.

### Gerar o instalador

```bash
build.bat setup
```

O resultado é `zdisplay-setup.exe`, um instalador visual e autocontido. Ele
instala por usuário em `%LOCALAPPDATA%\Programs\Zdisplay`, cria o atalho do menu
Iniciar, registra o Zdisplay em **Aplicativos instalados** e inclui o
desinstalador. Não pede administrador. O executável incorporado é validado por
CRC-32, SHA-256 e estrutura PE x64 antes de qualquer arquivo ser gravado.

## Usar

```bash
zdisplay.exe
```

Clique duas vezes no ícone da bandeja para abrir as configurações; clique do meio
pausa e restaura a tela na hora.

### Linha de comando

Com o Zdisplay já rodando, qualquer chamada vira um comando para a instância aberta —
serve para scripts, Stream Deck, AutoHotkey ou atalhos do Windows.

```bash
zdisplay.exe --profile "Jogo"      # ativa um perfil
zdisplay.exe --brightness 70       # brilho por software (10..150)
zdisplay.exe --saturation 130      # saturação (0..200)
zdisplay.exe --vibrance 60         # vibrance da GPU (0..100)
zdisplay.exe --temperature 3400    # temperatura de cor em Kelvin
zdisplay.exe --shadows 78          # levanta só os tons escuros (0..100)
zdisplay.exe --clarity 65          # detalhe nas sombras (0..100)
zdisplay.exe --hwbrightness 40     # brilho físico do monitor (DDC/CI)
zdisplay.exe --dim 25              # escurecimento extra por sobreposição
zdisplay.exe --auto                # volta ao modo automático
zdisplay.exe --toggle              # pausa / retoma
zdisplay.exe --reset               # devolve a tela ao estado original
zdisplay.exe --panic               # EMERGÊNCIA: devolve a tela e pausa
zdisplay.exe --status              # imprime o estado atual
zdisplay.exe --diag                # imprime os backends detectados
zdisplay.exe --list                # lista os perfis
zdisplay.exe --aba 5               # abre numa aba: 0 Ajustes, 1 Visão, 2 Perfis,
                                   #   3 Automação, 4 Sistema, 5 Diagnóstico
zdisplay.exe --quit
```

Os comandos que ajustam um valor (`--brightness`, `--saturation`, …) mexem no
valor **global** do perfil. Monitores com ajuste próprio mantêm o que você
configurou para eles — a resposta diz quantos ficaram de fora. Para mexer num
monitor específico, use a janela.

Chamado de um terminal, ele escreve a resposta no terminal; fora dele, mostra uma
caixa de diálogo. `zdisplay.exe --help` lista tudo.

### Atalhos globais (padrão)

| Atalho | Ação |
|---|---|
| `Ctrl+Alt+↑` / `↓` | brilho |
| `Ctrl+Alt+→` / `←` | saturação |
| `Ctrl+Alt+K` | pausar / retomar |
| `Ctrl+Alt+P` | abrir a janela |
| `Ctrl+Alt+Shift+K` | **emergência**: devolve a tela e pausa |

Configuráveis na aba **Sistema**, e cada perfil pode ter o seu.

### Perfis e automação

Um perfil guarda todos os ajustes, com sobrescrita opcional **por monitor**. Seis
vêm prontos: Padrão, Jogo, Competitivo, Noite, Filme e Leitura.

A escolha do perfil segue esta prioridade:

1. perfil fixado à mão (bandeja, janela ou atalho);
2. **regra por aplicativo** — o Zdisplay usa `SetWinEventHook` para saber qual
   programa está em foco, sem laço de verificação, então o custo parado é zero;
3. **regra por horário** (aceita faixas que cruzam a meia-noite);
4. perfil padrão.

Os campos *Início* e *Fim* de uma regra de horário aceitam relógio (`22:00`) ou o
**próprio sol**: `por`, `nascer`, e com deslocamento como `por-30` ou
`nascer+45`. O horário do pôr do sol anda mais de duas horas ao longo do ano,
então uma faixa fixa fica errada em metade dos meses. Para isso funcionar,
preencha *Latitude* e *Longitude* na mesma aba — sem localização, uma regra solar
simplesmente não casa, em vez de trocar o perfil no horário de um lugar onde você
não está.

Dentro de cada nível, quem tem **prioridade** maior ganha — tanto nas regras por
aplicativo quanto nas de horário, para duas faixas que se sobrepõem terem um
desempate explícito em vez de depender da ordem das linhas no arquivo.

**Não é preciso saber o nome do executável.** O campo *Processo*, na aba
**Automação**, é uma lista com os programas que você tem abertos agora — a mesma
lista do Alt+Tab, reenumerada toda vez que você abre o menu. Escolha e pronto. O
campo continua aceitando texto digitado, para programas que não estão rodando no
momento e para curingas: `cs*` pega `cs2` e `csgo`. O botão **Usar o programa em
foco** continua ali para o caso do jogo em tela cheia, que some da lista enquanto
você está na janela do Zdisplay.

As trocas são suavizadas quadro a quadro. Comandos lentos de hardware (DDC/CI e
backlight) só são enviados no valor final, nunca durante a transição.

**Teclas de brilho do teclado.** Num notebook acoplado, as teclas de brilho só
mexem no painel de dentro e as duas telas ficam desencontradas. Com *Teclas de
brilho também valem nos monitores externos* ligado (aba **Sistema**), o Zdisplay
percebe a mudança e leva o mesmo valor aos monitores externos por DDC/CI. Um
perfil que já gerencia o brilho físico continua mandando: nesse caso o
espelhamento não entra, para os dois não brigarem.

### Configuração

Arquivo INI legível e editável à mão, gravado de forma atômica:

```
%APPDATA%\Zdisplay\zdisplay.ini
```

**Modo portátil:** crie um arquivo vazio chamado `zdisplay-portable.txt` ao lado do
executável e tudo passa a ser gravado na mesma pasta — nada no registro (a não ser
que você ligue o início automático), nada em `%APPDATA%`.

---

## Proteções

O programa mexe na tela inteira. Se um ajuste der errado, o usuário pode ficar sem
enxergar o suficiente para desfazê-lo — então as proteções abaixo existem para que
isso nunca aconteça.

**O reset devolve a *sua* tela, não uma tela "neutra".** Ao iniciar, o Zdisplay lê e
guarda em `baseline.dat` a rampa de gamma de cada monitor, o brilho e o contraste
físicos (DDC/CI), o backlight do notebook e o vibrance que já estava no painel da
GPU. O reset restaura exatamente esses valores. Se o monitor tem calibração ICC,
ela é **preservada**: os ajustes passam a ser aplicados *por cima* da calibração,
com interpolação para não criar faixas de cor.

**Recuperação depois de um travamento.** Enquanto roda, existe um arquivo
`session.lock`. Se ele ainda estiver lá no arranque seguinte, a execução anterior
não terminou direito — e o Zdisplay devolve a tela ao estado guardado antes de
qualquer outra coisa. Nem um desligamento na tomada deixa a tela presa num ajuste.

**Confirmação com reversão automática.** Quando os ajustes deixam a tela escura
demais (menos de 20% de luz estimada), aparece uma janela com contagem regressiva
de 15 segundos. Não fazer nada desfaz o ajuste — é o mesmo desenho que o Windows
usa ao trocar a resolução, e funciona justamente no caso em que o usuário não
consegue enxergar o botão. Se o Zdisplay abrir já num estado escuro, o ponto de
retorno passa a ser o neutro.

**Atalho de emergência: `Ctrl+Alt+Shift+K`.** Devolve a tela e pausa, de qualquer
lugar do Windows. Apagar esse campo nas configurações não o desativa — o padrão
volta sozinho. Também disponível como `zdisplay.exe --panic` e no menu da bandeja.

**Piso absoluto de luz.** Nenhum caminho de entrada — arquivo editado à mão, linha
de comando, perfil importado de outra máquina — consegue produzir menos de 8% de
luz. Valores fora da faixa, `NaN` e infinito são corrigidos na leitura.

A luz que sobra é **medida sobre a curva que vai para a tela**, e não estimada
por uma fórmula à parte: brilho, contraste, gamma, temperatura, balanço de branco
e visão nas sombras entram todos na conta, com os pesos Rec.709. Isso importa
porque cada um deles escurece de verdade — gamma 0,30 com os ganhos em 50 e a
temperatura em 1500 K deixa a tela quase preta sem que a barra de brilho saia dos
100%. Quando a combinação fica abaixo do piso, o Zdisplay afrouxa na ordem em que
menos custa: primeiro a sobreposição, depois o brilho físico, o brilho por
software, os ganhos, a temperatura, o gamma e por fim o contraste.

**A configuração tem cópia de segurança.** Gravação atômica, `zdisplay.ini.bak` da
versão anterior, e leitura tolerante: um arquivo estragado cai no backup, e um
arquivo sem nada de aproveitável é guardado como `.invalido` em vez de ser
sobrescrito em silêncio.

**A EEPROM do monitor é poupada.** Comandos DDC/CI passam por fila com intervalo
mínimo de 140 ms, são unificados quando repetidos, nunca são enviados durante
transições e têm teto rígido de 40 por minuto por monitor.

**Toda alteração no sistema tem volta.** O único ponto que pede administrador é a
liberação da faixa de gamma, e o mesmo botão desfaz. O início automático usa
`HKCU\...\Run`, que some junto com o programa.

## Testes

```bash
build.bat test
```

Ou, para compilar o programa **e** rodar a suíte na mesma invocação:

```bash
make check
```

404 testes que não dependem de hardware — rodam igual em qualquer máquina. Cobrem
a matemática de cor (5.400 combinações de rampa verificando que ela nunca decresce
nem apaga a tela), as 441 combinações da curva das sombras, os limites de
segurança, as regras de aplicativo e horário, a ida e volta da configuração, dez
arquivos de configuração propositalmente estragados, a gravação do estado original
e oito configurações de PC simuladas (NVIDIA, AMD, Intel, notebook, máquina
virtual, quatro monitores, e uma máquina sem nenhum backend disponível).

Uma seção de **regressões** prende, um a um, os defeitos que a auditoria
encontrou: o piso de luz furado por temperatura e ganhos, o `NaN` que atravessava
o `Clamp`, o perfil importado sem saneamento, a descontinuidade da curva de
temperatura em 6600 K, as sombras engolindo a imagem com brilho baixo, e o
baseline cortado deixando estado pela metade.

O que o hardware publica sobre si mesmo também é testado sem hardware nenhum: o
**EDID** é montado byte a byte no teste (cabeçalho, checksum, fabricante
empacotado em 5 bits, primárias sRGB e DCI-P3) e o parser precisa recusar bloco
truncado, zerado ou com checksum errado — porque aceitar um desses viraria um
"número de série" inventado, e a chave do ajuste por monitor passaria a apontar
para o painel errado. A **string de capacidades DDC/CI** é testada com uma
resposta real, incluindo o caso que mais engana: os números dentro de
`14(01 05 06)` são os valores aceitos por aquele código, não códigos — e é
justamente essa lista que decide quais entradas o programa oferece, então ela é
testada também contra firmware que remove todo o espaço e contra lixo não
hexadecimal, que chega por I2C e não é dado confiável.

A conversão do **nome de instância do WMI** para o caminho canônico do
dispositivo tem seção própria, porque é o que casa o painel que o WMI comanda com
o monitor que o resto do programa conhece: dois painéis embutidos do mesmo modelo
(Zenbook Duo, Yoga Book 9i) precisam continuar distintos, senão o brilho vai para
a tela errada. As **regras por modelo de monitor** são testadas na ida e na volta
pelo arquivo, e uma regra com erro de digitação precisa ser recusada em vez de
virar uma regra vazia que silenciosamente não faz nada.

O **nascer e o pôr do sol** são conferidos contra os solstícios de São Paulo e de
Londres (dentro de 5 a 10 minutos), mais as invariantes que pegam troca de sinal:
dezembro é o dia longo no hemisfério sul e o curto no norte, no equador o dia dura
pouco mais de 12 h o ano inteiro, e acima do círculo polar a função precisa dizer
que não há nascer nem pôr em vez de devolver um número.

## O limite de gamma do Windows

Sem uma alteração no registro, o Windows **recusa** rampas de gamma que se afastem
demais da linear — e recusa em silêncio. É por isso que programas do gênero às
vezes "não fazem nada" em combinações fortes (brilho baixo + temperatura quente).

O Zdisplay não finge que funcionou: quando o Windows recusa, ele procura a maior
fração do efeito que o sistema aceita, aplica essa fração e avisa na barra de
status e no diagnóstico ("o Windows limitou o efeito a X%"). Ele também tenta
periodicamente voltar ao efeito integral, caso você libere a faixa.

Isso pesa especialmente na **visão nas sombras**: levantar o piso do preto é, por
definição, um afastamento grande da linear, então é o ajuste que mais chega ao
teto do Windows. Ele continua funcionando na fração aceita — só não com a força
inteira. Se as Sombras parecerem fracas, é quase sempre isso; olhe o aviso no
diagnóstico antes de culpar a barra.

Para liberar a faixa completa: aba **Sistema → Liberar faixa completa de gamma**.
Isso grava `GdiIcmGammaRange=256` em
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ICM`, exige uma execução como
administrador e só passa a valer depois de reiniciar a sessão do Windows.

## Outras ressalvas honestas

- **Luz noturna do Windows** disputa a mesma rampa. O Zdisplay detecta e avisa; o
  "reforçar ajustes a cada N segundos" (aba Sistema) reafirma os valores quando
  outro programa sobrescreve.
- **Jogos em tela cheia exclusiva**: a rampa de gamma e as APIs de fabricante
  continuam valendo; a matriz universal e a sobreposição, não. Use modo janela sem
  bordas se precisar da saturação universal dentro do jogo.
- **HDR ligado** faz o Windows ignorar a rampa de gamma — ele aceita a escrita e
  não muda um pixel. Nessas telas o brilho vai pelo **nível de branco SDR**, que é
  a mesma coisa que a barra "Brilho do conteúdo SDR" do Windows move: funciona por
  tela, sem I2C e sem gravar em EEPROM, e vale em jogo de tela cheia exclusiva.

  | Arranjo | O que passa a valer |
  |---|---|
  | Qualquer tela em HDR | **brilho: nível de branco SDR**, ancorado no valor que a tela já tinha |
  | Todas as telas em HDR | contraste e temperatura vão pela **matriz de cor** |
  | HDR em algumas telas | contraste e temperatura ficam sem efeito na tela HDR |

  No arranjo misto a matriz não pode assumir contraste e temperatura: o efeito
  dela é da área de trabalho inteira, e as telas sem HDR — que já receberam a
  rampa — ficariam com o ajuste em dobro. Gamma e visão nas sombras são curvas e
  não têm equivalente, então continuam sem efeito em tela HDR.

  O nível de branco SDR governa a área de trabalho e o conteúdo comum. Filme ou
  jogo em HDR de verdade desenha na faixa HDR e mantém o brilho próprio — não há
  o que fazer por software nesse caso.

  A detecção usa a consulta de **modo ativo** do Windows 11 24H2, e não só o bit
  antigo de "cor avançada". A diferença importa: com a Gestão Automática de Cor
  ligada, o bit antigo fica verdadeiro **com a tela em SDR**, e quem confia nele
  desliga gamma e sombras sem motivo numa máquina em que eles funcionam.
- **DDC/CI** grava na EEPROM do monitor. O Zdisplay limita a taxa, coalesce pedidos e
  nunca envia comandos durante transições — e de propósito **não** restaura o
  brilho físico ao sair, porque esse valor é seu.
- **Monitores que não seguem o padrão** têm tabela própria. Há painel que responde
  brilho em outro registrador (e no 0x10 aceita o comando sem mudar nada) e há
  firmware que derruba o driver de vídeo ao receber DDC/CI. O que já se sabe vem
  embutido; o resto você acrescenta no arquivo de configuração, por modelo:

  ```ini
  [modelo:FUS087C]
  regra=brilho-vcp:6B
  ```

  Aceita `bloquear`, `sem-capacidades` e `brilho-vcp:XX`, e a sua regra manda mais
  que a embutida — dá para desfazer uma entrada de fábrica que atrapalhe o seu
  hardware. O identificador é o do EDID (três letras do fabricante + código do
  produto), visível na aba **Diagnóstico**.
- **"Detectei" não é "funciona".** O botão *Testar o monitor*, na aba Diagnóstico,
  muda o brilho um passo, lê de volta, confere e devolve o valor que estava. É o
  que separa um monitor que obedece de um que aceita o comando, responde sucesso e
  não muda nada — caso comum o bastante para merecer prova, e impossível de
  diagnosticar no olho.

## Segurança

Sem rede, sem telemetria, sem elevação. O único ponto que pede administrador é o
botão de liberar a faixa de gamma, e ele é opcional e explicitamente confirmado.
O início automático usa `HKCU\...\Run`, trivial de desfazer.

As DLLs de sistema que o Zdisplay carrega em tempo de execução (`nvapi64`,
`atiadlxx`, `dxva2`, `magnification`) vêm **sempre de System32**, via
`SetDefaultDllDirectories` mais `LOAD_LIBRARY_SEARCH_SYSTEM32`. Sem isso, o
diretório do executável precede o do sistema na ordem de busca — e num programa
portátil, que costuma ser executado de Downloads ou de um pendrive, bastava um
arquivo com o nome certo ao lado dele para virar execução de código.

O build de release falha se o executável ou o instalador não estiverem marcados
com DEP, ASLR e ASLR de alta entropia. Em execução, ambos também recusam imagens
remotas e de baixa integridade. O desinstalador remove somente os arquivos que o
instalador criou; nunca apaga recursivamente uma pasta escolhida pelo usuário.

A instância única e o canal de comando são **por sessão do Windows**, não por
máquina: em um PC com dois usuários logados, cada um tem o seu Zdisplay e nenhum
esbarra no outro.

## Contribuir

Veja [CONTRIBUTING.md](CONTRIBUTING.md) para compilar, rodar a suíte e entender
as regras que a base de código não abre mão — o piso de luz absoluto, o binário
sem exceções, todo backend opcional e a proibição de tratar "detectei" como
"funciona".

Falha de segurança não vai em issue pública: veja [SECURITY.md](SECURITY.md).

## Estrutura

```
src/
  version.h           número de versão, usado pelo .rc e pelo código
  ui_dpi.h            escala por DPI, compartilhada entre as janelas
  ui_theme.cpp/.h     tema escuro: paleta, moldura da janela e desenho proprio
                      dos controles que não aceitam cor por mensagem
  common.*            utilitários, log, caminhos, carregamento dinâmico de DLL
  core.*              ajustes, perfis, INI, matemática de cor, monitores
  backends.h          interface comum e capacidades
  backends_display.*  rampa de gamma, matriz universal, sobreposição
  backends_vendor.*   NVAPI (NVIDIA) e ADL (AMD)
  backends_hw.*       DDC/CI e backlight por WMI
  engine.*            resolução de perfil, transições, watchdog, recuos
  services.*          atalhos globais, app em foco, named pipe, início automático
  ui_*.cpp            bandeja, janela de configuração e eventos
  icon.cpp            o ícone, desenhado em código
  main.cpp            entrada, instância única, CLI
```

Comentários do código são em **inglês**; texto de interface e mensagens de log,
em **português** — é o idioma de quem usa o programa.

## Licença

[GPL-3.0-or-later](LICENSE). Você pode usar, estudar, modificar e redistribuir.
Se distribuir uma versão modificada, ela precisa vir com o código-fonte e sob a
mesma licença.
