// Interface text in every language the program ships with.
//
// The table lives in the binary rather than in files beside it: Zdisplay is a
// single portable executable, and a translation read from disk would be one
// more input path to validate for something that never changes at run time.
//
// The key of a message is its English wording, so a call site still reads as
// the sentence it produces and the source stays in the language the repository
// is written in. Translations live only in the table.
#pragma once
#include "common.h"

namespace zdisplay {

/// Languages built into this binary.
///
/// English is first because it is the primary language of the project: the
/// repository, the code and the source wording are all English, and it is what
/// every other language falls back to.
enum class Lang { En = 0, Pt = 1, Count = 2 };

/// What the configuration asks for, which is not the same as what ends up in
/// use: `Auto` is resolved against the Windows UI language at startup.
enum class LangChoice { Auto = 0, Pt = 1, En = 2 };

const wchar_t* LangChoiceName(LangChoice c);
LangChoice ParseLangChoice(const std::wstring& text);

/// Reduces a Windows LANGID to a language this binary carries.
///
/// Only the primary language matters: pt-BR and pt-PT differ in wording this
/// program never uses, so both resolve to Portuguese. Everything else resolves
/// to English.
///
/// Pure, so the suite can check the mapping without a machine set to each locale.
Lang LangFromLangId(unsigned langId);

/// The language Windows is configured for, reduced as above.
Lang DetectSystemLanguage();

/// Resolves and installs the language.
///
/// Call before any window is built: controls read their captions when they are
/// created, so a language installed later would leave a window half translated.
void SetLanguage(LangChoice choice);
Lang CurrentLanguage();

/// Interface text in the language in use.
///
/// `english` is both the text and the key. Under English it is returned
/// unchanged, so the primary language costs no lookup at all. Under any other
/// language a missing entry also returns it unchanged, so an untranslated
/// control shows real English text rather than nothing.
const wchar_t* T(const wchar_t* english);

/// Number of entries in the translation table, and the pair at an index.
/// Exposed for the suite, which walks the whole table looking for an empty
/// side or a duplicated key.
size_t TranslationCount();
void TranslationAt(size_t index, const wchar_t** english, const wchar_t** translated);

}  // namespace zdisplay
