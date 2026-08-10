# Política de segurança do Zdisplay

## Como relatar uma falha

**Não abra issue pública para falha de segurança.** Use o canal privado de
relato de vulnerabilidade do repositório (aba *Security* → *Report a
vulnerability*), que só você e quem mantém o projeto enxergam.

Um bom relato traz: versão do Zdisplay (`--status`), versão do Windows, o que
acontece, e como reproduzir. Se houver prova de conceito, anexe — ela não fica
pública enquanto o relato estiver aberto.

Resposta esperada em até 7 dias. Correção publicada antes da divulgação.

## O que conta como falha de segurança

O Zdisplay roda sem privilégio de administrador (`asInvoker`), na sessão do
usuário. A fronteira que ele se compromete a defender é esta:

**Conta como falha:**

- Qualquer coisa que permita a um processo de **outro usuário** da máquina ler,
  alterar ou influenciar o Zdisplay de um usuário — pelo canal de comando, pelo
  arquivo de configuração ou por qualquer outro caminho.
- Execução de código no processo do Zdisplay a partir de dado que ele lê e não
  controla: `zdisplay.ini`, `baseline.dat`, perfil importado, EDID e string de
  capacidades MCCS vindos do monitor.
- Carga de DLL a partir de diretório que não seja `System32`.
- Escrita em arquivo fora da pasta de configuração por caminho controlado por
  quem chama.

**Não conta como falha** (por desenho, não por descuido):

- Um processo do **mesmo usuário** conseguir mandar comandos pelo canal. É a
  função dele: linha de comando, Stream Deck, AutoHotkey. Quem já roda como
  você já pode fazer tudo o que você pode.
- Um processo do mesmo usuário editar o `zdisplay.ini`. Mesmo motivo.
- O Zdisplay em modo portátil ler a configuração de uma pasta onde outra pessoa
  escreve. Se a pasta do executável é gravável por terceiros, o próprio
  executável também é — não há o que defender a partir daí. Use a instalação
  normal (`%APPDATA%\Zdisplay`) em máquina compartilhada.

## Proteções em vigor

| Área | Medida |
|---|---|
| Carga de DLL | `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` no início do `wWinMain` e `LoadLibraryExW` com `LOAD_LIBRARY_SEARCH_SYSTEM32` em todo carregamento |
| Canal de comando (servidor) | Instância única com `FILE_FLAG_FIRST_PIPE_INSTANCE`, DACL explícita e protegida (só o dono e o SYSTEM), `PIPE_REJECT_REMOTE_CLIENTS`, e falha alta e visível se o nome já estiver ocupado |
| Canal de comando (cliente) | `SECURITY_SQOS_PRESENT \| SECURITY_IDENTIFICATION` (o servidor não pode personificar quem chama) e conferência do SID do dono do processo servidor antes de enviar |
| Mensagens de janela | `WM_ZDISPLAY_COMMAND` carrega cookie opaco validado em tabela, nunca ponteiro |
| Processo auxiliar | `--ddc-caps-worker` só grava em `caps-result-*.tmp` dentro da pasta de configuração |
| Dado externo | EDID confere cabeçalho e checksum; `baseline.dat` limita contagens e tamanhos antes de alocar; sem `std::sto*` nem `.at()` em nenhum ponto (o binário é `-fno-exceptions`) |
| Binário | DEP, ASLR, ASLR de alta entropia e `-fstack-protector-strong`, todos explícitos no `flags.mk` |
| Processo | Encerra em corrupção do heap e recusa imagens remotas/de baixa integridade; prefere imagens de `System32` |
| Instalador | Payload limitado a 64 MB e validado por CRC-32, SHA-256 e estrutura PE x64 antes da gravação |
| Desinstalação | Caminho absoluto e canônico, pasta final obrigatoriamente `Zdisplay`; remove somente arquivos conhecidos e preserva qualquer arquivo do usuário |
| Build | Falha se `objdump` não confirmar DEP, ASLR e ASLR de alta entropia no aplicativo e no instalador |
| Toolchain | O `build.bat` confere SHA-256 **e** assinatura Authenticode do w64devkit antes de extrair, e nunca executa o auto-extrator |

### Limitações conhecidas

- **Sem Control Flow Guard.** O GCC/MinGW não implementa CFG. Só entraria
  migrando a compilação para MSVC ou clang-cl.
- **Binário sem assinatura Authenticode.** Enquanto não houver certificado, use
  o SHA-256 publicado no release para conferir o download.

## Conferindo o que você baixou

A compilação é reproduzível: o carimbo de tempo do PE é zerado
(`-Wl,--no-insert-timestamp`) e as flags vêm todas do `flags.mk`. Compilando a
mesma etiqueta de versão com a mesma versão do w64devkit, o SHA-256 do
`zdisplay.exe` tem que bater com o publicado no release.

```powershell
Get-FileHash .\zdisplay.exe -Algorithm SHA256
```

Se não bater, não execute — e avise pelo canal privado acima.
