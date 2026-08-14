// Interface text for the installer.
//
// Same shape as the application's translation table — the English wording is
// the key — but a table of its own rather than a shared one. The installer is a
// single dependency-free executable that carries a compressed zdisplay.exe, so
// linking the application's table would add its several hundred entries, almost
// none of which the installer says, to a binary whose size is the point.
//
// There is no setting to read here: the installer runs before any
// configuration exists, so the system language is the only thing to follow.
#include "setup.h"

#include <map>

namespace setup {
namespace {

struct Entry {
    const wchar_t* en;
    const wchar_t* pt;
};

const Entry kTable[] = {
    // Window and header.
    { L"Update Zdisplay",               L"Atualizar o Zdisplay" },
    { L"Uninstall Zdisplay",            L"Desinstalar o Zdisplay" },
    { L"Brightness, color and saturation  ·  version ",
      L"Brilho, cor e saturação  ·  versão " },
    { L"INSTALL FOLDER",                L"PASTA DE INSTALAÇÃO" },
    { L"WILL BE REMOVED FROM",          L"SERÁ REMOVIDO DE" },
    { L"Change",                        L"Alterar" },
    { L"SHA-256 integrity  ·  no administrator",
      L"Integridade SHA-256  ·  sem administrador" },

    // Options.
    { L"Also delete the settings and the profiles",
      L"Apagar também as configurações e os perfis" },
    { L"Start with Windows",            L"Iniciar junto com o Windows" },
    { L"Create a desktop shortcut",     L"Criar atalho na área de trabalho" },
    { L"Open Zdisplay when finished",   L"Abrir o Zdisplay ao terminar" },
    { L"Unlock the full gamma range — asks for administrator",
      L"Liberar a faixa completa de gama — pede administrador" },

    // Buttons and progress.
    { L"Install",                       L"Instalar" },
    { L"Update",                        L"Atualizar" },
    { L"Uninstall",                     L"Desinstalar" },
    { L"Finish",                        L"Concluir" },
    { L"Close",                         L"Fechar" },
    { L"Preparing...",                  L"Preparando..." },

    // Outcome.
    { L"Zdisplay installed",            L"Zdisplay instalado" },
    { L"Zdisplay removed",              L"Zdisplay removido" },
    { L"It did not work",               L"Não deu certo" },
    { L"It is already running in the tray, next to the clock.",
      L"Ele já está rodando na bandeja, ao lado do relógio." },
    { L"Look for Zdisplay in the Start menu.",
      L"Procure por Zdisplay no menu Iniciar." },
    { L"The files and the shortcuts have been deleted.",
      L"Os arquivos e os atalhos foram apagados." },
    { L"Could not start the installation.",
      L"Não consegui iniciar a instalação." },

    // Gamma range result.
    { L"Zdisplay was installed, but the full gamma range "
      L"was not unlocked.\n\n",
      L"O Zdisplay foi instalado, mas a faixa completa de gama não foi "
      L"liberada.\n\n" },
    { L"The program works all the same; it is the shadow "
      L"adjustment that arrives weaker. It can be unlocked "
      L"later by running the installer again.",
      L"O programa funciona assim mesmo; o ajuste de sombras é que chega mais "
      L"fraco. Dá para liberar depois rodando o instalador de novo." },
    { L"Full gamma range unlocked.\n\n",  L"Faixa completa de gama liberada.\n\n" },
    { L"It only takes effect at the next Windows sign-in. "
      L"Until then the shadow adjustment stays diluted.",
      L"Ela só passa a valer no próximo login do Windows. Até lá o ajuste de "
      L"sombras continua diluído." },

    // Payload and integrity failures.
    { L"This installer was built without Zdisplay inside it.\n"
      L"Build it again with  build.bat setup.",
      L"Este instalador foi montado sem o Zdisplay dentro dele.\n"
      L"Gere-o de novo com  build.bat setup." },
    { L"This installer was built without the program inside it.",
      L"Este instalador foi montado sem o programa dentro dele." },
    { L"The embedded program is truncated.",
      L"O programa embutido está truncado." },
    { L"The embedded program is not a valid PE executable.",
      L"O programa embutido não é um executável PE válido." },
    { L"The embedded program does not match Zdisplay for Windows x64.",
      L"O programa embutido não corresponde ao Zdisplay para Windows x64." },
    { L"The embedded content is inconsistent.",
      L"O conteúdo embutido está inconsistente." },
    { L"Unknown format in the embedded content.",
      L"Formato desconhecido no conteúdo embutido." },
    { L"This Windows does not offer the decompression the installer uses.",
      L"Este Windows não oferece a descompactação usada pelo instalador." },
    { L"Failed to prepare the decompression.",
      L"Falha ao preparar a descompactação." },
    { L"Not enough memory to decompress the program.",
      L"Sem memória para descompactar o programa." },
    { L"The embedded program could not be decompressed.",
      L"O programa embutido não pôde ser descompactado." },
    { L"The embedded program failed the integrity check.",
      L"O programa embutido não passou na conferência de integridade." },
    { L"The SHA-256 of the embedded program does not match.",
      L"O SHA-256 do programa embutido nao confere." },

    // Install folder failures.
    { L"The installation path is not valid.",
      L"O caminho de instalacao nao e valido." },
    { L"Use a full local path, such as C:\\Programs\\Zdisplay.",
      L"Use uma pasta local completa, como C:\\Programas\\Zdisplay." },
    { L"Could not normalise the installation folder.",
      L"Nao consegui normalizar a pasta de instalacao." },
    { L"A file already exists with the name of the installation folder.",
      L"Ja existe um arquivo com o nome da pasta de instalacao." },
    { L"The installation folder cannot be a link or a junction.",
      L"A pasta de instalacao nao pode ser um link ou junction." },
    { L"Could not mark the installation folder: ",
      L"Nao consegui marcar a pasta de instalacao: " },
    { L"Could not finish the installation folder: ",
      L"Nao consegui concluir a pasta de instalacao: " },
    { L"Could not create the installation folder: ",
      L"Não consegui criar a pasta de instalação: " },
    { L"Found a folder where a file should be: ",
      L"Encontrei uma pasta onde deveria existir um arquivo: " },
    { L"Could not remove ",                L"Nao consegui remover " },
    { L"Could not create the file: ",      L"Não consegui criar o arquivo: " },
    { L"Failed to write: ",                L"Falha ao gravar: " },
    { L"Could not finish writing: ",       L"Não consegui concluir a gravação: " },
    { L"Could not create the uninstaller: ",
      L"Não consegui criar o desinstalador: " },
    { L"Zdisplay is still running and the file could not be replaced. "
      L"Close it from the tray and try again.",
      L"O Zdisplay continua em execução e o arquivo não pôde ser substituído. "
      L"Feche-o pela bandeja e tente de novo." },
    { L"The program was copied, but Windows refused the entry in Installed apps.",
      L"O programa foi copiado, mas o Windows recusou o registro em Aplicativos instalados." },

    // Registry and elevation.
    { L"Could not open the system key (error ",
      L"Não consegui abrir a chave do sistema (erro " },
    { L"Could not write the value (error ",
      L"Não consegui gravar o valor (erro " },
    { L"Could not ask for elevation (error ",
      L"Não consegui pedir elevação (erro " },
    { L"You declined the administrator prompt.",
      L"Você recusou o pedido de administrador." },
    { L"The elevated process could not write the value.",
      L"O processo elevado não conseguiu gravar o valor." },
    { L"The value was not stored.",        L"O valor não ficou gravado." },
    { L"error %lu",                        L"erro %lu" },

    // Steps and shortcut text.
    { L"Where to install Zdisplay",        L"Onde instalar o Zdisplay" },
    { L"Choose an installation folder.",   L"Escolha uma pasta de instalação." },
    { L"The final folder has to be named Zdisplay.",
      L"A pasta final precisa se chamar Zdisplay." },
    { L"Creating the folder...",           L"Criando a pasta..." },
    { L"Creating the shortcuts...",        L"Criando os atalhos..." },
    { L"Registering...",                   L"Registrando..." },
    { L"Could not create the Start menu shortcut.",
      L"Não consegui criar o atalho no menu Iniciar." },
    { L"Could not create the desktop shortcut.",
      L"Não consegui criar o atalho na área de trabalho." },
    { L"Windows refused the automatic startup setting.",
      L"O Windows recusou a configuração de início automático." },
    { L"Removing the shortcuts...",        L"Removendo os atalhos..." },
    { L"Removing the files...",            L"Removendo os arquivos..." },
    { L"Done.",                            L"Pronto." },
    { L"Removing the automatic startup...",
      L"Removendo o início automático..." },
    { L"Deleting the settings...",         L"Apagando as configurações..." },
    { L"Brightness, contrast, saturation and color temperature",
      L"Brilho, contraste, saturação e temperatura de cor" },
};

enum class Lang { En = 0, Pt = 1 };

Lang g_lang = Lang::En;

Lang LangFromLangId(unsigned langId) {
    // LANG_PORTUGUESE is 0x16; the low 10 bits are the language without its
    // regional variant, so every variant of Portuguese lands on Portuguese and
    // everything else on English.
    return (langId & 0x3FFu) == 0x16u ? Lang::Pt : Lang::En;
}

/// Built inside the initialiser of a function-local static, which the language
/// guarantees is thread-safe. The install runs on a worker thread while the
/// window paints on the interface thread, and both produce text, so two
/// threads reaching this at the same time is a real ordering, not a theory.
const std::map<std::wstring, const wchar_t*>& Index() {
    static const std::map<std::wstring, const wchar_t*> pt = [] {
        std::map<std::wstring, const wchar_t*> m;
        for (const auto& e : kTable) m[e.en] = e.pt;
        return m;
    }();
    return pt;
}

}  // namespace

void DetectLanguage() {
    g_lang = LangFromLangId((unsigned)::GetUserDefaultUILanguage());
}

const wchar_t* Text(const wchar_t* english) {
    if (!english) return L"";
    if (g_lang == Lang::En) return english;   // the key is the answer
    const auto& index = Index();
    auto it = index.find(english);
    if (it != index.end() && it->second && it->second[0]) return it->second;
    return english;   // untranslated still reads as real text
}

}  // namespace setup
