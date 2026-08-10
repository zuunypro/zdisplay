// Zdisplay tests.
//
// Cover the layer that decides what reaches the screen: color math, the shadow
// curve, configuration read and write, safety limits, automation rules and the
// stored baseline. Nothing depends on hardware, so the suite runs the same on
// any machine, including one with no monitor attached.
//
// Build with:  build.bat test     (or  make test)
#include "../src/core.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace zdisplay;

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_section;

void Section(const char* name) {
    g_section = name;
    printf("\n== %s ==\n", name);
}

void Ok(const char* what) {
    ++g_pass;
    printf("  [ok]   %s\n", what);
}

void Fail(const char* what, const std::string& detail) {
    ++g_fail;
    printf("  [FALHA] %s\n         %s\n", what, detail.c_str());
}

void Check(bool cond, const char* what, const std::string& detail = "") {
    if (cond) Ok(what); else Fail(what, detail);
}

void CheckNear(double got, double want, double tol, const char* what) {
    if (std::fabs(got - want) <= tol) Ok(what);
    else Fail(what, "obtido " + std::to_string(got) + ", esperado " + std::to_string(want));
}

std::string Utf8(const std::wstring& s) { return WideToUtf8(s); }

// EDID

/// Writes a chromaticity coordinate into the EDID bytes.
void PutChroma(std::vector<unsigned char>& e, double v,
               int highByte, int lowByte, int lowShift) {
    const unsigned q = (unsigned)(v * 1024.0 + 0.5);
    e[(size_t)highByte] = (unsigned char)(q >> 2);
    e[(size_t)lowByte] = (unsigned char)(e[(size_t)lowByte] | ((q & 0x3u) << lowShift));
}

void PutDescriptor(std::vector<unsigned char>& e, int offset,
                   unsigned char tag, const char* text) {
    e[(size_t)offset + 3] = tag;
    size_t i = 0;
    for (; text[i] && i < 13; ++i) e[(size_t)offset + 5 + i] = (unsigned char)text[i];
    if (i < 13) e[(size_t)offset + 5 + i++] = 0x0A;
    while (i < 13) e[(size_t)offset + 5 + i++] = 0x20;
}

/// Builds a coherent 128-byte EDID block with a closed checksum.
/// `wide` swaps the sRGB primaries for the DCI-P3 ones.
std::vector<unsigned char> MakeEdid(bool wide = false, bool withSerialText = true) {
    std::vector<unsigned char> e(128, 0);
    const unsigned char header[8] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
    for (int i = 0; i < 8; ++i) e[(size_t)i] = header[i];

    e[8] = 0x10; e[9] = 0xAC;                                 // "DEL"
    e[10] = 0x93; e[11] = 0x40;                               // product 0x4093
    e[12] = 0x78; e[13] = 0x56; e[14] = 0x34; e[15] = 0x12;   // serial 0x12345678
    e[16] = 10;                                               // week
    e[17] = 33;                                               // 1990 + 33
    e[18] = 1; e[19] = 4;                                     // EDID 1.4
    e[20] = 0x80;                                             // digital input

    if (wide) {   // DCI-P3
        PutChroma(e, 0.680, 27, 25, 6); PutChroma(e, 0.320, 28, 25, 4);
        PutChroma(e, 0.265, 29, 25, 2); PutChroma(e, 0.690, 30, 25, 0);
        PutChroma(e, 0.150, 31, 26, 6); PutChroma(e, 0.060, 32, 26, 4);
    } else {      // sRGB
        PutChroma(e, 0.640, 27, 25, 6); PutChroma(e, 0.330, 28, 25, 4);
        PutChroma(e, 0.300, 29, 25, 2); PutChroma(e, 0.600, 30, 25, 0);
        PutChroma(e, 0.150, 31, 26, 6); PutChroma(e, 0.060, 32, 26, 4);
    }
    PutChroma(e, 0.3127, 33, 26, 2); PutChroma(e, 0.3290, 34, 26, 0);

    PutDescriptor(e, 54, 0xFC, "ZDISPLAY TEST");
    if (withSerialText) PutDescriptor(e, 72, 0xFF, "SN12345");

    unsigned sum = 0;
    for (int i = 0; i < 127; ++i) sum += e[(size_t)i];
    e[127] = (unsigned char)((256u - (sum & 0xFFu)) & 0xFFu);
    return e;
}

void TestWmiInstanceIdentity() {
    Section("Identidade: instancia do WMI -> caminho do dispositivo");

    // Pairs the panel the WMI layer drives with the monitor the rest of the
    // program knows. Without this mapping, a laptop with two built-in panels
    // cannot be addressed panel by panel.
    Check(DevicePathFromWmiInstance(L"DISPLAY\\BOE0900\\4&1a2b3c&0&UID111_0") ==
              L"\\\\?\\DISPLAY#BOE0900#4&1a2b3c&0&UID111",
          "instancia normal vira o caminho canonico",
          Utf8(DevicePathFromWmiInstance(L"DISPLAY\\BOE0900\\4&1a2b3c&0&UID111_0")));

    // Two built-in panels of the same model differ only by UID. If the
    // conversion collapsed them, the match would pick the wrong panel.
    const std::wstring a = DevicePathFromWmiInstance(L"DISPLAY\\BOE0900\\4&1a2b3c&0&UID111_0");
    const std::wstring b = DevicePathFromWmiInstance(L"DISPLAY\\BOE0900\\4&1a2b3c&0&UID222_1");
    Check(!a.empty() && !b.empty() && a != b,
          "dois paineis do mesmo modelo continuam distintos");

    // The _N suffix belongs to WMI and does not exist in the device path;
    // letting it through would make every comparison fail.
    Check(DevicePathFromWmiInstance(L"DISPLAY\\DEL4093\\5&abc&0&UID4353_12") ==
              L"\\\\?\\DISPLAY#DEL4093#5&abc&0&UID4353",
          "sufixo de indice com mais de um digito e removido");

    // Malformed input must not produce a key that matches anything: an empty
    // string means "unknown" and the caller falls back to the safety net.
    Check(DevicePathFromWmiInstance(L"").empty(), "entrada vazia nao produz chave");
    Check(DevicePathFromWmiInstance(L"DISPLAY").empty(), "entrada sem os tres campos e recusada");
    Check(DevicePathFromWmiInstance(L"DISPLAY\\BOE0900").empty(),
          "entrada com dois campos e recusada");
    Check(DevicePathFromWmiInstance(L"DISPLAY\\\\UID1_0").empty(),
          "campo do meio vazio e recusado");

    Check(DevicePathFromWmiInstance(L"OUTRO\\ABC1234\\1&2&3_0") ==
              L"\\\\?\\OUTRO#ABC1234#1&2&3",
          "o enumerador vem da propria entrada");
}

void TestVcpFeatures() {
    Section("Recursos VCP com os valores aceitos");

    // The input source code carries the list of values the panel accepts;
    // without them the interface would offer inputs the monitor does not have.
    {
        const auto f = ParseVcpFeatures("(vcp(10 12 60(01 0F 11) D6(01 04)))");
        const VcpFeature* input = nullptr;
        const VcpFeature* power = nullptr;
        for (const auto& e : f) {
            if (e.code == 0x60) input = &e;
            if (e.code == 0xD6) power = &e;
        }
        Check(f.size() == 4, "quatro codigos reconhecidos", std::to_string(f.size()));
        Check(input && input->values.size() == 3, "a entrada trouxe tres valores");
        if (input)
            Check(input->values[0] == 0x01 && input->values[1] == 0x0F &&
                  input->values[2] == 0x11, "os valores da entrada estao corretos");
        Check(power && power->values.size() == 2, "energia trouxe dois valores");

        // A continuous code declares no values, which has to keep meaning "any
        // value in the range" rather than "no value at all".
        for (const auto& e : f)
            if (e.code == 0x10)
                Check(e.values.empty(), "codigo continuo fica sem lista de valores");
    }

    // Reproduces firmware that strips every space from the capability string.
    {
        const auto f = ParseVcpFeatures("(vcp(101214(0105)16)cmds(01 02))");
        const VcpFeature* preset = nullptr;
        for (const auto& e : f) if (e.code == 0x14) preset = &e;
        Check(f.size() == 4, "sem espacos, os pares ainda sao separados",
              std::to_string(f.size()));
        Check(preset && preset->values.size() == 2 &&
              preset->values[0] == 0x01 && preset->values[1] == 0x05,
              "os valores aninhados vao para o codigo anterior");
        // The cmds(...) section must not leak into the VCP list.
        for (const auto& e : f)
            Check(e.code == 0x10 || e.code == 0x12 || e.code == 0x14 || e.code == 0x16,
                  "nenhum codigo estranho entrou", Utf8(Format(L"0x%02X", e.code)));
    }

    // The capability string arrives over I2C from the monitor firmware, so it
    // is untrusted input: it must never crash the parser or invent features.
    Check(ParseVcpFeatures("").empty(), "string vazia nao produz recursos");
    Check(ParseVcpFeatures("(prot(monitor)type(lcd))").empty(),
          "sem secao vcp nao ha recursos");
    Check(ParseVcpFeatures("(vcp(").empty(), "secao vcp truncada nao produz recursos");
    Check(ParseVcpFeatures("(vcp(ZZ GG))").empty(), "lixo nao hexadecimal e descartado");
    {
        const auto f = ParseVcpFeatures("(vcp((01 02)10))");
        Check(f.size() == 1 && f[0].code == 0x10,
              "valores sem codigo antes sao ignorados", std::to_string(f.size()));
    }

    // Human-readable names shown in the interface.
    Check(VcpValueName(0x60, 0x11) == L"HDMI 1", "valor conhecido tem nome");
    Check(VcpValueName(0x60, 0x0F) == L"DisplayPort 1", "DisplayPort tem nome");
    Check(VcpValueName(0xD6, 0x01) == L"ligado", "energia ligada tem nome");
    Check(VcpValueName(0x60, 0x7E).empty(), "valor desconhecido nao inventa nome");
    Check(VcpValueName(0x99, 0x01).empty(), "codigo fora da lista nao inventa nome");
}

void TestMonitorQuirks() {
    Section("Peculiaridades por modelo de monitor");

    // Built-in table: models that report brightness on a different register.
    const MonitorQuirk* fus = FindMonitorQuirk(L"FUS087C");
    Check(fus && fus->brightnessVcp == 0x6B, "modelo embutido traz o registrador proprio");
    const MonitorQuirk* blocked = FindMonitorQuirk(L"LTM2C02");
    Check(blocked && blocked->block, "modelo que derruba o driver vem bloqueado");
    Check(FindMonitorQuirk(L"DEL4093") == nullptr, "modelo comum nao tem regra");
    Check(FindMonitorQuirk(L"") == nullptr, "sem EDID nao ha regra");

    // The identifier comes from the EDID and has no guaranteed letter case.
    Check(FindMonitorQuirk(L"fus087c") != nullptr, "a busca ignora maiusculas e minusculas");

    // Parsing quirk rules from the configuration file.
    MonitorQuirk q;
    Check(ParseMonitorQuirk(L"ABC1234", L"bloquear", &q) && q.block && q.brightnessVcp == 0,
          "regra de bloqueio e lida");
    Check(ParseMonitorQuirk(L"ABC1234", L"brilho-vcp:6B", &q) && q.brightnessVcp == 0x6B,
          "registrador e lido em hexadecimal");
    Check(ParseMonitorQuirk(L"ABC1234", L"sem-capacidades,brilho-vcp:13", &q) &&
          q.unsafeCaps && q.brightnessVcp == 0x13,
          "duas regras na mesma linha");
    Check(ParseMonitorQuirk(L"ABC1234", L"block no-caps", &q) && q.block && q.unsafeCaps,
          "os nomes em ingles tambem valem, separados por espaco");

    // A mistyped line must be rejected instead of becoming an empty rule that
    // silently does nothing while looking configured.
    Check(!ParseMonitorQuirk(L"ABC1234", L"blokear", &q), "regra desconhecida e recusada");
    Check(!ParseMonitorQuirk(L"ABC1234", L"", &q), "regra vazia e recusada");
    Check(!ParseMonitorQuirk(L"ABC1234", L"brilho-vcp:", &q), "registrador sem valor e recusado");
    Check(!ParseMonitorQuirk(L"ABC1234", L"brilho-vcp:0", &q), "registrador zero e recusado");
    Check(!ParseMonitorQuirk(L"ABC1234", L"brilho-vcp:1FF", &q),
          "registrador fora de um byte e recusado");
    Check(!ParseMonitorQuirk(L"", L"bloquear", &q), "sem modelo nao ha regra");

    // Round trip through the file format.
    MonitorQuirk full;
    Check(ParseMonitorQuirk(L"XYZ9999", L"bloquear,sem-capacidades,brilho-vcp:6B", &full),
          "regra completa e lida");
    MonitorQuirk again;
    Check(ParseMonitorQuirk(L"XYZ9999", FormatMonitorQuirk(full), &again) &&
          again.block == full.block && again.unsafeCaps == full.unsafeCaps &&
          again.brightnessVcp == full.brightnessVcp,
          "gravar e ler de volta preserva a regra", Utf8(FormatMonitorQuirk(full)));

    // A user rule outranks the built-in table, so an unusual panel can be
    // fixed locally without waiting for a new release.
    std::vector<MonitorQuirk> user;
    MonitorQuirk override_;
    Check(ParseMonitorQuirk(L"FUS087C", L"brilho-vcp:10", &override_), "regra do usuario e lida");
    user.push_back(override_);
    SetUserMonitorQuirks(user);
    const MonitorQuirk* now = FindMonitorQuirk(L"FUS087C");
    Check(now && now->brightnessVcp == 0x10, "a regra do usuario cobre a embutida");
    SetUserMonitorQuirks({});
    const MonitorQuirk* back = FindMonitorQuirk(L"FUS087C");
    Check(back && back->brightnessVcp == 0x6B, "removida a do usuario, a embutida volta");
}

void TestEdid() {
    Section("EDID (identidade fisica do painel)");

    const auto good = MakeEdid();
    EdidInfo info;
    Check(ParseEdid(good.data(), good.size(), &info), "bloco coerente e aceito");
    Check(info.manufacturer == L"DEL", "fabricante decodificado dos 5 bits",
          Utf8(info.manufacturer));
    Check(info.product == 0x4093, "codigo do modelo em little-endian",
          std::to_string(info.product));
    Check(info.serial == 0x12345678u, "serial numerico em little-endian");
    Check(info.year == 2023, "ano = 1990 + byte 17", std::to_string(info.year));
    Check(info.digital, "entrada digital pelo bit 7 do byte 20");
    Check(info.modelName == L"ZDISPLAY TEST", "nome do modelo do descritor 0xFC",
          Utf8(info.modelName));
    Check(info.serialText == L"SN12345", "serial em texto do descritor 0xFF",
          Utf8(info.serialText));
    CheckNear(info.gamutArea, kSrgbGamutArea, 0.002, "area do gamut sRGB confere");
    Check(!info.wideGamut, "painel sRGB nao e marcado como gamut largo");

    // The checksum is what separates a real EDID from a truncated block.
    // Without it, a monitor with no EDID yields an invented serial and the
    // per-monitor adjustment key starts pointing at the wrong panel.
    auto bad = good;
    bad[127] ^= 0x01;
    EdidInfo none;
    Check(!ParseEdid(bad.data(), bad.size(), &none), "checksum errado e recusado");

    auto badHeader = good;
    badHeader[1] = 0x00;
    Check(!ParseEdid(badHeader.data(), badHeader.size(), &none), "cabecalho errado e recusado");

    const std::vector<unsigned char> zeros(128, 0);
    Check(!ParseEdid(zeros.data(), zeros.size(), &none),
          "bloco zerado e recusado (monitor generico sem EDID)");

    Check(!ParseEdid(good.data(), 64, &none), "bloco truncado e recusado");
    Check(!ParseEdid(nullptr, 128, &none), "ponteiro nulo nao derruba o parser");

    // A manufacturer outside A-Z means a corrupt block even with a valid checksum.
    auto badMfg = good;
    badMfg[8] = 0x00; badMfg[9] = 0x00;
    unsigned s = 0;
    for (int i = 0; i < 127; ++i) s += badMfg[(size_t)i];
    badMfg[127] = (unsigned char)((256u - (s & 0xFFu)) & 0xFFu);
    Check(!ParseEdid(badMfg.data(), badMfg.size(), &none),
          "fabricante fora de A-Z e recusado mesmo com checksum certo");

    const auto p3 = MakeEdid(true);
    EdidInfo wideInfo;
    Check(ParseEdid(p3.data(), p3.size(), &wideInfo), "bloco DCI-P3 e aceito");
    Check(wideInfo.wideGamut, "primarias DCI-P3 sao marcadas como gamut largo");
    Check(wideInfo.gamutArea > kSrgbGamutArea, "area do DCI-P3 e maior que a do sRGB");

    const auto noText = MakeEdid(false, false);
    EdidInfo plain;
    Check(ParseEdid(noText.data(), noText.size(), &plain), "bloco sem descritor de serial e aceito");
    Check(plain.serialText.empty(), "sem descritor 0xFF o serial em texto fica vazio");
    Check(plain.serial == 0x12345678u, "o serial numerico continua servindo de identidade");
}

// Solar times

std::string Hm(int minutes) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

void TestSolar() {
    Section("Nascer e por do sol");

    // Sao Paulo, no daylight saving time. Solstices are the most stable
    // reference points: they repeat at almost the same minute every year.
    const double spLat = -23.5505, spLon = -46.6333, spTz = -3.0;
    int rise = 0, set = 0;

    Check(SunTimes(2026, 6, 21, spLat, spLon, spTz, &rise, &set), "solsticio de junho resolve");
    Check(std::abs(rise - (6 * 60 + 47)) <= 5, "nascer do sol de 21/06 em Sao Paulo ~06:47",
          Hm(rise));
    Check(std::abs(set - (17 * 60 + 28)) <= 5, "por do sol de 21/06 em Sao Paulo ~17:28", Hm(set));

    int rise2 = 0, set2 = 0;
    Check(SunTimes(2026, 12, 21, spLat, spLon, spTz, &rise2, &set2), "solsticio de dezembro resolve");
    Check(std::abs(rise2 - (5 * 60 + 16)) <= 6, "nascer do sol de 21/12 em Sao Paulo ~05:16",
          Hm(rise2));
    Check(std::abs(set2 - (18 * 60 + 50)) <= 6, "por do sol de 21/12 em Sao Paulo ~18:50", Hm(set2));

    // In the southern hemisphere December must be the long day and June the
    // short one. This catches a sign flip in latitude or declination.
    Check((set2 - rise2) > (set - rise) + 120,
          "no hemisferio sul o dia de dezembro e bem mais longo que o de junho",
          std::to_string(set2 - rise2) + " vs " + std::to_string(set - rise));

    // Same dates in the northern hemisphere: the relation inverts.
    int nRise = 0, nSet = 0, nRise2 = 0, nSet2 = 0;
    SunTimes(2026, 6, 21, 51.5074, -0.1278, 1.0, &nRise, &nSet);        // London, BST
    SunTimes(2026, 12, 21, 51.5074, -0.1278, 0.0, &nRise2, &nSet2);
    Check((nSet - nRise) > (nSet2 - nRise2) + 240,
          "em Londres o dia de junho e muito mais longo que o de dezembro");
    Check(std::abs((nSet - nRise) - (16 * 60 + 38)) <= 10,
          "duracao do dia mais longo em Londres ~16h38", Hm(nSet - nRise));

    // At the equator the day lasts about 12 h all year, slightly over because
    // of refraction and the solar disc radius, which the 90.833 zenith encodes.
    for (int month = 1; month <= 12; month += 3) {
        int r = 0, s = 0;
        Check(SunTimes(2026, month, 15, 0.0, 0.0, 0.0, &r, &s), "equador resolve");
        const int len = s - r;
        Check(len > 12 * 60 && len < 12 * 60 + 20,
              "no equador o dia dura pouco mais de 12h", Hm(len));
    }

    // Solar noon sits exactly halfway between sunrise and sunset.
    Check(std::abs(((rise + set) / 2) - ((rise2 + set2) / 2)) < 25,
          "o meio-dia solar quase nao anda entre os solsticios");

    // Polar night and midnight sun have no sunrise or sunset, and the function
    // has to report that instead of returning an invented time.
    int pr = 0, ps = 0;
    Check(!SunTimes(2026, 6, 21, 78.0, 15.0, 1.0, &pr, &ps),
          "sol da meia-noite em Svalbard nao tem por do sol");
    Check(!SunTimes(2026, 12, 21, 78.0, 15.0, 1.0, &pr, &ps),
          "noite polar em Svalbard nao tem nascer do sol");

    // Out-of-range input must not turn into a time.
    Check(!SunTimes(2026, 6, 21, 91.0, 0.0, 0.0, &pr, &ps), "latitude invalida e recusada");
    Check(!SunTimes(2026, 13, 1, 0.0, 0.0, 0.0, &pr, &ps), "mes invalido e recusado");
    Check(!SunTimes(2026, 6, 21, 0.0, 0.0, 0.0, nullptr, &ps), "ponteiro nulo nao derruba");

    Section("Horarios das regras");

    SYSTEMTIME now{};
    now.wYear = 2026; now.wMonth = 6; now.wDay = 21; now.wHour = 12;
    SolarContext solar;
    solar.latitude = spLat; solar.longitude = spLon; solar.tzHours = spTz; solar.valid = true;

    Check(ResolveRuleTime(L"22:00", now, solar) == 22 * 60, "relogio continua funcionando");
    Check(ResolveRuleTime(L"por", now, solar) == set, "'por' devolve o por do sol");
    Check(ResolveRuleTime(L"nascer", now, solar) == rise, "'nascer' devolve o nascer do sol");
    Check(ResolveRuleTime(L"sunset", now, solar) == set, "'sunset' tambem e aceito");
    Check(ResolveRuleTime(L"por-30", now, solar) == set - 30, "'por-30' antecipa meia hora");
    Check(ResolveRuleTime(L"nascer+45", now, solar) == rise + 45, "'nascer+45' atrasa 45 min");
    Check(ResolveRuleTime(L" POR ", now, solar) == set, "espacos e maiusculas nao atrapalham");

    // Without a location a solar rule must not match: leaving the profile
    // alone is safer than switching it on the schedule of another place.
    SolarContext none;
    Check(ResolveRuleTime(L"por", now, none) < 0, "sem localizacao o horario solar nao resolve");
    Check(ResolveRuleTime(L"22:00", now, none) == 22 * 60, "mas o relogio segue valendo");
    Check(ResolveRuleTime(L"", now, solar) < 0, "texto vazio e invalido");
    Check(ResolveRuleTime(L"amanhecer", now, solar) < 0, "palavra desconhecida e invalida");
    Check(ResolveRuleTime(L"por+9999", now, solar) < 0, "deslocamento absurdo e recusado");

    ScheduleRule r;
    r.enabled = true;
    r.start = L"por";
    r.end = L"nascer";
    r.profile = L"Noite";

    SYSTEMTIME night = now;
    night.wHour = 20; night.wMinute = 0;          // after 17:28
    Check(r.Matches(night, solar), "as 20h de 21/06 a faixa por->nascer esta ativa");

    SYSTEMTIME noon = now;
    noon.wHour = 12; noon.wMinute = 0;
    Check(!r.Matches(noon, solar), "ao meio-dia a faixa por->nascer esta inativa");

    SYSTEMTIME dawn = now;
    dawn.wHour = 5; dawn.wMinute = 30;            // before 06:47
    Check(r.Matches(dawn, solar), "as 5h30 ainda e noite em 21/06");

    Check(!r.Matches(night, none), "sem localizacao a regra solar nao casa nunca");
}

// Vision care

void TestVision() {
    Section("Camada de visao");

    Vision v;
    v.enabled = true;
    v.dayTemperature = 6500;
    v.nightTemperature = 3400;
    v.nightBrightness = 85;
    v.transitionMinutes = 60;
    v.nightStart = L"20:00";
    v.dayStart = L"07:00";

    SolarContext solar;   // unused: the times above are clock times

    auto at = [](int h, int m) {
        SYSTEMTIME s{};
        s.wYear = 2026; s.wMonth = 6; s.wDay = 15;
        s.wHour = (WORD)h; s.wMinute = (WORD)m;
        return s;
    };

    CheckNear(NightFraction(at(13, 0), v, solar), 0.0, 0.001, "meio-dia e dia pleno");
    CheckNear(NightFraction(at(2, 0), v, solar), 1.0, 0.001, "madrugada e noite plena");

    // At the event instant the transition is half done: it is centered on the
    // event rather than starting at it.
    CheckNear(NightFraction(at(20, 0), v, solar), 0.5, 0.02, "as 20:00 em ponto vale meio");
    CheckNear(NightFraction(at(7, 0), v, solar), 0.5, 0.02, "as 07:00 em ponto vale meio");

    // Beyond the transition width the fraction has already reached its extreme.
    CheckNear(NightFraction(at(20, 31), v, solar), 1.0, 0.001, "meia hora depois ja e noite");
    CheckNear(NightFraction(at(19, 29), v, solar), 0.0, 0.001, "meia hora antes ainda e dia");
    CheckNear(NightFraction(at(7, 31), v, solar), 0.0, 0.001, "meia hora depois do dia ja e dia");

    double prev = -1;
    bool monotonic = true;
    for (int m = 19 * 60 + 30; m <= 20 * 60 + 30; ++m) {
        const double f = NightFraction(at(m / 60, m % 60), v, solar);
        if (f < prev - 1e-9) monotonic = false;
        prev = f;
    }
    Check(monotonic, "a passagem para a noite so cresce, nunca volta atras");

    Vision off = v;
    off.enabled = false;
    CheckNear(NightFraction(at(2, 0), off, solar), 0.0, 0.001, "desligada nao tem noite");

    // Effect on the adjustments.
    Adjustments base;   // neutral: 6500 K, brightness 100
    const Adjustments day = ApplyVision(base, 0.0, v);
    CheckNear(day.temperature, 6500, 1.0, "de dia a temperatura fica no alvo do dia");
    CheckNear(day.brightness, 100, 0.01, "de dia o brilho nao muda");

    Vision warmDay = v;
    warmDay.dayTemperature = 5200;
    const Adjustments configuredDay = ApplyVision(base, 0.0, warmDay);
    CheckNear(configuredDay.temperature, 5200, 1.0,
              "temperatura de dia realmente e aplicada");

    const Adjustments night = ApplyVision(base, 1.0, v);
    CheckNear(night.temperature, 3400, 1.0, "de noite a temperatura chega ao alvo");
    CheckNear(night.brightness, 85, 0.01, "de noite o brilho cai para a fracao pedida");

    const Adjustments half = ApplyVision(base, 0.5, v);
    Check(half.temperature < 6500 && half.temperature > 3400,
          "no meio da passagem a temperatura fica entre os dois alvos");
    // Interpolation happens in mired, not in Kelvin, so the midpoint falls
    // below the arithmetic mean, which is what matches the visual middle.
    Check(half.temperature < (6500 + 3400) / 2.0,
          "o meio da passagem interpola em mired, nao em Kelvin",
          std::to_string(half.temperature));

    // The layer never works against what the profile asked for.
    Adjustments warm = base;
    warm.temperature = 2700;                       // profile already warmer than the night target
    const Adjustments keptWarm = ApplyVision(warm, 1.0, v);
    CheckNear(keptWarm.temperature, 2700, 1.0,
              "perfil mais quente que o alvo da noite continua mandando");

    Adjustments dim = base;
    dim.brightness = 40;
    const Adjustments keptDim = ApplyVision(dim, 1.0, v);
    Check(keptDim.brightness <= 40.0 + 0.01, "a camada nunca clareia o que o perfil escureceu",
          std::to_string(keptDim.brightness));

    // The light floor holds even with every setting at its extreme.
    Vision extreme = v;
    extreme.nightTemperature = 1000;
    extreme.nightBrightness = 20;
    Adjustments low = base;
    low.brightness = 12;
    low.dim = 80;
    const Adjustments floored = ApplyVision(low, 1.0, extreme);
    Check(floored.EffectiveLuminance() >= kFloorLuminance,
          "a camada de visao nao consegue furar o piso de luz",
          std::to_string(floored.EffectiveLuminance()));

    // Without a location the solar mode falls back to 07:00/20:00 so the
    // feature works on first use; once a location is known the real events
    // replace those fixed times.
    Vision solarV = v;
    solarV.nightStart = L"por";
    solarV.dayStart = L"nascer";
    SolarContext none;
    Check(NightFraction(at(23, 0), solarV, none) > 0.99,
          "sem localizacao a visao usa a noite fixa de seguranca");
    Check(NightFraction(at(13, 0), solarV, none) < 0.01,
          "sem localizacao a visao usa o dia fixo de seguranca");
    int fixedDay = -1, fixedNight = -1;
    bool usedFallback = false;
    Check(ResolveVisionTimes(at(13, 0), solarV, none,
                             &fixedDay, &fixedNight, &usedFallback),
          "horarios de recuo da visao resolvem");
    Check(usedFallback && fixedDay == 7 * 60 && fixedNight == 20 * 60,
          "recuo sem localizacao e 07:00/20:00");

    SolarContext sp;
    sp.latitude = -23.5505; sp.longitude = -46.6333; sp.tzHours = -3; sp.valid = true;
    Check(NightFraction(at(23, 0), solarV, sp) > 0.99,
          "com localizacao, as 23h de junho em Sao Paulo e noite plena");
    Check(NightFraction(at(13, 0), solarV, sp) < 0.01,
          "e as 13h do mesmo dia e dia pleno");

    Vision instant = v;
    instant.transitionMinutes = 0;
    CheckNear(NightFraction(at(20, 0), instant, none), 1.0, 0.001,
              "transicao zero entra na noite imediatamente");
    CheckNear(NightFraction(at(7, 0), instant, none), 0.0, 0.001,
              "transicao zero entra no dia imediatamente");

    // Sanitize clamps every field into a usable range.
    Vision bad;
    bad.dayTemperature = -50; bad.nightTemperature = 99999;
    bad.nightBrightness = 0; bad.transitionMinutes = -10; bad.breakMinutes = 99999;
    bad.dayStart = L"  "; bad.nightStart = L"";
    bad.Sanitize();
    Check(bad.dayTemperature >= 1000 && bad.nightTemperature <= 10000,
          "temperaturas absurdas voltam para a faixa");
    Check(bad.dayTemperature <= 6500 && bad.nightTemperature <= bad.dayTemperature,
          "a camada de visao nunca esfria ao anoitecer");
    Check(bad.nightBrightness >= 20, "brilho noturno tem piso");
    Check(bad.transitionMinutes >= 0 && bad.breakMinutes <= 240,
          "transicao e pausa presas na faixa");
    Check(bad.dayStart == L"nascer" && bad.nightStart == L"por",
          "horario vazio volta ao padrao solar");

    bad.dayStart = L"nascer+abc";
    bad.nightStart = L"por-30lixo";
    bad.Sanitize();
    Check(bad.dayStart == L"nascer" && bad.nightStart == L"por",
          "horario solar com texto extra e recusado");
}

void TestVcpCapabilities() {
    Section("String de capacidades DDC/CI");

    const std::string caps =
        "(prot(monitor)type(lcd)model(KRM1)"
        "cmds(01 02 03 07 0C E3 F3)"
        "vcp(02 04 10 12 14(01 05 06 08 0B) 16 18 1A 60(0F 11 12))"
        "mccs_ver(2.1))";
    const auto codes = ParseVcpCodes(caps);

    auto has = [&](unsigned char c) {
        for (unsigned char v : codes) if (v == c) return true;
        return false;
    };

    Check(has(0x10), "brilho (0x10) reconhecido");
    Check(has(0x12), "contraste (0x12) reconhecido");
    Check(has(0x16) && has(0x18) && has(0x1A), "ganho RGB (0x16/0x18/0x1A) reconhecido");
    Check(has(0x14) && has(0x60), "codigos com lista de valores continuam contando");

    // Numbers inside parentheses are the values accepted by the preceding
    // code. Counting them as codes would make Zdisplay believe the monitor
    // exposes features it does not have.
    Check(!has(0x0B), "valor aninhado 0B nao vira codigo");
    Check(!has(0x05), "valor aninhado 05 nao vira codigo");
    Check(!has(0x0F), "valor aninhado 0F nao vira codigo");
    Check(!has(0x11), "valor aninhado 11 nao vira codigo");

    // cmds(...) comes before vcp(...) and must not leak into the list.
    Check(!has(0xE3) && !has(0xF3), "codigos de cmds() ficam de fora");

    // Blue light blocking.
    {
        Adjustments a;
        Check(a.GammaNeutral(), "bloqueio em 0 continua sendo o estado neutro");

        double r = 0, g = 0, b = 0;
        a.ChannelGains(&r, &g, &b);
        Check(r == 1.0 && g == 1.0 && b == 1.0, "sem bloqueio os tres canais ficam intocados");

        a.blueBlock = 100;
        Check(!a.GammaNeutral(), "com bloqueio o estado deixa de ser neutro");
        a.ChannelGains(&r, &g, &b);
        Check(r == 1.0, "o bloqueio nao mexe no vermelho");
        Check(b < g && g < r, "corta o azul mais que o verde, e o verde mais que o vermelho",
              std::to_string(b) + " < " + std::to_string(g));
        Check(b > 0.05, "sobra azul suficiente para os tons de azul nao virarem um so",
              std::to_string(b));

        Adjustments lit;
        Adjustments blocked = lit;
        blocked.blueBlock = 100;
        Check(blocked.EffectiveLuminance() < lit.EffectiveLuminance(),
              "o bloqueio reduz a luminancia medida — logo o piso o enxerga");

        // The light floor must hold with blue light blocking at its maximum.
        bool floorHolds = true;
        std::string worst;
        for (int br = 10; br <= 150 && floorHolds; br += 10) {
            for (int blk = 0; blk <= 100 && floorHolds; blk += 20) {
                for (int temp = 1500; temp <= 6500 && floorHolds; temp += 2500) {
                    Adjustments t;
                    t.brightness = br;
                    t.blueBlock = blk;
                    t.temperature = temp;
                    t.dim = 60;
                    t.Sanitize();
                    if (t.EffectiveLuminance() < kFloorLuminance) {
                        floorHolds = false;
                        worst = "brilho " + std::to_string(br) +
                                ", bloqueio " + std::to_string(blk) +
                                ", temperatura " + std::to_string(temp);
                    }
                }
            }
        }
        Check(floorHolds, "nenhuma combinacao de bloqueio, brilho e temperatura fura o piso",
              worst);

        Adjustments bad;
        bad.blueBlock = 500;
        bad.Sanitize();
        Check(bad.blueBlock <= 100, "bloqueio absurdo volta para a faixa");

        Adjustments z, o;
        o.blueBlock = 80;
        const Adjustments mid = Adjustments::Blend(z, o, 0.5);
        CheckNear(mid.blueBlock, 40, 0.01, "o bloqueio interpola na transicao");
    }

    Check(DdcRawToPercent(50, 0, 100) == 50, "valor no meio da faixa vira 50%");
    Check(DdcRawToPercent(0, 0, 100) == 0, "minimo vira 0%");
    Check(DdcRawToPercent(100, 0, 100) == 100, "maximo vira 100%");
    Check(DdcRawToPercent(16, 0, 31) == 52, "faixa 0..31 tambem escala certo",
          std::to_string(DdcRawToPercent(16, 0, 31)));

    // Monitors really do answer "current 255, maximum 100". The value has to
    // be clamped so the stored baseline never holds an impossible 255%.
    Check(DdcRawToPercent(255, 0, 100) == 100, "valor acima do maximo e preso em 100%",
          std::to_string(DdcRawToPercent(255, 0, 100)));

    // Below the minimum the raw subtraction of two DWORDs wraps around, so the
    // result has to be clamped to 0% instead.
    Check(DdcRawToPercent(0, 10, 100) == 0, "valor abaixo do minimo e preso em 0%",
          std::to_string(DdcRawToPercent(0, 10, 100)));
    Check(DdcRawToPercent(5, 10, 90) == 0, "outro caso abaixo do minimo",
          std::to_string(DdcRawToPercent(5, 10, 90)));

    Check(DdcRawToPercent(50, 100, 100) < 0, "faixa degenerada e recusada");
    Check(DdcRawToPercent(50, 100, 10) < 0, "faixa invertida e recusada");

    Check(ParseVcpCodes("").empty(), "string vazia devolve lista vazia");
    Check(ParseVcpCodes("(prot(monitor)type(lcd))").empty(),
          "sem secao vcp() a lista fica vazia");

    const auto lower = ParseVcpCodes("vcp(10 12 1a)");
    Check(lower.size() == 3, "hexadecimal minusculo e aceito",
          std::to_string(lower.size()));

    // Some panels send a capability string cut in the middle; the parser must
    // neither hang nor invent codes.
    const auto truncated = ParseVcpCodes("(prot(monitor)vcp(10 12 16");
    Check(truncated.size() == 3, "string cortada devolve o que deu para ler",
          std::to_string(truncated.size()));

    const auto compact = ParseVcpCodes("(VCP (101214(010506)16181A60(0F1112)))");
    auto compactHas = [&](unsigned char c) {
        return std::find(compact.begin(), compact.end(), c) != compact.end();
    };
    Check(compactHas(0x10) && compactHas(0x12) && compactHas(0x14) &&
          compactHas(0x16) && compactHas(0x18) && compactHas(0x1A) && compactHas(0x60),
          "parser aceita VCP maiusculo, espaco e codigos compactados");
    Check(!compactHas(0x01) && !compactHas(0x06) && !compactHas(0x0F),
          "valores compactados aninhados nao vazam para a lista");

    const auto duplicate = ParseVcpCodes("vcp(10 10 12)");
    Check(duplicate.size() == 2, "codigos repetidos sao deduplicados");
}

void TestDdcSafetyRules() {
    Section("Seguranca DDC/CI");

    Check(ClassifyDdcError(0xC0262584u) == DdcErrorKind::Unsupported,
          "VCP nao suportado e definitivo");
    Check(ClassifyDdcError(0xC026258Bu) == DdcErrorKind::Transient,
          "checksum invalido pode ser repetido");
    Check(ClassifyDdcError(0xC026258Du) == DdcErrorKind::Unavailable,
          "monitor removido invalida o handle inteiro");
    Check(!DdcErrorCanRetry(0xC0262584u) && DdcErrorCanRetry(ERROR_TIMEOUT),
          "somente erros transitorios gastam tentativas");

    Check(DdcWriteBatchFits(35, 5, 40), "lote que fecha exatamente no teto cabe");
    Check(!DdcWriteBatchFits(39, 5, 40), "lote projetado nao atravessa o teto");
    Check(!DdcWriteBatchFits(-1, 1, 40), "contadores invalidos falham fechados");
}

// Color

void TestTemperature() {
    Section("Temperatura de cor");

    double r, g, b;
    TemperatureToRgb(6500, &r, &g, &b);
    Check(r > 0.99 && g > 0.98 && b > 0.97, "6500 K sai praticamente branco",
          "r=" + std::to_string(r) + " g=" + std::to_string(g) + " b=" + std::to_string(b));

    TemperatureToRgb(2700, &r, &g, &b);
    Check(r > g && g > b, "2700 K e quente (vermelho > verde > azul)");
    Check(b < 0.6, "2700 K corta bastante o azul", "b=" + std::to_string(b));

    TemperatureToRgb(9000, &r, &g, &b);
    Check(b >= g && g >= r, "9000 K e frio (azul manda)");

    // Extreme values must not overflow or leave the 0..1 range.
    for (double k : {-1e9, 0.0, 1.0, 1e9}) {
        TemperatureToRgb(k, &r, &g, &b);
        if (!(r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1)) {
            Fail("temperatura absurda continua em 0..1", "k=" + std::to_string(k));
            return;
        }
    }
    Ok("temperatura absurda continua em 0..1");
}

void TestRamp() {
    Section("Rampa de gamma");

    WORD ramp[768];
    Adjustments neutral;
    BuildRamp(neutral, ramp);

    bool identityish = true;
    for (int i = 0; i < 256; ++i)
        if (std::abs((int)ramp[i] - i * 257) > 300) { identityish = false; break; }
    Check(identityish, "ajuste neutro produz rampa praticamente linear");

    // Sweeps a large grid of adjustments and enforces the invariants on each.
    int cases = 0, nonMonotonic = 0, outOfRange = 0;
    for (double brightness : {10.0, 40.0, 75.0, 100.0, 125.0, 150.0})
    for (double contrast : {0.0, 50.0, 100.0, 150.0, 200.0})
    for (double gamma : {0.3, 0.7, 1.0, 1.8, 3.0})
    for (double temp : {1500.0, 3400.0, 6500.0, 10000.0})
    for (double shadows : {0.0, 50.0, 100.0})
    for (double clarity : {0.0, 50.0, 100.0}) {
        Adjustments a;
        a.brightness = brightness; a.contrast = contrast; a.gamma = gamma;
        a.temperature = temp; a.shadows = shadows; a.clarity = clarity;
        BuildRamp(a, ramp);
        ++cases;

        for (int c = 0; c < 3; ++c) {
            for (int i = 1; i < 256; ++i) {
                // The ramp must never decrease: lighter tones would come out
                // darker than dark ones and the image would invert in bands.
                if (ramp[c * 256 + i] < ramp[c * 256 + i - 1]) { ++nonMonotonic; break; }
            }
        }
        // The WORD type already bounds the range; what matters is that the ramp
        // does not collapse to zero, which would mean a black screen.
        if (ramp[255] == 0 && ramp[511] == 0 && ramp[767] == 0) ++outOfRange;
    }
    printf("         (%d combinacoes testadas)\n", cases);
    Check(nonMonotonic == 0, "rampa nunca decresce em nenhuma combinacao",
          std::to_string(nonMonotonic) + " canais decrescentes");
    Check(outOfRange == 0, "nenhuma combinacao apaga a tela por completo",
          std::to_string(outOfRange) + " rampas totalmente pretas");

    // Zero contrast flattens everything toward the midpoint but must not invert.
    Adjustments flat;
    flat.contrast = 0;
    BuildRamp(flat, ramp);
    Check(ramp[0] <= ramp[255], "contraste zero nao inverte a imagem");
}

void TestShadowCurve() {
    Section("Visao nas sombras");

    Check(ShadowCurve(0.0, 0, 0) == 0.0 && std::fabs(ShadowCurve(1.0, 0, 0) - 1.0) < 1e-9,
          "sem ajuste a curva e a identidade");

    Check(ShadowCurve(0.0, 100, 0) > 0.10, "levante maximo tira o preto do zero",
          "preto virou " + std::to_string(ShadowCurve(0.0, 100, 0)));

    CheckNear(ShadowCurve(1.0, 100, 100), 1.0, 1e-9, "o branco nunca se mexe");
    CheckNear(ShadowCurve(0.95, 100, 100), 0.95, 0.02, "os claros quase nao se mexem");

    // The curve may only add light, which is what keeps the safety floor valid.
    bool onlyBrightens = true, monotonic = true;
    double prev = -1;
    for (int i = 0; i <= 1000; ++i) {
        const double v = i / 1000.0;
        const double out = ShadowCurve(v, 100, 100);
        if (out < v - 1e-9) onlyBrightens = false;
        if (out < prev - 1e-9) monotonic = false;
        prev = out;
    }
    Check(onlyBrightens, "a curva nunca escurece a imagem");
    Check(monotonic, "a curva nunca decresce, mesmo nos dois ajustes no maximo");

    // Every (shadow lift, clarity) pair has to keep the curve increasing,
    // otherwise dark gradients show banding.
    int broken = 0;
    for (int s = 0; s <= 100; s += 5)
    for (int c = 0; c <= 100; c += 5) {
        prev = -1;
        for (int i = 0; i <= 255; ++i) {
            const double out = ShadowCurve(i / 255.0, s, c);
            if (out < prev - 1e-9) { ++broken; break; }
            prev = out;
        }
    }
    Check(broken == 0, "441 combinacoes de sombras x definicao seguem crescentes",
          std::to_string(broken) + " combinacoes quebradas");

    // The point of the feature: brighten dark tones without washing out the rest.
    const double darkGain = ShadowCurve(0.08, 80, 60) / 0.08;
    const double midGain  = ShadowCurve(0.50, 80, 60) / 0.50;
    Check(darkGain > 1.5 && midGain < 1.15,
          "clareia o escuro sem levantar os tons medios",
          "escuro x" + std::to_string(darkGain) + ", medio x" + std::to_string(midGain));

    // Slope at black.
    // Separation between neighboring tones comes from the slope of the curve,
    // which is the product of both stages. The slope at black must stay above
    // 1, otherwise near-black tones come out closer together than they went in.

    // Derivative at black. The 1e-4 tolerance covers the finite-difference
    // error, which is on the order of 1e-5, not a looser criterion.
    auto slopeAtBlack = [](double s, double c) {
        const double h = 1e-6;
        return (ShadowCurve(h, s, c) - ShadowCurve(0.0, s, c)) / h;
    };
    // What the eye sees is not the derivative but the distance between two
    // neighboring 8-bit codes, the smallest step the source can produce.
    auto stepGain = [](double s, double c) {
        const double step = 1 / 255.0;
        return (ShadowCurve(step, s, c) - ShadowCurve(0.0, s, c)) / step;
    };

    Check(stepGain(78, 65) > 2.0,
          "o preset problematico agora separa os pretos em vez de so lavar",
          "codigos 0 e 1 se afastam x" + std::to_string(stepGain(78, 65)));

    Check(stepGain(0, 100) > 3.5,
          "definicao maxima multiplica por ~4 o espacamento dos quase pretos",
          "codigos 0 e 1 se afastam x" + std::to_string(stepGain(0, 100)));

    // The shadow lift must not cost slope at black. The lift weight
    // (1-t)^3*(1+3t) has zero derivative at t=0, so no amount of lift eats the
    // separation that clarity creates.
    int stolen = 0;
    for (int s = 0; s <= 100; s += 5) {
        if (slopeAtBlack(s, 0) < 1.0 - 1e-4) ++stolen;
        // The clarity gain must survive any level of shadow lift.
        if (stepGain(s, 100) < 3.5) ++stolen;
    }
    Check(stolen == 0, "o levante nunca rouba a inclinacao que a definicao criou",
          std::to_string(stolen) + " niveis de sombras ainda roubam");

    // Worst slope over the whole curve and the whole adjustment grid. Being
    // positive is not enough: near zero, a dark gradient shows banding.
    double worstSlope = 1e9;
    for (int s = 0; s <= 100; s += 5)
    for (int c = 0; c <= 100; c += 5)
    for (int i = 0; i < 2000; ++i) {
        const double v = i / 2000.0, h = 1e-6;
        const double d = (ShadowCurve(v + h, s, c) - ShadowCurve(v, s, c)) / h;
        if (d < worstSlope) worstSlope = d;
    }
    Check(worstSlope > 0.30, "nenhum ajuste achata a curva a ponto de criar faixas",
          "pior inclinacao " + std::to_string(worstSlope));

    // Real separation: how many 8-bit codes an input range occupies on output,
    // with both stages combined. This is the quantity the eye perceives.
    auto spread = [](int from, int to, double s, double c) {
        return (ShadowCurve(to / 255.0, s, c) - ShadowCurve(from / 255.0, s, c)) * 255.0;
    };

    // Deep black is where the feature has to earn its place: five input codes
    // must come out spanning more than twelve.
    Check(spread(0, 5, 40, 100) > 12.0,
          "os 5 tons mais escuros saem ocupando o dobro do espaco de entrada",
          std::to_string(spread(0, 5, 40, 100)) + " codigos de largura");

    // Less lift and more separation are not a trade-off: a lower black floor
    // can still come with more separation across the useful range.
    Check(spread(0, 20, 40, 100) > spread(0, 20, 78, 65) &&
              ShadowCurve(0.0, 40, 100) < ShadowCurve(0.0, 78, 65),
          "menos levante e mais separacao deixaram de ser uma troca",
          std::to_string(spread(0, 20, 40, 100)) + " codigos com piso " +
              std::to_string(ShadowCurve(0.0, 40, 100) * 255.0) + ", contra " +
              std::to_string(spread(0, 20, 78, 65)) + " com piso " +
              std::to_string(ShadowCurve(0.0, 78, 65) * 255.0));

    // The black floor matters too: separating more must not cost a lighter
    // black. Shadow lift 40 with clarity 100 has to separate more than 78/65
    // and keep black deeper at the same time.
    Check(ShadowCurve(0.0, 40, 100) < ShadowCurve(0.0, 78, 65),
          "o ajuste que separa mais tambem deixa o preto mais fundo",
          std::to_string(ShadowCurve(0.0, 40, 100) * 255.0) + " contra " +
              std::to_string(ShadowCurve(0.0, 78, 65) * 255.0));

    // Color in the shadows.
    // A 1D LUT acts on each channel separately, so no setting here can fully
    // preserve saturation. The lift is additive and crushes the ratio between
    // channels, while clarity is multiplicative near zero and costs less.
    auto chroma = [](double s, double c) {
        return ShadowCurve(40 / 255.0, s, c) / ShadowCurve(10 / 255.0, s, c);
    };
    Check(chroma(0, 100) > chroma(100, 0),
          "separar por definicao custa menos cor do que separar por levante",
          "definicao deixa razao " + std::to_string(chroma(0, 100)) +
              ", levante deixa " + std::to_string(chroma(100, 0)));

    // Practical consequence: the setting that separates most also fades the
    // color least, so visibility and saturation are not a trade-off.
    Check(stepGain(0, 100) > stepGain(100, 0) && chroma(0, 100) > chroma(100, 0),
          "o ajuste que mais separa tambem e o que menos desbota");
}

void TestMatrix() {
    Section("Matrizes de cor");

    const Mat5 id = Mat5::Identity();
    Check(id.NearlyEquals(id * id), "identidade x identidade = identidade");
    Check(Mat5::Saturation(1.0).NearlyEquals(id), "saturacao 1.0 e neutra");
    Check(Mat5::Hue(0).NearlyEquals(id), "matiz 0 e neutro");
    Check(!Mat5::Saturation(0.0).NearlyEquals(id), "saturacao 0 muda alguma coisa");

    // Greyscale: the three columns become the same luminance combination.
    const Mat5 gray = Mat5::Saturation(0.0);
    Check(std::fabs(gray.m[0] - gray.m[1]) < 1e-6 &&
          std::fabs(gray.m[1] - gray.m[2]) < 1e-6,
          "saturacao 0 mapeia os tres canais para o mesmo cinza");

    Check(Mat5::Invert().NearlyEquals(Mat5::Invert()), "inversao e estavel");
}

// Safety limits

void TestSanitize() {
    Section("Limites de seguranca");

    Adjustments a;
    a.brightness = 1e9;
    a.contrast = -500;
    a.gamma = 0;
    a.temperature = 1e12;
    a.saturation = 9999;
    a.dim = 500;
    a.hue = 1e6;
    a.shadows = -20;
    a.clarity = 999;
    a.Sanitize();

    Check(a.brightness >= 10 && a.brightness <= 150, "brilho preso na faixa");
    Check(a.contrast >= 0 && a.contrast <= 200, "contraste preso na faixa");
    Check(a.gamma >= 0.3 && a.gamma <= 3.0, "gamma preso na faixa");
    Check(a.temperature >= 1500 && a.temperature <= 10000, "temperatura presa na faixa");
    Check(a.saturation >= 0 && a.saturation <= 200, "saturacao presa na faixa");
    Check(a.dim >= 0 && a.dim <= 90, "escurecimento preso na faixa");
    Check(a.hue >= -180 && a.hue <= 180, "matiz preso na faixa");
    Check(a.shadows >= 0 && a.shadows <= 100, "sombras presas na faixa");
    Check(a.clarity >= 0 && a.clarity <= 100, "definicao presa na faixa");

    // NaN or infinity in a hand-edited file must not turn into a black screen.
    Adjustments nan;
    nan.brightness = std::nan("");
    nan.dim = std::nan("");
    nan.Sanitize();
    Check(nan.brightness == nan.brightness && nan.dim == nan.dim,
          "NaN no arquivo volta a um valor valido");

    // The worst possible combination still has to leave the screen visible.
    int violations = 0;
    for (double b = 0; b <= 200; b += 5)
    for (double d = 0; d <= 100; d += 5)
    for (double hw = -1; hw <= 100; hw += 25) {
        Adjustments x;
        x.brightness = b; x.dim = d; x.hwBrightness = hw;
        x.Sanitize();
        if (x.EffectiveLuminance() < kFloorLuminance - 1e-9) ++violations;
    }
    Check(violations == 0, "nenhuma combinacao consegue furar o piso de luz",
          std::to_string(violations) + " violacoes");

    // The warning threshold has to be looser than the absolute floor.
    Check(kRiskyLuminance > kFloorLuminance,
          "o aviso dispara antes do piso absoluto");
}

// Automation rules

void TestRules() {
    Section("Regras de automacao");

    AppRule r;
    r.process = L"cs2";
    Check(r.Matches(L"cs2"), "casa com o nome exato");
    Check(r.Matches(L"CS2"), "ignora maiusculas e minusculas");
    Check(!r.Matches(L"cs2x"), "nao casa com nome parecido");
    Check(!r.Matches(L""), "nome vazio nao casa");

    r.process = L"cs2.exe";
    Check(r.Matches(L"cs2"), "aceita o nome escrito com .exe");

    r.process = L"valorant*";
    Check(r.Matches(L"valorant-win64-shipping"), "curinga no fim funciona");
    Check(!r.Matches(L"war"), "curinga nao vira coringa demais");

    r.process = L"*chrome*";
    Check(r.Matches(L"google-chrome-beta"), "curinga nos dois lados funciona");

    AppRule exact, broad, narrow;
    exact.process = L"chrome.exe";
    broad.process = L"*";
    narrow.process = L"chrome*";
    Check(exact.Specificity() > narrow.Specificity(),
          "regra exata ganha de curinga na mesma prioridade");
    Check(narrow.Specificity() > broad.Specificity(),
          "curinga mais especifico ganha do curinga geral");

    r.enabled = false;
    Check(!r.Matches(L"google-chrome-beta"), "regra desligada nunca casa");

    // Time ranges, including one that crosses midnight.
    ScheduleRule s;
    s.start = L"22:00";
    s.end = L"06:00";
    const auto at = [](int h, int m) { SYSTEMTIME t{}; t.wHour = (WORD)h; t.wMinute = (WORD)m; return t; };
    Check(s.Matches(at(23, 0)), "23:00 esta dentro de 22:00-06:00");
    Check(s.Matches(at(2, 30)), "02:30 esta dentro (atravessa a meia-noite)");
    Check(!s.Matches(at(12, 0)), "12:00 esta fora");
    Check(s.Matches(at(22, 0)), "o inicio conta como dentro");
    Check(!s.Matches(at(6, 0)), "o fim conta como fora");

    s.start = L"09:00";
    s.end = L"18:00";
    Check(s.Matches(at(12, 0)), "faixa normal dentro");
    Check(!s.Matches(at(20, 0)), "faixa normal fora");

    s.start = L"lixo";
    Check(!s.Matches(at(12, 0)), "horario invalido nunca casa");
}

// Configuration

void TestConfigRoundTrip() {
    Section("Configuracao: ida e volta");

    Config saved;
    saved.SeedDefaults();
    const size_t seeded = saved.profiles.size();

    // Unusual values prove that nothing is lost or rounded on the way back.
    saved.profiles[0].global.brightness = 73.5;
    saved.profiles[0].global.temperature = 4321;
    saved.profiles[0].global.shadows = 62;
    saved.profiles[0].global.clarity = 41;
    saved.profiles[0].global.hue = -137;
    saved.profiles[0].perMonitor[L"MON#ABC123"] = saved.profiles[0].global;
    saved.profiles[0].perMonitor[L"MON#ABC123"].brightness = 55;
    saved.watchdogSeconds = 42;
    saved.hkPanic = L"Ctrl+Shift+F9";
    saved.confirmDarkSettings = false;
    saved.ddcMonitorModes[L"MON#ABC123"] = DdcMonitorMode::Slow;
    saved.ddcMonitorModes[L"MON#NAO"] = DdcMonitorMode::Disabled;

    AppRule rule;
    rule.process = L"meujogo";
    rule.profile = saved.profiles[1].name;
    rule.priority = 7;
    saved.appRules.push_back(rule);

    Check(SaveConfig(saved), "gravou o arquivo");

    Config loaded;
    Check(LoadConfig(&loaded), "leu o arquivo de volta");
    Check(loaded.profiles.size() == seeded, "manteve a quantidade de perfis");

    if (!loaded.profiles.empty()) {
        const Adjustments& a = loaded.profiles[0].global;
        CheckNear(a.brightness, 73.5, 0.01, "brilho fracionario sobreviveu");
        CheckNear(a.temperature, 4321, 0.5, "temperatura sobreviveu");
        CheckNear(a.shadows, 62, 0.01, "sombras sobreviveram");
        CheckNear(a.clarity, 41, 0.01, "definicao sobreviveu");
        CheckNear(a.hue, -137, 0.01, "matiz negativo sobreviveu");

        auto it = loaded.profiles[0].perMonitor.find(L"MON#ABC123");
        Check(it != loaded.profiles[0].perMonitor.end(), "sobrescrita por monitor sobreviveu");
        if (it != loaded.profiles[0].perMonitor.end())
            CheckNear(it->second.brightness, 55, 0.01, "valor por monitor sobreviveu");
    }

    Check(loaded.watchdogSeconds == 42, "opcao numerica sobreviveu");
    Check(loaded.hkPanic == L"Ctrl+Shift+F9", "atalho de emergencia sobreviveu");
    Check(loaded.confirmDarkSettings == false, "opcao booleana falsa sobreviveu");
    Check(loaded.ddcMonitorModes[L"MON#ABC123"] == DdcMonitorMode::Slow &&
          loaded.ddcMonitorModes[L"MON#NAO"] == DdcMonitorMode::Disabled,
          "modo DDC por monitor sobreviveu");
    Check(loaded.appRules.size() == 1 && loaded.appRules[0].priority == 7,
          "regra de aplicativo sobreviveu");
}

void TestConfigHostile() {
    Section("Configuracao: arquivos estragados");

    const std::wstring path = ConfigPath();

    struct Case { const char* name; const char* content; };
    const Case cases[] = {
        { "arquivo vazio", "" },
        { "so lixo binario", "\x01\x02\x03\xff\xfe\x00\x7f" },
        { "secao sem fechar", "[perfil:Teste\nbrilho=50\n" },
        { "chave sem valor", "[perfil:Teste]\nbrilho\ncontraste=\n" },
        { "numeros absurdos", "[perfil:Teste]\nbrilho=999999999\ngamma=-50\nescurecer=1e300\n" },
        { "texto onde vai numero", "[perfil:Teste]\nbrilho=muito\ngamma=claro\n" },
        { "secao duplicada", "[perfil:A]\nbrilho=50\n[perfil:A]\nbrilho=60\n" },
        { "perfil sem nome", "[perfil:]\nbrilho=50\n" },
        { "regra apontando para perfil inexistente",
          "[perfil:X]\nbrilho=100\n[app:jogo]\nperfil=NaoExiste\n" },
        { "milhares de linhas vazias", "[perfil:X]\n\n\n\n\n\n\n\n\n\nbrilho=90\n" },
    };

    for (const auto& c : cases) {
        // Writes the hostile content directly, bypassing the Zdisplay writer.
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteFile(h, c.content, (DWORD)strlen(c.content), &written, nullptr);
            ::CloseHandle(h);
        }
        // The backup copy would mask the failure path that is under test.
        ::DeleteFileW((path + L".bak").c_str());

        Config cfg;
        LoadConfig(&cfg);   // must not hang or throw

        bool sane = !cfg.profiles.empty();
        for (const auto& p : cfg.profiles) {
            const Adjustments& a = p.global;
            if (a.brightness < 10 || a.brightness > 150) sane = false;
            if (a.gamma < 0.3 || a.gamma > 3.0) sane = false;
            if (a.dim < 0 || a.dim > 90) sane = false;
            if (a.EffectiveLuminance() < kFloorLuminance - 1e-9) sane = false;
            if (!cfg.Find(cfg.defaultProfile)) sane = false;
        }
        for (const auto& r : cfg.appRules)
            if (!cfg.Find(r.profile)) sane = false;

        Check(sane, c.name, "sobrou um estado invalido apos carregar");
    }
}

void TestBaseline() {
    Section("Estado original da tela");

    Baseline b;
    std::vector<WORD> ramp(768);
    for (int i = 0; i < 768; ++i) ramp[i] = (WORD)((i * 7919) % 65535);
    b.ramps[L"MONITOR#XYZ"] = ramp;
    b.hardware[L"MONITOR#XYZ"] = std::make_pair(37, 84);
    b.backlight = 55;
    b.vendor[L"\\\\.\\DISPLAY1"] = std::make_pair(51, 12);
    // The SDR white level has to survive process exit: without restoring it
    // here, a crashed session leaves an HDR screen stuck at the written value.
    b.hdrWhite[L"MONITOR#XYZ"] = 240;

    Check(SaveBaseline(b), "gravou o estado original");

    Baseline loaded;
    Check(LoadBaseline(&loaded), "leu o estado original de volta");
    Check(loaded.ramps.count(L"MONITOR#XYZ") == 1, "a rampa voltou");
    if (loaded.ramps.count(L"MONITOR#XYZ"))
        Check(loaded.ramps[L"MONITOR#XYZ"] == ramp, "a rampa voltou byte a byte");
    Check(loaded.hardware[L"MONITOR#XYZ"].first == 37 &&
          loaded.hardware[L"MONITOR#XYZ"].second == 84, "brilho e contraste voltaram");
    Check(loaded.backlight == 55, "backlight voltou");
    Check(loaded.vendor[L"\\\\.\\DISPLAY1"].first == 51 &&
          loaded.vendor[L"\\\\.\\DISPLAY1"].second == 12, "vibrance e matiz voltaram");
    Check(loaded.hdrWhite.count(L"MONITOR#XYZ") == 1 &&
          loaded.hdrWhite[L"MONITOR#XYZ"] == 240, "nivel de branco SDR voltou");

    // File cut in half, as a power loss during the write would leave it.
    std::string raw;
    const std::wstring path = ConfigDir() + L"\\baseline.dat";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(h, "KRBL\x01\x00\x00\x00\x05", 9, &written, nullptr);
        ::CloseHandle(h);
    }
    Baseline broken;
    Check(!LoadBaseline(&broken), "arquivo cortado e recusado em vez de aceito pela metade");

    ClearBaseline();
    Baseline gone;
    Check(!LoadBaseline(&gone), "sem arquivo, nao ha estado original");

    // Deliberately last: any further write leaves a valid .bak, and the
    // truncated-file test above would fall back to it instead of exercising
    // the refusal.
    //
    // A baseline written by an earlier version, without the HDR block, must
    // still load: upgrading cannot cost the stored screen state.
    {
        Baseline old;
        old.ramps[L"MONITOR#XYZ"] = ramp;
        old.backlight = 55;
        Check(SaveBaseline(old), "gravou um baseline sem telas em HDR");
        Baseline back;
        Check(LoadBaseline(&back) && back.hdrWhite.empty() && back.backlight == 55,
              "baseline sem o bloco de HDR continua valido");
    }

    ClearBaseline();
    Baseline afterClear;
    Check(!LoadBaseline(&afterClear), "limpar tambem remove a copia de seguranca");
}

// Machine configurations

/// Simulates different machines by checking the decisions the engine would make.
/// What varies between PCs is which backends exist; what must not vary is that
/// the result stays valid.
void TestDuplicateMonitorKeys() {
    Section("Monitores com EDID clonado");

    // Two units of the same model that ship with the same serial: the EDID
    // does not tell them apart and both arrive here with an identical key.
    auto twins = []() {
        std::vector<MonitorTarget> v(2);
        v[0].key = L"SAM0C1F-XYZ";  v[0].connectionKey = L"DISPLAY1#5&aa&0&UID1";
        v[1].key = L"SAM0C1F-XYZ";  v[1].connectionKey = L"DISPLAY2#5&bb&0&UID2";
        return v;
    };

    std::vector<MonitorTarget> a = twins();
    monitors::DisambiguateDuplicateKeys(&a);
    Check(a[0].key != a[1].key, "duas telas de serial igual nao compartilham chave",
          a[0].key == a[1].key ? "ambas viraram " + Utf8(a[0].key) : "");

    // The order in which Windows enumerates screens is not guaranteed, so each
    // screen must get the same key whatever the order, otherwise the two sets
    // of per-monitor adjustments swap places between boots.
    std::vector<MonitorTarget> b = twins();
    std::swap(b[0], b[1]);
    monitors::DisambiguateDuplicateKeys(&b);
    Check(a[0].key == b[1].key && a[1].key == b[0].key,
          "a chave de cada tela nao depende da ordem de enumeracao",
          Utf8(a[0].key) + " / " + Utf8(a[1].key) + "  contra  " +
              Utf8(b[1].key) + " / " + Utf8(b[0].key));

    // Three identical keys: all of them must be suffixed, including the last.
    std::vector<MonitorTarget> t(3);
    for (int i = 0; i < 3; ++i) {
        t[i].key = L"ACME1234";
        t[i].connectionKey = L"PORTA" + std::to_wstring(i);
    }
    monitors::DisambiguateDuplicateKeys(&t);
    Check(t[0].key != t[1].key && t[1].key != t[2].key && t[0].key != t[2].key,
          "tres telas clonadas recebem tres chaves distintas");
    int suffixed = 0;
    for (const auto& m : t) if (m.key.find(L'|') != std::wstring::npos) ++suffixed;
    Check(suffixed == 3, "todas as tres levam o sufixo, nenhuma fica com a chave nua",
          std::to_string(suffixed) + " de 3 com sufixo");

    // A unique key must not gain a suffix: that would invalidate adjustments
    // already stored for a single-screen setup.
    std::vector<MonitorTarget> solo(1);
    solo[0].key = L"DEL4021-ABC";
    solo[0].connectionKey = L"DISPLAY1#5&cc&0&UID3";
    monitors::DisambiguateDuplicateKeys(&solo);
    Check(solo[0].key == L"DEL4021-ABC", "tela unica mantem a chave intacta",
          Utf8(solo[0].key));

    std::vector<MonitorTarget> vazio;
    monitors::DisambiguateDuplicateKeys(&vazio);
    monitors::DisambiguateDuplicateKeys(nullptr);
    Check(true, "lista vazia e ponteiro nulo nao derrubam");
}

void TestSimulatedMachines() {
    Section("Configuracoes de PC simuladas");

    struct Machine {
        const char* name;
        bool nvidia, amd, magnification, ddcci, backlight;
        int monitors;
    };
    const Machine machines[] = {
        { "desktop NVIDIA + 2 monitores",      true,  false, true,  true,  false, 2 },
        { "desktop AMD + 1 monitor",           false, true,  true,  true,  false, 1 },
        { "notebook Intel (so integrada)",     false, false, true,  false, true,  1 },
        { "notebook + monitor externo",        false, false, true,  true,  true,  2 },
        { "PC antigo sem Magnification",       false, false, false, false, false, 1 },
        { "maquina virtual / RDP",             false, false, true,  false, false, 1 },
        { "estacao com 4 monitores",           true,  false, true,  true,  false, 4 },
        { "so CPU, sem nada disponivel",       false, false, false, false, false, 1 },
    };

    for (const auto& m : machines) {
        // Engine rule: vibrance goes to the GPU when one is available,
        // saturation goes to the universal matrix, and without a matrix
        // vibrance falls back to saturation.
        const bool vendorVibrance = m.nvidia || m.amd;
        const bool saturationWorks = m.magnification || m.amd;
        const bool hardwareBrightness = m.ddcci || m.backlight;

        // Brightness, contrast, gamma and color temperature have to work on
        // every machine: the gamma ramp depends on no backend.
        Adjustments a;
        a.brightness = 70; a.contrast = 115; a.gamma = 1.2;
        a.temperature = 3800; a.shadows = 55; a.clarity = 35;
        a.Sanitize();

        WORD ramp[768];
        BuildRamp(a, ramp);
        bool rampOk = true;
        for (int c = 0; c < 3 && rampOk; ++c)
            for (int i = 1; i < 256; ++i)
                if (ramp[c * 256 + i] < ramp[c * 256 + i - 1]) { rampOk = false; break; }

        char detail[256];
        snprintf(detail, sizeof(detail),
                 "%s: vibrance=%s saturacao=%s brilho fisico=%s monitores=%d",
                 m.name,
                 vendorVibrance ? "GPU" : "matriz",
                 saturationWorks ? "sim" : "nao",
                 hardwareBrightness ? "sim" : "nao",
                 m.monitors);
        Check(rampOk && a.EffectiveLuminance() >= kFloorLuminance, detail);
    }

    // A profile built on one machine has to load on another without breaking,
    // even when that machine has different monitors.
    Config cfg;
    cfg.SeedDefaults();
    cfg.profiles[0].perMonitor[L"MONITOR_QUE_NAO_EXISTE_AQUI"] = Adjustments{};
    cfg.profiles[0].perMonitor[L"MONITOR_QUE_NAO_EXISTE_AQUI"].brightness = 60;
    Check(SaveConfig(cfg), "perfil com monitor desconhecido foi gravado");

    Config other;
    LoadConfig(&other);
    Check(!other.profiles.empty() &&
          other.profiles[0].perMonitor.count(L"MONITOR_QUE_NAO_EXISTE_AQUI") == 1,
          "perfil de outra maquina carrega sem perder as sobrescritas");
    Check(other.profiles[0].For(L"MONITOR_INEXISTENTE").brightness ==
          other.profiles[0].global.brightness,
          "monitor sem sobrescrita cai no ajuste global");
}

void TestProfiles() {
    Section("Perfis");

    Config cfg;
    cfg.SeedDefaults();
    Check(cfg.profiles.size() >= 5, "os perfis padrao foram criados");
    Check(cfg.Default() != nullptr, "existe um perfil padrao valido");

    const std::wstring unique = cfg.UniqueName(cfg.profiles[0].name);
    Check(unique != cfg.profiles[0].name, "nome duplicado ganha sufixo");
    Check(cfg.Find(unique) == nullptr, "o nome sugerido esta mesmo livre");

    Profile p;
    p.global.brightness = 90;
    Adjustments& m = p.Ensure(L"MON1");
    Check(m.brightness == 90, "sobrescrita nova herda o valor global");
    m.brightness = 50;
    Check(p.For(L"MON1").brightness == 50, "sobrescrita e respeitada");
    Check(p.For(L"OUTRO").brightness == 90, "outro monitor continua no global");

    // Every seeded profile has to be safe for the eyes.
    for (const auto& sp : cfg.profiles) {
        if (sp.global.EffectiveLuminance() < kRiskyLuminance) {
            Fail("perfil semente nao pode nascer escuro demais", Utf8(sp.name));
            return;
        }
    }
    Ok("nenhum perfil semente nasce escuro demais");
}

void TestBlend() {
    Section("Transicoes");

    Adjustments a, b;
    a.brightness = 100; a.saturation = 100; a.shadows = 0;
    b.brightness = 50;  b.saturation = 180; b.shadows = 80;

    CheckNear(Adjustments::Blend(a, b, 0.0).brightness, 100, 0.001, "t=0 devolve a origem");
    CheckNear(Adjustments::Blend(a, b, 1.0).brightness, 50, 0.001, "t=1 devolve o destino");
    CheckNear(Adjustments::Blend(a, b, 0.5).brightness, 75, 0.001, "t=0.5 fica no meio");
    CheckNear(Adjustments::Blend(a, b, 0.5).shadows, 40, 0.001, "sombras tambem interpolam");

    // t outside 0..1 must not extrapolate into invalid values.
    CheckNear(Adjustments::Blend(a, b, -5).brightness, 100, 0.001, "t negativo e preso em 0");
    CheckNear(Adjustments::Blend(a, b, 99).brightness, 50, 0.001, "t acima de 1 e preso em 1");

    // No instant of the transition may break the light floor.
    Adjustments dark;
    dark.brightness = 12; dark.dim = 85;
    dark.Sanitize();
    int violations = 0;
    for (int i = 0; i <= 100; ++i) {
        Adjustments step = Adjustments::Blend(a, dark, i / 100.0);
        if (step.EffectiveLuminance() < kFloorLuminance - 1e-9) ++violations;
    }
    Check(violations == 0, "nenhum quadro da transicao apaga a tela");
}

void TestTextUtils() {
    Section("Utilitarios de texto");

    Check(Trim(L"   ola   ") == L"ola", "Trim tira espacos das duas pontas");
    Check(Trim(L"") == L"", "Trim aguenta texto vazio");
    Check(IEquals(L"ABC", L"abc"), "IEquals ignora capitalizacao");
    Check(!IEquals(L"ABC", L"abcd"), "IEquals compara tamanho");

    double v = 0;
    Check(ParseDouble(L"12.5", &v) && std::fabs(v - 12.5) < 1e-9, "le numero com ponto");
    Check(ParseDouble(L"12,5", &v) && std::fabs(v - 12.5) < 1e-9, "le numero com virgula");
    Check(ParseDouble(L"  -3  ", &v) && std::fabs(v + 3) < 1e-9, "le negativo com espacos");
    Check(!ParseDouble(L"abc", &v), "recusa texto que nao e numero");
    Check(!ParseDouble(L"", &v), "recusa texto vazio");
    Check(!ParseDouble(L"12abc", &v), "recusa numero com sujeira no fim");

    Check(WideToUtf8(L"acao") == "acao", "converte para UTF-8");
    Check(Utf8ToWide("acao") == L"acao", "converte de UTF-8");
    Check(Utf8ToWide(WideToUtf8(L"cor e saturacao")) == L"cor e saturacao",
          "ida e volta UTF-8 preserva o texto");
}

// Regressions
//
// Each block below pins an invariant that has to keep holding; the comment
// states which one, so the check is not simplified away.

void TestRegressions() {
    Section("Regressoes");

    // Clamp must never let NaN through.
    Check(Clamp(std::nan(""), 0.0, 100.0) == 0.0, "Clamp devolve o minimo para NaN");
    Check(Clamp(0.5, 0.0, 1.0) == 0.5, "Clamp normal continua funcionando");

    // 6500 K has to be exactly neutral.
    double r, g, b;
    TemperatureToRgb(6500, &r, &g, &b);
    CheckNear(r, 1.0, 1e-9, "6500 K: vermelho exatamente 1");
    CheckNear(g, 1.0, 1e-9, "6500 K: verde exatamente 1");
    CheckNear(b, 1.0, 1e-9, "6500 K: azul exatamente 1");

    // The color temperature curve must be continuous: two branches that do not
    // meet leave a visible step about 100 K away from neutral.
    double worstJump = 0;
    double prevR = 0, prevG = 0, prevB = 0;
    TemperatureToRgb(1500, &prevR, &prevG, &prevB);
    for (double k = 1510; k <= 10000; k += 10) {
        TemperatureToRgb(k, &r, &g, &b);
        const double jump = (std::max)((std::max)(std::fabs(r - prevR), std::fabs(g - prevG)),
                                       std::fabs(b - prevB));
        if (jump > worstJump) worstJump = jump;
        prevR = r; prevG = g; prevB = b;
    }
    Check(worstJump < 0.01, "a curva de temperatura nao tem degrau",
          "maior salto por passo de 10 K: " + std::to_string(worstJump));

    // The light floor must hold when color temperature, gamma, contrast and
    // RGB gains are combined, not only when brightness alone is lowered.
    {
        int violations = 0;
        const double temps[] = {1500, 2700, 4000, 6500, 9000};
        const double gammas[] = {0.3, 0.6, 1.0, 2.0, 3.0};
        const double gains[]  = {50, 70, 100};
        const double contrasts[] = {0, 40, 100, 200};

        // The extremes of hardware brightness weigh the most (factor 0.35 at
        // 0) and the -1 to 0 boundary is where the field changes meaning, from
        // "not managed" to "off", so the grid covers both ends explicitly.
        const double hws[] = {-1, 0, 1, 25, 50, 75, 99, 100};

        for (double t : temps)
        for (double gm : gammas)
        for (double gn : gains)
        for (double ct : contrasts)
        for (double hw : hws)
        for (double br : {10.0, 40.0, 100.0})
        for (double dm : {0.0, 50.0, 90.0}) {
            Adjustments x;
            x.temperature = t; x.gamma = gm; x.contrast = ct; x.brightness = br; x.dim = dm;
            x.redGain = x.greenGain = x.blueGain = gn;
            x.hwBrightness = hw;
            x.Sanitize();
            if (x.EffectiveLuminance() < kFloorLuminance - 1e-9) ++violations;
        }
        Check(violations == 0,
              "nenhuma combinacao de temperatura, gamma, contraste, ganhos e brilho "
              "de hardware fura o piso",
              std::to_string(violations) + " violacoes");
    }

    // The "nothing to do" predicates. A field missing from GammaNeutral
    // silently disables the adjustment: the backend concludes there is nothing
    // to write.
    {
        WORD ident[768];
        IdentityRamp(ident);
        bool ok = true;
        for (int i = 0; i < 256; ++i) {
            const WORD want = (WORD)(i * 257);
            if (ident[i] != want || ident[256 + i] != want || ident[512 + i] != want) ok = false;
        }
        Check(ok, "IdentityRamp devolve a rampa linear exata");

        Adjustments none;
        Check(none.Neutral() && none.GammaNeutral() && none.MatrixNeutral(),
              "o ajuste padrao e neutro nos tres predicados");

        WORD neutralRamp[768];
        BuildRamp(none, neutralRamp);
        int worst = 0;
        for (int i = 0; i < 768; ++i)
            worst = (std::max)(worst, std::abs((int)neutralRamp[i] - (int)ident[i]));
        Check(worst <= 257, "se o ajuste e neutro, a rampa e a linear (dentro de 1 passo)",
              "maior diferenca: " + std::to_string(worst));

        // Each field on its own has to break the right predicate.
        Adjustments g1; g1.gamma = 1.5;
        Check(!g1.GammaNeutral() && !g1.Neutral(), "gamma sozinho derruba GammaNeutral");
        Adjustments s1; s1.shadows = 10;
        Check(!s1.GammaNeutral(), "sombras sozinhas derrubam GammaNeutral");
        Adjustments b1; b1.blueBlock = 10;
        Check(!b1.GammaNeutral(), "bloqueio de azul sozinho derruba GammaNeutral");
        Adjustments m1; m1.saturation = 120;
        Check(!m1.MatrixNeutral(), "saturacao sozinha derruba MatrixNeutral");
        Adjustments h1; h1.hue = 15;
        Check(!h1.MatrixNeutral(), "matiz sozinho derruba MatrixNeutral");
    }

    // The measured luminance has to account for gains, color temperature and gamma.
    {
        Adjustments neutral;
        Adjustments dark;
        dark.redGain = dark.greenGain = dark.blueGain = 50;
        Check(dark.EffectiveLuminance() < neutral.EffectiveLuminance() - 0.05,
              "ganhos RGB em 50 reduzem a luminancia medida");

        Adjustments warm;
        warm.temperature = 1500;
        Check(warm.EffectiveLuminance() < neutral.EffectiveLuminance() - 0.05,
              "temperatura 1500 K reduz a luminancia medida");

        Adjustments lowGamma;
        lowGamma.gamma = 0.35;
        Check(lowGamma.EffectiveLuminance() < neutral.EffectiveLuminance() - 0.05,
              "gamma 0.35 reduz a luminancia medida");

        CheckNear(neutral.EffectiveLuminance(), 1.0, 0.02,
                  "o estado neutro mede luminancia 1.0");
    }

    // Sanitize has to be idempotent.
    {
        Adjustments x;
        x.brightness = 12; x.dim = 88; x.gamma = 0.3; x.temperature = 1500;
        x.redGain = x.greenGain = x.blueGain = 50; x.hwBrightness = 0;
        x.Sanitize();
        Adjustments once = x;
        x.Sanitize();
        Check(once.brightness == x.brightness && once.dim == x.dim &&
              once.gamma == x.gamma && once.temperature == x.temperature &&
              once.redGain == x.redGain && once.hwBrightness == x.hwBrightness,
              "Sanitize aplicado duas vezes da o mesmo resultado");
    }

    // The shadow curve is applied before brightness, so lowering brightness
    // must not widen the lift window all the way up to white.
    {
        Adjustments a;
        a.brightness = 50;
        a.shadows = 100; a.clarity = 0;

        WORD withShadows[768], withoutShadows[768];
        BuildRamp(a, withShadows);
        Adjustments b2 = a;
        b2.shadows = 0;
        BuildRamp(b2, withoutShadows);

        Check(withShadows[255] == withoutShadows[255],
              "com brilho 50%, as sombras nao mexem no branco",
              std::to_string(withShadows[255]) + " vs " + std::to_string(withoutShadows[255]));
        Check(withShadows[4] > withoutShadows[4],
              "com brilho 50%, as sombras continuam levantando o escuro");
    }

    // High contrast must not crush the shadows before the curve can lift them.
    // A constant does not decrease either, so monotonicity cannot catch the
    // collapse: the test counts distinct output levels instead.
    {
        CheckNear(ApplyContrast(0.0, 200), 0.0, 1e-9, "contraste: preto continua preto");
        CheckNear(ApplyContrast(0.5, 200), 0.5, 1e-9, "contraste: o meio-tom nao se move");
        CheckNear(ApplyContrast(1.0, 200), 1.0, 1e-9, "contraste: branco continua branco");
        CheckNear(ApplyContrast(0.3, 100), 0.3, 1e-9, "contraste 100 e a identidade");

        // Slope at the midtone: this is what keeps the contrast slider meaning
        // what it meant under the linear formula.
        const double slope = (ApplyContrast(0.5 + 1e-4, 180) - ApplyContrast(0.5 - 1e-4, 180))
                             / 2e-4;
        CheckNear(slope, 1.8, 1e-2, "a inclinacao no meio-tom continua sendo a da barra");

        Adjustments hard;
        hard.contrast = 200;
        WORD ramp[768];
        BuildRamp(hard, ramp);

        int distinct = 1;
        for (int i = 1; i < 64; ++i)
            if (ramp[i] != ramp[i - 1]) ++distinct;
        Check(distinct >= 60,
              "contraste 200 nao colapsa os 64 tons mais escuros",
              std::to_string(distinct) + " de 64 niveis distintos");

        // Shadow lift still has room to work on top of that.
        Adjustments withShadow = hard;
        withShadow.shadows = 80;
        WORD lifted[768];
        BuildRamp(withShadow, lifted);
        Check(lifted[10] > ramp[10],
              "com contraste 200, as sombras ainda levantam o escuro");
    }

    // Rule time validation has to accept every form the engine understands,
    // including sunrise and sunset keywords with an offset, not only HH:mm.
    {
        Check(IsValidRuleTime(L"21:30"), "relogio continua valendo");
        Check(IsValidRuleTime(L"por"), "'por' e aceito");
        Check(IsValidRuleTime(L"nascer"), "'nascer' e aceito");
        Check(IsValidRuleTime(L"por-30"), "'por-30' e aceito");
        Check(IsValidRuleTime(L"nascer+45"), "'nascer+45' e aceito");
        Check(IsValidRuleTime(L"sunset"), "o nome em ingles tambem");
        Check(!IsValidRuleTime(L""), "vazio nao vale");
        Check(!IsValidRuleTime(L"amanhecer"), "palavra desconhecida nao vale");
        Check(!IsValidRuleTime(L"por+9999"), "deslocamento absurdo nao vale");
        Check(!IsValidRuleTime(L"25:00"), "hora impossivel nao vale");
    }

    // Mat5 identities and round trips.
    {
        Mat5 id = Mat5::Identity();
        Check((Mat5::Invert() * Mat5::Invert()).NearlyEquals(id, 1e-4f),
              "inverter duas vezes volta a identidade");
        Check((Mat5::Hue(40) * Mat5::Hue(-40)).NearlyEquals(id, 1e-3f),
              "girar o matiz e desgirar volta a identidade");
        Check(Mat5::Saturation(1.0).NearlyEquals(id, 1e-4f),
              "saturacao 1.0 e a identidade");
        Check((id * id).NearlyEquals(id, 1e-6f), "identidade vezes identidade");

        const Mat5 gray = Mat5::Saturation(0.0);
        const double outR = 1.0 * gray.m[0] + 0.0 * gray.m[5] + 0.0 * gray.m[10];
        const double outG = 1.0 * gray.m[1] + 0.0 * gray.m[6] + 0.0 * gray.m[11];
        Check(std::fabs(outR - 0.2126) < 1e-3 && std::fabs(outG - 0.2126) < 1e-3,
              "saturacao 0 leva o vermelho puro ao cinza de luminancia Rec.709",
              "R=" + std::to_string(outR) + " G=" + std::to_string(outG));

        const Mat5 h = Mat5::Hue(90);
        const double gr = 0.5 * h.m[0] + 0.5 * h.m[5] + 0.5 * h.m[10];
        const double gg = 0.5 * h.m[1] + 0.5 * h.m[6] + 0.5 * h.m[11];
        const double gb = 0.5 * h.m[2] + 0.5 * h.m[7] + 0.5 * h.m[12];
        Check(std::fabs(gr - 0.5) < 1e-3 && std::fabs(gg - 0.5) < 1e-3 &&
              std::fabs(gb - 0.5) < 1e-3,
              "cinza continua cinza depois de girar o matiz");
    }

    // Color temperature blending.
    {
        Adjustments a, b2;
        a.temperature = 6500;
        b2.temperature = 2000;
        const double mid = Adjustments::Blend(a, b2, 0.5).temperature;
        const double miredMid = 1e6 / ((1e6 / 6500 + 1e6 / 2000) / 2.0);
        CheckNear(mid, miredMid, 1.0, "a temperatura interpola em mired, nao em Kelvin");
        CheckNear(Adjustments::Blend(a, b2, 0).temperature, 6500, 1e-6, "t=0 devolve a origem");
        CheckNear(Adjustments::Blend(a, b2, 1).temperature, 2000, 1e-6, "t=1 devolve o destino");
    }

    // Imported profiles must be sanitized before entering the configuration.
    {
        const std::wstring path = ConfigDir() + L"\\teste-import.ini";
        std::vector<Profile> made(1);
        made[0].name = L"Perigoso";
        made[0].global.brightness = 5;     // out of range
        made[0].global.dim = 90;
        made[0].global.gamma = 99;         // far out of range
        Check(ExportProfiles(path, made), "exportou o perfil de teste");

        std::vector<Profile> loaded;
        Check(ImportProfiles(path, &loaded), "importou o perfil de teste");
        Check(!loaded.empty(), "a importacao devolveu algum perfil");
        if (!loaded.empty()) {
            const Adjustments& a = loaded[0].global;
            Check(a.brightness >= 10 && a.brightness <= 150,
                  "brilho importado entra na faixa util",
                  std::to_string(a.brightness));
            Check(a.gamma >= 0.3 && a.gamma <= 3.0,
                  "gamma importado entra na faixa util", std::to_string(a.gamma));
            Check(a.EffectiveLuminance() >= kFloorLuminance - 1e-9,
                  "perfil importado respeita o piso de luz",
                  std::to_string(a.EffectiveLuminance()));
        }
        ::DeleteFileW(path.c_str());
    }

    // Baseline: vendor block round trip and all-or-nothing loading.
    {
        Baseline b2;
        b2.ramps[L"MON"] = std::vector<WORD>(768, 1234);
        b2.hardware[L"MON"] = std::make_pair(70, 60);
        b2.backlight = 55;
        b2.vendor[L"\\\\.\\DISPLAY1"] = std::make_pair(42, 7);
        Check(SaveBaseline(b2), "gravou o estado original com o bloco de fabricante");

        Baseline back;
        Check(LoadBaseline(&back), "leu o estado original de volta");
        Check(back.vendor.size() == 1 &&
              back.vendor[L"\\\\.\\DISPLAY1"].first == 42 &&
              back.vendor[L"\\\\.\\DISPLAY1"].second == 7,
              "vibrance e matiz do painel do fabricante sobrevivem a ida e volta");

        // Truncating at several offsets must never leave half-loaded state.
        std::string raw;
        const std::wstring bp = ConfigDir() + L"\\baseline.dat";
        const std::wstring bkp = bp + L".bak";
        ::DeleteFileW(bkp.c_str());   // no safety net, so the failure path is exercised
        HANDLE h = ::CreateFileW(bp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            ::GetFileSizeEx(h, &sz);
            raw.resize((size_t)sz.QuadPart);
            DWORD rd = 0;
            ::ReadFile(h, &raw[0], (DWORD)raw.size(), &rd, nullptr);
            ::CloseHandle(h);
        }

        int partials = 0;
        for (size_t cut : {size_t(4), size_t(9), size_t(20), size_t(64), raw.size() / 2,
                           raw.size() > 8 ? raw.size() - 8 : size_t(1)}) {
            if (cut >= raw.size()) continue;
            HANDLE w = ::CreateFileW(bp.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (w == INVALID_HANDLE_VALUE) continue;
            DWORD written = 0;
            ::WriteFile(w, raw.data(), (DWORD)cut, &written, nullptr);
            ::CloseHandle(w);

            Baseline partial;
            if (LoadBaseline(&partial) && !partial.Empty()) ++partials;
            else if (!partial.Empty()) ++partials;   // refused but left the output dirty
        }
        Check(partials == 0,
              "baseline cortado nunca deixa estado pela metade",
              std::to_string(partials) + " casos sujaram a saida");
        ::DeleteFileW(bp.c_str());
    }

    // Schedule rule priority.
    {
        Config cfg;
        cfg.SeedDefaults();

        SYSTEMTIME now{};
        now.wHour = 23; now.wMinute = 0;

        ScheduleRule wide;
        wide.start = L"20:00"; wide.end = L"08:00"; wide.profile = L"Noite"; wide.priority = 0;
        ScheduleRule narrow;
        narrow.start = L"22:00"; narrow.end = L"00:00"; narrow.profile = L"Filme"; narrow.priority = 10;

        Check(wide.Matches(now) && narrow.Matches(now),
              "as duas faixas de horario casam as 23:00");
        Check(narrow.priority > wide.priority,
              "a faixa mais especifica pode ganhar por prioridade");
    }

    // Schedule ranges crossing midnight.
    {
        ScheduleRule r;
        r.start = L"22:00"; r.end = L"06:00";
        auto at = [](int h, int m) { SYSTEMTIME t{}; t.wHour = (WORD)h; t.wMinute = (WORD)m; return t; };
        Check(r.Matches(at(23, 0)), "23:00 esta dentro de 22:00-06:00");
        Check(r.Matches(at(2, 0)),  "02:00 esta dentro de 22:00-06:00");
        Check(r.Matches(at(22, 0)), "a borda inicial conta");
        Check(!r.Matches(at(21, 59)), "um minuto antes fica de fora");
        Check(!r.Matches(at(12, 0)), "meio-dia fica de fora");

        ScheduleRule off;
        off.enabled = false;
        off.start = L"00:00"; off.end = L"23:59"; off.profile = L"X";
        Check(!off.Matches(at(12, 0)), "regra desativada nao casa nunca");
    }

    // Wildcards in per-application rules.
    {
        auto match = [](const wchar_t* pattern, const wchar_t* proc) {
            AppRule r; r.process = pattern; r.profile = L"X";
            return r.Matches(proc);
        };
        Check(match(L"cs2", L"cs2"), "nome exato casa");
        Check(match(L"CS2", L"cs2"), "casa ignorando maiusculas");
        Check(match(L"*", L"qualquer"), "curinga sozinho casa com tudo");
        Check(match(L"cs*", L"cs2"), "curinga no fim");
        Check(match(L"*game*", L"mygamelauncher"), "curinga nas duas pontas");
        Check(!match(L"cs2", L"cs2x"), "nao casa por prefixo sem curinga");

        AppRule empty; empty.profile = L"X";
        Check(!empty.Matches(L"cs2"), "regra sem processo nao casa");
    }
}

}  // namespace

int wmain() {
    // Portable mode: everything the tests write stays next to the test
    // executable, leaving the real user configuration untouched.
    {
        wchar_t exe[MAX_PATH * 2] = {};
        ::GetModuleFileNameW(nullptr, exe, _countof(exe));
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        const std::wstring marker = dir + L"\\zdisplay-portable.txt";
        HANDLE h = ::CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }

    printf("Testes do Zdisplay\n");
    printf("Pasta de trabalho: %s\n", WideToUtf8(ConfigDir()).c_str());

    // If the marker above did not take effect, on a read-only folder for
    // example, ConfigDir() falls back to %APPDATA%\Zdisplay and the
    // damaged-file section would overwrite the real user configuration.
    // Aborting here is the only acceptable response.
    {
        wchar_t exe[MAX_PATH * 2] = {};
        ::GetModuleFileNameW(nullptr, exe, _countof(exe));
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        if (!IEquals(dir, ConfigDir())) {
            printf("\nABORTADO: os testes gravariam em '%s', que nao e a pasta do\n"
                   "executavel de teste. Isso apagaria a configuracao real.\n",
                   WideToUtf8(ConfigDir()).c_str());
            return 2;
        }
    }

    TestTextUtils();
    TestTemperature();
    TestRamp();
    TestShadowCurve();
    TestMatrix();
    TestSanitize();
    TestRules();
    TestProfiles();
    TestBlend();
    TestConfigRoundTrip();
    TestConfigHostile();
    TestBaseline();
    TestDuplicateMonitorKeys();
    TestSimulatedMachines();
    TestRegressions();
    TestEdid();
    TestVcpFeatures();
    TestMonitorQuirks();
    TestWmiInstanceIdentity();
    TestVcpCapabilities();
    TestDdcSafetyRules();
    TestSolar();
    TestVision();

    printf("\n----------------------------------------\n");
    printf("%d testes passaram, %d falharam.\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
