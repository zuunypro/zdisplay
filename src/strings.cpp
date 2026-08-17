#include "strings.h"

#include <map>

namespace zdisplay {
namespace {

/// English wording, then its translation, one pair per message.
///
/// Only languages other than English appear here; English is the key and is
/// returned as it stands. Ordering is by where the text appears in the
/// interface, not alphabetical, so a whole window can be reviewed in one pass.
struct Entry {
    const wchar_t* en;
    const wchar_t* pt;
};

const Entry kTable[] = {
    // Shipped profile names. Written into the configuration on a fresh install
    // and never rewritten afterwards, so a profile keeps the name it was born
    // with even if the interface language changes later.
    { L"Default",                                   L"Padrão" },
    { L"Game",                                      L"Jogo" },
    { L"Competitive",                               L"Competitivo" },
    { L"Night",                                     L"Noite" },
    { L"Movie",                                     L"Filme" },
    { L"Reading",                                   L"Leitura" },
    { L"%s (new)",                                  L"%s novo" },

    // Generic actions.
    { L"Add",                                       L"Adicionar" },
    { L"Update",                                    L"Atualizar" },
    { L"Remove",                                    L"Remover" },
    { L"New",                                       L"Novo" },
    { L"Duplicate",                                 L"Duplicar" },
    { L"Delete",                                    L"Excluir" },
    { L"Refresh",                                   L"Atualizar" },
    { L"Copy",                                      L"Copiar" },
    { L"Enabled",                                   L"Ativa" },
    { L"Name",                                      L"Nome" },
    { L"Start",                                     L"Início" },
    { L"End",                                       L"Fim" },
    { L"Priority",                                  L"Prioridade" },
    { L"Prio.",                                     L"Prior." },
    { L"Process",                                   L"Processo" },
    { L"All files",                                 L"Todos os arquivos" },

    // Language and performance settings.
    { L"Language",                                  L"Idioma" },
    { L"Automatic (from Windows)",                  L"Automático (do Windows)" },
    { L"The language is applied when this window is reopened.",
      L"O idioma é aplicado ao reabrir esta janela." },
    { L"Performance",                               L"Desempenho" },
    { L"Quality",                                   L"Qualidade" },
    { L"Balanced",                                  L"Equilibrado" },
    { L"Light",                                     L"Leve" },
    { L"Reasserts every 5 s and searches for the maximum effect even while a slider is moving.",
      L"Reafirma a cada 5 s e procura o efeito máximo mesmo enquanto a barra se move." },
    { L"The default: reasserts every 10 s and completes the search once the slider stops.",
      L"O padrão: reafirma a cada 10 s e completa a busca quando a barra para." },
    { L"Lowest cost: reasserts every 30 s and switches profile without animation. "
      L"No feature is turned off.",
      L"Menor consumo: reafirma a cada 30 s e troca de perfil sem animação. "
      L"Nenhum recurso é desligado." },

    // Tray icon and its menu.
    { L"Profile: ",                                 L"Perfil: " },
    { L"Zdisplay paused",                           L"Zdisplay pausado" },
    { L"Zdisplay — paused",                         L"Zdisplay — pausado" },
    { L"Zdisplay — %s\nBrightness %.0f%%  Saturation %.0f%%",
      L"Zdisplay — %s\nBrilho %.0f%%  Saturação %.0f%%" },
    { L"Automatic (app and schedule rules)",        L"Automático (regras de app e horário)" },
    { L"Pause (restores the display)",              L"Pausar (devolve a tela)" },
    { L"Resume",                                    L"Retomar" },
    { L"Restore the display now",                   L"Restaurar a tela agora" },
    { L"Settings...",                               L"Configurações..." },
    { L"Exit",                                      L"Sair" },
    { L"Pause",                                     L"Pausar" },

    // Window title and tab names.
    { L"Zdisplay — settings",                       L"Zdisplay — configurações" },
    { L"Adjustments",                               L"Ajustes" },
    { L"Vision",                                    L"Visão" },
    { L"Profiles",                                  L"Perfis" },
    { L"Automation",                                L"Automação" },
    { L"System",                                    L"Sistema" },
    { L"Diagnostics",                               L"Diagnóstico" },

    // Adjustments tab: header row.
    { L"Profile",                                   L"Perfil" },
    { L"Automatic",                                 L"Automático" },
    { L"Monitor",                                   L"Monitor" },
    { L"All monitors",                              L"Todos os monitores" },
    { L"(primary)",                                 L"(principal)" },
    { L"This monitor has its own settings",         L"Ajuste próprio deste monitor" },

    // Adjustments tab: light and tone.
    { L"Light and tone",                            L"Luz e tom" },
    { L"Everything here goes through the graphics card's gamma ramp: it works "
      L"on any GPU, on any monitor, and applies inside fullscreen games too. "
      L"It does not reduce the panel's light — it only interprets color.",
      L"Tudo aqui vai pela rampa de gamma da placa de vídeo: funciona em "
      L"qualquer GPU, em qualquer monitor, e vale inclusive dentro de jogos "
      L"em tela cheia. Não reduz a luz do painel — só interpreta a cor." },
    { L"Brightness",                                L"Brilho" },
    { L"Contrast",                                  L"Contraste" },
    { L"Gamma",                                     L"Gamma" },
    { L"Temperature",                               L"Temperatura" },
    { L"Blue light filter",                         L"Bloqueio de luz azul" },

    // Adjustments tab: shadow detail, the feature this program leads with.
    { L"Shadow detail",                             L"Visão nas sombras" },
    { L"Brightens the dark without washing out the rest of the image. Shadows "
      L"raises the black floor; Clarity separates the near-black tones so the "
      L"lift does not flatten them. Together they reveal what was hidden.",
      L"Clareia o escuro sem lavar o resto da imagem. Sombras levanta o piso "
      L"do preto; Definição separa os tons quase pretos para o levante não "
      L"achatá-los. As duas juntas é que mostram o que estava escondido." },
    { L"Shadows",                                   L"Sombras" },
    { L"Clarity",                                   L"Definição" },

    // Adjustments tab: white balance.
    { L"White balance",                             L"Balanço de branco" },
    { L"A ceiling for each channel, to match two displays side by side. To warm "
      L"or cool the whole image, use Temperature — it is more predictable.",
      L"Teto de cada canal, para casar duas telas lado a lado. Para esquentar "
      L"ou esfriar a imagem inteira, use Temperatura — é mais previsível." },
    { L"Red gain",                                  L"Ganho vermelho" },
    { L"Green gain",                                L"Ganho verde" },
    { L"Blue gain",                                 L"Ganho azul" },
    { L"Reset this profile",                        L"Zerar ajustes do perfil" },
    { L"Compare (hold)",                            L"Comparar (segure)" },

    // Adjustments tab: color.
    { L"Color",                                     L"Cor" },
    { L"Saturation through the universal matrix, which gives the same result "
      L"on any PC, and vibrance through the GPU, which lifts the weak colors "
      L"without blowing out the strong ones.",
      L"Saturação pela matriz universal, que dá o mesmo resultado em qualquer "
      L"PC, e vibrance pela GPU, que realça as cores fracas sem estourar as "
      L"fortes." },
    { L"Saturation",                                L"Saturação" },
    { L"Vibrance (GPU)",                            L"Vibrance (GPU)" },
    { L"Vibrance (no GPU)",                         L"Vibrance (sem GPU)" },
    { L"Hue",                                       L"Matiz" },
    { L"Invert colors",                             L"Inverter cores" },

    // Adjustments tab: extra dimming.
    { L"Extra dimming",                             L"Escurecimento extra" },
    { L"For when the monitor at its minimum is still too bright. It is a black "
      L"layer over the screen: it works, but it washes out contrast and shows "
      L"up in screenshots.",
      L"Para quando o monitor no mínimo ainda está claro demais. É uma camada "
      L"preta por cima da tela: resolve, mas lava o contraste e aparece em "
      L"capturas de tela." },
    { L"Dim",                                       L"Escurecer" },

    // Adjustments tab: monitor hardware.
    { L"Monitor hardware",                          L"Hardware do monitor" },
    { L"Talks to the panel over the video cable and reduces light for real, "
      L"without washing out contrast. This is the right way to lower "
      L"brightness — the gamma ramp only darkens the image.",
      L"Fala com o painel pelo cabo de vídeo e reduz a luz de verdade, sem "
      L"lavar o contraste. É o caminho certo para baixar o brilho — a rampa "
      L"de gamma só escurece a imagem." },
    { L"Control the physical brightness",           L"Controlar o brilho físico" },
    { L"Control the physical brightness — no compatible monitor",
      L"Controlar o brilho físico — sem monitor compatível" },
    { L"Physical brightness",                       L"Brilho físico" },
    { L"Control the physical contrast",             L"Controlar o contraste físico" },
    { L"Control the physical contrast — unavailable",
      L"Controlar o contraste físico — indisponível" },
    { L"Physical contrast",                         L"Contraste físico" },
    { L"DDC mode (needs a restart)",                L"Modo DDC (requer reinício)" },

    // Monitor colour window.
    { L"Monitor colour (RGB gain)...",              L"Cor do monitor (ganho RGB)..." },
    { L"Monitor colour",                            L"Cor do monitor" },
    { L"A monitor applies these only while its colour preset is the user one. "
      L"Set that preset first, on the panel or in Monitor commands.",
      L"O monitor só aplica isto enquanto a predefinição de cor dele for a do "
      L"usuário. Escolha essa predefinição antes, no painel ou em Comandos do "
      L"monitor." },
    { L"The monitor is on a user colour preset, so it keeps what is "
      L"written here.",
      L"O monitor está numa predefinição de cor do usuário, então ele mantém o "
      L"que for escrito aqui." },
    { L"The monitor is on the '%s' preset and will hold its own values: switch "
      L"it to a user preset, under Monitor commands, before these sliders have "
      L"any effect.",
      L"O monitor está na predefinição '%s' e vai manter os valores dele: "
      L"mude para uma predefinição de usuário, em Comandos do monitor, antes "
      L"que estes controles tenham efeito." },
    { L"Control the monitor's RGB gain",            L"Controlar o ganho RGB do monitor" },
    { L"Control the monitor's saturation",          L"Controlar a saturação do monitor" },
    { L"Profile '%s' on %s.",                       L"Perfil '%s' em %s." },
    { L"every monitor",                             L"todos os monitores" },
    { L"  Neither RGB gain nor saturation is available here.",
      L"  Aqui não há ganho RGB nem saturação disponíveis." },
    { L"  Only saturation is available here.",
      L"  Aqui só há saturação disponível." },
    { L"  Only RGB gain is available here.",
      L"  Aqui só há ganho RGB disponível." },
    { L"Close",                                     L"Fechar" },
    { L"The panel's own colour registers, over the video cable: they cost no "
      L"tonal range and survive anything that resets the gamma ramp. Only on a "
      L"monitor that answers DDC/CI, and almost always only in its user colour "
      L"preset.",
      L"Os registradores de cor do próprio painel, pelo cabo de vídeo: não "
      L"custam faixa tonal e sobrevivem a qualquer coisa que zere a rampa de "
      L"gamma. Só em monitor que responde a DDC/CI e quase sempre só na "
      L"predefinição de cor do usuário." },
    { L"Automatic (recommended)",                   L"Automático (recomendado)" },
    { L"Slow DDC (dock/adapter)",                   L"DDC lento (dock/adaptador)" },
    { L"Never use DDC on this monitor",             L"Nunca usar DDC neste monitor" },

    // Adjustments tab: monitor commands.
    { L"Monitor commands",                          L"Comandos do monitor" },
    { L"Input",                                     L"Entrada" },
    { L"Preset",                                    L"Predefinição" },
    { L"Power",                                     L"Energia" },
    { L"Monitor commands — choose a monitor above",
      L"Comandos do monitor — escolha um monitor acima" },
    { L"Monitor commands — this one does not answer DDC/CI",
      L"Comandos do monitor — este não responde a DDC/CI" },
    { L"Monitor commands — asking the monitor...",
      L"Comandos do monitor — perguntando ao monitor..." },
    { L"Monitor commands — they act at once, outside the profile",
      L"Comandos do monitor — agem na hora, fora do perfil" },
    { L"Monitor commands — this one exposed none of them",
      L"Comandos do monitor — este não expôs nenhum deles" },
    { L"value 0x%02X",                              L"valor 0x%02X" },

    // Vision tab.
    { L"Adjust the display automatically through the day",
      L"Ajustar a tela automaticamente conforme o horário" },
    { L"The display warms on its own as the sun goes down and returns to "
      L"normal in the morning. It works on top of any profile — you do not "
      L"have to create a profile or a schedule rule for it.",
      L"A tela vai esquentando sozinha conforme o sol se põe e volta ao "
      L"normal de manhã. Funciona por cima de qualquer perfil — você não "
      L"precisa criar perfil nem regra de horário para isso." },
    { L"Display color",                             L"Cor da tela" },
    { L"The color target at each end of the day. Zdisplay walks between the "
      L"two as the hours pass, instead of switching all at once.",
      L"O alvo de cor em cada ponta do dia. O Zdisplay caminha entre os dois "
      L"conforme o horário, em vez de trocar de repente." },
    { L"Daytime temperature",                       L"Temperatura de dia" },
    { L"6500 K is neutral white. Leave it there if daytime already looks right.",
      L"6500 K é o branco neutro. Deixe assim se de dia está bom." },
    { L"Night temperature",                         L"Temperatura de noite" },
    { L"3400 K is the color of an incandescent bulb. The lower it goes, the "
      L"more orange — and the less blue reaching your eyes at night.",
      L"3400 K é a cor de lâmpada incandescente. Quanto menor, mais "
      L"alaranjado — e menos azul chegando aos olhos à noite." },
    { L"Night brightness (% of the profile)",       L"Brilho à noite (% do perfil)" },
    { L"100 leaves brightness alone. It matters as much as the color does: "
      L"what tires the eyes is the display being far brighter than the room.",
      L"100 não mexe no brilho. Vale tanto quanto a cor: o que cansa a vista "
      L"é a tela estar muito mais clara que o ambiente em volta." },
    { L"When",                                      L"Quando" },
    { L"The times that separate day from night, and how long the crossing takes.",
      L"Os horários que separam dia e noite, e quanto tempo a passagem leva." },
    { L"Night starts",                              L"Início da noite" },
    { L"Day starts",                                L"Início do dia" },
    { L"Transition length (min)",                   L"Duração da transição (min)" },
    { L"The change happens gradually around the time, half before and half "
      L"after. An hour is enough that the display is never caught changing.",
      L"A mudança acontece aos poucos em torno do horário, metade antes e "
      L"metade depois. Uma hora é o suficiente para não dar para perceber a "
      L"tela mudando." },
    { L"Accepts a clock time (22:00) or the sun: 'sunset', 'sunrise', "
      L"'sunset-30'. With no location Zdisplay uses 20:00 and 07:00; fill in "
      L"Latitude and Longitude on the Automation tab and it follows the sun.",
      L"Aceita relógio (22:00) ou o sol: 'sunset', 'sunrise', 'sunset-30'. Sem "
      L"localização, o Zdisplay usa 20:00 e 07:00; ao preencher Latitude e "
      L"Longitude na aba Automação, passa a seguir o sol." },
    { L"Eye break",                                 L"Pausa para os olhos" },
    { L"Independent of the color adjustment: it works even with the switch "
      L"on this tab turned off.",
      L"Independente do ajuste de cor: funciona mesmo com a chave desta aba "
      L"desligada." },
    { L"Reminder interval (min)",                   L"Intervalo do lembrete (min)" },
    { L"0 turns it off. Every 20 minutes Zdisplay reminds you to look at "
      L"something far away, about 6 metres, for 20 seconds. It is the one "
      L"recommendation against screen eye strain with real clinical support "
      L"behind it — more than any color adjustment has.",
      L"0 desliga. Em 20 minutos o Zdisplay avisa para você olhar 20 "
      L"segundos para algo longe, uns 6 metros. É a única recomendação "
      L"contra cansaço visual de tela com apoio clínico de verdade — "
      L"mais do que qualquer ajuste de cor tem." },
    { L"Test the reminder",                         L"Testar lembrete" },
    { L"The reminder works even with the color adjustment turned off.",
      L"O lembrete funciona mesmo com o ajuste de cor desligado." },
    { L"Preview day (5 s)",                         L"Prévia do dia (5 s)" },
    { L"Preview night (5 s)",                       L"Prévia da noite (5 s)" },
    { L"Click once; the display returns on its own after five seconds.",
      L"Clique uma vez; a tela volta sozinha após cinco segundos." },

    // Vision tab: status line.
    { L"Off — the display follows the active profile exactly.",
      L"Desligado — a tela segue exatamente o perfil ativo." },
    { L"The times are not valid. Use, for example, 20:00 and 07:00.",
      L"Os horários não são válidos. Use, por exemplo, 20:00 e 07:00." },
    { L"Temporary preview: night.",                 L"Prévia temporária: noite." },
    { L"Temporary preview: day.",                   L"Prévia temporária: dia." },
    { L"Now: day.",                                 L"Agora: dia." },
    { L"Now: night.",                               L"Agora: noite." },
    { L"Now: crossing into night (%.0f%%).",        L"Agora: passando para a noite (%.0f%%)." },
    { L"Now: crossing into day (%.0f%%).",          L"Agora: passando para o dia (%.0f%%)." },
    { L"  Target: %.0f K and %.0f%% of the profile's brightness.",
      L"  Alvo: %.0f K e %.0f%% do brilho do perfil." },
    { L"\r\nNight starts at %02d:%02d and day at %02d:%02d.",
      L"\r\nA noite começa às %02d:%02d e o dia às %02d:%02d." },
    { L"  Fixed times while the location is left empty.",
      L"  Horário fixo enquanto a localização não estiver preenchida." },

    // Profiles tab.
    { L"Global hotkey",                             L"Atalho global" },
    { L"Transition (ms)",                           L"Transição (ms)" },
    { L"Saturation engine",                         L"Motor de saturação" },
    { L"Automatic: the GPU handles vibrance and the universal matrix handles "
      L"saturation, which keeps the result the same on any machine.",
      L"Automático: a GPU cuida do vibrance e a matriz universal cuida da "
      L"saturação, o que mantém o resultado igual em qualquer máquina." },
    { L"Force the vendor GPU",                      L"Forçar GPU do fabricante" },
    { L"Force universal (any PC)",                  L"Forçar universal (qualquer PC)" },
    { L"Leave saturation alone",                    L"Não mexer em saturação" },
    { L"Make default",                              L"Tornar padrão" },
    { L"Export profiles...",                        L"Exportar perfis..." },
    { L"Import profiles...",                        L"Importar perfis..." },
    { L"This is the default profile.",              L"Este é o perfil padrão." },
    { L"Current default profile: %s",               L"Perfil padrão atual: %s" },

    // Automation tab.
    { L"Rules by application",                      L"Regras por aplicativo" },
    { L"They switch profile when the program comes to the foreground. They "
      L"take precedence over the schedule rules: while one of them matches, "
      L"the time of day is not consulted.",
      L"Trocam de perfil quando o programa vai para o primeiro plano. Têm "
      L"prioridade sobre as regras por horário: enquanto uma delas bate, "
      L"o horário não é consultado." },
    { L"Use the program in focus",                  L"Usar o programa em foco" },
    { L"The Process list shows the programs open right now. It can also be "
      L"typed into, including with '*' (cs* catches cs2 and csgo).",
      L"A lista de Processo mostra os programas abertos agora. Dá para "
      L"digitar também, inclusive com '*' (ex.: cs* pega cs2 e csgo)." },
    { L"Rules by time of day",                      L"Regras por horário" },
    { L"They apply when no application rule matches. If no range covers the "
      L"current time, the default profile takes over.",
      L"Valem quando nenhuma regra de aplicativo bate. Se nenhuma faixa "
      L"pegar o horário atual, entra o perfil padrão." },
    { L"Start and End accept a clock time (22:00) or the sun itself: "
      L"'sunset', 'sunrise', and with an offset such as 'sunset-30' or "
      L"'sunrise+45'. Sunset moves by more than two hours across the year, "
      L"so a fixed range is wrong for half the months.",
      L"Início e Fim aceitam relógio (22:00) ou o próprio sol: 'sunset', "
      L"'sunrise', e com deslocamento como 'sunset-30' ou 'sunrise+45'. O "
      L"horário do pôr do sol anda mais de duas horas ao longo do ano, "
      L"então uma faixa fixa fica errada em metade dos meses." },
    { L"Location",                                  L"Localização" },
    { L"Used only to work out sunrise and sunset. It stays on your PC, in "
      L"zdisplay.ini — Zdisplay does not reach the network.",
      L"Só é usada para calcular o nascer e o pôr do sol. Fica no seu PC, no "
      L"zdisplay.ini — o Zdisplay não acessa a rede." },
    { L"Latitude",                                  L"Latitude" },
    { L"Longitude",                                 L"Longitude" },

    // System tab: behaviour.
    { L"Behaviour",                                 L"Comportamento" },
    { L"How Zdisplay behaves at start, on exit and while it is open.",
      L"Como o Zdisplay se comporta ao ligar, ao sair e enquanto está aberto." },
    { L"Start with Windows",                        L"Iniciar com o Windows" },
    { L"Start minimised to the tray",               L"Iniciar minimizado na bandeja" },
    { L"Switch profile by the program in focus",
      L"Trocar de perfil conforme o programa em foco" },
    { L"Switch profile by the time of day",         L"Trocar de perfil conforme o horário" },
    { L"Restore the display on exit",               L"Restaurar a tela ao sair" },
    { L"Ask for confirmation when the adjustments go too dark",
      L"Confirmar quando os ajustes escurecerem demais" },
    { L"Brightness keys also reach the external monitors",
      L"Teclas de brilho também valem nos monitores externos" },
    { L"Reapply the adjustments every (s)",         L"Reaplicar ajustes a cada (s)" },

    // System tab: backends.
    { L"Backends",                                  L"Backends" },
    { L"The paths through which Zdisplay reaches the screen. Turning one off "
      L"is useful only to isolate a problem. Changes take effect at the next "
      L"start.",
      L"Os caminhos pelos quais o Zdisplay mexe na tela. Desligar um só é "
      L"útil para isolar um problema. Mudanças valem no próximo início." },
    { L"Vendor APIs (NVIDIA NVAPI / AMD ADL)",      L"APIs do fabricante (NVIDIA NVAPI / AMD ADL)" },
    { L"Universal color matrix (Magnification API)",
      L"Matriz de cor universal (Magnification API)" },
    { L"Monitor hardware over DDC/CI",              L"Hardware do monitor por DDC/CI" },
    { L"Laptop backlight (WMI)",                    L"Backlight do notebook (WMI)" },
    { L"Dimming layer",                             L"Camada de escurecimento" },
    { L"Unlock the full gamma range (admin)",       L"Liberar faixa completa de gamma (admin)" },
    { L"Restore the standard Windows range (admin)",
      L"Restaurar a faixa padrão do Windows (admin)" },
    { L"Open the configuration folder",             L"Abrir pasta de configuração" },
    { L"Factory reset...",                          L"Padrão de fábrica..." },

    // System tab: global hotkeys.
    { L"Global hotkeys",                            L"Atalhos globais" },
    { L"They work from anywhere in Windows, even with this window closed. "
      L"Click a field and press the combination you want.",
      L"Valem de qualquer lugar do Windows, mesmo com esta janela fechada. "
      L"Clique num campo e pressione a combinação que quiser." },
    { L"Brightness up",                             L"Aumentar brilho" },
    { L"Brightness down",                           L"Diminuir brilho" },
    { L"Saturation up",                             L"Aumentar saturação" },
    { L"Saturation down",                           L"Diminuir saturação" },
    { L"Pause / resume",                            L"Pausar / retomar" },
    { L"Open this window",                          L"Abrir esta janela" },
    { L"EMERGENCY: give the screen back",           L"EMERGÊNCIA: devolver tela" },
    { L"Hotkey step",                               L"Passo dos atalhos" },
    { L"Key not accepted",                          L"Tecla não aceita" },
    { L"This key cannot be stored as a hotkey. Use a letter, a number, a "
      L"function key or one of the navigation keys.",
      L"Esta tecla não pode ser guardada como atalho. Use uma letra, um "
      L"número, uma tecla de função ou uma das teclas de navegação." },
    { L"Add a modifier",                            L"Falta um modificador" },
    { L"A key on its own would be taken from every other program. Hold Ctrl, "
      L"Alt, Shift or Win — or use a function key, which works alone.",
      L"Uma tecla sozinha seria tirada de todos os outros programas. Segure "
      L"Ctrl, Alt, Shift ou Win — ou use uma tecla de função, que vale "
      L"sozinha." },
    { L"Already in use",                            L"Já está em uso" },
    { L"%s already answers to %s.",                 L"%s já responde por %s." },
    { L"the profile '%s'",                          L"o perfil '%s'" },
    { L"The field records what you press: Ctrl, Alt, Shift or Win plus one "
      L"key — a function key on its own also works. Backspace or Delete "
      L"turns that hotkey off.",
      L"O campo grava o que você pressiona: Ctrl, Alt, Shift ou Win mais uma "
      L"tecla — tecla de função sozinha também vale. Backspace ou Delete "
      L"desliga aquele atalho." },
    { L"Not registered (another program already uses the combination): %s. "
      L"Choose a different combination for those.",
      L"Não registrados (outro programa já usa a combinação): %s. Escolha "
      L"outra combinação para esses." },

    // Diagnostics tab buttons.
    { L"Open the log",                              L"Abrir o log" },
    { L"Read capabilities...",                      L"Ler capacidades..." },
    { L"Test the monitor",                          L"Testar o monitor" },
    { L"Clear the DDC quarantine",                  L"Liberar quarentena DDC" },

    // Status bar.
    { L"Zdisplay paused — the display is in its original state.",
      L"Zdisplay pausado — a tela está no estado original." },
    { L"Windows limited the effect to %.0f%% — see 'Unlock the full gamma "
      L"range' on the System tab.",
      L"O Windows limitou o efeito a %.0f%% — veja 'Liberar faixa completa "
      L"de gamma' na aba Sistema." },
    { L"Profile '%s'   ·   %d backend(s) active   ·   %d monitor(s)   ·   %s",
      L"Perfil '%s'   ·   %d backend(s) ativo(s)   ·   %d monitor(es)   ·   %s" },
    { L"automatic mode",                            L"modo automático" },
    { L"profile pinned",                            L"perfil fixado" },

    // Slider tooltips.
    { L"Brightness in software, through the gamma ramp. It works on any GPU and "
      L"inside fullscreen games too. It does not dim the panel's backlight — use "
      L"the physical brightness for that.",
      L"Brilho por software, pela rampa de gamma. Funciona em qualquer GPU e "
      L"também dentro de jogos em tela cheia. Não apaga a luz de fundo do "
      L"painel — para isso use o brilho físico." },
    { L"Contrast around mid grey. High values clip white and black, losing "
      L"detail at both ends.",
      L"Contraste em torno do cinza médio. Valores altos estouram branco e "
      L"preto, perdendo detalhe nas duas pontas." },
    { L"The response curve. Below 1 the midtones darken, above 1 they lighten. "
      L"It touches neither black nor white.",
      L"Curva de resposta. Abaixo de 1 escurece os tons médios, acima de 1 "
      L"clareia. Não mexe no preto nem no branco." },
    { L"Color temperature. 6500 K is neutral; below that the image warms, as it "
      L"does under Windows Night light.",
      L"Temperatura de cor. 6500 K é o neutro; abaixo disso a imagem esquenta, "
      L"como na Luz noturna do Windows." },
    { L"Cuts the blue band and leaves the rest. Unlike temperature, which "
      L"rebalances all three colors — here the image yellows faster, in exchange "
      L"for blocking more blue.",
      L"Corta a faixa azul mantendo o resto. Diferente da temperatura, que "
      L"reequilibra as três cores — aqui a imagem amarela mais rápido, em "
      L"troca de bloquear mais azul." },
    { L"Raises only the bottom of the curve: black gains a floor and the effect "
      L"dies out before the midtones, so highlights, strong colors and overall "
      L"contrast stay intact. This is what gaming monitors call Black eQualizer. "
      L"It applies inside fullscreen games too.",
      L"Levanta só a parte baixa da curva: o preto ganha um piso e o efeito "
      L"morre antes dos tons médios, então claros, cores fortes e contraste "
      L"geral ficam intactos. É o que os monitores de jogo chamam de Black "
      L"eQualizer. Vale também dentro de jogos em tela cheia." },
    { L"Pushes the near-black tones apart before the lift, giving back the "
      L"detail the lift would flatten. On its own it brightens nothing: raise it "
      L"together with Shadows. It is not edge sharpening — it is contrast in the "
      L"low tones, which is what makes dark detail reappear.",
      L"Afasta os tons quase pretos uns dos outros antes do levante, "
      L"devolvendo o detalhe que ele achataria. Sozinha não clareia nada: "
      L"suba junto com Sombras. Não é nitidez de contorno — é contraste nos "
      L"tons baixos, que é o que faz o detalhe escuro reaparecer." },
    { L"Saturation through the universal color matrix, which gives the same "
      L"result on any PC — including one with no dedicated GPU.",
      L"Saturação pela matriz de cor universal, que dá o mesmo resultado em "
      L"qualquer PC — inclusive sem GPU dedicada." },
    { L"GPU vibrance: it lifts weak colors without blowing out the strong ones. "
      L"Uses NVAPI on NVIDIA and ADL on AMD.",
      L"Vibrance da GPU: realça cores fracas sem estourar as fortes. Usa "
      L"NVAPI na NVIDIA e ADL na AMD." },
    { L"Rotates every color around the color wheel. It is for correcting a panel "
      L"that pulls to one side; on a normal image it just breaks the color.",
      L"Gira todas as cores no círculo cromático. Serve para corrigir um "
      L"painel puxado para um lado; em imagem normal, desconfigura." },
    { L"Darkens with a black layer over the screen. It goes below the panel's "
      L"minimum, but it washes out contrast and shows up in screenshots.",
      L"Escurece com uma camada preta por cima da tela. Vai além do mínimo do "
      L"painel, mas lava o contraste e aparece em capturas de tela." },
    { L"Ceiling for the red channel. It is for matching two displays side by "
      L"side; to warm or cool the image, use Temperature.",
      L"Teto do canal vermelho. Serve para casar duas telas lado a lado; para "
      L"esquentar ou esfriar a imagem, use Temperatura." },
    { L"Ceiling for the green channel. See Red gain.",
      L"Teto do canal verde. Ver Ganho vermelho." },
    { L"Ceiling for the blue channel. See Red gain.",
      L"Teto do canal azul. Ver Ganho vermelho." },
    { L"The monitor's physical brightness, over DDC/CI or through the laptop "
      L"backlight. It reduces light for real, without washing out contrast.",
      L"Brilho físico do monitor, por DDC/CI ou pela luz de fundo do notebook. "
      L"Reduz a luz de verdade, sem lavar o contraste." },
    { L"The panel's own contrast, over DDC/CI. It acts on the hardware, so it "
      L"applies with HDR on as well.",
      L"Contraste do próprio painel, por DDC/CI. Mexe no hardware, então vale "
      L"inclusive com HDR ligado." },

    // Adjustments tab tooltips.
    { L"A named set of adjustments. Choosing one here pins the profile by "
      L"hand and stops the automatic switching until you click Automatic.",
      L"Conjunto de ajustes com nome. Escolher aqui fixa o perfil à mão e "
      L"desliga a troca automática até você clicar em Automático." },
    { L"Hands control back to the rules: the program in focus, the time of "
      L"day or the default profile apply again, in that order.",
      L"Devolve o comando às regras: volta a valer o programa em foco, o "
      L"horário ou o perfil padrão, nessa ordem." },
    { L"Returns the display to its original state and stops reapplying. "
      L"Nothing is lost — on resume everything comes back as it was.",
      L"Devolve a tela ao estado original e para de reaplicar. Nada é "
      L"perdido — ao retomar, tudo volta como estava." },
    { L"Chooses which display the adjustments below refer to. Under 'All', "
      L"whatever you change applies to every one of them.",
      L"Escolhe a qual tela os ajustes abaixo se referem. Em 'Todos', o "
      L"que você mexer vale para o conjunto." },
    { L"Replaces each color with its opposite. Useful for reading on a light "
      L"screen and as an accessibility feature.",
      L"Troca cada cor pela sua oposta. Serve para leitura em tela clara e "
      L"como recurso de acessibilidade." },
    { L"Lets the profile drive the monitor's own brightness. While it is "
      L"unchecked, Zdisplay leaves whatever is set on the panel alone.",
      L"Deixa o perfil mandar no brilho do próprio monitor. Enquanto "
      L"desmarcado, o Zdisplay não toca no que está no painel." },
    { L"The same for the panel's contrast. Few monitors accept it, and the "
      L"factory value is usually the best one.",
      L"Mesma coisa para o contraste do painel. Poucos monitores aceitam, "
      L"e o valor de fábrica costuma ser o melhor." },
    { L"Returns EVERY slider in this profile to neutral. It changes the "
      L"profile, not just the screen — and it is saved.",
      L"Devolve TODAS as barras deste perfil ao neutro. Mexe no perfil, "
      L"não só na tela — e é gravado." },
    { L"Returns the display to its original state without touching the "
      L"profile. Use it when another program messes up the color.",
      L"Devolve a tela ao estado original sem mexer no perfil. Use quando "
      L"outro programa bagunçar a cor." },
    { L"Returns this slider to its neutral value.",
      L"Volta esta barra ao valor neutro." },
    { L"Check this for the monitor to hold its own values, independent of the "
      L"others. Unchecked, it follows the profile's common adjustment.",
      L"Marque para este monitor ter valores próprios, independentes dos "
      L"demais. Desmarcado, ele segue o ajuste comum do perfil." },
    { L"Hold it down to see the display as it was before the adjustments. "
      L"Release and the adjustment comes back.",
      L"Mantenha pressionado para ver a tela como ela era antes dos "
      L"ajustes. Ao soltar, o ajuste volta." },
    { L"Automatic uses the normal interval between commands. Slow waits "
      L"longer, which helps on unstable docks and adapters. Never use excludes "
      L"this monitor from any DDC/CI probing. Takes effect at the next start.",
      L"Automático usa o intervalo normal entre comandos. Lento espera "
      L"mais, útil em docks e adaptadores instáveis. Nunca usar exclui "
      L"este monitor de qualquer sondagem DDC/CI. Vale no próximo início." },
    { L"Switches the monitor's video input (HDMI, DisplayPort, USB-C). It is "
      L"the same as using the buttons on the panel — useful when two machines "
      L"share one display.",
      L"Troca a entrada de vídeo do monitor (HDMI, DisplayPort, USB-C). É "
      L"o mesmo que fazer pelos botões do painel — útil quando duas "
      L"máquinas dividem a mesma tela." },
    { L"The monitor's own color preset. It acts on the hardware, so it keeps "
      L"working with HDR on, where the gamma ramp does not.",
      L"Predefinição de cor do próprio monitor. Age no hardware, então "
      L"continua valendo com HDR ligado, quando a rampa de gamma não vale." },
    { L"Turns the monitor off or suspends it. Bringing it back may need the "
      L"button on the panel: not every monitor answers DDC/CI while it is "
      L"off.",
      L"Desliga ou suspende o monitor. Para religá-lo pode ser necessário "
      L"o botão do painel: nem todo monitor responde a DDC/CI enquanto "
      L"está desligado." },

    // Vision tab tooltips.
    { L"A layer that acts on top of the active profile, whichever it is. It "
      L"does not replace profiles or schedule rules: it adds to them.",
      L"Uma camada que age por cima do perfil ativo, seja ele qual for. "
      L"Não substitui perfis nem regras de horário: soma-se a eles." },
    { L"The display color during the day, in kelvin. 6500 K is the neutral "
      L"white of sRGB — leave it there if daytime already looks right.",
      L"Cor da tela durante o dia, em kelvin. 6500 K é o branco neutro do "
      L"sRGB — deixe assim se de dia já está bom." },
    { L"The display color at night. 3400 K is the color of an incandescent "
      L"bulb; the lower it goes, the less blue reaches your eyes.",
      L"Cor da tela durante a noite. 3400 K é a cor de lâmpada "
      L"incandescente; quanto menor, menos azul chega aos olhos." },
    { L"The percentage of the profile's brightness applied at night. 100 "
      L"changes nothing. A display too bright for the room tires the eyes "
      L"more than its color does.",
      L"Percentual do brilho do perfil aplicado à noite. 100 não mexe. "
      L"Cansa mais a vista a tela estar clara demais para o ambiente do "
      L"que a cor dela." },
    { L"A clock time (22:00) or the sun: 'sunset', 'sunset-30', 'sunset+45'. "
      L"Without latitude and longitude on the Automation tab, 20:00 applies.",
      L"Relógio (22:00) ou o sol: 'sunset', 'sunset-30', 'sunset+45'. Sem "
      L"latitude e longitude preenchidas na aba Automação, vale 20:00." },
    { L"A clock time (07:00) or the sun: 'sunrise', 'sunrise+45'. With no "
      L"location, 07:00 applies.",
      L"Relógio (07:00) ou o sol: 'sunrise', 'sunrise+45'. Sem localização, "
      L"vale 07:00." },
    { L"How many minutes the change takes, half before and half after the "
      L"time. An hour is enough for it to pass unnoticed.",
      L"Quantos minutos a mudança leva, metade antes e metade depois do "
      L"horário. Uma hora é o bastante para não se perceber acontecendo." },
    { L"0 turns it off. Every so many minutes a reminder appears to look at "
      L"something about 6 metres away for 20 seconds — the one recommendation "
      L"against eye strain with real clinical support behind it.",
      L"0 desliga. A cada tantos minutos aparece um aviso para olhar 20 "
      L"segundos para algo a uns 6 metros — a única recomendação contra "
      L"cansaço visual com apoio clínico de verdade." },
    { L"Shows the reminder now, without waiting for the interval. If Windows "
      L"notifications are off, it says so instead of vanishing silently.",
      L"Mostra o aviso agora, sem esperar o intervalo. Se as notificações "
      L"do Windows estiverem desligadas, ele avisa em vez de sumir calado." },
    { L"Applies the daytime color for five seconds and returns on its own. "
      L"With daytime at 6500 K there is nothing to see: 6500 K is neutral.",
      L"Aplica a cor de dia por cinco segundos e volta sozinho. Com o dia "
      L"em 6500 K não há o que ver: 6500 K já é o neutro." },
    { L"Applies the night color for five seconds and returns on its own.",
      L"Aplica a cor de noite por cinco segundos e volta sozinho." },

    // Profiles tab tooltips.
    { L"Every saved profile. The selected one is what the fields beside it "
      L"edit — selecting here applies nothing to the display.",
      L"Todos os perfis salvos. O selecionado é o que os campos ao lado "
      L"editam — selecionar aqui não aplica nada à tela." },
    { L"Renaming updates the application and schedule rules that point at "
      L"this profile, on its own.",
      L"Renomear atualiza sozinho as regras de aplicativo e de horário que "
      L"apontam para este perfil." },
    { L"The combination that activates this profile from anywhere in "
      L"Windows. Click here and press it — Ctrl+Alt+1, for instance. "
      L"Backspace turns it off.",
      L"Combinação que ativa este perfil de qualquer lugar do Windows. "
      L"Clique aqui e pressione — Ctrl+Alt+1, por exemplo. Backspace "
      L"desliga." },
    { L"How long the display takes to reach this profile. 0 switches at "
      L"once; a few hundred ms hide the jump.",
      L"Quanto tempo a tela leva para chegar neste perfil. 0 troca de "
      L"imediato; algumas centenas de ms disfarçam o salto." },
    { L"Who performs the saturation. Automatic uses the universal matrix, "
      L"which gives the same result on any machine, and leaves vibrance to "
      L"the GPU.",
      L"Quem faz a saturação. Automático usa a matriz universal, que dá o "
      L"mesmo resultado em qualquer máquina, e deixa o vibrance com a GPU." },
    { L"Creates a profile at neutral.",             L"Cria um perfil no neutro." },
    { L"Copies the selected profile, with the same values.",
      L"Copia o perfil selecionado, com os mesmos valores." },
    { L"Deletes the selected profile. The rules that pointed at it stop "
      L"having any effect until you correct them.",
      L"Apaga o perfil selecionado. As regras que apontavam para ele ficam "
      L"sem efeito até você corrigi-las." },
    { L"The profile used when no application or schedule rule matches.",
      L"O perfil usado quando nenhuma regra de aplicativo ou de horário bate." },
    { L"Writes the profiles to a text file, to carry to another PC or to "
      L"keep before experimenting.",
      L"Grava os perfis num arquivo de texto, para levar a outro PC ou "
      L"guardar antes de experimentar." },
    { L"Reads profiles from an exported file. A repeated name comes in as a "
      L"copy; nothing is overwritten.",
      L"Lê perfis de um arquivo exportado. Nome repetido entra como cópia; "
      L"nada é sobrescrito." },

    // Automation tab tooltips.
    { L"They switch profile when the program comes to the foreground. The "
      L"box on each row turns the rule on and off without deleting it.",
      L"Trocam de perfil quando o programa vai para o primeiro plano. A "
      L"caixa de cada linha liga e desliga a regra sem apagá-la." },
    { L"The executable name, without .exe. The list shows what is open right "
      L"now, and it can be typed with '*' — 'cs*' catches cs2 and csgo.",
      L"Nome do executável, sem .exe. A lista mostra o que está aberto "
      L"agora, e dá para digitar com '*' — 'cs*' pega cs2 e csgo." },
    { L"The profile applied while that program is in focus.",
      L"Perfil aplicado enquanto esse programa estiver em foco." },
    { L"Breaks the tie when two patterns catch the same program. Higher wins.",
      L"Desempata quando dois padrões pegam o mesmo programa. Maior vence." },
    { L"Turns the rule off without deleting it.",   L"Desliga a regra sem apagá-la." },
    { L"Creates a rule from what is in the fields above.",
      L"Cria uma regra com o que está nos campos acima." },
    { L"Saves the changes to the rule selected in the list.",
      L"Grava as alterações na regra selecionada na lista." },
    { L"Deletes the selected rule.",                L"Apaga a regra selecionada." },
    { L"Fills the Process field with the program in the foreground right "
      L"now — it saves you finding the executable name.",
      L"Preenche o campo Processo com o programa que está em primeiro "
      L"plano agora — poupa descobrir o nome do executável." },
    { L"They apply when no application rule matches. The box on each row "
      L"turns the range on and off without deleting it.",
      L"Valem quando nenhuma regra de aplicativo bate. A caixa de cada "
      L"linha liga e desliga a faixa sem apagá-la." },
    { L"A clock time (22:00) or the sun: 'sunset', 'sunrise', 'sunset-30', "
      L"'sunrise+45'. Sunset moves by more than two hours across the year, so "
      L"a fixed range is wrong for half the months.",
      L"Relógio (22:00) ou o sol: 'sunset', 'sunrise', 'sunset-30', "
      L"'sunrise+45'. O pôr do sol anda mais de duas horas ao longo do ano, então faixa "
      L"fixa fica errada em metade dos meses." },
    { L"Same format as Start. A range may cross midnight.",
      L"Mesmo formato do Início. A faixa pode cruzar a meia-noite." },
    { L"The profile applied inside the range.",     L"Perfil aplicado dentro da faixa." },
    { L"Breaks the tie between overlapping ranges. Higher wins.",
      L"Desempata faixas que se sobrepõem. Maior vence." },
    { L"Turns the range off without deleting it.",  L"Desliga a faixa sem apagá-la." },
    { L"Creates a range from what is in the fields above.",
      L"Cria uma faixa com o que está nos campos acima." },
    { L"Saves the changes to the range selected in the list.",
      L"Grava as alterações na faixa selecionada na lista." },
    { L"Deletes the selected range.",               L"Apaga a faixa selecionada." },
    { L"In decimal degrees, positive to the north. Berlin: 52.52. Left empty, "
      L"the rules using 'sunrise' and 'sunset' never fire — switching profile "
      L"on the clock of a place you are not in would be worse than not "
      L"switching at all.",
      L"Em graus decimais, positivo ao norte. São Paulo: -23,55. Sem "
      L"preencher, as regras com 'sunrise' e 'sunset' não entram — trocar o "
      L"perfil no horário de um lugar onde você não está seria pior que "
      L"não trocar." },
    { L"In decimal degrees, positive to the east. Berlin: 13.40.",
      L"Em graus decimais, positivo a leste. São Paulo: -46,63." },

    // System tab tooltips.
    { L"Registers Zdisplay to open at login. It applies only to this Windows "
      L"account and does not ask for administrator.",
      L"Registra o Zdisplay para abrir no login. Vale só para esta conta "
      L"do Windows e não pede administrador." },
    { L"Opens straight to the tray, without this window. The adjustments are "
      L"applied just the same.",
      L"Abre direto na bandeja, sem esta janela. Os ajustes são aplicados "
      L"do mesmo jeito." },
    { L"The master switch for the application rules. Off, Zdisplay stops "
      L"following which program is in focus.",
      L"Chave geral das regras por aplicativo. Desligada, o Zdisplay para "
      L"de acompanhar qual programa está em foco." },
    { L"The master switch for the schedule rules.",
      L"Chave geral das regras por horário." },
    { L"On exit, returns the display to its original state. Unchecked, the "
      L"last adjustment stays on the screen after closing.",
      L"Ao sair, devolve a tela ao estado original. Desmarcado, o último "
      L"ajuste continua na tela depois de fechar." },
    { L"Applies the adjustment and undoes it on its own after 15 s if you do "
      L"not confirm. It is what stops someone locking themselves out behind "
      L"a black screen.",
      L"Aplica o ajuste e desfaz sozinho em 15 s se você não confirmar. É "
      L"o que impede alguém de se trancar numa tela preta." },
    { L"How many seconds pass between one reapplication and the next. 0 "
      L"turns it off. It guards against Night light, drivers and games that "
      L"overwrite the gamma ramp.",
      L"De quantos em quantos segundos o ajuste é reaplicado. 0 desliga. "
      L"Protege contra a Luz noturna, drivers e jogos que sobrescrevem a "
      L"rampa de gamma." },
    { L"NVAPI on NVIDIA, ADL on AMD. It is the only path to the card's real "
      L"vibrance.",
      L"NVAPI na NVIDIA, ADL na AMD. É o único caminho para o vibrance de "
      L"verdade da placa." },
    { L"A color matrix applied by the Windows compositor. It gives the same "
      L"saturation and hue on any machine, with or without a dedicated card.",
      L"Matriz de cor aplicada pelo compositor do Windows. Dá saturação e "
      L"matiz iguais em qualquer máquina, com ou sem placa dedicada." },
    { L"Talks to the monitor over the video cable. It is what allows changing "
      L"the physical brightness, the contrast and the input.",
      L"Fala com o monitor pelo cabo de vídeo. É o que permite mexer no "
      L"brilho físico, no contraste e na entrada." },
    { L"The backlight of a laptop's internal panel, over WMI.",
      L"Luz de fundo do painel interno de notebooks, via WMI." },
    { L"A translucent black window over everything. It darkens below the "
      L"panel's minimum, but it washes out contrast and shows up in "
      L"screenshots.",
      L"Janela preta translúcida por cima de tudo. Escurece além do mínimo "
      L"do painel, mas lava o contraste e aparece em capturas de tela." },
    { L"Windows halves the strength of the gamma ramp. Unlocking the full "
      L"range requires administrator and writes a system key; it applies to "
      L"every program, not only Zdisplay.",
      L"O Windows corta pela metade a força da rampa de gamma. Liberar a "
      L"faixa completa exige administrador e escreve uma chave do sistema; "
      L"vale para todos os programas, não só o Zdisplay." },
    { L"Gives the gamma ramp back the standard Windows limit. Requires "
      L"administrator and applies to every program.",
      L"Devolve o limite padrão do Windows à rampa de gamma. Exige "
      L"administrador e vale para todos os programas." },
    { L"Erases everything you have configured — profiles, rules, hotkeys, "
      L"location and the options on this tab — and returns Zdisplay to its "
      L"freshly installed state. It asks for confirmation and keeps a copy of "
      L"the current file before erasing.",
      L"Apaga tudo o que você configurou — perfis, regras, atalhos, "
      L"localização e as opções desta aba — e devolve o Zdisplay ao "
      L"estado de recém-instalado. Pede confirmação e guarda uma cópia "
      L"do arquivo atual antes de apagar." },
    { L"Opens the folder holding zdisplay.ini, the log and the copy of the "
      L"display's original state.",
      L"Abre a pasta com zdisplay.ini, o log e a cópia do estado original "
      L"da tela." },
    { L"Raises the active profile's brightness, from anywhere in Windows.",
      L"Sobe o brilho do perfil ativo, de qualquer lugar do Windows." },
    { L"Lowers the active profile's brightness.",   L"Desce o brilho do perfil ativo." },
    { L"Raises the active profile's saturation.",   L"Sobe a saturação do perfil ativo." },
    { L"Lowers the active profile's saturation.",   L"Desce a saturação do perfil ativo." },
    { L"Pauses and resumes without opening this window.",
      L"Pausa e retoma sem abrir esta janela." },
    { L"Brings this window to the front.",          L"Traz esta janela para a frente." },
    { L"How far the brightness and saturation hotkeys move per press, in "
      L"percentage points.",
      L"De quanto andam os atalhos de brilho e saturação a cada toque, em "
      L"pontos percentuais." },
    { L"On a docked laptop, the brightness keys only reach the internal panel. "
      L"With this on, Zdisplay carries the same change to the external "
      L"monitors over DDC/CI. A profile that already manages the physical "
      L"brightness stays in charge, so the two do not fight.",
      L"Num notebook acoplado, as teclas de brilho só mexem no painel de "
      L"dentro. Com isto ligado, o Zdisplay leva a mesma mudança aos "
      L"monitores externos por DDC/CI. Perfil que já gerencia o brilho "
      L"físico continua mandando, para os dois não brigarem." },
    { L"Returns the display to its original state and pauses Zdisplay, from "
      L"anywhere in Windows. Clearing this field brings the default back on "
      L"its own — it is the emergency exit.",
      L"Devolve a tela ao estado original e pausa o Zdisplay, de qualquer "
      L"lugar do Windows. Se apagar este campo, o padrão volta sozinho — "
      L"é a saída de emergência." },

    // Diagnostics tab tooltips and the labels held in members.
    { L"A portrait of what Zdisplay sees: monitors, graphics card, EDID and "
      L"which backends came up. This is what is worth attaching to a problem "
      L"report.",
      L"Retrato do que o Zdisplay enxerga: monitores, placa, EDID e quais "
      L"backends subiram. É o que vale anexar num relato de problema." },
    { L"Reads everything again now, without restarting the program.",
      L"Relê tudo agora, sem reiniciar o programa." },
    { L"They act at once and do not enter the profile: switching the input or "
      L"turning the display off is a one-off action, not an adjustment worth "
      L"reapplying on every profile change.",
      L"Agem na hora e não entram no perfil: trocar a entrada ou desligar "
      L"a tela é uma ação pontual, não um ajuste que faça sentido "
      L"reaplicar a cada troca de perfil." },
    { L"The active profile, how many adjustment paths came up and how many "
      L"monitors Zdisplay sees right now.",
      L"Perfil ativo, quantos caminhos de ajuste subiram e quantos "
      L"monitores o Zdisplay enxerga agora." },
    { L"What is in effect at this moment and what comes next. It updates on "
      L"its own as the day passes.",
      L"O que está valendo neste instante e o que vem a seguir. Atualiza "
      L"sozinho conforme o dia passa." },
    { L"The default profile is the one used when no application or schedule "
      L"rule matches.",
      L"O perfil padrão é o usado quando nenhuma regra de aplicativo ou de "
      L"horário bate." },
    { L"It appears when Windows refuses a hotkey because another program has "
      L"already taken it. Empty means every one was accepted.",
      L"Aparece quando o Windows recusa um atalho porque outro programa já "
      L"o tomou. Vazio significa que todos foram aceitos." },
    { L"Copies this text to the clipboard.",
      L"Copia este texto para a área de transferência." },
    { L"Asks each monitor which DDC/CI features it declares. It is not done on "
      L"its own: there is a Windows defect in which a malformed answer — from "
      L"the generic monitor driver, of all things — brings the system down. It "
      L"is only for diagnosis; Zdisplay does not need it.",
      L"Pergunta a cada monitor quais recursos DDC/CI ele declara. Não é "
      L"feito sozinho: existe um defeito do Windows em que uma resposta "
      L"malformada — justo a de monitor genérico — derruba o sistema. Só "
      L"serve para diagnóstico; o Zdisplay não precisa dela." },
    { L"Proves the monitor really obeys: it moves the brightness one step, "
      L"reads it back, checks it and restores the value that was there. Some "
      L"panels accept the command, answer success and change nothing — only "
      L"this test separates that case from an adjustment that worked.",
      L"Prova que o monitor obedece de verdade: muda o brilho um passo, lê "
      L"de volta, confere e devolve o valor que estava. Há painel que "
      L"aceita o comando, responde sucesso e não muda nada — só este teste "
      L"separa esse caso de um ajuste que funcionou." },
    { L"Releases monitors whose capability read was blocked after a crash. It "
      L"starts no read of its own.",
      L"Libera monitores cuja leitura de capacidades ficou bloqueada "
      L"depois de uma queda. Não inicia leitura nenhuma sozinho." },
    { L"Opens zdisplay.log in the default editor. That is where the errors "
      L"and the history of the last runs live.",
      L"Abre o zdisplay.log no editor padrão. É lá que ficam os erros e o "
      L"histórico das últimas execuções." },

    // Dialogs: profiles.
    { L"Zdisplay profiles (*.ini)",                 L"Perfis do Zdisplay (*.ini)" },
    { L"At least one profile has to be kept.",      L"É preciso manter pelo menos um perfil." },
    { L"Delete the profile '%s'?",                  L"Excluir o perfil '%s'?" },
    { L"Profiles exported.",                        L"Perfis exportados." },
    { L"Could not write the file.",                 L"Não consegui gravar o arquivo." },
    { L"No valid profile in the file.",             L"Nenhum perfil válido no arquivo." },
    { L"%d profile(s) imported.",                   L"%d perfil(is) importado(s)." },
    { L"A profile name cannot contain “|”, “[” or “]” — those symbols "
      L"separate the sections of the configuration file.",
      L"O nome de um perfil não pode conter “|”, “[” ou “]” — esses "
      L"símbolos separam as seções do arquivo de configuração." },
    { L"A profile with that name already exists.",  L"Já existe um perfil com esse nome." },

    // Dialogs: rules.
    { L"Enter the process name (for example cs2, chrome).",
      L"Informe o nome do processo (ex.: cs2, chrome)." },
    { L"No program in focus has been detected yet.",
      L"Ainda não detectei nenhum programa em foco." },
    { L"Use a clock time (21:30) or the sun itself: 'sunset', 'sunrise', "
      L"'sunset-30', 'sunrise+45'.",
      L"Use o relógio (21:30) ou o próprio sol: 'sunset', 'sunrise', "
      L"'sunset-30', 'sunrise+45'." },

    // Dialogs: eye break.
    { L"Notifications are turned off in Windows, so this reminder would never "
      L"reach the screen.\n\nOpen Settings > System > Notifications to turn "
      L"them on?",
      L"As notificações estão desligadas no Windows, então este aviso não "
      L"apareceria na tela.\n\nQuer abrir Configurações > Sistema > "
      L"Notificações para ligar?" },
    { L"Zdisplay — eye break",                      L"Zdisplay — pausa para os olhos" },
    { L"Look at something about 6 metres away for 20 seconds. That relaxes "
      L"the muscle holding your near focus.",
      L"Olhe 20 segundos para algo a uns 6 metros. Isso relaxa o músculo "
      L"que mantém o foco de perto." },

    // Dialogs: system options.
    { L"The confirmation is off. If an adjustment leaves the screen too "
      L"dark, use the emergency hotkey (Ctrl+Alt+Shift+K by default) to "
      L"get the display back.",
      L"A confirmação está desligada. Se um ajuste deixar a tela escura "
      L"demais, use o atalho de emergência (por padrão Ctrl+Alt+Shift+K) "
      L"para devolver a tela." },
    { L"This monitor is about to go into low power mode.\n\n"
      L"Some monitors only come back through the button on the panel:\n"
      L"they stop answering DDC/CI while they are off.\n\n"
      L"Continue?",
      L"Este monitor vai entrar em modo de baixa energia.\n\n"
      L"Alguns monitores só voltam pelo botão do painel: eles param\n"
      L"de responder a DDC/CI enquanto estão desligados.\n\n"
      L"Continuar?" },
    { L"Zdisplay — monitor power",                  L"Zdisplay — energia do monitor" },
    { L"This writes GdiIcmGammaRange=256 into the Windows registry, "
      L"unlocking brightness and contrast well beyond the standard limit.\n\n"
      L"It needs administrator permission and only takes effect after "
      L"signing out of Windows.\n\nContinue?",
      L"Isto grava GdiIcmGammaRange=256 no registro do Windows, liberando "
      L"brilho e contraste bem além do limite padrão.\n\n"
      L"Precisa de permissão de administrador e só passa a valer depois de "
      L"reiniciar a sessão do Windows.\n\nContinuar?" },
    { L"Done. The full range takes effect after you sign out of "
      L"Windows.\n\nThis same button now undoes the change.",
      L"Pronto. A faixa completa passa a valer depois de reiniciar a "
      L"sessão do Windows.\n\nEste mesmo botão agora desfaz a alteração." },
    { L"Could not write to the registry. Close Zdisplay and run it as "
      L"administrator once to apply this option.",
      L"Não consegui gravar no registro. Feche o Zdisplay e execute-o como "
      L"administrador uma única vez para aplicar esta opção." },
    { L"Remove GdiIcmGammaRange from the registry and go back to the "
      L"standard Windows limit?\n\nIt needs administrator and only "
      L"applies after signing out.",
      L"Remover GdiIcmGammaRange do registro e voltar ao limite padrão do "
      L"Windows?\n\nPrecisa de administrador e só vale após reiniciar a "
      L"sessão." },
    { L"Could not change the registry. Run Zdisplay as administrator once.",
      L"Não consegui alterar o registro. Execute o Zdisplay como "
      L"administrador uma única vez." },
    { L"Could not register the emergency hotkey (%s): another program is "
      L"already using that combination.\n\nChoose a different one on the "
      L"System tab. In the meantime the emergency exit stays available from "
      L"the tray menu and through \"zdisplay.exe --panic\".",
      L"Não consegui registrar o atalho de emergência (%s): outro programa "
      L"já está usando essa combinação.\n\nEscolha outra na aba Sistema. "
      L"Enquanto isso, a emergência continua disponível pelo menu da "
      L"bandeja e por \"zdisplay.exe --panic\"." },

    // Dialogs: factory reset.
    { L"This erases everything you have configured: profiles, application "
      L"rules, schedule rules, hotkeys, location and the options on this "
      L"tab. Zdisplay goes back to the way it was installed.\n\n"
      L"The display is returned to its original state before anything "
      L"else.\n\n"
      L"A copy of the current file is kept in the configuration folder as "
      L"zdisplay.ini.before-reset.\n\n"
      L"The full gamma range unlocked in Windows is left alone — it is a "
      L"system option with a button of its own, which asks for "
      L"administrator.\n\n"
      L"Continue?",
      L"Isto apaga tudo o que você configurou: perfis, regras por "
      L"aplicativo, regras por horário, atalhos, localização e as "
      L"opções desta aba. O Zdisplay volta como veio instalado.\n\n"
      L"A tela é devolvida ao estado original antes de qualquer coisa.\n\n"
      L"Uma cópia do arquivo atual fica guardada na pasta de "
      L"configuração como zdisplay.ini.before-reset.\n\n"
      L"A faixa completa de gamma liberada no Windows não é mexida — "
      L"é uma opção do sistema e tem botão próprio, que pede "
      L"administrador.\n\n"
      L"Continuar?" },
    { L"Zdisplay — factory reset",                  L"Zdisplay — padrão de fábrica" },
    { L"Done. Zdisplay is as it was installed.",
      L"Pronto. O Zdisplay está como veio instalado." },

    // Dialogs: diagnostics actions.
    { L"Each monitor is about to be asked which DDC/CI features it "
      L"declares.\n\n"
      L"Warning: on some monitors whose answer is malformed, this read hits\n"
      L"a defect in Windows itself that can freeze the system. The risk is\n"
      L"Windows', not Zdisplay's, and it reaches few models — but save what\n"
      L"you have open before continuing.\n\n"
      L"This is only for diagnosis. Zdisplay works without this read.\n\n"
      L"Continue?",
      L"Vou perguntar a cada monitor quais recursos DDC/CI ele declara.\n\n"
      L"Aviso: em alguns monitores com a resposta malformada, essa leitura\n"
      L"esbarra num defeito do próprio Windows que pode travar o sistema.\n"
      L"O risco é do Windows, não do Zdisplay, e atinge poucos modelos — mas\n"
      L"salve o que estiver aberto antes de continuar.\n\n"
      L"Isto serve só para diagnóstico. O Zdisplay funciona sem essa leitura.\n\n"
      L"Continuar?" },
    { L"Zdisplay — read the monitor capabilities",
      L"Zdisplay — ler capacidades do monitor" },
    { L"Read requested. It runs in the background and may take a\n"
      L"few seconds per monitor.\n\n"
      L"Use 'Refresh' in a moment to see the result.",
      L"Leitura pedida. Ela roda em segundo plano e pode levar\n"
      L"alguns segundos por monitor.\n\n"
      L"Use 'Atualizar' daqui a pouco para ver o resultado." },
    { L"Each monitor's brightness is about to be moved one step, read\n"
      L"back to check, and restored to the value that was there.\n\n"
      L"The screen may flicker slightly during the test. It is the only\n"
      L"way to tell a monitor that obeys from one that accepts the\n"
      L"command and changes nothing.\n\n"
      L"Test now?",
      L"Vou mudar o brilho de cada monitor um passo, ler de volta para\n"
      L"conferir e devolver o valor que estava.\n\n"
      L"A tela pode piscar de leve durante o teste. É a única forma de\n"
      L"distinguir um monitor que obedece de um que aceita o comando e\n"
      L"não muda nada.\n\n"
      L"Testar agora?" },
    { L"Zdisplay — test the monitor",               L"Zdisplay — testar o monitor" },
    { L"Test requested. It runs in the background and takes a few\n"
      L"seconds per monitor.\n\n"
      L"Use 'Refresh' in a moment to see the result.",
      L"Teste pedido. Ele roda em segundo plano e leva alguns\n"
      L"segundos por monitor.\n\n"
      L"Use 'Atualizar' daqui a pouco para ver o resultado." },
    { L"This allows monitors blocked after a crash to be tried again. "
      L"Clearing it sends no commands now.\n\nClear the DDC/CI quarantine?",
      L"Isto permite que monitores bloqueados depois de uma queda sejam "
      L"testados novamente. A liberação não envia comandos agora.\n\n"
      L"Liberar a quarentena DDC/CI?" },
    { L"Quarantine cleared. No dangerous command was sent.",
      L"Quarentena liberada. Nenhum comando perigoso foi enviado." },
    { L"No monitor was in quarantine.",             L"Não havia monitor em quarentena." },
    { L"There is no log yet.",                      L"Ainda não há log." },

    // Dark-screen confirmation.
    { L"The display was returned to its original state by the emergency "
      L"hotkey.",
      L"A tela foi devolvida ao estado original pelo atalho de emergência." },
    { L"These settings leave the screen very dark.\n"
      L"If you cannot see properly, do nothing: Zdisplay undoes it on its own.",
      L"Estes ajustes deixam a tela bem escura.\n"
      L"Se você não conseguir enxergar direito, não faça nada: o Zdisplay "
      L"desfaz sozinho." },
    { L"&Keep it",                                  L"&Manter assim" },
    { L"&Undo now",                                 L"&Desfazer agora" },
    { L"Undoing in %d seconds...",                  L"Desfazendo em %d segundos..." },

    // Command line help. Split into three blocks so that rewording one
    // section does not cost the translation of the other two. The flag names
    // and their column alignment are part of the text and stay as they are.
    { L" — brightness, contrast, saturation, gamma and color temperature "
      L"control.\n\n"
      L"With no arguments it opens the program (or brings up the window of the "
      L"instance already running).\n"
      L"With arguments it sends the command to the running instance.\n\n",
      L" — controle de brilho, contraste, saturação, gamma e temperatura de "
      L"cor.\n\n"
      L"Sem argumentos, abre o programa (ou traz a janela da instância já "
      L"aberta).\n"
      L"Com argumentos, envia o comando para a instância que está rodando.\n\n" },
    { L"STARTUP\n"
      L"  --tray                 start straight in the tray, with no window\n"
      L"  --verbose              detailed log\n"
      L"  --make-icon            write assets\\zdisplay.ico and exit\n\n",
      L"ARRANQUE\n"
      L"  --tray                 inicia direto na bandeja, sem janela\n"
      L"  --verbose              log detalhado\n"
      L"  --make-icon            gera assets\\zdisplay.ico e sai\n\n" },
    { L"COMMANDS\n"
      L"  --profile \"Game\"       activate a profile\n"
      L"  --auto                 return to automatic mode\n"
      L"  --brightness 80        software brightness, 10..150\n"
      L"  --contrast 110         contrast, 0..200\n"
      L"  --saturation 130       saturation, 0..200\n"
      L"  --vibrance 50          GPU vibrance, 0..100\n"
      L"  --temperature 3400     color temperature in kelvin\n"
      L"  --gamma 1.1            gamma, 0.3..3.0\n"
      L"  --shadows 70           raise only the dark tones, 0..100\n"
      L"  --clarity 50           shadow detail, 0..100\n"
      L"  --hue 15               hue, -180..180\n"
      L"  --dim 20               dimming by overlay, 0..90\n"
      L"  --hwbrightness 60      physical brightness (DDC/CI or backlight)\n"
      L"  --toggle | --on | --off\n"
      L"  --reset                restore the display to its original state\n"
      L"  --panic                EMERGENCY: give the display back and pause\n"
      L"  --status               show the current state\n"
      L"  --diag                 list the detected backends\n"
      L"  --list                 list the profiles\n"
      L"  --show                 open the settings window\n"
      L"  --tab 5                open the window on a tab: 0 Adjustments,\n"
      L"                         1 Vision, 2 Profiles, 3 Automation,\n"
      L"                         4 System, 5 Diagnostics\n"
      L"  --quit                 close the program\n",
      L"COMANDOS\n"
      L"  --profile \"Jogo\"       ativa um perfil\n"
      L"  --auto                 volta ao modo automático\n"
      L"  --brightness 80        brilho por software, 10..150\n"
      L"  --contrast 110         contraste, 0..200\n"
      L"  --saturation 130       saturação, 0..200\n"
      L"  --vibrance 50          vibrance da GPU, 0..100\n"
      L"  --temperature 3400     temperatura de cor em kelvin\n"
      L"  --gamma 1.1            gamma, 0.3..3.0\n"
      L"  --shadows 70           levanta só os tons escuros, 0..100\n"
      L"  --clarity 50           detalhe nas sombras, 0..100\n"
      L"  --hue 15               matiz, -180..180\n"
      L"  --dim 20               escurecimento por sobreposição, 0..90\n"
      L"  --hwbrightness 60      brilho físico (DDC/CI ou backlight)\n"
      L"  --toggle | --on | --off\n"
      L"  --reset                restaura a tela ao estado original\n"
      L"  --panic                EMERGÊNCIA: devolve a tela e pausa\n"
      L"  --status               mostra o estado atual\n"
      L"  --diag                 lista os backends detectados\n"
      L"  --list                 lista os perfis\n"
      L"  --show                 abre a janela de configuração\n"
      L"  --tab 5                abre a janela numa aba: 0 Ajustes,\n"
      L"                         1 Visão, 2 Perfis, 3 Automação,\n"
      L"                         4 Sistema, 5 Diagnóstico\n"
      L"  --quit                 encerra o programa\n" },
};

Lang g_lang = Lang::En;

/// Key to translation, built the first time a language other than English asks
/// for text. A map rather than a scan of the table because window creation
/// resolves every caption at once.
///
/// Filled inside the initialiser of a function-local static, not behind a
/// separate flag. Initialising one of these is thread-safe by the language
/// rules, so two threads asking for text at the same time cannot both start
/// building the same map. Callers today are all on the interface thread, and
/// this is what keeps that from being something to remember.
const std::map<std::wstring, const wchar_t*>& Index() {
    static const std::map<std::wstring, const wchar_t*> pt = [] {
        std::map<std::wstring, const wchar_t*> m;
        for (const auto& e : kTable) m[e.en] = e.pt;
        return m;
    }();
    return pt;
}

}  // namespace

const wchar_t* LangChoiceName(LangChoice c) {
    switch (c) {
        case LangChoice::Pt: return L"pt";
        case LangChoice::En: return L"en";
        default:             return L"auto";
    }
}

LangChoice ParseLangChoice(const std::wstring& text) {
    const std::wstring s = Trim(text);
    if (IEquals(s, L"pt") || IEquals(s, L"pt-br") || IEquals(s, L"portugues") ||
        IEquals(s, L"português"))
        return LangChoice::Pt;
    if (IEquals(s, L"en") || IEquals(s, L"en-us") || IEquals(s, L"english") ||
        IEquals(s, L"ingles") || IEquals(s, L"inglês"))
        return LangChoice::En;
    // Anything unrecognised, including an empty value, means automatic. A
    // mistyped language must not leave the program with no interface text.
    return LangChoice::Auto;
}

Lang LangFromLangId(unsigned langId) {
    // LANG_PORTUGUESE is 0x16. PRIMARYLANGID keeps the low 10 bits, which is
    // what separates the language from the regional variant.
    return (langId & 0x3FFu) == 0x16u ? Lang::Pt : Lang::En;
}

Lang DetectSystemLanguage() {
    return LangFromLangId((unsigned)::GetUserDefaultUILanguage());
}

void SetLanguage(LangChoice choice) {
    switch (choice) {
        case LangChoice::Pt: g_lang = Lang::Pt; break;
        case LangChoice::En: g_lang = Lang::En; break;
        default:             g_lang = DetectSystemLanguage(); break;
    }
}

Lang CurrentLanguage() { return g_lang; }

const wchar_t* T(const wchar_t* english) {
    if (!english) return L"";
    // The primary language is the key, so it costs no lookup.
    if (g_lang == Lang::En) return english;

    const auto& index = Index();
    auto it = index.find(english);
    if (it != index.end() && it->second && it->second[0]) return it->second;

    // Untranslated: the English wording is still real text, and saying so in
    // the log is what turns a missing entry into something noticed.
    KLOG_D(L"No translation for: %s", english);
    return english;
}

size_t TranslationCount() { return sizeof(kTable) / sizeof(kTable[0]); }

void TranslationAt(size_t index, const wchar_t** english, const wchar_t** translated) {
    if (index >= TranslationCount()) return;
    if (english)    *english = kTable[index].en;
    if (translated) *translated = kTable[index].pt;
}

}  // namespace zdisplay
