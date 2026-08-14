// GPU vendor API backends. Both DLLs are loaded at runtime, so on a machine
// without the corresponding GPU the backend simply reports itself unavailable
// and the engine falls back to the universal color matrix.
#include "backends.h"

namespace zdisplay {

// NVIDIA

namespace nv {

// NVAPI is an undocumented interface: entry points are resolved through
// NvAPI_QueryInterface by ordinal, not by exported name.
enum : unsigned {
    ID_Initialize                    = 0x0150E828,
    ID_Unload                        = 0xD22BDD7E,
    ID_EnumNvidiaDisplayHandle       = 0x9ABDD40D,
    ID_GetAssociatedNvidiaDisplayName= 0x22A78B05,
    ID_GetAssociatedDisplayOutputId  = 0xD995937E,
    ID_GetDVCInfo                    = 0x4085DE45,
    ID_SetDVCLevel                   = 0x172409B4,
    ID_GetDVCInfoEx                  = 0x0E45002D,
    ID_SetDVCLevelEx                 = 0x4A82C2B1,
    ID_SetHUEAngle                   = 0xF5A0F22C,
};

// Indices into NvapiBackend::fns_.
enum FnIndex {
    FN_Init = 0, FN_Unload, FN_EnumDisplay, FN_GetName, FN_GetOutputId,
    FN_GetDvc, FN_SetDvc, FN_GetDvcEx, FN_SetDvcEx, FN_SetHue, FN_COUNT
};

constexpr int NVAPI_OK = 0;

struct DvcInfo {
    unsigned version;
    int currentLevel, minLevel, maxLevel;
};

struct DvcInfoEx {
    unsigned version;
    int currentLevel, minLevel, maxLevel, defaultLevel;
};

// MAKE_NVAPI_VERSION(struct, version) = sizeof | (version << 16)
constexpr unsigned MakeVersion(size_t size, unsigned ver) {
    return (unsigned)size | (ver << 16);
}

typedef void* (__cdecl *PfnQueryInterface)(unsigned id);
typedef int   (__cdecl *PfnInitialize)(void);
typedef int   (__cdecl *PfnUnload)(void);
typedef int   (__cdecl *PfnEnumDisplay)(unsigned index, void** handle);
typedef int   (__cdecl *PfnGetName)(void* display, char* name);
typedef int   (__cdecl *PfnGetOutputId)(void* display, unsigned* outputId);
typedef int   (__cdecl *PfnGetDvc)(void* display, unsigned outputId, DvcInfo* info);
typedef int   (__cdecl *PfnSetDvc)(void* display, unsigned outputId, int level);
typedef int   (__cdecl *PfnGetDvcEx)(void* display, unsigned outputId, DvcInfoEx* info);
typedef int   (__cdecl *PfnSetDvcEx)(void* display, unsigned outputId, DvcInfoEx* info);
typedef int   (__cdecl *PfnSetHue)(void* display, unsigned outputId, int angle);

}  // namespace nv

bool NvapiBackend::Init() {
    const wchar_t* dllName = (sizeof(void*) == 8) ? L"nvapi64.dll" : L"nvapi.dll";
    if (!lib_.Load(dllName)) {
        details_ = L"nvapi64.dll not found (no NVIDIA GPU)";
        return false;
    }

    auto query = lib_.Get<nv::PfnQueryInterface>("nvapi_QueryInterface");
    if (!query) query = lib_.Get<nv::PfnQueryInterface>("NvAPI_QueryInterface");
    if (!query) {
        details_ = L"NvAPI_QueryInterface ausente";
        return false;
    }

    fns_[nv::FN_Init]        = query(nv::ID_Initialize);
    fns_[nv::FN_Unload]      = query(nv::ID_Unload);
    fns_[nv::FN_EnumDisplay] = query(nv::ID_EnumNvidiaDisplayHandle);
    fns_[nv::FN_GetName]     = query(nv::ID_GetAssociatedNvidiaDisplayName);
    fns_[nv::FN_GetOutputId] = query(nv::ID_GetAssociatedDisplayOutputId);
    fns_[nv::FN_GetDvc]      = query(nv::ID_GetDVCInfo);
    fns_[nv::FN_SetDvc]      = query(nv::ID_SetDVCLevel);
    fns_[nv::FN_GetDvcEx]    = query(nv::ID_GetDVCInfoEx);
    fns_[nv::FN_SetDvcEx]    = query(nv::ID_SetDVCLevelEx);
    fns_[nv::FN_SetHue]      = query(nv::ID_SetHUEAngle);

    auto initFn = (nv::PfnInitialize)fns_[nv::FN_Init];
    if (!initFn || initFn() != nv::NVAPI_OK) {
        details_ = L"NvAPI_Initialize failed";
        return false;
    }

    auto enumFn = (nv::PfnEnumDisplay)fns_[nv::FN_EnumDisplay];
    if (!enumFn || (!fns_[nv::FN_SetDvcEx] && !fns_[nv::FN_SetDvc])) {
        details_ = L"NVIDIA driver without the vibrance entry points";
        return false;
    }

    apiInitialized_ = true;

    Enumerate();
    if (displays_.empty()) {
        details_ = L"no active NVIDIA display";
        return false;
    }

    const Display& first = displays_.begin()->second;
    details_ = Format(L"%d display(s); vibrance %d..%d (neutro %d, atual %d)",
                      (int)displays_.size(), first.minLevel, first.maxLevel,
                      first.defaultLevel, first.origLevel);
    available_ = true;
    return true;
}

/// Enumerates the NVIDIA displays and binds each one to its GDI name.
///
/// Range and original level are read per display, since each display can carry
/// a different vibrance value in the NVIDIA control panel.
void NvapiBackend::Enumerate() {
    auto enumFn = (nv::PfnEnumDisplay)fns_[nv::FN_EnumDisplay];
    auto getName = (nv::PfnGetName)fns_[nv::FN_GetName];
    auto getOut  = (nv::PfnGetOutputId)fns_[nv::FN_GetOutputId];
    if (!enumFn) return;

    // Keep originals already captured: re-reading after a write would record
    // the applied value as if it were the user's own.
    std::map<std::wstring, int> knownOriginals;
    for (const auto& kv : displays_)
        if (kv.second.origLevel >= 0) knownOriginals[kv.first] = kv.second.origLevel;

    displays_.clear();

    for (unsigned i = 0; i < 16; ++i) {
        void* handle = nullptr;
        if (enumFn(i, &handle) != nv::NVAPI_OK || !handle) break;

        std::wstring gdiName;
        if (getName) {
            char buf[64] = {};
            if (getName(handle, buf) == nv::NVAPI_OK) {
                buf[63] = '\0';
                gdiName = Utf8ToWide(std::string(buf));
                gdiName = Trim(gdiName);
            }
        }
        if (gdiName.empty()) gdiName = Format(L"\\\\.\\DISPLAY%u", i + 1);

        Display d{};
        d.handle = handle;
        d.outputId = 0;
        if (getOut) getOut(handle, &d.outputId);

        // Use the driver's actual range for this display instead of assuming 0..63.
        if (auto getEx = (nv::PfnGetDvcEx)fns_[nv::FN_GetDvcEx]) {
            nv::DvcInfoEx info{};
            info.version = nv::MakeVersion(sizeof(nv::DvcInfoEx), 1);
            if (getEx(handle, d.outputId, &info) == nv::NVAPI_OK) {
                d.minLevel = info.minLevel;
                d.maxLevel = info.maxLevel;
                d.defaultLevel = info.defaultLevel;
                d.origLevel = info.currentLevel;
            }
        } else if (auto get = (nv::PfnGetDvc)fns_[nv::FN_GetDvc]) {
            nv::DvcInfo info{};
            info.version = nv::MakeVersion(sizeof(nv::DvcInfo), 1);
            if (get(handle, d.outputId, &info) == nv::NVAPI_OK) {
                d.minLevel = info.minLevel;
                d.maxLevel = info.maxLevel;
                d.defaultLevel = info.minLevel;
                d.origLevel = info.currentLevel;
            }
        }
        if (d.maxLevel <= d.minLevel) { d.minLevel = 0; d.maxLevel = 63; d.defaultLevel = 0; }
        if (d.origLevel < d.minLevel || d.origLevel > d.maxLevel) d.origLevel = d.defaultLevel;

        auto known = knownOriginals.find(gdiName);
        if (known != knownOriginals.end()) d.origLevel = known->second;

        d.hasHue = fns_[nv::FN_SetHue] != nullptr;

        displays_[gdiName] = d;
        KLOG_D(L"NVAPI display %s outputId=0x%X vibrance %d..%d (atual %d)",
               gdiName.c_str(), d.outputId, d.minLevel, d.maxLevel, d.origLevel);
    }
}

void NvapiBackend::Rediscover() {
    if (!available_) return;
    Enumerate();
    // Handles from the previous layout are dead, so the values applied to them
    // no longer describe the new displays.
    lastLevel_.clear();
    lastHue_.clear();
}

void NvapiBackend::AdoptBaseline(const Baseline& b) {
    for (auto& kv : displays_) {
        auto it = b.vendor.find(kv.first);
        if (it != b.vendor.end() && it->second.first >= 0)
            kv.second.origLevel = it->second.first;
    }
}

void NvapiBackend::ExportBaseline(Baseline* b) const {
    for (const auto& kv : displays_)
        b->vendor[kv.first] = std::make_pair(kv.second.origLevel, -1);
}

NvapiBackend::Display* NvapiBackend::Resolve(const MonitorTarget& m) {
    // The vendor path is applied only to displays driven by that adapter. On a
    // hybrid laptop the output usually hangs off the integrated adapter even
    // with an NVIDIA present, and the API returns success for a display it does
    // not drive, without changing anything. When the adapter cannot be
    // determined (gpuVendorId == 0) the call proceeds, so machines whose
    // topology is unreadable still get the vendor path.
    if (m.gpuVendorId != 0 && m.gpuVendorId != kVendorNvidia) return nullptr;

    auto it = displays_.find(m.deviceName);
    if (it != displays_.end()) return &it->second;
    // A single NVIDIA display leaves no ambiguity.
    if (displays_.size() == 1) return &displays_.begin()->second;
    return nullptr;
}

bool NvapiBackend::SetVibrance(const Display& d, int level) {
    if (auto setEx = (nv::PfnSetDvcEx)fns_[nv::FN_SetDvcEx]) {
        nv::DvcInfoEx info{};
        info.version = nv::MakeVersion(sizeof(nv::DvcInfoEx), 1);
        info.currentLevel = level;
        info.minLevel = d.minLevel;
        info.maxLevel = d.maxLevel;
        info.defaultLevel = d.defaultLevel;
        if (setEx(d.handle, d.outputId, &info) == nv::NVAPI_OK) return true;
    }
    if (auto set = (nv::PfnSetDvc)fns_[nv::FN_SetDvc])
        return set(d.handle, d.outputId, level) == nv::NVAPI_OK;
    return false;
}

void NvapiBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!available_) return;

    Display* d = Resolve(m);
    if (!d) return;

    // Vibrance 0..100 mapped onto this display's actual range, with 0 as neutral.
    int level = (int)llround(d->defaultLevel +
                             (Clamp(a.vibrance, 0.0, 100.0) / 100.0) * (d->maxLevel - d->defaultLevel));
    level = Clamp(level, d->minLevel, d->maxLevel);

    auto itLevel = lastLevel_.find(m.key);
    // The first write is never the neutral level itself: writing defaultLevel
    // at startup would flatten the vibrance already set in the NVIDIA panel.
    const bool neverWroteLevel = itLevel == lastLevel_.end();
    if ((neverWroteLevel && level != d->defaultLevel) ||
        (!neverWroteLevel && itLevel->second != level)) {
        if (SetVibrance(*d, level)) lastLevel_[m.key] = level;
    }

    if (auto setHue = (nv::PfnSetHue)fns_[nv::FN_SetHue]; setHue && handleHue_) {
        int angle = ((int)llround(a.hue) % 360 + 360) % 360;
        auto itHue = lastHue_.find(m.key);
        // Hue 0 is never the first value written: the API exposes no hue read,
        // so that write would clear a hue set in the NVIDIA panel.
        const bool neverWrote = itHue == lastHue_.end();
        if ((neverWrote && angle != 0) || (!neverWrote && itHue->second != angle)) {
            if (setHue(d->handle, d->outputId, angle) == nv::NVAPI_OK)
                lastHue_[m.key] = angle;
        }
    }
}

bool NvapiBackend::HasHue(const MonitorTarget& m) const {
    if (!available_ || !fns_[nv::FN_SetHue]) return false;
    // Const mirror of Resolve(): same adapter rule and same single-display
    // fallback.
    if (m.gpuVendorId != 0 && m.gpuVendorId != kVendorNvidia) return false;
    auto it = displays_.find(m.deviceName);
    if (it == displays_.end()) {
        if (displays_.size() != 1) return false;
        it = displays_.begin();
    }
    return it->second.hasHue;
}

void NvapiBackend::Reset(const MonitorTarget& m) {
    if (!available_) return;
    Display* d = Resolve(m);
    if (!d) return;

    // Restores the vibrance found in the NVIDIA panel, not the factory default.
    SetVibrance(*d, d->origLevel >= 0 ? d->origLevel : d->defaultLevel);

    // Hue returns to zero only when it was written from here. The API exposes
    // no hue read, so an unconditional 0 would clear a hue set in the panel.
    auto itHue = lastHue_.find(m.key);
    if (itHue != lastHue_.end()) {
        if (auto setHue = (nv::PfnSetHue)fns_[nv::FN_SetHue])
            setHue(d->handle, d->outputId, 0);
    }

    lastLevel_.erase(m.key);
    lastHue_.erase(m.key);
}

void NvapiBackend::Shutdown() {
    if (available_)
        for (const auto& m : monitors::All()) Reset(m);

    // NvAPI_Unload must follow every successful Initialize, even when
    // enumeration failed afterwards and available_ is false; otherwise
    // FreeLibrary below unloads the DLL with the API still live.
    if (apiInitialized_) {
        if (auto unload = (nv::PfnUnload)fns_[nv::FN_Unload]) unload();
        apiInitialized_ = false;
    }
    displays_.clear();
    lastLevel_.clear();
    lastHue_.clear();
    lib_.Free();
    available_ = false;
}

// AMD

namespace adl {

constexpr int ADL_OK = 0;
constexpr int ADL_MAX_PATH = 256;

constexpr int COLOR_BRIGHTNESS = 1;
constexpr int COLOR_CONTRAST   = 2;
constexpr int COLOR_SATURATION = 4;
constexpr int COLOR_HUE        = 8;

constexpr int DISPLAYINFO_CONNECTED = 0x00000001;
constexpr int DISPLAYINFO_MAPPED    = 0x00000002;

enum FnIndex {
    FN_Create = 0, FN_Destroy, FN_NumAdapters, FN_AdapterInfo,
    FN_DisplayInfo, FN_ColorGet, FN_ColorSet, FN_COUNT
};

#pragma pack(push, 4)
struct AdapterInfo {
    int  iSize;
    int  iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int  iBusNumber;
    int  iDeviceNumber;
    int  iFunctionNumber;
    int  iVendorID;
    char strAdapterName[ADL_MAX_PATH];
    char strDisplayName[ADL_MAX_PATH];
    int  iPresent;
    int  iExist;
    char strDriverPath[ADL_MAX_PATH];
    char strDriverPathExt[ADL_MAX_PATH];
    char strPNPString[ADL_MAX_PATH];
    int  iOSDisplayIndex;
};

struct ADLDisplayID {
    int iDisplayLogicalIndex;
    int iDisplayPhysicalIndex;
    int iDisplayLogicalAdapterIndex;
    int iDisplayPhysicalAdapterIndex;
};

struct ADLDisplayInfo {
    ADLDisplayID displayID;
    int  iDisplayControllerIndex;
    char strDisplayName[ADL_MAX_PATH];
    char strDisplayManufacturerName[ADL_MAX_PATH];
    int  iDisplayType;
    int  iDisplayOutputType;
    int  iDisplayConnector;
    int  iDisplayInfoMask;
    int  iDisplayInfoValue;
};
#pragma pack(pop)

typedef void* (__stdcall *PfnMalloc)(int size);

typedef int (__cdecl *PfnMainControlCreate)(PfnMalloc cb, int enumConnected);
typedef int (__cdecl *PfnMainControlDestroy)(void);
typedef int (__cdecl *PfnNumberOfAdapters)(int* count);
typedef int (__cdecl *PfnAdapterInfoGet)(AdapterInfo* info, int size);
typedef int (__cdecl *PfnDisplayInfoGet)(int adapter, int* num, ADLDisplayInfo** info, int forceDetect);
typedef int (__cdecl *PfnColorGet)(int adapter, int display, int type,
                                   int* current, int* def, int* min, int* max, int* step);
typedef int (__cdecl *PfnColorSet)(int adapter, int display, int type, int current);

void* __stdcall AllocCallback(int size) {
    return size > 0 ? ::malloc((size_t)size) : nullptr;
}

}  // namespace adl

bool AdlBackend::Init() {
    if (!lib_.Load(L"atiadlxx.dll") && !lib_.Load(L"atiadlxy.dll")) {
        details_ = L"atiadlxx.dll not found (no AMD GPU)";
        return false;
    }

    fns_[adl::FN_Create]      = (void*)lib_.Get<adl::PfnMainControlCreate>("ADL_Main_Control_Create");
    fns_[adl::FN_Destroy]     = (void*)lib_.Get<adl::PfnMainControlDestroy>("ADL_Main_Control_Destroy");
    fns_[adl::FN_NumAdapters] = (void*)lib_.Get<adl::PfnNumberOfAdapters>("ADL_Adapter_NumberOfAdapters_Get");
    fns_[adl::FN_AdapterInfo] = (void*)lib_.Get<adl::PfnAdapterInfoGet>("ADL_Adapter_AdapterInfo_Get");
    fns_[adl::FN_DisplayInfo] = (void*)lib_.Get<adl::PfnDisplayInfoGet>("ADL_Display_DisplayInfo_Get");
    fns_[adl::FN_ColorGet]    = (void*)lib_.Get<adl::PfnColorGet>("ADL_Display_Color_Get");
    fns_[adl::FN_ColorSet]    = (void*)lib_.Get<adl::PfnColorSet>("ADL_Display_Color_Set");

    for (int i = 0; i < adl::FN_COUNT; ++i) {
        if (!fns_[i]) {
            details_ = L"AMD driver without the ADL color entry points";
            return false;
        }
    }

    auto create = (adl::PfnMainControlCreate)fns_[adl::FN_Create];
    if (create(adl::AllocCallback, 1) != adl::ADL_OK) {
        details_ = L"ADL_Main_Control_Create failed";
        return false;
    }
    apiInitialized_ = true;

    Enumerate();
    if (displays_.empty()) {
        details_ = L"no AMD display with color control";
        return false;
    }

    details_ = Format(L"%d display(s) with hardware saturation/hue", (int)displays_.size());
    available_ = true;
    return true;
}

void AdlBackend::Enumerate() {
    // Keep originals already captured: re-reading after a write would record
    // the applied value as if it were the user's own.
    std::map<std::wstring, std::pair<int, int>> knownOriginals;
    for (const auto& d : displays_)
        if (d.origSat >= 0 || d.origHue >= 0)
            knownOriginals[d.gdiName] = std::make_pair(d.origSat, d.origHue);

    displays_.clear();

    auto numAdapters = (adl::PfnNumberOfAdapters)fns_[adl::FN_NumAdapters];
    auto adapterInfo = (adl::PfnAdapterInfoGet)fns_[adl::FN_AdapterInfo];
    auto displayInfo = (adl::PfnDisplayInfoGet)fns_[adl::FN_DisplayInfo];
    auto colorGet    = (adl::PfnColorGet)fns_[adl::FN_ColorGet];

    int count = 0;
    if (numAdapters(&count) != adl::ADL_OK || count <= 0) return;

    // ADL requires the block to be zeroed before it is filled.
    std::vector<adl::AdapterInfo> adapters((size_t)count);
    memset(adapters.data(), 0, adapters.size() * sizeof(adl::AdapterInfo));

    if (adapterInfo(adapters.data(), (int)(adapters.size() * sizeof(adl::AdapterInfo))) != adl::ADL_OK)
        return;

    for (const auto& info : adapters) {
        if (info.iPresent == 0) continue;
        if (info.strDisplayName[0] == '\0') continue;

        int numDisplays = 0;
        adl::ADLDisplayInfo* list = nullptr;
        if (displayInfo(info.iAdapterIndex, &numDisplays, &list, 0) != adl::ADL_OK ||
            !list || numDisplays <= 0) {
            if (list) ::free(list);
            continue;
        }

        for (int d = 0; d < numDisplays; ++d) {
            const adl::ADLDisplayInfo& di = list[d];
            const bool connected = (di.iDisplayInfoValue & adl::DISPLAYINFO_CONNECTED) != 0;
            const bool mapped    = (di.iDisplayInfoValue & adl::DISPLAYINFO_MAPPED) != 0;
            if (!connected || !mapped) continue;
            if (di.displayID.iDisplayLogicalAdapterIndex != info.iAdapterIndex) continue;

            Disp entry;
            entry.gdiName = Trim(Utf8ToWide(std::string(info.strDisplayName)));
            entry.adapter = info.iAdapterIndex;
            entry.display = di.displayID.iDisplayLogicalIndex;

            int cur = 0, step = 0;
            entry.hasSat = colorGet(entry.adapter, entry.display, adl::COLOR_SATURATION,
                                    &cur, &entry.satDefault, &entry.satMin,
                                    &entry.satMax, &step) == adl::ADL_OK &&
                           entry.satMax > entry.satMin;
            // Keep the value already set in the AMD panel: reset returns to it,
            // not to the factory default.
            if (entry.hasSat) entry.origSat = cur;

            entry.hasHue = colorGet(entry.adapter, entry.display, adl::COLOR_HUE,
                                    &cur, &entry.hueDefault, &entry.hueMin,
                                    &entry.hueMax, &step) == adl::ADL_OK &&
                           entry.hueMax > entry.hueMin;
            if (entry.hasHue) entry.origHue = cur;

            if (!entry.hasSat && !entry.hasHue) continue;

            auto known = knownOriginals.find(entry.gdiName);
            if (known != knownOriginals.end()) {
                if (known->second.first  >= 0) entry.origSat = known->second.first;
                if (known->second.second >= 0) entry.origHue = known->second.second;
            }

            bool duplicate = false;
            for (const auto& existing : displays_)
                if (existing.gdiName == entry.gdiName) { duplicate = true; break; }
            if (duplicate) continue;

            KLOG_D(L"ADL %s adapter=%d display=%d sat=%d..%d (neutro %d)",
                   entry.gdiName.c_str(), entry.adapter, entry.display,
                   entry.satMin, entry.satMax, entry.satDefault);
            displays_.push_back(entry);
        }
        ::free(list);
    }
}

void AdlBackend::Rediscover() {
    if (!available_) return;
    Enumerate();
    lastApplied_.clear();
}

void AdlBackend::AdoptBaseline(const Baseline& b) {
    for (auto& d : displays_) {
        auto it = b.vendor.find(d.gdiName);
        if (it == b.vendor.end()) continue;
        if (it->second.first  >= 0) d.origSat = it->second.first;
        if (it->second.second >= 0) d.origHue = it->second.second;
    }
}

void AdlBackend::ExportBaseline(Baseline* b) const {
    for (const auto& d : displays_)
        b->vendor[d.gdiName] = std::make_pair(d.origSat, d.origHue);
}

const AdlBackend::Disp* AdlBackend::Resolve(const MonitorTarget& m) const {
    // Same rule as NvapiBackend::Resolve: on a machine with two adapters the
    // vendor path applies only to displays the AMD adapter actually drives.
    if (m.gpuVendorId != 0 && m.gpuVendorId != kVendorAmd) return nullptr;

    for (const auto& d : displays_)
        if (IEquals(d.gdiName, m.deviceName)) return &d;
    // `gdiName` comes from AdapterInfo::strDisplayName, which names the
    // adapter: two displays on one adapter arrive under the same name and one
    // is dropped as a duplicate. The single-display fallback therefore applies
    // only when the machine also has a single display, otherwise the write
    // would land on a display that was never identified.
    if (displays_.size() == 1 && monitors::All().size() == 1) return &displays_[0];
    return nullptr;
}

void AdlBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!available_) return;
    const Disp* d = Resolve(m);
    if (!d) return;

    auto colorSet = (adl::PfnColorSet)fns_[adl::FN_ColorSet];

    int satValue = d->satDefault;
    if (d->hasSat) {
        if (handleSaturation_) {
            // 0..200 % with 100 at the driver's neutral value.
            const double pct = Clamp(a.saturation, 0.0, 200.0);
            satValue = pct >= 100.0
                ? (int)llround(d->satDefault + (pct - 100.0) / 100.0 * (d->satMax - d->satDefault))
                : (int)llround(d->satMin + pct / 100.0 * (d->satDefault - d->satMin));
        }
        // Vibrance is applied as a boost on top of saturation.
        if (a.vibrance > 0.01)
            satValue += (int)llround(a.vibrance / 100.0 * (d->satMax - d->satDefault));
        satValue = Clamp(satValue, d->satMin, d->satMax);
    }

    // Hue pivots on the driver's neutral value, like saturation above, so hue 0
    // lands on neutral and the slider means degrees on either vendor. With
    // handleHue_ off the hue goes through the universal matrix and the target
    // here stays neutral, so it is not applied twice.
    int hueValue = d->hueDefault;
    if (d->hasHue && handleHue_) {
        hueValue = Clamp((int)llround(d->hueDefault + Clamp(a.hue, -180.0, 180.0)),
                         d->hueMin, d->hueMax);
    }

    auto it = lastApplied_.find(m.key);
    // A first write that would be the driver's own neutral is skipped: writing
    // satDefault/hueDefault at startup would clear the saturation and hue
    // already set in the AMD panel.
    const bool neverWrote = it == lastApplied_.end();
    if (neverWrote && satValue == d->satDefault && hueValue == d->hueDefault)
        return;
    if (!neverWrote && it->second.first == satValue && it->second.second == hueValue)
        return;

    if (d->hasSat) colorSet(d->adapter, d->display, adl::COLOR_SATURATION, satValue);
    if (d->hasHue) colorSet(d->adapter, d->display, adl::COLOR_HUE, hueValue);
    lastApplied_[m.key] = std::make_pair(satValue, hueValue);
}

bool AdlBackend::HasHue(const MonitorTarget& m) const {
    if (!available_) return false;
    const Disp* d = Resolve(m);
    return d && d->hasHue;
}

void AdlBackend::Reset(const MonitorTarget& m) {
    if (!available_) return;
    const Disp* d = Resolve(m);
    if (!d) return;
    auto colorSet = (adl::PfnColorSet)fns_[adl::FN_ColorSet];
    if (d->hasSat)
        colorSet(d->adapter, d->display, adl::COLOR_SATURATION,
                 d->origSat >= 0 ? d->origSat : d->satDefault);
    if (d->hasHue)
        colorSet(d->adapter, d->display, adl::COLOR_HUE,
                 d->origHue >= 0 ? d->origHue : d->hueDefault);
    lastApplied_.erase(m.key);
}

void AdlBackend::Shutdown() {
    if (available_)
        for (const auto& m : monitors::All()) Reset(m);

    // Same rule as NVAPI: Destroy follows Create, not available_, otherwise
    // FreeLibrary runs with ADL still initialized.
    if (apiInitialized_) {
        if (auto destroy = (adl::PfnMainControlDestroy)fns_[adl::FN_Destroy]) destroy();
        apiInitialized_ = false;
    }
    displays_.clear();
    lastApplied_.clear();
    lib_.Free();
    available_ = false;
}

}  // namespace zdisplay
