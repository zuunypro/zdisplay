#pragma once
#include "common.h"

// Dark theme: cool greys, an accent red and a glass backdrop.
//
// Win32 controls have no colour property; each family paints itself from the
// system visual style. Darkening them takes three paths, all gathered here:
// colour messages for the controls that ask (statics, edits), dedicated APIs
// for those that store colours (list view, status bar), and owner-draw for
// those that accept neither (button, trackbar, tabs).
namespace zdisplay {
namespace theme {

/// Palette: cool grey background, accent red only where there is action or value.
struct Palette {
    COLORREF base;        ///< window background
    COLORREF surface;     ///< panel and list surface
    COLORREF surfaceAlt;  ///< input field
    COLORREF hover;       ///< hover highlight
    COLORREF line;        ///< subtle border
    COLORREF text;
    COLORREF textDim;     ///< secondary and disabled text
    COLORREF accent;      ///< accent red
    COLORREF accentHot;
    COLORREF accentDim;
};

const Palette& Colors();

/// Sets the process dark mode before any window or menu is created. Native menus
/// cache their theme the first time they open, so this has to run before the
/// first menu is shown.
void InitializeProcess();

/// Enables the dark window frame and, where Windows offers it, the glass
/// backdrop. Returns true when glass is active; the background is then not
/// painted.
bool ApplyWindowBackdrop(HWND hwnd);
bool GlassActive();

/// Applies to each child what can be set through APIs: dark scroll bar theme,
/// list colours, status bar colour. Safe to call again after creating further
/// controls.
void ApplyToControls(HWND parent);

/// Marks a label as dimmed without disabling it (see SliderRow::Enable); the
/// Windows disabled-static rendering is unsuitable on a dark background.
void SetDimmed(HWND ctl, bool dim);

HBRUSH BaseBrush();
HBRUSH SurfaceBrush();
HBRUSH SurfaceAltBrush();

/// Handles WM_CTLCOLOR* and WM_ERASEBKGND. Returns true when the message was
/// handled.
bool HandleColorMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

/// Handles NM_CUSTOMDRAW for buttons and trackbars.
bool HandleCustomDraw(LPARAM lp, LRESULT* result);

/// Records a trackbar's effective position outside the paint cycle.
///
/// Some builds of the common control report the initial rectangle and value
/// while handling NM_CUSTOMDRAW with the native drawing skipped, so the value is
/// captured when it changes rather than queried during painting.
void SyncTrackbarVisual(HWND bar);

/// Draws one list box row (WM_DRAWITEM for a LISTBOX with LBS_OWNERDRAWFIXED).
bool HandleDrawItem(const DRAWITEMSTRUCT* dis);

/// Takes over drawing of the tab control entirely: neither colour messages nor
/// WM_DRAWITEM reach the frame it draws around each tab.
void SubclassTabControl(HWND tabs);

void Shutdown();

}  // namespace theme
}  // namespace zdisplay
