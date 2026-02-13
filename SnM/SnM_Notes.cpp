/******************************************************************************
/ SnM_Notes.cpp
/
/ Copyright (c) 2010 and later Jeffos
/
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/ 
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/ 
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/

//JFB
// - if REAPER >= v4.55pre2 use MarkProjectDirty(), otherwise creates undo point for each key stroke (!) to fix:
//   * SaveExtensionConfig() that is not called when there is no proj mods but some notes have been entered..
//   * missing updates on project switches
// - OSX: no action help support (SWELL's GetPrivateProfileSection assumes key=value)
// - clicking the empty area of the tcp does not remove focus (important for refresh)
// - undo does not restore caret position
// TODO?
// - take changed => title not updated
// - drag-drop text?
// - use action_help_t? (not finished according to Cockos)
// - handle concurent item/project notes updates?

#include "stdafx.h"

#ifdef _WIN32
#pragma comment(lib, "comctl32.lib")
#endif

#include "SnM.h"
#include "SnM_Dlg.h"
#include "SnM_Notes.h"
#include "SnM_Project.h"
#include "SnM_Track.h"
#include "SnM_Util.h"
#include "SnM_Window.h"

#include "../cfillion/cfillion.hpp"

#include <WDL/localize/localize.h>
#include <WDL/projectcontext.h>

#define NOTES_WND_ID				"SnMNotesHelp"
#define NOTES_INI_SEC				"Notes"
#define MAX_HELP_LENGTH				(64*1024) //JFB! instead of MAX_INI_SECTION (too large)
#define UPDATE_TIMER				1

enum {
  WRAP_MSG = 0xF001,
  SAVE_GLOBAL_NOTES_MSG,
  LINK_ACTOR_MSG,
  UNLINK_ACTOR_MSG,
  CHANGE_COLOR_MSG,
  COLORED_REGIONS_MSG,
  DISPLAY_ACTOR_PREFIX_MSG,
  IMPORT_ROLES_MSG,
  EXPORT_ROLES_MSG,
  ENABLE_ALL_MSG,
  DISABLE_ALL_MSG,
  COPY_MARKERS_MSG,
  COPY_ROLE_DISTRIBUTION_MSG,
  LAST_MSG
};

enum {
  BTNID_LOCK = LAST_MSG,
  CMBID_TYPE,
  CMBID_REGION,
  TXTID_LABEL,
  BTNID_IMPORT_SUB,
  BTNID_CLEAR_SUBS,
  TXTID_BIG_NOTES
};

enum {
  REQUEST_REFRESH = 0,
  NO_REFRESH
};

int GenerateActorColor(const char *actorName);

SNM_WindowManager<NotesWnd> g_notesWndMgr(NOTES_WND_ID);

SWSProjConfig<WDL_PtrList_DOD<SNM_TrackNotes> > g_SNM_TrackNotes;
SWSProjConfig<WDL_PtrList_DOD<SNM_RegionSubtitle> > g_pRegionSubs;
SWSProjConfig<WDL_PtrList_DOD<SNM_Actor> > g_actors;
SWSProjConfig<WDL_FastString> g_prjNotes;
// global notes #647, saved in <REAPER Resource Path>/SWS_GlobalNotes.txt
// (no SWSProjConfig, one instance across all projects, i.e. global)
WDL_FastString g_globalNotes;
bool g_globalNotesDirty = false;
bool g_coloredRegions = true;
bool g_displayActorInPrefix = true;
SWSProjConfig<WDL_PtrList_DOD<SNM_Actor> > g_importedRoleActors;

void CleanupStaleActors();
void ClearAllSubtitles();
void UpdateRegionColors();
bool ExportRolesFile(const char *fn);
bool ImportRolesFile(const char *fn);

int g_notesType = -1;
int g_prevNotesType = -1;
bool g_locked = true;
char g_lastText[MAX_HELP_LENGTH] = "";
char g_lastImportSubFn[SNM_MAX_PATH] = "";
char g_lastRolesFn[SNM_MAX_PATH] = "";
char g_notesBigFontName[64] = SNM_DYN_FONT_NAME;
bool g_wrapText = false;

// other vars for updates tracking
double g_lastMarkerPos = -1.0;
int g_lastMarkerRegionId = -1;
MediaItem *g_mediaItemNote = NULL;
MediaTrack *g_trNote = NULL;

// to distinguish internal marker/region updates from external ones
bool g_internalMkrRgnChange = false;


SNM_TrackNotes *SNM_TrackNotes::find(MediaTrack *track) {
  const GUID *guid = TrackToGuid(track);
  if (!guid)
    return nullptr;

  for (int i = 0; i < g_SNM_TrackNotes.Get()->GetSize(); ++i) {
    SNM_TrackNotes *notes = g_SNM_TrackNotes.Get()->Get(i);
    if (GuidsEqual(guid, notes->GetGUID()))
      return notes;
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// NotesWnd
///////////////////////////////////////////////////////////////////////////////

// S&M windows lazy init: below's "" prevents registering the SWS' screenset callback
// (use the S&M one instead - already registered via SNM_WindowManager::Init())
NotesWnd::NotesWnd()
  : SWS_DockWnd(IDD_SNM_NOTES, __LOCALIZE("ReNotes", "sws_DLG_152"), ""),
    m_edit{nullptr},
    m_settingText{false},
    m_actorListView{nullptr},
    m_contextMenuActor{nullptr} {
  m_id.Set(NOTES_WND_ID);
  Init();
}

NotesWnd::~NotesWnd() = default;

void NotesWnd::OnInitDlg() {
  m_edit = GetDlgItem(m_hwnd, IDC_EDIT1);
  HWND edit2 = GetDlgItem(m_hwnd, IDC_EDIT2);

  // don't passthrough input to the main window
  // https://forum.cockos.com/showthread.php?p=1208961
  SetWindowLongPtr(m_edit, GWLP_USERDATA, 0xdeadf00b);
  SetWindowLongPtr(edit2, GWLP_USERDATA, 0xdeadf00b);

#ifdef __APPLE__
  // Prevent shortcuts in the menubar from triggering main window actions
  // bypassing the accelerator hook return value
  SWS_Mac_MakeDefaultWindowMenu(m_hwnd);

  // WS_VSCROLL makes SWELL use an NSTextView instead of NSTextField
  Mac_TextViewSetAllowsUndo(m_edit, true);
  Mac_TextViewSetAllowsUndo(edit2, true);
#endif

  m_resize.init_item(IDC_EDIT1, 0.0, 0.0, 1.0, 1.0);
  m_resize.init_item(IDC_EDIT2, 0.0, 0.0, 1.0, 1.0);

  SetWrapText(g_wrapText);

  LICE_CachedFont *font = SNM_GetThemeFont();

  m_vwnd_painter.SetGSC(WDL_STYLE_GetSysColor);
  m_parentVwnd.SetRealParent(m_hwnd);

  m_btnLock.SetID(BTNID_LOCK);
  m_parentVwnd.AddChild(&m_btnLock);

  m_cbType.SetID(CMBID_TYPE);
  m_cbType.SetFont(font);
  m_cbType.AddItem(__LOCALIZE("Заметки дорожки", "sws_DLG_152"));
  m_cbType.AddItem(__LOCALIZE("Заметки айтема", "sws_DLG_152"));
  m_cbType.AddItem(__LOCALIZE("Заметки проекта", "sws_DLG_152"));
  m_cbType.AddItem(__LOCALIZE("Доп. заметки проекта", "sws_DLG_152"));
  m_cbType.AddItem(__LOCALIZE("Глобальные заметки", "sws_DLG_152"));
  m_cbType.AddItem("<SEP>");
  m_cbType.AddItem(__LOCALIZE("Субтитры", "sws_DLG_152"));
  m_parentVwnd.AddChild(&m_cbType);
  // ...the selected item is set through SetType() below

  m_cbRegion.SetID(CMBID_REGION);
  m_cbRegion.SetFont(font);
  m_parentVwnd.AddChild(&m_cbRegion);

  m_txtLabel.SetID(TXTID_LABEL);
  m_txtLabel.SetFont(font);
  m_parentVwnd.AddChild(&m_txtLabel);

  m_btnImportSub.SetID(BTNID_IMPORT_SUB);
  m_parentVwnd.AddChild(&m_btnImportSub);

  m_btnClearSubs.SetID(BTNID_CLEAR_SUBS);
  m_parentVwnd.AddChild(&m_btnClearSubs);


#ifdef _WIN32
  HWND actorListHwnd = CreateWindowEx(0,
                                      WC_LISTVIEW,
                                      "",
                                      WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                      LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER,
                                      0,
                                      0,
                                      150,
                                      200,
                                      m_hwnd,
                                      NULL,
                                      g_hInst,
                                      NULL);
  ListView_SetExtendedListViewStyle(actorListHwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  HIMAGELIST hImgList = ImageList_Create(1, 34, ILC_COLOR, 1, 0);
  ListView_SetImageList(actorListHwnd, hImgList, LVSIL_SMALL);
#else
  int style = 0x0001 /*LVS_REPORT*/ | 0x0004 /*LVS_SINGLESEL*/ |
      0x4000 /*LVS_NOCOLUMNHEADER*/ | 0x8000 /*LVS_NOSORTHEADER*/;
  SWELL_MakeSetCurParms(1.0, 1.0, 0.0, 0.0, m_hwnd, false, false);
  HWND actorListHwnd = SWELL_MakeControl("", 0, "SysListView32", style, 0, 0, 150, 200, 0);
  ListView_SetExtendedListViewStyleEx(actorListHwnd, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  SWELL_MakeSetCurParms(1.0, 1.0, 0.0, 0.0, NULL, false, false);
#endif
  m_actorListView = new ActorListView(actorListHwnd, NULL);
  m_pLists.Add(m_actorListView);

  m_bigNotes.SetID(TXTID_BIG_NOTES);
  m_bigNotes.SetFontName(g_notesBigFontName);
  m_parentVwnd.AddChild(&m_bigNotes);

  g_prevNotesType = -1; // will force refresh
  SetType(BOUNDED(g_notesType, 0, m_cbType.GetCount()-1)); // + Update()

  /* see OnTimer()
      RegisterToMarkerRegionUpdates(&m_mkrRgnListener);
  */
  SetTimer(m_hwnd, UPDATE_TIMER, NOTES_UPDATE_FREQ, NULL);
}

void NotesWnd::OnDestroy() {
  KillTimer(m_hwnd, UPDATE_TIMER);
  /* see OnTimer()
      UnregisterToMarkerRegionUpdates(&m_mkrRgnListener);
  */
#ifdef _WIN32
  if (m_actorListView) {
    HIMAGELIST hImgList = ListView_GetImageList(m_actorListView->GetHWND(), LVSIL_SMALL);
    if (hImgList)
      ImageList_Destroy(hImgList);
  }
#endif
  g_prevNotesType = -1;
  m_cbType.Empty();
  m_cbRegion.Empty();
  m_overlappingRegionIds.Resize(0);
  m_edit = nullptr;
  memset(g_lastText, 0, sizeof(g_lastText));
  m_actorListView = nullptr;
}

// note: no diff with current type, init would fail otherwise
void NotesWnd::SetType(int _type) {
  g_notesType = _type;
  m_cbType.SetCurSel2(g_notesType);
  SendMessage(m_hwnd, WM_SIZE, 0, 0); // to update the bottom of the GUI

  // force an initial refresh (when IDC_EDIT has the focus, re-enabling the timer
  // isn't enough: Update() is skipped, see OnTimer() & IsActive()
  Update();
}

void NotesWnd::SetText(const char *_str, bool _addRN) {
  if (!_str)
    return;

  char rnStr[sizeof(g_lastText)];
  if (_addRN) {
    GetStringWithRN(_str, rnStr, sizeof(rnStr));
    _str = &rnStr[0];
  }

  if (!strcmp(_str, g_lastText))
    return;

  lstrcpyn(g_lastText, _str, sizeof(g_lastText));
  m_settingText = true;
  SetWindowText(m_edit, g_lastText);
  m_settingText = false;
}

void NotesWnd::RefreshGUI() {
  bool bHide = true;
  switch (g_notesType) {
    case SNM_NOTES_PROJECT:
    case SNM_NOTES_PROJECT_EXTRA:
    case SNM_NOTES_GLOBAL:
      bHide = false;
      break;
    case SNM_NOTES_ITEM:
      if (g_mediaItemNote)
        bHide = false;
      break;
    case SNM_NOTES_TRACK:
      if (g_trNote)
        bHide = false;
      break;
    case SNM_NOTES_RGN_SUB:
      if (g_lastMarkerRegionId > 0)
        bHide = false;
      break;
  }
  ShowWindow(m_edit, bHide || g_locked ? SW_HIDE : SW_SHOW);
  m_parentVwnd.RequestRedraw(NULL); // the meat!
}

void NotesWnd::SetFontsizeFrominiFile() {
  int notesFontsize = GetPrivateProfileInt(NOTES_INI_SEC, "Fontsize", -666, g_SNM_IniFn.Get());
  if (notesFontsize != -666) {
    // first try to get current font, if this doesn't work use theme font as fallback
    HFONT hfont = (HFONT) SendMessage(m_edit, WM_GETFONT, 0, 0);
    if (!hfont)
      hfont = SNM_GetThemeFont()->GetHFont();

    LOGFONT lf;
    GetObject(hfont, sizeof(lf), &lf);
    lf.lfHeight = notesFontsize;
    SendMessage(m_edit, WM_SETFONT, (WPARAM) CreateFontIndirect(&lf), TRUE);
  }
}

void NotesWnd::SetWrapText(const bool wrap) {
  g_wrapText = wrap;

  char buf[sizeof(g_lastText)]{};
  GetWindowText(m_edit, buf, sizeof(buf));
  m_edit = GetDlgItem(m_hwnd, wrap ? IDC_EDIT2 : IDC_EDIT1);
  SetFontsizeFrominiFile();
  m_settingText = true;
  SetWindowText(m_edit, buf);
  m_settingText = false;

#ifdef _WIN32
  // avoid flickering on Windows
  SendMessage(m_hwnd, WM_SETREDRAW, 0, 0);
#endif
  ShowWindow(GetDlgItem(m_hwnd, IDC_EDIT1), wrap ? SW_HIDE : SW_SHOW);
  ShowWindow(GetDlgItem(m_hwnd, IDC_EDIT2), wrap ? SW_SHOW : SW_HIDE);
#ifdef _WIN32
  SendMessage(m_hwnd, WM_SETREDRAW, 1, 0);
  InvalidateRect(m_hwnd, nullptr, 0);
#endif
}

static const char *GetLinkedActorForDisplay(const char *actorName) {
  if (!g_displayActorInPrefix || !actorName || !*actorName || !strcmp(actorName, "?"))
    return NULL;
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    if (!strcmp(a->GetName(), actorName) && a->HasLinkedActor())
      return a->GetLinkedActorName();
  }
  return NULL;
}

static void BuildRegionName(WDL_String *out, const char *actor, const char *notes) {
  if (actor && *actor && strcmp(actor, "?")) {
    const char *linked = GetLinkedActorForDisplay(actor);
    if (linked)
      out->SetFormatted(512, "%s (%s) %s", linked, actor, notes);
    else
      out->SetFormatted(512, "(%s) %s", actor, notes);
  } else {
    out->Set(notes);
  }
  char *p = out->Get();
  while (*p) {
    if (*p == '\r' || *p == '\n') *p = ' ';
    p++;
  }
  out->Ellipsize(0, 64);
}

static void AppendDisplayLine(WDL_FastString *out, const char *actorName, const char *notes) {
  if (actorName && *actorName) {
    const char *linked = GetLinkedActorForDisplay(actorName);
    if (linked)
      out->AppendFormatted(MAX_HELP_LENGTH, "%s (%s) %s", linked, actorName, notes);
    else
      out->AppendFormatted(MAX_HELP_LENGTH, "(%s) %s", actorName, notes);
  } else {
    out->Append(notes);
  }
}

void DeleteActorRegions(const char *actorName) {
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = subs->GetSize() - 1; i >= 0; i--) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!strcmp(sub->GetActor(), actorName) && sub->IsValid()) {
      double pos, endPos;
      if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, NULL, NULL) >= 0)
        sub->SetTimes(pos, endPos);
      int idx = GetMarkerRegionIndexFromId(NULL, sub->GetId());
      if (idx >= 0)
        DeleteProjectMarkerByIndex(NULL, idx);
      sub->SetId(-1);
    }
  }
}

void RecreateActorRegions(const char *actorName) {
  SNM_Actor *actor = NULL;
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    if (!strcmp(actors->Get(i)->GetName(), actorName)) {
      actor = actors->Get(i);
      break;
    }
  if (!actor) return;

  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!strcmp(sub->GetActor(), actorName) && !sub->IsValid()) {
      int color = g_coloredRegions ? actor->GetEffectiveColor() : 0;
      WDL_String regionName;
      BuildRegionName(&regionName, sub->GetActor(), sub->GetNotes());
      int num = AddProjectMarker2(NULL,
                                  true,
                                  sub->GetStartTime(),
                                  sub->GetEndTime(),
                                  regionName.Get(),
                                  -1,
                                  color);
      if (num >= 0) {
        int newId = MakeMarkerRegionId(num, true);
        sub->SetId(newId);
      }
    }
  }
  UpdateTimeline();
}

void UpdateRegionColors() {
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

  PreventUIRefresh(1);

  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!sub->IsValid())
      continue;

    int color = 0;
    if (g_coloredRegions) {
      for (int j = 0; j < actors->GetSize(); j++) {
        if (!strcmp(actors->Get(j)->GetName(), sub->GetActor())) {
          color = actors->Get(j)->GetEffectiveColor();
          break;
        }
      }
    }

    double pos, endPos;
    int num;
    if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, &num, NULL) >= 0) {
      int idx = GetMarkerRegionIndexFromId(NULL, sub->GetId());
      if (idx >= 0)
        DeleteProjectMarkerByIndex(NULL, idx);
      WDL_String regionName;
      BuildRegionName(&regionName, sub->GetActor(), sub->GetNotes());
      int newNum = AddProjectMarker2(NULL, true, pos, endPos, regionName.Get(), -1, color);
      if (newNum >= 0)
        sub->SetId(MakeMarkerRegionId(newNum, true));
    }
  }

  PreventUIRefresh(-1);
  UpdateTimeline();
}

static void FormatSubTime(double seconds, char *buf, int bufSize) {
  int totalSec = (int) (seconds + 0.5);
  int h = totalSec / 3600;
  int m = (totalSec % 3600) / 60;
  int s = totalSec % 60;
  if (h > 0)
    snprintf(buf, bufSize, "%d:%02d:%02d", h, m, s);
  else
    snprintf(buf, bufSize, "%d:%02d", m, s);
}

struct MarkerEntry {
  double pos;
  WDL_FastString text;
};

static void CopyMarkersToClipboard(HWND hwnd) {
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  if (!subs->GetSize()) return;

  int enumIdx = 0;
  bool isRgn;
  double mPos;
  WDL_TypedBuf<double> markerPositions;
  while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &mPos, NULL, NULL, NULL))) {
    if (!isRgn)
      markerPositions.Add(mPos);
  }
  struct SubEntry {
    double startTime;
    double endTime;
    const char *text;
    const char *linkedActorName;
  };

  WDL_PtrList_DOD<WDL_PtrList_DOD<SubEntry> > groups;
  WDL_PtrList_DOD<WDL_FastString> groupNames;

  WDL_PtrList_DOD<SubEntry> unlinkedGroup;
  WDL_FastString unlinkedName("");

  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!sub->IsValid()) continue;

    double pos, endPos;
    if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, NULL, NULL) < 0)
      continue;

    bool hasMarker = false;
    for (int m = 0; m < markerPositions.GetSize(); m++) {
      double mp = markerPositions.Get()[m];
      if (mp >= pos && mp < endPos) {
        hasMarker = true;
        break;
      }
    }
    if (!hasMarker) continue;

    const char *linkedName = "";
    const char *actorName = sub->GetActor();
    if (actorName[0]) {
      for (int j = 0; j < actors->GetSize(); j++) {
        SNM_Actor *a = actors->Get(j);
        if (!strcmp(a->GetName(), actorName)) {
          if (a->HasLinkedActor())
            linkedName = a->GetLinkedActorName();
          break;
        }
      }
    }

    SubEntry *entry = new SubEntry();
    entry->startTime = pos;
    entry->endTime = endPos;
    entry->text = sub->GetNotes();
    entry->linkedActorName = linkedName;

    if (!linkedName[0]) {
      unlinkedGroup.Add(entry);
    } else {
      WDL_PtrList_DOD<SubEntry> *targetGroup = NULL;
      for (int g = 0; g < groupNames.GetSize(); g++) {
        if (!strcmp(groupNames.Get(g)->Get(), linkedName)) {
          targetGroup = groups.Get(g);
          break;
        }
      }
      if (!targetGroup) {
        targetGroup = new WDL_PtrList_DOD<SubEntry>();
        groups.Add(targetGroup);
        groupNames.Add(new WDL_FastString(linkedName));
      }
      targetGroup->Add(entry);
    }
  }

  WDL_FastString output;

  auto appendGroup = [&output](WDL_PtrList_DOD<SubEntry> *grp) {
    for (int a = 0; a < grp->GetSize() - 1; a++) {
      for (int b = a + 1; b < grp->GetSize(); b++) {
        if (grp->Get(a)->startTime > grp->Get(b)->startTime) {
          SubEntry *tmp = grp->Get(a);
          grp->Set(a, grp->Get(b));
          grp->Set(b, tmp);
        }
      }
    }
    for (int j = 0; j < grp->GetSize(); j++) {
      SubEntry *e = grp->Get(j);
      char startBuf[32], endBuf[32];
      FormatSubTime(e->startTime, startBuf, sizeof(startBuf));
      FormatSubTime(e->endTime, endBuf, sizeof(endBuf));

      WDL_FastString text(e->text);
      for (int p = 0; p < text.GetLength();) {
        char c = text.Get()[p];
        bool isBreak = false;
        int breakLen = 0;
        if (c == '\\' && p + 1 < text.GetLength() && (text.Get()[p + 1] == 'N' || text.Get()[p + 1]
          == 'n')) {
          isBreak = true;
          breakLen = 2;
        } else if (c == '\r' && p + 1 < text.GetLength() && text.Get()[p + 1] == '\n') {
          isBreak = true;
          breakLen = 2;
        } else if (c == '\n' || c == '\r') {
          isBreak = true;
          breakLen = 1;
        }
        if (isBreak) {
          int start = p;
          while (start > 0 && text.Get()[start - 1] == ' ') start--;
          int end = p + breakLen;
          while (end < text.GetLength() && text.Get()[end] == ' ') end++;
          text.DeleteSub(start, end - start);
          text.Insert(" ", start);
          p = start + 1;
        } else {
          p++;
        }
      }

      output.AppendFormatted(512, "%s - %s %s\n", startBuf, endBuf, text.Get());
    }
  };

  if (unlinkedGroup.GetSize() > 0) {
    appendGroup(&unlinkedGroup);
  }

  for (int g = 0; g < groups.GetSize(); g++) {
    if (output.GetLength() > 0)
      output.Append("\n");
    output.AppendFormatted(256, "%s\n", groupNames.Get(g)->Get());
    appendGroup(groups.Get(g));
  }

  if (output.GetLength() > 0 && output.Get()[output.GetLength() - 1] == '\n')
    output.DeleteSub(output.GetLength() - 1, 1);

  if (output.GetLength() > 0)
    CF_SetClipboard(output.Get());
  else
    MessageBox(hwnd,
               __LOCALIZE("Нет маркеров внутри регионов.", "sws_DLG_152"),
               __LOCALIZE("ReNotes", "sws_DLG_152"),
               MB_OK);

  for (int i = 0; i < unlinkedGroup.GetSize(); i++)
    delete unlinkedGroup.Get(i);
  for (int g = 0; g < groups.GetSize(); g++) {
    for (int i = 0; i < groups.Get(g)->GetSize(); i++)
      delete groups.Get(g)->Get(i);
  }
}

static WDL_DLGRET LinkActorDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (INT_PTR r = SNM_HookThemeColorsMessage(hwndDlg, uMsg, wParam, lParam))
    return r;

  switch (uMsg) {
    case WM_INITDIALOG: {
      SetWindowLongPtr(hwndDlg, GWLP_USERDATA, lParam);
      SetDlgItemText(hwndDlg, -1, __LOCALIZE("Имя актёра:", "sws_DLG_152"));
      SetDlgItemText(hwndDlg, IDCANCEL, __LOCALIZE("Отменить", "sws_DLG_152"));
      HWND combo = GetDlgItem(hwndDlg, IDC_COMBO);
      WDL_UTF8_HookComboBox(combo);
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
      WDL_PtrList<const char> added;
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a->HasLinkedActor()) {
          const char *name = a->GetLinkedActorName();
          bool found = false;
          for (int j = 0; j < added.GetSize(); j++) {
            if (!strcmp(added.Get(j), name)) {
              found = true;
              break;
            }
          }
          if (!found) {
            SendMessage(combo, CB_ADDSTRING, 0, (LPARAM) name);
            added.Add(name);
          }
        }
      }
      SetWindowPos(hwndDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDOK: {
          char *buf = (char *) GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
          GetDlgItemText(hwndDlg, IDC_COMBO, buf, 256);
          EndDialog(hwndDlg, 1);
          return 0;
        }
        case IDCANCEL:
          EndDialog(hwndDlg, 0);
          return 0;
      }
      break;
  }
  return 0;
}

void NotesWnd::OnCommand(WPARAM wParam, LPARAM lParam) {
  switch (LOWORD(wParam)) {
    case IDC_EDIT1:
    case IDC_EDIT2:
      if (HIWORD(wParam) == EN_CHANGE && !m_settingText)
        SaveCurrentText(g_notesType, MarkProjectDirty == NULL);
      // MarkProjectDirty() avail. since v4.55pre2
      break;
    case WRAP_MSG:
      SetWrapText(!g_wrapText);
      break;
    case SAVE_GLOBAL_NOTES_MSG:
      WriteGlobalNotesToFile();
      RefreshGUI();
      break;
    case BTNID_LOCK:
      if (!HIWORD(wParam))
        ToggleLock();
      break;
    case BTNID_IMPORT_SUB:
      ImportSubTitleFile(NULL);
      break;
    case BTNID_CLEAR_SUBS:
      if (!HIWORD(wParam))
        ClearAllSubtitlesAction(NULL);
      break;
    case COLORED_REGIONS_MSG:
      ToggleColoredRegions(NULL);
      break;
    case DISPLAY_ACTOR_PREFIX_MSG:
      g_displayActorInPrefix = !g_displayActorInPrefix;
      UpdateRegionColors();
      ForceUpdateRgnSub();
      RefreshGUI();
      break;
    case ENABLE_ALL_MSG:
      EnableAllActors(NULL);
      break;
    case DISABLE_ALL_MSG:
      DisableAllActors(NULL);
      break;
    case COPY_MARKERS_MSG:
      CopyMarkersToClipboard(GetHWND());
      break;
    case COPY_ROLE_DISTRIBUTION_MSG: {
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

      WDL_PtrList_DOD<WDL_FastString> linkedNames;
      WDL_PtrList_DOD<WDL_FastString> charLists;

      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (!a->HasLinkedActor()) continue;

        const char *linked = a->GetLinkedActorName();
        int idx = -1;
        for (int j = 0; j < linkedNames.GetSize(); j++) {
          if (!strcmp(linkedNames.Get(j)->Get(), linked)) {
            idx = j;
            break;
          }
        }
        if (idx < 0) {
          linkedNames.Add(new WDL_FastString(linked));
          charLists.Add(new WDL_FastString());
          idx = linkedNames.GetSize() - 1;
        }
        charLists.Get(idx)->AppendFormatted(512, "- %s\n", a->GetName());
      }

      if (!linkedNames.GetSize()) {
        MessageBox(GetHWND(),
                   __LOCALIZE("Нет привязанных актёров.", "sws_DLG_152"),
                   __LOCALIZE("ReNotes", "sws_DLG_152"),
                   MB_OK);
        break;
      }

      WDL_FastString output;
      for (int i = 0; i < linkedNames.GetSize(); i++) {
        if (i > 0) output.Append("\n");
        output.AppendFormatted(512, "%s:\n", linkedNames.Get(i)->Get());
        output.Append(charLists.Get(i)->Get());
      }

      if (output.GetLength() > 0 && output.Get()[output.GetLength() - 1] == '\n')
        output.DeleteSub(output.GetLength() - 1, 1);

      CF_SetClipboard(output.Get());
    }
    break;
    case CMBID_TYPE:
      if (HIWORD(wParam) == CBN_SELCHANGE) {
        SetType(m_cbType.GetCurSel2());
        if (!g_locked)
          SetFocus(m_edit);
      }
      break;
    case CMBID_REGION:
      if (HIWORD(wParam) == CBN_SELCHANGE) {
        int selIdx = m_cbRegion.GetCurSel2();
        if (selIdx >= 0 && selIdx < m_overlappingRegionIds.GetSize()) {
          g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
          for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
            SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i);
            if (sub->GetId() == g_lastMarkerRegionId) {
              SetText(sub->GetNotes());
              break;
            }
          }
          RefreshGUI();
          if (!g_locked)
            SetFocus(m_edit);
        }
      }
      break;
    case LINK_ACTOR_MSG:
      if (m_contextMenuActor) {
        char reply[256] = "";
        if (DialogBoxParam(g_hInst,
                           MAKEINTRESOURCE(IDD_LINK_ACTOR),
                           m_hwnd,
                           LinkActorDlgProc,
                           (LPARAM)reply) && reply[0]) {
          m_contextMenuActor->SetLinkedActorName(reply);
          WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
          for (int i = 0; i < actors->GetSize(); i++) {
            SNM_Actor *a = actors->Get(i);
            if (a != m_contextMenuActor && a->HasLinkedActor()
              && !strcmp(a->GetLinkedActorName(), reply) && a->HasCustomColor()) {
              m_contextMenuActor->SetColor(a->GetColor());
              m_contextMenuActor->SetHasCustomColor(true);
              break;
            }
          }
          UpdateRegionColors();
          RefreshActorList();
          MarkProjectDirty(NULL);
        }
      }
      break;
    case UNLINK_ACTOR_MSG:
      if (m_contextMenuActor) {
        m_contextMenuActor->SetLinkedActorName("");
        m_contextMenuActor->SetHasCustomColor(false);
        m_contextMenuActor->SetColor(GenerateActorColor(m_contextMenuActor->GetName()));
        UpdateRegionColors();
        RefreshActorList();
        MarkProjectDirty(NULL);
      }
      break;
    case CHANGE_COLOR_MSG:
      if (m_contextMenuActor) {
        int color = m_contextMenuActor->GetEffectiveColor() & 0xFFFFFF;
        if (GR_SelectColor(m_hwnd, &color)) {
          color |= 0x1000000;
          if (m_contextMenuActor->HasLinkedActor()) {
            const char *linked = m_contextMenuActor->GetLinkedActorName();
            WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
            for (int i = 0; i < actors->GetSize(); i++) {
              SNM_Actor *a = actors->Get(i);
              if (a->HasLinkedActor() && !strcmp(a->GetLinkedActorName(), linked)) {
                a->SetColor(color);
                a->SetHasCustomColor(true);
              }
            }
          } else {
            m_contextMenuActor->SetColor(color);
            m_contextMenuActor->SetHasCustomColor(true);
          }
          UpdateRegionColors();
          RefreshActorList();
          MarkProjectDirty(NULL);
        }
      }
      break;
    case EXPORT_ROLES_MSG: {
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
      bool hasData = false;
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a->HasLinkedActor() || a->HasCustomColor()) {
          hasData = true;
          break;
        }
      }
      if (!hasData) {
        MessageBox(m_hwnd,
                   __LOCALIZE("Ничего не найдено для экспорта.", "sws_DLG_152"),
                   __LOCALIZE("ReNotes", "sws_DLG_152"),
                   MB_OK);
        break;
      }
      char fn[SNM_MAX_PATH] = "";
      if (BrowseForSaveFile(
        __LOCALIZE("ReNotes - Экспортировать ролёвку", "sws_DLG_152"),
        g_lastRolesFn,
        NULL,
        SNM_ROLES_EXT_LIST,
        fn,
        sizeof(fn))) {
        lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
        if (!ExportRolesFile(fn))
          MessageBox(m_hwnd,
                     __LOCALIZE("Не удалось экспортировать ролёвку.", "sws_DLG_152"),
                     __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                     MB_OK);
      }
    }
    break;
    case IMPORT_ROLES_MSG:
      if (char *fn = BrowseForFiles(
        __LOCALIZE("ReNotes - Импортировать ролёвку", "sws_DLG_152"),
        g_lastRolesFn,
        NULL,
        false,
        SNM_ROLES_EXT_LIST)) {
        lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
        if (ImportRolesFile(fn)) {
          UpdateRegionColors();
          RefreshActorList();
          ForceUpdateRgnSub();
          RefreshGUI();
          MarkProjectDirty(NULL);
        } else
          MessageBox(m_hwnd,
                     __LOCALIZE("Не удалось импортировать ролёвку.", "sws_DLG_152"),
                     __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                     MB_OK);
        free(fn);
      }
      break;
    default:
      Main_OnCommand((int) wParam, (int) lParam);
      break;
  }
}

// bWantEdit ignored: no list view in there
bool NotesWnd::IsActive(bool bWantEdit) {
  return (IsValidWindow() && (GetForegroundWindow() == m_hwnd || GetFocus() == m_edit));
}

HMENU NotesWnd::OnContextMenu(int x, int y, bool *wantDefaultItems) {
  HMENU hMenu = CreatePopupMenu();

  if (g_notesType == SNM_NOTES_RGN_SUB && m_actorListView) {
    HWND listHwnd = m_actorListView->GetHWND();
    RECT listRect;
    GetWindowRect(listHwnd, &listRect);
    POINT pt = {x, y};
    if (PtInRect(&listRect, pt) && g_actors.Get()->GetSize() > 0) {
      int iCol;
      SWS_ListItem *hitItem = m_actorListView->GetHitItem(x, y, &iCol);
      if (hitItem) {
        ActorListItem *actorItem = (ActorListItem *) hitItem;
        m_contextMenuActor = actorItem->actor;
        if (m_contextMenuActor) {
          if (m_contextMenuActor->HasLinkedActor())
            AddToMenu(hMenu,
                      __LOCALIZE("Отвязать актёра", "sws_DLG_152"),
                      UNLINK_ACTOR_MSG,
                      -1,
                      false);
          else
            AddToMenu(hMenu,
                      __LOCALIZE("Привязать актёра", "sws_DLG_152"),
                      LINK_ACTOR_MSG,
                      -1,
                      false);
          AddToMenu(hMenu,
                    m_contextMenuActor->HasLinkedActor()
                      ? __LOCALIZE("Изменить цвет (актёр)", "sws_DLG_152")
                      : __LOCALIZE("Изменить цвет", "sws_DLG_152"),
                    CHANGE_COLOR_MSG,
                    -1,
                    false);
          AddToMenu(hMenu, SWS_SEPARATOR, 0);
        }
      }
      AddToMenu(hMenu,
                __LOCALIZE("Все ✓", "sws_DLG_152"),
                ENABLE_ALL_MSG,
                -1,
                false);
      AddToMenu(hMenu,
                __LOCALIZE("Все 🗙", "sws_DLG_152"),
                DISABLE_ALL_MSG,
                -1,
                false);
      AddToMenu(hMenu, SWS_SEPARATOR, 0);
      AddToMenu(hMenu,
                __LOCALIZE("Импортировать ролёвку", "sws_DLG_152"),
                IMPORT_ROLES_MSG,
                -1,
                false);
      AddToMenu(hMenu,
                __LOCALIZE("Экспортировать ролёвку", "sws_DLG_152"),
                EXPORT_ROLES_MSG,
                -1,
                false);
      AddToMenu(hMenu, SWS_SEPARATOR, 0);
      AddToMenu(hMenu,
                __LOCALIZE("Скопировать ролёвку в буфер обмена", "sws_DLG_152"),
                COPY_ROLE_DISTRIBUTION_MSG,
                -1,
                false);
      *wantDefaultItems = false;
      return hMenu;
    }
  }

  if (g_notesType == SNM_NOTES_RGN_SUB) {
    AddToMenu(hMenu,
              __LOCALIZE("Отображать цветные регионы", "sws_DLG_152"),
              COLORED_REGIONS_MSG,
              -1,
              false);
    if (g_coloredRegions)
      CheckMenuItem(hMenu, COLORED_REGIONS_MSG, MF_BYCOMMAND | MF_CHECKED);

    AddToMenu(hMenu,
              __LOCALIZE("Отображать привязанного актёра в префиксе", "sws_DLG_152"),
              DISPLAY_ACTOR_PREFIX_MSG,
              -1,
              false);
    if (g_displayActorInPrefix)
      CheckMenuItem(hMenu, DISPLAY_ACTOR_PREFIX_MSG, MF_BYCOMMAND | MF_CHECKED);

    AddToMenu(hMenu, SWS_SEPARATOR, 0);
    AddToMenu(hMenu,
              __LOCALIZE("Скопировать маркеры в буфер обмена", "sws_DLG_152"),
              COPY_MARKERS_MSG,
              -1,
              false);
  }

  if (g_notesType == SNM_NOTES_GLOBAL)
    AddToMenu(hMenu,
              __LOCALIZE("Сохранить глобальные заметки", "sws_DLG_152"),
              SAVE_GLOBAL_NOTES_MSG,
              -1,
              false);
  return hMenu;
}

// OSX fix/workaround (SWELL bug?)
#ifdef _SNM_SWELL_ISSUES
void OSXForceTxtChangeJob::Perform() {
  if (NotesWnd *w = g_notesWndMgr.Get())
    SendMessage(w->GetHWND(), WM_COMMAND, MAKEWPARAM(IDC_EDIT1, EN_CHANGE), 0);
}
#endif


// returns:
// -1 = catch and send to the control
//  0 = pass-thru to main window (then -666 in SWS_DockWnd::keyHandler())
//  1 = eat
int NotesWnd::OnKey(MSG *_msg, int _iKeyState) {
  if (g_locked) {
    _msg->hwnd = m_hwnd; // redirect to main window
    return 0;
  } else if (_msg->hwnd == m_edit) {
#ifdef _SNM_SWELL_ISSUES
    // fix/workaround (SWELL bug?): EN_CHANGE is not always sent...
    ScheduledJob::Schedule(new OSXForceTxtChangeJob());
#endif
    return -1;
  } else if (_msg->message == WM_KEYDOWN || _msg->message == WM_CHAR) {
    // ctrl+A => select all
    if (_msg->wParam == 'A' && _iKeyState == LVKF_CONTROL) {
      SetFocus(m_edit);
      SendMessage(m_edit, EM_SETSEL, 0, -1);
      return 1;
    }
  }
  return 0;
}

void NotesWnd::OnTimer(WPARAM wParam) {
  if (wParam == UPDATE_TIMER) {
    // register to marker and region updates only when needed (less stress for REAPER)
    if (g_notesType == SNM_NOTES_RGN_SUB)
      RegisterToMarkerRegionUpdates(&m_mkrRgnListener); // no-op if alreday registered
    else
      UnregisterToMarkerRegionUpdates(&m_mkrRgnListener);

    // no update when editing text or when the view is hidden (e.g. inactive docker tab).
    // when the view is active: update only for regions and if the view is locked
    // => updates during playback, in other cases (e.g. item selection change) the main
    // window will be the active one, not our NotesWnd
    if (g_notesType != SNM_NOTES_PROJECT && g_notesType != SNM_NOTES_PROJECT_EXTRA && g_notesType !=
      SNM_NOTES_GLOBAL &&
      IsWindowVisible(m_hwnd) && (!IsActive() || (g_locked && g_notesType == SNM_NOTES_RGN_SUB))) {
      Update();
    }
  }
}

void NotesWnd::OnResize() {
  if (g_notesType != g_prevNotesType) {
    // room for buttons?
    if (g_notesType == SNM_NOTES_RGN_SUB) {
      m_resize.get_item(IDC_EDIT1)->orig.bottom = m_resize.get_item(IDC_EDIT1)->real_orig.bottom -
          41; //JFB!! 41 is tied to the current .rc!
      m_resize.get_item(IDC_EDIT2)->orig.bottom = m_resize.get_item(IDC_EDIT2)->real_orig.bottom -
          41; //JFB!! 41 is tied to the current .rc!
    } else {
      m_resize.get_item(IDC_EDIT1)->orig = m_resize.get_item(IDC_EDIT1)->real_orig;
      m_resize.get_item(IDC_EDIT2)->orig = m_resize.get_item(IDC_EDIT2)->real_orig;
    }
    InvalidateRect(m_hwnd, NULL, 0);
  }

  if (g_notesType == SNM_NOTES_RGN_SUB) {
    RECT r;
    GetClientRect(m_hwnd, &r);
    int panelWidth = max(150, (int) r.right / 6);
    m_resize.get_item(IDC_EDIT1)->orig.right = m_resize.get_item(IDC_EDIT1)->real_orig.right -
        panelWidth;
    m_resize.get_item(IDC_EDIT1)->orig.bottom = m_resize.get_item(IDC_EDIT1)->real_orig.bottom - 41;
    m_resize.get_item(IDC_EDIT2)->orig.right = m_resize.get_item(IDC_EDIT2)->real_orig.right -
        panelWidth;
    m_resize.get_item(IDC_EDIT2)->orig.bottom = m_resize.get_item(IDC_EDIT2)->real_orig.bottom - 41;
    HWND listHwnd = m_actorListView->GetHWND();
    int listBottom = r.bottom - 16;
    int rightMargin = SNM_GUI_X_MARGIN;
    SetWindowPos(listHwnd,
                 NULL,
                 r.right - panelWidth - rightMargin,
                 SNM_GUI_TOP_H,
                 panelWidth,
                 listBottom - SNM_GUI_TOP_H,
                 SWP_SHOWWINDOW);
    int vscrollWidth = GetSystemMetrics(SM_CXVSCROLL);
    ListView_SetColumnWidth(listHwnd, 0, 26);
    ListView_SetColumnWidth(listHwnd, 1, panelWidth - 26 - vscrollWidth);

    m_actorListView->Update();
  } else {
    ShowWindow(m_actorListView->GetHWND(), SW_HIDE);
    m_resize.get_item(IDC_EDIT1)->orig.right = m_resize.get_item(IDC_EDIT1)->real_orig.right;
    m_resize.get_item(IDC_EDIT2)->orig.right = m_resize.get_item(IDC_EDIT2)->real_orig.right;
  }
}

void NotesWnd::DrawControls(LICE_IBitmap *_bm, const RECT *_r, int *_tooltipHeight) {
  int h = SNM_GUI_TOP_H;
  if (_tooltipHeight)
    *_tooltipHeight = h;

  // "big" notes (dynamic font size)
  // drawn first so that it is displayed even with tiny sizing..
  if (g_locked) {
    // work on a copy rather than g_lastText (will get modified)
    char buf[sizeof(g_lastText)]{};
    GetWindowText(m_edit, buf, sizeof(buf));
    if (*buf) {
      RECT r = *_r;
      r.top += h;
      if (g_notesType == SNM_NOTES_RGN_SUB)
        r.right -= max(150, (int) (_r->right - _r->left) / 6);
      m_bigNotes.SetPosition(&r);

      m_bigNotes.ClearActorColors();
      if (g_notesType == SNM_NOTES_RGN_SUB) {
        int defaultRegionColor = 0;
        if (ColorTheme *ct = SNM_GetColorTheme())
          defaultRegionColor = ct->region;
        for (int i = 0; i < g_actors.Get()->GetSize(); i++) {
          SNM_Actor *actor = g_actors.Get()->Get(i);
          if (actor) {
            int color = actor->GetEffectiveColor();
            m_bigNotes.AddActorColor(actor->GetName(), color ? color : defaultRegionColor);
          }
        }
      }

      for (char *p = buf; p[0] && p[1]; p++) {
        if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n')) {
          p[0] = '\n';
          memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
      }

      m_bigNotes.SetText(buf);
      m_bigNotes.SetVisible(true);
    }
  }
  // clear last "big notes"
  else
    LICE_FillRect(_bm,
                  0,
                  h,
                  _bm->getWidth(),
                  _bm->getHeight() - h,
                  WDL_STYLE_GetSysColor(COLOR_WINDOW),
                  0.0,
                  LICE_BLIT_MODE_COPY);


  // 1st row of controls

  IconTheme *it = SNM_GetIconTheme();
  int x0 = _r->left + SNM_GUI_X_MARGIN;

  SNM_SkinButton(&m_btnLock,
                 it ? &it->toolbar_lock[!g_locked] : NULL,
                 g_locked
                   ? __LOCALIZE("Разблокировать текст", "sws_DLG_152")
                   : __LOCALIZE("Заблокировать текст", "sws_DLG_152"));
  if (SNM_AutoVWndPosition(DT_LEFT, &m_btnLock, NULL, _r, &x0, _r->top, h)) {
    if (SNM_AutoVWndPosition(DT_LEFT, &m_cbType, NULL, _r, &x0, _r->top, h)) {
      if (!g_locked && g_notesType == SNM_NOTES_RGN_SUB
        && m_overlappingRegionIds.GetSize() > 1) {
        SNM_AutoVWndPosition(DT_LEFT, &m_cbRegion, NULL, _r, &x0, _r->top, h);
        m_cbRegion.SetVisible(true);
      } else {
        m_cbRegion.SetVisible(false);
      }

      if (g_notesType == SNM_NOTES_RGN_SUB) {
        SNM_SkinToolbarButton(&m_btnImportSub, __LOCALIZE("Добавить субтитры", "sws_DLG_152"));
        if (SNM_AutoVWndPosition(DT_LEFT, &m_btnImportSub, NULL, _r, &x0, _r->top, h)) {
          SNM_SkinToolbarButton(&m_btnClearSubs, __LOCALIZE("Очистить", "sws_DLG_152"));
          SNM_AutoVWndPosition(DT_LEFT, &m_btnClearSubs, NULL, _r, &x0, _r->top, h);
        }
      }

      SNM_AddLogo(_bm, _r, x0, h);
    }
  }

  m_txtLabel.SetVisible(false);

  // 2nd row of controls
  x0 = _r->left + SNM_GUI_X_MARGIN;
  h = SNM_GUI_BOT_H;
  int y0 = _r->bottom - h;

  if (g_notesType == SNM_NOTES_RGN_SUB) {
    int panelWidth = max(150, (int) (_r->right - _r->left) / 6);
    int tableLeft = _r->right - panelWidth - SNM_GUI_X_MARGIN;

    int listTop = SNM_GUI_TOP_H;
    int listBottom = _r->bottom - 16;
    int listRight = tableLeft + panelWidth;
    int pencol = LICE_RGBA_FROMNATIVE(WDL_STYLE_GetSysColor(COLOR_3DSHADOW), 255);
    LICE_Line(_bm,
              tableLeft - 1,
              listTop - 1,
              listRight,
              listTop - 1,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
    LICE_Line(_bm,
              tableLeft - 1,
              listBottom,
              listRight,
              listBottom,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
    LICE_Line(_bm,
              tableLeft - 1,
              listTop - 1,
              tableLeft - 1,
              listBottom,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
    LICE_Line(_bm,
              listRight,
              listTop - 1,
              listRight,
              listBottom,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
  }

  if (g_locked)
    return;
}

bool NotesWnd::GetToolTipString(int _xpos, int _ypos, char *_bufOut, int _bufOutSz) {
  if (WDL_VWnd *v = m_parentVwnd.VirtWndFromPoint(_xpos, _ypos, 1)) {
    switch (v->GetID()) {
      case BTNID_LOCK:
        lstrcpyn(_bufOut,
                 g_locked
                   ? __LOCALIZE("Текст заблокирован", "sws_DLG_152")
                   : __LOCALIZE("Текст разблокирован", "sws_DLG_152"),
                 _bufOutSz);
        return true;
      case CMBID_TYPE:
        lstrcpyn(_bufOut, __LOCALIZE("Режим", "sws_DLG_152"), _bufOutSz);
        return true;
      case CMBID_REGION:
        lstrcpyn(_bufOut,
                 __LOCALIZE("Выберите регион для редактирования", "sws_DLG_152"),
                 _bufOutSz);
        return true;
    }
  }
  return false;
}

void NotesWnd::ToggleLock() {
  g_locked = !g_locked;
  RefreshToolbar(SWSGetCommandID(ToggleNotesLock));
  if (g_notesType == SNM_NOTES_RGN_SUB)
    Update(true); // play vs edit cursor when unlocking
  else
    RefreshGUI();

  if (!g_locked)
    SetFocus(m_edit);
}

void NotesWnd::RefreshActorList() {
  CleanupStaleActors();
  if (m_actorListView)
    m_actorListView->Update();
}


///////////////////////////////////////////////////////////////////////////////

void NotesWnd::SaveCurrentText(int _type, bool _wantUndo) {
  switch (_type) {
    case SNM_NOTES_PROJECT:
      SaveCurrentProjectNotes(_wantUndo);
      break;
    case SNM_NOTES_PROJECT_EXTRA:
      SaveCurrentExtraProjectNotes(_wantUndo);
      break;
    case SNM_NOTES_ITEM:
      SaveCurrentItemNotes(_wantUndo);
      break;
    case SNM_NOTES_TRACK:
      SaveCurrentTrackNotes(_wantUndo);
      break;
    case SNM_NOTES_GLOBAL:
      SaveCurrentGlobalNotes(_wantUndo);
      break;
    case SNM_NOTES_RGN_SUB:
      SaveCurrentRgnSub(_wantUndo);
      break;
  }
}

void NotesWnd::SaveCurrentProjectNotes(bool _wantUndo) {
  GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
  GetSetProjectNotes(NULL, true, g_lastText, sizeof(g_lastText));
  /* project notes are out of the undo system's scope, MarkProjectDirty is the best thing we can do...
      if (_wantUndo)
          Undo_OnStateChangeEx2(NULL, __LOCALIZE("Редактирование заметок проекта","sws_undo"), UNDO_STATE_ALL, -1);
      else
  */
  if (MarkProjectDirty)
    MarkProjectDirty(NULL);
}

void NotesWnd::SaveCurrentExtraProjectNotes(bool _wantUndo) {
  GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
  g_prjNotes.Get()->Set(g_lastText); // CRLF removed only when saving the project..
  MarkProjectDirty(NULL);
}

void NotesWnd::SaveCurrentItemNotes(bool _wantUndo) {
  if (g_mediaItemNote && GetMediaItem_Track(g_mediaItemNote)) {
    GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
    if (GetSetMediaItemInfo(g_mediaItemNote, "P_NOTES", g_lastText)) {
      //				UpdateItemInProject(g_mediaItemNote);
      UpdateTimeline(); // for the item's note button
      MarkProjectDirty(NULL);
    }
  }
}

void NotesWnd::SaveCurrentTrackNotes(bool _wantUndo) {
  if (g_trNote && CSurf_TrackToID(g_trNote, false) >= 0) {
    GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
    if (SNM_TrackNotes *notes = SNM_TrackNotes::find(g_trNote))
      notes->SetNotes(g_lastText); // CRLF removed only when saving the project
    else
      g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(nullptr, TrackToGuid(g_trNote), g_lastText));

    MarkProjectDirty(NULL);
  }
}

void NotesWnd::SaveCurrentGlobalNotes(bool _wantUndo) {
  GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
  g_globalNotes.Set(g_lastText);
  /* dirty / modified status is displayed directly in notes window
  if (_wantUndo)
      Undo_OnStateChangeEx2(NULL, __LOCALIZE("Редактирование глобальных заметок", "sws_undo"), UNDO_STATE_MISCCFG, -1);
  else
      MarkProjectDirty(NULL);
  */

  g_globalNotesDirty = true;
  RefreshGUI(); // to display [modified] in notes window immediately
}

void NotesWnd::SaveCurrentRgnSub(bool _wantUndo) {
  if (g_lastMarkerRegionId > 0) {
    GetWindowText(m_edit, g_lastText, sizeof(g_lastText));

    SNM_RegionSubtitle *curSub = NULL;
    for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
      if (g_pRegionSubs.Get()->Get(i)->GetId() == g_lastMarkerRegionId) {
        curSub = g_pRegionSubs.Get()->Get(i);
        curSub->SetNotes(g_lastText);
        break;
      }
    }
    if (!curSub) {
      curSub = new SNM_RegionSubtitle(nullptr, g_lastMarkerRegionId, g_lastText);
      g_pRegionSubs.Get()->Add(curSub);
    }

    double pos, end;
    int num, color;
    bool isRgn;
    if (EnumMarkerRegionById(NULL, g_lastMarkerRegionId, &isRgn, &pos, &end, NULL, &num, &color) >=
      0) {
      WDL_String regionName;
      BuildRegionName(&regionName, curSub->GetActor(), g_lastText);

      g_internalMkrRgnChange = true;
      SetProjectMarker4(NULL,
                        num,
                        isRgn,
                        pos,
                        end,
                        regionName.Get(),
                        color,
                        !*regionName.Get() ? 1 : 0);
      UpdateTimeline();
    }

    MarkProjectDirty(NULL);
  }
}

///////////////////////////////////////////////////////////////////////////////

void NotesWnd::Update(bool _force) {
  static bool sRecurseCheck = false;
  if (sRecurseCheck)
    return;

  sRecurseCheck = true;

  // force refresh if needed
  if (_force || g_notesType != g_prevNotesType) {
    g_prevNotesType = g_notesType;
    g_mediaItemNote = NULL;
    g_trNote = NULL;
    g_lastMarkerPos = -1.0;
    g_lastMarkerRegionId = -1;
    _force = true; // trick for RefreshGUI() below..
  }

  // update
  int refreshType = NO_REFRESH;
  switch (g_notesType) {
    case SNM_NOTES_PROJECT: {
      char buf[MAX_HELP_LENGTH];
      GetSetProjectNotes(NULL, false, buf, sizeof(buf));
      SetText(buf);
      refreshType = REQUEST_REFRESH;
      break;
    }
    case SNM_NOTES_PROJECT_EXTRA:
      SetText(g_prjNotes.Get()->Get());
      refreshType = REQUEST_REFRESH;
      break;
    case SNM_NOTES_ITEM:
      refreshType = UpdateItemNotes();
      break;
    case SNM_NOTES_TRACK:
      refreshType = UpdateTrackNotes();
      break;
    case SNM_NOTES_GLOBAL:
      SetText(g_globalNotes.Get());
      refreshType = REQUEST_REFRESH;
      break;
    case SNM_NOTES_RGN_SUB:
      refreshType = UpdateRgnSub();
      break;
  }

  if (_force || refreshType == REQUEST_REFRESH)
    RefreshGUI();

  if (g_notesType == SNM_NOTES_RGN_SUB)
    RefreshActorList();

  sRecurseCheck = false;
}

int NotesWnd::UpdateItemNotes() {
  int refreshType = NO_REFRESH;
  if (MediaItem *selItem = GetSelectedMediaItem(NULL, 0)) {
    if (selItem != g_mediaItemNote) {
      g_mediaItemNote = selItem;
      if (char *notes = (char *) GetSetMediaItemInfo(g_mediaItemNote, "P_NOTES", NULL))
        SetText(notes, false);
      refreshType = REQUEST_REFRESH;
    }
  } else if (g_mediaItemNote || *g_lastText) {
    g_mediaItemNote = NULL;
    SetText("");
    refreshType = REQUEST_REFRESH;
  }
  return refreshType;
}

int NotesWnd::UpdateTrackNotes() {
  int refreshType = NO_REFRESH;
  if (MediaTrack *selTr = SNM_GetSelectedTrack(NULL, 0, true)) {
    if (selTr != g_trNote) {
      g_trNote = selTr;

      if (SNM_TrackNotes *notes = SNM_TrackNotes::find(g_trNote)) {
        SetText(notes->GetNotes());
        return REQUEST_REFRESH;
      }

      g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(nullptr, TrackToGuid(g_trNote), ""));
      SetText("");
      refreshType = REQUEST_REFRESH;
    }
  } else if (g_trNote || *g_lastText) {
    g_trNote = NULL;
    SetText("");
    refreshType = REQUEST_REFRESH;
  }
  return refreshType;
}

int NotesWnd::UpdateRgnSub() {
  int refreshType = NO_REFRESH;

  double dPos = GetCursorPositionEx(NULL), accuracy = SNM_FUDGE_FACTOR;
  if (g_locked && GetPlayStateEx(NULL)) {
    dPos = GetPlayPositionEx(NULL);
    accuracy = 0.1;
  }

  if (fabs(g_lastMarkerPos - dPos) > accuracy) {
    g_lastMarkerPos = dPos;

    int id, idx = FindMarkerRegion(NULL, dPos, SNM_REGION_MASK, &id);
    if (id > 0) {
      WDL_TypedBuf<int> newOverlappingIds;
      int enumIdx = 0;
      bool isRgn;
      double p1, p2;
      int num;
      while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &p1, &p2, NULL, &num))) {
        if (isRgn && dPos >= p1 && dPos <= p2) {
          int regionId = MakeMarkerRegionId(num, true);
          for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
            if (g_pRegionSubs.Get()->Get(i)->GetId() == regionId) {
              newOverlappingIds.Add(regionId);
              break;
            }
          }
        }
      }

      bool regionsChanged = false;
      if (newOverlappingIds.GetSize() != m_overlappingRegionIds.GetSize())
        regionsChanged = true;
      else {
        for (int i = 0; i < newOverlappingIds.GetSize(); i++) {
          if (newOverlappingIds.Get()[i] != m_overlappingRegionIds.Get()[i]) {
            regionsChanged = true;
            break;
          }
        }
      }

      if (regionsChanged) {
        m_overlappingRegionIds.Resize(newOverlappingIds.GetSize());
        memcpy(m_overlappingRegionIds.Get(),
               newOverlappingIds.Get(),
               newOverlappingIds.GetSize() * sizeof(int));

        m_cbRegion.Empty();
        for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
          int regId = m_overlappingRegionIds.Get()[i];
          for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
            SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(j);
            if (sub->GetId() == regId) {
              char itemText[256];
              const char *actor = sub->GetActor();
              if (actor && *actor)
                snprintf(itemText, sizeof(itemText), "%s", actor);
              else {
                double pos, end;
                int num;
                if (EnumMarkerRegionById(NULL, regId, NULL, &pos, &end, NULL, &num, NULL) >= 0)
                  snprintf(itemText, sizeof(itemText), "R%d", num);
                else
                  snprintf(itemText, sizeof(itemText), "Region %d", i + 1);
              }
              m_cbRegion.AddItem(itemText);
              break;
            }
          }
        }

        int selIdx = 0;
        for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
          if (m_overlappingRegionIds.Get()[i] == g_lastMarkerRegionId) {
            selIdx = i;
            break;
          }
        }
        m_cbRegion.SetCurSel2(selIdx);

        if (m_overlappingRegionIds.GetSize() > 0)
          g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
      }

      if (g_lastMarkerRegionId <= 0 && m_overlappingRegionIds.GetSize() > 0) {
        int selIdx = m_cbRegion.GetCurSel2();
        if (selIdx < 0) selIdx = 0;
        g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
      }

      if (g_locked && m_overlappingRegionIds.GetSize() > 0) {
        int uniqueActorCount = 0;
        int unknownActorCount = 0;
        const char *firstActor = NULL;
        for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
          int regId = m_overlappingRegionIds.Get()[i];
          for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
            SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(j);
            if (sub->GetId() == regId) {
              const char *actor = sub->GetActor();
              if (!actor || !*actor || !strcmp(actor, "?"))
                unknownActorCount++;
              if (!actor || !*actor) actor = "?";
              if (!firstActor) {
                firstActor = actor;
                uniqueActorCount = 1;
              } else if (strcmp(firstActor, actor)) {
                uniqueActorCount = 2;
              }
              break;
            }
          }
        }
        bool showDefaultPrefix = uniqueActorCount >= 2 || unknownActorCount >= 2;

        WDL_FastString displayText;
        bool first = true;
        for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
          int regId = m_overlappingRegionIds.Get()[i];
          for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
            SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(j);
            if (sub->GetId() == regId) {
              if (!first)
                displayText.Append("\r\n");
              const char *actor = sub->GetActor();
              bool hasActor = actor && *actor && strcmp(actor, "?");
              if (hasActor || showDefaultPrefix)
                AppendDisplayLine(&displayText, hasActor ? actor : "?", sub->GetNotes());
              else
                displayText.Append(sub->GetNotes());
              first = false;
              break;
            }
          }
        }
        SetText(displayText.Get());
      } else if (g_lastMarkerRegionId > 0) {
        bool found = false;
        for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
          SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i);
          if (sub->GetId() == g_lastMarkerRegionId) {
            SetText(sub->GetNotes());
            found = true;
            break;
          }
        }
        if (!found) {
          g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(nullptr, id, ""));
          SetText("");
        }
      } else {
        g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(nullptr, id, ""));
        SetText("");
      }
      refreshType = REQUEST_REFRESH;
    } else if (g_lastMarkerRegionId > 0 || *g_lastText) {
      g_lastMarkerPos = -1.0;
      g_lastMarkerRegionId = -1;
      m_overlappingRegionIds.Resize(0);
      m_cbRegion.Empty();
      SetText("");
      refreshType = REQUEST_REFRESH;
    }
  }
  return refreshType;
}

void NotesWnd::ForceUpdateRgnSub() {
  if (g_locked && m_overlappingRegionIds.GetSize() > 0) {
    int uniqueActorCount = 0;
    int unknownActorCount = 0;
    const char *firstActor = NULL;
    for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
      int regId = m_overlappingRegionIds.Get()[i];
      for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
        SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(j);
        if (sub->GetId() == regId) {
          const char *actor = sub->GetActor();
          if (!actor || !*actor || !strcmp(actor, "?"))
            unknownActorCount++;
          if (!actor || !*actor) actor = "?";
          if (!firstActor) {
            firstActor = actor;
            uniqueActorCount = 1;
          } else if (strcmp(firstActor, actor)) {
            uniqueActorCount = 2;
          }
          break;
        }
      }
    }
    bool showDefaultPrefix = uniqueActorCount >= 2 || unknownActorCount >= 2;

    WDL_FastString displayText;
    bool first = true;
    for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
      int regId = m_overlappingRegionIds.Get()[i];
      for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
        SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(j);
        if (sub->GetId() == regId) {
          if (!first)
            displayText.Append("\r\n");
          const char *actor = sub->GetActor();
          bool hasActor = actor && *actor && strcmp(actor, "?");
          if (hasActor || showDefaultPrefix)
            AppendDisplayLine(&displayText, hasActor ? actor : "?", sub->GetNotes());
          else
            displayText.Append(sub->GetNotes());
          first = false;
          break;
        }
      }
    }
    SetText(displayText.Get());
    RefreshGUI();
  } else if (g_lastMarkerRegionId > 0) {
    for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
      SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i);
      if (sub->GetId() == g_lastMarkerRegionId) {
        SetText(sub->GetNotes());
        if (g_locked)
          RefreshGUI();
        break;
      }
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// Encode/decode notes (to/from RPP format)
// WDL's cfg_encode_textblock() & cfg_decode_textblock() would not help here..
///////////////////////////////////////////////////////////////////////////////

bool GetStringFromNotesChunk(WDL_FastString *_notesIn, char *_bufOut, int _bufOutSz) {
  if (!_bufOut || !_notesIn)
    return false;

  memset(_bufOut, 0, _bufOutSz);
  const char *pNotes = _notesIn->Get();
  if (pNotes && *pNotes) {
    // find 1st '|'
    int i = 0;
    while (pNotes[i] && pNotes[i] != '|') i++;
    if (pNotes[i]) i++;
    else return true;

    int j = 0;
    while (pNotes[i] && j < _bufOutSz) {
      if (pNotes[i] != '\r' && pNotes[i] != '\n') {
        _bufOut[j++] = (pNotes[i] == '|' && pNotes[i - 1] == '\n' ? '\n' : pNotes[i]);
        // i is >0 here
      }
      i++;
    }
    if (j >= 1 && !strcmp(_bufOut + j - 1, ">")) // remove trailing ">", if any
      _bufOut[j - 1] = '\0';
  }
  return true;
}

bool GetNotesChunkFromString(const char *_bufIn,
                             WDL_FastString *_notesOut,
                             const char *_startLine) {
  if (_notesOut && _bufIn) {
    if (!_startLine) _notesOut->Set("<NOTES\n|");
    else _notesOut->Set(_startLine);

    int i = 0;
    while (_bufIn[i]) {
      if (_bufIn[i] == '\n')
        _notesOut->Append("\n|");
      else if (_bufIn[i] != '\r')
        _notesOut->Append(_bufIn + i, 1);
      i++;
    }
    _notesOut->Append("\n>\n");
    return true;
  }
  return false;
}


///////////////////////////////////////////////////////////////////////////////
// NotesUpdateJob
///////////////////////////////////////////////////////////////////////////////

void NotesUpdateJob::Perform() {
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->Update(true);
}


///////////////////////////////////////////////////////////////////////////////
// NotesMarkerRegionListener
///////////////////////////////////////////////////////////////////////////////

// ScheduledJob because of multi-notifs during project switches (vs CSurfSetTrackListChange)
void NotesMarkerRegionListener::NotifyMarkerRegionUpdate(int _updateFlags) {
  if (g_notesType == SNM_NOTES_RGN_SUB) {
    if (g_internalMkrRgnChange)
      g_internalMkrRgnChange = false;
    else
      ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
  }
}


///////////////////////////////////////////////////////////////////////////////

void StripAssFormattingTags(WDL_FastString *text) {
  WDL_FastString result;
  const char *p = text->Get();
  while (*p) {
    if (*p == '{' && *(p + 1) == '\\') {
      while (*p && *p != '}')
        p++;
      if (*p == '}')
        p++;
    } else if (*p == '\\' && (*(p + 1) == 'N' || *(p + 1) == 'n')) {
      result.Append("\n");
      p += 2;
    } else {
      result.Append(p, 1);
      p++;
    }
  }
  text->Set(result.Get());
}

void StripSrtFormattingTags(WDL_FastString *text) {
  WDL_FastString result;
  const char *p = text->Get();
  while (*p) {
    if (*p == '<') {
      while (*p && *p != '>')
        p++;
      if (*p == '>')
        p++;
    } else {
      result.Append(p, 1);
      p++;
    }
  }
  text->Set(result.Get());
}

static double OklchSrgbGamma(double x) {
  if (x >= 0.0031308)
    return 1.055 * pow(x, 1.0 / 2.4) - 0.055;
  return 12.92 * x;
}

int GenerateActorColor(const char *actorName) {
  unsigned int hash = 2166136261u;
  const char *p = actorName;
  while (*p) {
    hash ^= (unsigned char) *p++;
    hash *= 16777619u;
  }
  hash ^= hash >> 16;
  hash *= 0x45d9f3bu;
  hash ^= hash >> 16;

  double hDeg = ((hash & 0x3FF) / 1024.0) * 360.0;
  double ch = 0.08 + ((hash >> 10) & 0xFF) / 255.0 * 0.09;
  double lk = 0.45 + ((hash >> 18) & 0xFF) / 255.0 * 0.20;

  double hRad = hDeg * (3.14159265358979323846 / 180.0);
  double a = ch * cos(hRad);
  double b = ch * sin(hRad);

  double l_ = lk + 0.3963377774 * a + 0.2158037573 * b;
  double m_ = lk - 0.1055613458 * a - 0.0638541728 * b;
  double s_ = lk - 0.0894841775 * a - 1.2914855480 * b;

  double l = l_ * l_ * l_;
  double m = m_ * m_ * m_;
  double s = s_ * s_ * s_;

  double rd = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
  double gd = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
  double bd = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

  if (rd < 0.0) rd = 0.0;
  else if (rd > 1.0) rd = 1.0;
  if (gd < 0.0) gd = 0.0;
  else if (gd > 1.0) gd = 1.0;
  if (bd < 0.0) bd = 0.0;
  else if (bd > 1.0) bd = 1.0;

  int ri = (int) (OklchSrgbGamma(rd) * 255.0 + 0.5);
  int gi = (int) (OklchSrgbGamma(gd) * 255.0 + 0.5);
  int bi = (int) (OklchSrgbGamma(bd) * 255.0 + 0.5);

  return RGB(ri, gi, bi) | 0x1000000;
}

int SNM_Actor::GetEffectiveColor() const {
  if (m_hasCustomColor)
    return m_color;
  if (m_linkedActorName.GetLength() > 0)
    return GenerateActorColor(m_linkedActorName.Get());
  return m_color;
}

SNM_Actor *FindOrCreateActor(const char *name) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    if (!strcmp(actors->Get(i)->GetName(), name))
      return actors->Get(i);
  int color = strcmp(name, "?") ? GenerateActorColor(name) : 0;
  SNM_Actor *actor = new SNM_Actor(name, color);
  actors->Add(actor);
  return actor;
}

void CleanupStaleActors() {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();

  for (int i = actors->GetSize() - 1; i >= 0; i--) {
    SNM_Actor *actor = actors->Get(i);
    bool hasSubtitle = false;

    for (int j = 0; j < subs->GetSize(); j++) {
      SNM_RegionSubtitle *sub = subs->Get(j);
      if (!strcmp(sub->GetActor(), actor->GetName())) {
        hasSubtitle = true;
        break;
      }
    }

    if (!hasSubtitle) {
      actors->Delete(i, true);
    }
  }
}

enum { COL_ENABLED = 0, COL_NAME, COL_COUNT };

static SWS_LVColumn g_actorCols[] = {
  {30, 2, ""},
  {100, 0, "Actor"}
};

ActorListView::ActorListView(HWND hwndList, HWND hwndEdit)
  : SWS_ListView(hwndList,
                 hwndEdit,
                 COL_COUNT,
                 g_actorCols,
                 "ACTORLISTVIEWSTATE",
                 false,
                 "sws_DLG_152") {
  m_iSortCol = COL_NAME + 1;
}

void ActorListView::GetItemList(SWS_ListItemList *pList) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    pList->Add((SWS_ListItem *) new ActorListItem(actors->Get(i)), false);
}

void ActorListView::GetItemText(SWS_ListItem *item, int iCol, char *str, int iStrMax) {
  if (!item || !str || iStrMax <= 0) {
    if (str && iStrMax > 0) str[0] = 0;
    return;
  }

  ActorListItem *actorItem = (ActorListItem *) item;
  if (!actorItem || !actorItem->actor) {
    str[0] = 0;
    return;
  }

  SNM_Actor *actor = actorItem->actor;

  switch (iCol) {
    case COL_ENABLED:
      lstrcpyn(str, actor->IsEnabled() ? UTF8_CHECKMARK : "", iStrMax);
      break;
    case COL_NAME:
      lstrcpyn(str, actor->GetName(), iStrMax);
      break;
    default:
      str[0] = 0;
      break;
  }
}

int ActorListView::OnItemSort(SWS_ListItem *item1, SWS_ListItem *item2) {
  ActorListItem *actorItem1 = (ActorListItem *) item1;
  ActorListItem *actorItem2 = (ActorListItem *) item2;

  if (!actorItem1 || !actorItem1->actor || !actorItem2 || !actorItem2->actor)
    return 0;

  int result = _stricmp(actorItem1->actor->GetName(), actorItem2->actor->GetName());

  if (m_iSortCol < 0)
    return -result;
  return result;
}

void ActorListView::OnItemClk(SWS_ListItem *item, int iCol, int iKeyState) {
  if (!item)
    return;

  ActorListItem *actorItem = (ActorListItem *) item;
  if (!actorItem || !actorItem->actor)
    return;

  SNM_Actor *clickedActor = actorItem->actor;
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

  if (iKeyState & LVKF_SHIFT) {
    bool clickedEnabled = clickedActor->IsEnabled();

    bool othersEnabled = false;
    for (int i = 0; i < actors->GetSize(); i++) {
      SNM_Actor *a = actors->Get(i);
      if (a != clickedActor && a->IsEnabled()) {
        othersEnabled = true;
        break;
      }
    }

    if (!clickedEnabled) {
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a == clickedActor) {
          a->SetEnabled(true);
          RecreateActorRegions(a->GetName());
        } else if (a->IsEnabled()) {
          a->SetEnabled(false);
          DeleteActorRegions(a->GetName());
        }
      }
    } else if (othersEnabled) {
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a != clickedActor && a->IsEnabled()) {
          a->SetEnabled(false);
          DeleteActorRegions(a->GetName());
        }
      }
    } else {
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a == clickedActor) {
          a->SetEnabled(false);
          DeleteActorRegions(a->GetName());
        } else {
          a->SetEnabled(true);
          RecreateActorRegions(a->GetName());
        }
      }
    }
    UpdateTimeline();
  } else {
    bool newState = !clickedActor->IsEnabled();
    clickedActor->SetEnabled(newState);

    if (newState)
      RecreateActorRegions(clickedActor->GetName());
    else {
      DeleteActorRegions(clickedActor->GetName());
      UpdateTimeline();
    }
  }

  Update();
}

bool ActorListView::CustomDrawItem(HDC hdc, SWS_ListItem *item, int iCol, RECT *r, bool selected) {
  if (!item || !hdc || !r)
    return false;

  ActorListItem *actorItem = (ActorListItem *) item;
  if (!actorItem || !actorItem->actor)
    return false;

  int sz;
  ColorTheme *ctheme = (ColorTheme *) GetColorThemeStruct(&sz);
  if (!ctheme || sz < (int) sizeof(ColorTheme))
    return false;

  int bgColor = selected
                  ? (GetFocus() == m_hwndList
                       ? ctheme->genlist_sel[0]
                       : ctheme->genlist_selinactive[0])
                  : ctheme->genlist_bg;
  HBRUSH bgBrush = CreateSolidBrush(bgColor);
  FillRect(hdc, r, bgBrush);
  DeleteObject(bgBrush);

  int gridColor = ctheme->genlist_gridlines;
  if (gridColor != bgColor && iCol == COL_NAME) {
    RECT listRect;
    GetClientRect(m_hwndList, &listRect);
    HPEN pen = CreatePen(PS_SOLID, 0, gridColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, listRect.left, r->bottom - 1, NULL);
    LineTo(hdc, listRect.right, r->bottom - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
  }

  if (iCol == COL_ENABLED) {
    int textColor = selected
                      ? (GetFocus() == m_hwndList
                           ? ctheme->genlist_sel[1]
                           : ctheme->genlist_selinactive[1])
                      : ctheme->genlist_fg;
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);
    const char *text = actorItem->actor->IsEnabled() ? UTF8_CHECKMARK : "";
    DrawTextUTF8(hdc, text, -1, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (iCol == COL_NAME) {
    int h = r->bottom - r->top;
    int dotSize = 8;
    int dotX = r->left + 6;
    int textLeft = dotX + dotSize + 4;

    int dotColor = actorItem->actor->GetEffectiveColor();
    HBRUSH dotBrush = CreateSolidBrush((dotColor ? dotColor : ctheme->region) & 0xFFFFFF);

    int textColor = selected
                      ? (GetFocus() == m_hwndList
                           ? ctheme->genlist_sel[1]
                           : ctheme->genlist_selinactive[1])
                      : ctheme->genlist_fg;
    SetBkMode(hdc, TRANSPARENT);

    if (actorItem->actor->HasLinkedActor()) {
      int halfH = h / 2;
      int dotY = r->top + (halfH - dotSize) / 2;
      RECT dotRect = {dotX, dotY, dotX + dotSize, dotY + dotSize};
      FillRect(hdc, &dotRect, dotBrush);

      SetTextColor(hdc, textColor);
      RECT topRect = {textLeft, r->top, r->right, r->top + halfH};
      DrawTextUTF8(hdc,
                   actorItem->actor->GetName(),
                   -1,
                   &topRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

      int linkedColor = actorItem->actor->GetEffectiveColor();
      SetTextColor(hdc, (linkedColor ? linkedColor : ctheme->region) & 0xFFFFFF);
      RECT botRect = {textLeft, r->top + halfH, r->right, r->bottom};
      DrawTextUTF8(hdc,
                   actorItem->actor->GetLinkedActorName(),
                   -1,
                   &botRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
      int dotY = r->top + (h - dotSize) / 2;
      RECT dotRect = {dotX, dotY, dotX + dotSize, dotY + dotSize};
      FillRect(hdc, &dotRect, dotBrush);

      SetTextColor(hdc, textColor);
      RECT textRect = {textLeft, r->top, r->right, r->bottom};
      DrawTextUTF8(hdc,
                   actorItem->actor->GetName(),
                   -1,
                   &textRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    DeleteObject(dotBrush);
  }

  return true;
}

bool ActorListView::GetCustomColumnColor(SWS_ListItem *item,
                                         int iCol,
                                         bool selected,
                                         COLORREF *textColor) {
  if (!item || !textColor)
    return false;

  ActorListItem *actorItem = (ActorListItem *) item;
  if (!actorItem || !actorItem->actor)
    return false;

  if (iCol == COL_NAME) {
    int sz;
    ColorTheme *ctheme = (ColorTheme *) GetColorThemeStruct(&sz);
    int color = actorItem->actor->GetEffectiveColor();
    if (color) {
      *textColor = color & 0xFFFFFF;
      return true;
    }
    if (ctheme && sz >= (int) sizeof(ColorTheme)) {
      *textColor = ctheme->region & 0xFFFFFF;
      return true;
    }
  }

  return false;
}

bool ImportAssFile(const char *_fn) {
  bool ok = false;
  double firstPos = -1.0;
  bool inEventsSection = false;

  if (FILE *f = fopenUTF8(_fn, "rt")) {
    char buf[4096];
    if (fgets(buf, 4, f) && strcmp("\xEF\xBB\xBF", buf))
      rewind(f);

    while (fgets(buf, sizeof(buf), f)) {
      if (buf[0] == '[') {
        inEventsSection = (_strnicmp(buf, "[Events]", 8) == 0);
        continue;
      }

      if (!inEventsSection)
        continue;

      if (_strnicmp(buf, "Dialogue:", 9) != 0)
        continue;

      const char *p = buf + 9;
      while (*p == ' ')
        p++;

      int field = 0;
      int p1[3] = {0}, p1cs = 0;
      int p2[3] = {0}, p2cs = 0;
      const char *textStart = NULL;
      const char *actorStart = NULL;
      int actorLen = 0;
      bool validTimes = true;

      while (*p && field < 9) {
        const char *fieldStart = p;
        while (*p && *p != ',')
          p++;

        if (field == 1) {
          if (sscanf(fieldStart, "%d:%d:%d.%d", &p1[0], &p1[1], &p1[2], &p1cs) != 4)
            validTimes = false;
        } else if (field == 2) {
          if (sscanf(fieldStart, "%d:%d:%d.%d", &p2[0], &p2[1], &p2[2], &p2cs) != 4)
            validTimes = false;
        } else if (field == 4) {
          actorStart = fieldStart;
          const char *actorEnd = p;
          actorLen = (int) (actorEnd - actorStart);
        }

        if (*p == ',')
          p++;
        field++;
      }

      if (field == 9)
        textStart = p;

      if (!validTimes)
        continue;

      if (!textStart || !*textStart)
        continue;

      const char *textEnd = textStart;
      while (*textEnd && *textEnd != '\r' && *textEnd != '\n')
        textEnd++;

      WDL_FastString notes;
      notes.Append(textStart, (int) (textEnd - textStart));
      StripAssFormattingTags(&notes);

      WDL_FastString actor;
      if (actorStart && actorLen > 0)
        actor.Append(actorStart, actorLen);
      else
        actor.Set("?");

      SNM_Actor *actorObj = FindOrCreateActor(actor.Get());

      WDL_String name;
      BuildRegionName(&name, actor.Get(), notes.Get());

      double startTime = p1[0] * 3600 + p1[1] * 60 + p1[2] + double(p1cs) / 100;
      double endTime = p2[0] * 3600 + p2[1] * 60 + p2[2] + double(p2cs) / 100;

      int color = g_coloredRegions ? actorObj->GetEffectiveColor() : 0;
      int num = AddProjectMarker2(NULL,
                                  true,
                                  startTime,
                                  endTime,
                                  name.Get(),
                                  -1,
                                  color);
      if (num >= 0) {
        ok = true;
        if (firstPos < 0.0)
          firstPos = startTime;
        int id = MakeMarkerRegionId(num, true);
        if (id > 0) {
          SNM_RegionSubtitle *sub = new SNM_RegionSubtitle(nullptr, id, notes.Get());
          sub->SetActor(actor.Get());
          sub->SetTimes(startTime, endTime);
          g_pRegionSubs.Get()->Add(sub);
        }
      }
    }
    fclose(f);
  }

  if (ok) {
    UpdateTimeline();
    if (firstPos > 0.0)
      SetEditCurPos2(NULL, firstPos, true, false);
  }
  return ok;
}

bool ImportSubRipFile(const char *_fn) {
  bool ok = false;
  double firstPos = -1.0;

  // no need to check extension here, it's done for us
  if (FILE *f = fopenUTF8(_fn, "rt")) {
    char buf[1024];
    if (fgets(buf, 4, f) && strcmp("\xEF\xBB\xBF", buf)) // UTF-8 BOM
      rewind(f);
    while (fgets(buf, sizeof(buf), f) && *buf) {
      if (int num = atoi(buf)) {
        if (fgets(buf, sizeof(buf), f) && *buf) {
          int p1[4], p2[4];
          if (sscanf(buf,
                     "%d:%d:%d,%d --> %d:%d:%d,%d",
                     &p1[0],
                     &p1[1],
                     &p1[2],
                     &p1[3],
                     &p2[0],
                     &p2[1],
                     &p2[2],
                     &p2[3]) != 8) {
            break;
          }

          WDL_FastString notes;
          while (fgets(buf, sizeof(buf), f) && *buf) {
            if (*buf == '\r' || *buf == '\n') break;
            notes.Append(buf);
          }
          StripSrtFormattingTags(&notes);

          SNM_Actor *actorObj = FindOrCreateActor("?");

          WDL_String name;
          BuildRegionName(&name, "?", notes.Get());

          double startTime = p1[0] * 3600 + p1[1] * 60 + p1[2] + double(p1[3]) / 1000;
          double endTime = p2[0] * 3600 + p2[1] * 60 + p2[2] + double(p2[3]) / 1000;

          int color = g_coloredRegions ? actorObj->GetEffectiveColor() : 0;
          num = AddProjectMarker2(NULL,
                                  true,
                                  startTime,
                                  endTime,
                                  name.Get(),
                                  num,
                                  color);

          if (num >= 0) {
            ok = true; // region added (at least)

            if (firstPos < 0.0)
              firstPos = startTime;

            int id = MakeMarkerRegionId(num, true);
            if (id > 0) // add the sub, no duplicate mgmt..
              g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(nullptr, id, notes.Get(), "?"));
          }
        } else
          break;
      }
    }
    fclose(f);
  }

  if (ok) {
    UpdateTimeline(); // redraw the ruler (andd arrange view)
    if (firstPos > 0.0)
      SetEditCurPos2(NULL, firstPos, true, false);
  }
  return ok;
}

void ImportSubTitleFile(COMMAND_T *_ct) {
  if (char *fn = BrowseForFiles(
    __LOCALIZE("ReNotes - Добавить субтитры", "sws_DLG_152"),
    g_lastImportSubFn,
    NULL,
    false,
    SNM_SUB_EXT_LIST)) {
    lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
    bool imported = false;
    if (HasFileExtension(fn, "ASS"))
      imported = ImportAssFile(fn);
    else
      imported = ImportSubRipFile(fn);
    if (imported) {
      MarkProjectDirty(NULL);
      if (NotesWnd *w = g_notesWndMgr.Get())
        w->RefreshActorList();
    } else
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Некорректный файл субтитров!", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                 MB_OK);
    free(fn);
  }
}

bool ExportRolesFile(const char *fn) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

  WDL_PtrList<SNM_Actor> merged;
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    if (a->HasLinkedActor() || a->HasCustomColor())
      merged.Add(a);
  }
  WDL_PtrList_DOD<SNM_Actor> *imported = g_importedRoleActors.Get();
  for (int i = 0; i < imported->GetSize(); i++) {
    SNM_Actor *ia = imported->Get(i);
    bool found = false;
    for (int j = 0; j < actors->GetSize(); j++) {
      if (!strcmp(actors->Get(j)->GetName(), ia->GetName())) {
        found = true;
        break;
      }
    }
    if (!found)
      merged.Add(ia);
  }

  if (merged.GetSize() == 0)
    return false;

  FILE *f = fopenUTF8(fn, "w");
  if (!f)
    return false;

  WDL_PtrList<const char> writtenLinked;
  for (int i = 0; i < merged.GetSize(); i++) {
    SNM_Actor *a = merged.Get(i);
    if (!a->HasLinkedActor())
      continue;
    const char *linked = a->GetLinkedActorName();
    bool alreadyWritten = false;
    for (int j = 0; j < writtenLinked.GetSize(); j++) {
      if (!strcmp(writtenLinked.Get(j), linked)) {
        alreadyWritten = true;
        break;
      }
    }
    if (alreadyWritten)
      continue;
    writtenLinked.Add(linked);

    fprintf(f, "[%s]\n", linked);
    bool colorWritten = false;
    for (int j = 0; j < merged.GetSize(); j++) {
      SNM_Actor *b = merged.Get(j);
      if (b->HasLinkedActor() && !strcmp(b->GetLinkedActorName(), linked)) {
        if (!colorWritten && b->HasCustomColor()) {
          int c = b->GetColor() & 0xFFFFFF;
          int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
          fprintf(f, "color = #%02X%02X%02X\n", r, g, bl);
          colorWritten = true;
        }
        fprintf(f, "character = %s\n", b->GetName());
      }
    }
    fprintf(f, "\n");
  }

  for (int i = 0; i < merged.GetSize(); i++) {
    SNM_Actor *a = merged.Get(i);
    if (!a->HasLinkedActor() && a->HasCustomColor()) {
      int c = a->GetColor() & 0xFFFFFF;
      int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
      fprintf(f, "[character:%s]\n", a->GetName());
      fprintf(f, "color = #%02X%02X%02X\n", r, g, bl);
      fprintf(f, "\n");
    }
  }

  fclose(f);
  return true;
}

bool ImportRolesFile(const char *fn) {
  FILE *f = fopenUTF8(fn, "rt");
  if (!f)
    return false;

  g_importedRoleActors.Get()->Empty(true);

  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    a->SetLinkedActorName("");
    a->SetHasCustomColor(false);
    a->SetColor(GenerateActorColor(a->GetName()));
  }

  enum { SECTION_NONE, SECTION_LINKED, SECTION_CHARACTER };
  int sectionType = SECTION_NONE;
  WDL_FastString sectionName;
  int parsedColor = 0;
  bool hasColor = false;
  WDL_PtrList_DOD<WDL_FastString> charNames;

  auto applySection = [&]() {
    if (sectionType == SECTION_LINKED) {
      for (int i = 0; i < charNames.GetSize(); i++) {
        const char *charName = charNames.Get(i)->Get();
        SNM_Actor *a = FindOrCreateActor(charName);
        a->SetLinkedActorName(sectionName.Get());
        if (hasColor) {
          a->SetColor(parsedColor);
          a->SetHasCustomColor(true);
        }
        SNM_Actor *ia = new SNM_Actor(charName,
                                      hasColor ? parsedColor : GenerateActorColor(charName));
        ia->SetLinkedActorName(sectionName.Get());
        if (hasColor) ia->SetHasCustomColor(true);
        g_importedRoleActors.Get()->Add(ia);
      }
    } else if (sectionType == SECTION_CHARACTER) {
      SNM_Actor *a = FindOrCreateActor(sectionName.Get());
      if (hasColor) {
        a->SetColor(parsedColor);
        a->SetHasCustomColor(true);
        SNM_Actor *ia = new SNM_Actor(sectionName.Get(), parsedColor);
        ia->SetHasCustomColor(true);
        g_importedRoleActors.Get()->Add(ia);
      }
    }
  };

  char buf[4096];
  if (fgets(buf, 4, f) && strcmp("\xEF\xBB\xBF", buf))
    rewind(f);

  while (fgets(buf, sizeof(buf), f)) {
    char *line = buf;
    while (*line == ' ' || *line == '\t')
      line++;
    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
      end--;
    *end = '\0';

    if (!*line || *line == '#' || *line == ';')
      continue;

    if (*line == '[') {
      applySection();

      charNames.Empty(true);
      hasColor = false;
      parsedColor = 0;

      char *closing = strrchr(line, ']');
      if (!closing)
        continue;
      *closing = '\0';
      const char *header = line + 1;

      if (_strnicmp(header, "character:", 10) == 0) {
        sectionType = SECTION_CHARACTER;
        sectionName.Set(header + 10);
      } else {
        sectionType = SECTION_LINKED;
        sectionName.Set(header);
      }
      continue;
    }

    if (sectionType == SECTION_NONE)
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;

    char *keyEnd = eq;
    while (keyEnd > line && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t'))
      keyEnd--;
    *keyEnd = '\0';

    char *value = eq + 1;
    while (*value == ' ' || *value == '\t')
      value++;

    if (_stricmp(line, "color") == 0 && *value == '#' && strlen(value) == 7) {
      int r, g, b;
      if (sscanf(value + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        parsedColor = RGB(r, g, b) | 0x1000000;
        hasColor = true;
      }
    } else if (_stricmp(line, "character") == 0 && *value) {
      charNames.Add(new WDL_FastString(value));
    }
  }

  applySection();

  fclose(f);
  return true;
}

void ClearAllSubtitles() {
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = subs->GetSize() - 1; i >= 0; i--) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (sub && sub->IsValid()) {
      int idx = GetMarkerRegionIndexFromId(NULL, sub->GetId());
      if (idx >= 0)
        DeleteProjectMarkerByIndex(NULL, idx);
    }
  }
  subs->Empty(true);

  g_actors.Get()->Empty(true);
  g_importedRoleActors.Get()->Empty(true);

  UpdateTimeline();
  MarkProjectDirty(NULL);
}

void WriteGlobalNotesToFile() {
  if (!g_globalNotesDirty) return;

  WDL_FastString filePath;
  filePath.SetFormatted(SNM_MAX_PATH, "%s/SWS_Global ReNotes.txt", GetResourcePath());
  WDL_FileWrite outfile(filePath.Get(), 1, 65536, 16, 16);
  // NF: not sure about these params, taken from
  //https://github.com/justinfrankel/licecap/blob/3721ce33ac72ff05ef89d2e92ca58a0f96164134/WDL/lice/lice_gif_write.cpp#L386
  const int globalNotesLength = g_globalNotes.GetLength();
  const int wrtPos = outfile.Write(g_globalNotes.Get(), globalNotesLength);
  if (wrtPos == globalNotesLength) // writing OK
  {
    g_globalNotesDirty = false;
    if (NotesWnd *w = g_notesWndMgr.Get())
      w->RefreshGUI();
  } else // writing failed
  {
    MessageBox(GetMainHwnd(),
               __LOCALIZE("Не удалось сохранить глобальные заметки.", "sws_mbox"),
               __LOCALIZE("ReNotes - Ошибка", "sws_mbox"),
               MB_OK);
  }
}


///////////////////////////////////////////////////////////////////////////////
// project_config_extension_t
///////////////////////////////////////////////////////////////////////////////

static bool ProcessExtensionLine(const char *line,
                                 ProjectStateContext *ctx,
                                 bool isUndo,
                                 struct project_config_extension_t *reg) {
  LineParser lp(false);
  if (lp.parse(line) || lp.getnumtokens() < 1)
    return false;

  ReaProject *p = GetCurrentProjectInLoadSave();

  if (!strcmp(lp.gettoken_str(0), "<S&M_PROJNOTES")) {
    WDL_FastString notes;
    ExtensionConfigToString(&notes, ctx);

    char buf[MAX_HELP_LENGTH] = "";
    GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH);

    g_prjNotes.Get()->Set(buf);
    return true;
  } else if (!strcmp(lp.gettoken_str(0), "<S&M_TRACKNOTES")) {
    WDL_FastString notes;
    ExtensionConfigToString(&notes, ctx);

    char buf[MAX_HELP_LENGTH] = "";
    if (GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH)) {
      GUID g;
      stringToGuid(lp.gettoken_str(1), &g);
      g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(p, &g, buf));
    }
    return true;
  } else if (!strcmp(lp.gettoken_str(0), "<S&M_SUBTITLE")) {
    int id = lp.gettoken_int(1);
    bool isNewFormat = (lp.getnumtokens() >= 4);
    double startTime = isNewFormat ? lp.gettoken_float(2) : 0.0;
    double endTime = isNewFormat ? lp.gettoken_float(3) : 0.0;

    WDL_FastString notes;
    ExtensionConfigToString(&notes, ctx);

    char buf[MAX_HELP_LENGTH] = "";
    char actor[256] = "";

    if (isNewFormat) {
      const char *notesPtr = notes.Get();
      const char *newlinePos = strchr(notesPtr, '\n');
      if (newlinePos) {
        int actorLen = (int) (newlinePos - notesPtr);
        if (actorLen >= (int) sizeof(actor))
          actorLen = (int) sizeof(actor) - 1;
        if (actorLen > 0)
          memcpy(actor, notesPtr, actorLen);
        actor[actorLen] = 0;
      }
    } else {
      strcpy(actor, "?");
      FindOrCreateActor("?");
    }

    if (GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH)) {
      SNM_RegionSubtitle *sub = new SNM_RegionSubtitle(p, id, buf);
      sub->SetActor(actor);
      if (isNewFormat)
        sub->SetTimes(startTime, endTime);
      g_pRegionSubs.Get()->Add(sub);
    }
    return true;
  } else if (!strcmp(lp.gettoken_str(0), "<S&M_ACTOR")) {
    bool enabled = (lp.gettoken_int(1) == 1);
    int color = lp.gettoken_int(2);
    bool hasCustomColor = (lp.gettoken_int(3) == 1);

    char actorName[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    if (ctx->GetLine(actorName, sizeof(actorName)) >= 0 && actorName[0] && strcmp(actorName, ">") !=
      0) {
      SNM_Actor *actor = new SNM_Actor(actorName, color);
      actor->SetEnabled(enabled);
      actor->SetHasCustomColor(hasCustomColor);

      char nextLine[SNM_MAX_CHUNK_LINE_LENGTH] = "";
      if (ctx->GetLine(nextLine, sizeof(nextLine)) >= 0 && strcmp(nextLine, ">") != 0) {
        actor->SetLinkedActorName(nextLine);
        ctx->GetLine(nextLine, sizeof(nextLine));
      }

      g_actors.Get()->Add(actor);
    }
    return true;
  }
  return false;
}

static void SaveExtensionConfig(ProjectStateContext *ctx,
                                bool isUndo,
                                struct project_config_extension_t *reg) {
  char line[SNM_MAX_CHUNK_LINE_LENGTH] = "";
  char strId[128] = "";
  WDL_FastString formatedNotes;

  // save project notes
  if (g_prjNotes.Get()->GetLength()) {
    strcpy(line, "<S&M_PROJNOTES\n|");
    if (GetNotesChunkFromString(g_prjNotes.Get()->Get(), &formatedNotes, line))
      StringToExtensionConfig(&formatedNotes, ctx);
  }

  // save track notes
  for (int i = 0; i < g_SNM_TrackNotes.Get()->GetSize(); i++) {
    if (SNM_TrackNotes *tn = g_SNM_TrackNotes.Get()->Get(i)) {
      MediaTrack *tr = tn->GetTrack();
      if (tr && tn->GetNotesLength() > 0) {
        guidToString((GUID *) tn->GetGUID(), strId);
        if (snprintfStrict(line, sizeof(line), "<S&M_TRACKNOTES %s\n|", strId) > 0)
          if (GetNotesChunkFromString(tn->GetNotes(), &formatedNotes, line))
            StringToExtensionConfig(&formatedNotes, ctx);
      } else
        g_SNM_TrackNotes.Get()->Delete(i--, true);
    }
  }

  // write global notes to file (only when cur. project is saved)
  if (!isUndo && IsActiveProjectInLoadSave()) {
    WriteGlobalNotesToFile();
  }

  // save region/marker subs
  for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
    if (SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i)) {
      if (!sub->GetNotesLength())
        continue;
      double startTime = sub->GetStartTime();
      double endTime = sub->GetEndTime();
      if (sub->IsValid())
        EnumMarkerRegionById(NULL, sub->GetId(), NULL, &startTime, &endTime, NULL, NULL, NULL);
      if (snprintfStrict(line,
                         sizeof(line),
                         "<S&M_SUBTITLE %d %.6f %.6f\n%s\n|",
                         sub->GetId(),
                         startTime,
                         endTime,
                         sub->GetActor()) > 0)
        if (GetNotesChunkFromString(sub->GetNotes(), &formatedNotes, line))
          StringToExtensionConfig(&formatedNotes, ctx);
    }
  }

  for (int i = 0; i < g_actors.Get()->GetSize(); i++) {
    if (SNM_Actor *actor = g_actors.Get()->Get(i)) {
      if (snprintfStrict(line,
                         sizeof(line),
                         "<S&M_ACTOR %d %d %d",
                         actor->IsEnabled() ? 1 : 0,
                         actor->GetColor(),
                         actor->HasCustomColor() ? 1 : 0) > 0) {
        ctx->AddLine("%s", line);
        ctx->AddLine("%s", actor->GetName());
        if (actor->HasLinkedActor())
          ctx->AddLine("%s", actor->GetLinkedActorName());
        ctx->AddLine(">");
      }
    }
  }
}

static void BeginLoadProjectState(bool isUndo, struct project_config_extension_t *reg) {
  g_prjNotes.Cleanup();
  g_prjNotes.Get()->Set("");

  g_SNM_TrackNotes.Cleanup();
  g_SNM_TrackNotes.Get()->Empty(true);

  g_pRegionSubs.Cleanup();
  g_pRegionSubs.Get()->Empty(true);

  g_actors.Cleanup();
  g_actors.Get()->Empty(true);
  g_importedRoleActors.Cleanup();

  // g_globalNotes is loaded in NotesInit()
}

static project_config_extension_t s_projectconfig = {
  ProcessExtensionLine, SaveExtensionConfig, BeginLoadProjectState, NULL
};


///////////////////////////////////////////////////////////////////////////////

void NotesSetTrackTitle() {
  if (g_notesType == SNM_NOTES_TRACK)
    if (NotesWnd *w = g_notesWndMgr.Get())
      w->RefreshGUI();
}

// this is our only notification of active project tab change, so update everything
// (ScheduledJob because of multi-notifs)
void NotesSetTrackListChange() {
  ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
}


///////////////////////////////////////////////////////////////////////////////

int NotesInit() {
  lstrcpyn(g_lastImportSubFn, GetResourcePath(), sizeof(g_lastImportSubFn));
  lstrcpyn(g_lastRolesFn, GetResourcePath(), sizeof(g_lastRolesFn));

  // load prefs
  g_notesType = GetPrivateProfileInt(NOTES_INI_SEC, "Type", SNM_NOTES_RGN_SUB, g_SNM_IniFn.Get());
  g_locked = (GetPrivateProfileInt(NOTES_INI_SEC, "Lock", 1, g_SNM_IniFn.Get()) == 1);
  GetPrivateProfileString(NOTES_INI_SEC,
                          "BigFontName",
                          SNM_DYN_FONT_NAME,
                          g_notesBigFontName,
                          sizeof(g_notesBigFontName),
                          g_SNM_IniFn.Get());
  g_wrapText = (GetPrivateProfileInt(NOTES_INI_SEC, "WrapText", 0, g_SNM_IniFn.Get()) == 1);
  g_coloredRegions = (GetPrivateProfileInt(NOTES_INI_SEC, "ColoredRegions", 1, g_SNM_IniFn.Get()) ==
    1);
  g_displayActorInPrefix =
      (GetPrivateProfileInt(NOTES_INI_SEC, "DisplayActorInPrefix", 1, g_SNM_IniFn.Get()) == 1);

  WDL_FastString filePath;
  filePath.SetFormatted(SNM_MAX_PATH, "%s/SWS_Global ReNotes.txt", GetResourcePath());
  WDL_FileRead infile(filePath.Get(), 0, 65536);
  int infileSize = static_cast<int>(infile.GetSize());
  if (infileSize < 0) {
    filePath.SetFormatted(SNM_MAX_PATH, "%s/SWS_Global notes.txt", GetResourcePath());
    WDL_FileRead oldFile(filePath.Get(), 0, 65536);
    infileSize = static_cast<int>(oldFile.GetSize());
    if (infileSize >= 0 && g_globalNotes.SetLen(infileSize, true))
      oldFile.Read(const_cast<char *>(g_globalNotes.Get()), infileSize);
  } else if (infileSize >= 0 && g_globalNotes.SetLen(infileSize, true))
    infile.Read(const_cast<char *>(g_globalNotes.Get()), infileSize);

  // instanciate the window if needed, can be NULL
  g_notesWndMgr.Init();

  if (!plugin_register("projectconfig", &s_projectconfig))
    return 0;

  return 1;
}

void NotesExit() {
  plugin_register("-projectconfig", &s_projectconfig);

  char tmp[4] = "";
  if (snprintfStrict(tmp, sizeof(tmp), "%d", g_notesType) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "Type", tmp, g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC, "Lock", g_locked ? "1" : "0", g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC, "BigFontName", g_notesBigFontName, g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC, "WrapText", g_wrapText ? "1" : "0", g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC,
                            "ColoredRegions",
                            g_coloredRegions ? "1" : "0",
                            g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC,
                            "DisplayActorInPrefix",
                            g_displayActorInPrefix ? "1" : "0",
                            g_SNM_IniFn.Get());

  g_notesWndMgr.Delete();
}

void OpenNotes(COMMAND_T *_ct) {
  if (NotesWnd *w = g_notesWndMgr.Create()) {
    int newType = (int) _ct->user; // -1 means toggle current type
    if (newType == -1)
      newType = g_notesType;

    w->Show(g_notesType == newType /* i.e toggle */, true);

    if (!w->GetHWND())
      return;

    w->SetType(newType);

    if (!g_locked)
      SetFocus(w->GetEditControl());
  }
}

int IsNotesDisplayed(COMMAND_T *_ct) {
  if (NotesWnd *w = g_notesWndMgr.Get())
    return w->IsWndVisible();
  return 0;
}

void ToggleNotesLock(COMMAND_T *) {
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->ToggleLock();
}

int IsNotesLocked(COMMAND_T *) {
  return g_locked;
}

void ToggleColoredRegions(COMMAND_T *) {
  g_coloredRegions = !g_coloredRegions;
  UpdateRegionColors();
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshGUI();
}

int IsColoredRegions(COMMAND_T *) {
  return g_coloredRegions;
}

void ClearAllSubtitlesAction(COMMAND_T *) {
  ClearAllSubtitles();
  if (NotesWnd *w = g_notesWndMgr.Get()) {
    w->RefreshActorList();
    w->Update(true);
  }
}

void EnableAllActors(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *actor = actors->Get(i);
    if (!actor->IsEnabled()) {
      actor->SetEnabled(true);
      RecreateActorRegions(actor->GetName());
    }
  }
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshActorList();
}

void DisableAllActors(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *actor = actors->Get(i);
    if (actor->IsEnabled()) {
      actor->SetEnabled(false);
      DeleteActorRegions(actor->GetName());
    }
  }
  UpdateTimeline();
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshActorList();
}

void ImportRolesAction(COMMAND_T *) {
  if (char *fn = BrowseForFiles(
    __LOCALIZE("ReNotes - Импорт ролей", "sws_DLG_152"),
    g_lastRolesFn,
    NULL,
    false,
    SNM_ROLES_EXT_LIST)) {
    lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
    if (ImportRolesFile(fn)) {
      UpdateRegionColors();
      if (NotesWnd *w = g_notesWndMgr.Get()) {
        w->RefreshActorList();
        w->ForceUpdateRgnSub();
        w->RefreshGUI();
      }
      MarkProjectDirty(NULL);
    } else
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Не удалось импортировать файл ролей.", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                 MB_OK);
    free(fn);
  }
}

void ExportRolesAction(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  bool hasData = false;
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    if (a->HasLinkedActor() || a->HasCustomColor()) {
      hasData = true;
      break;
    }
  }
  if (!hasData && g_importedRoleActors.Get()->GetSize() > 0)
    hasData = true;
  if (!hasData) {
    MessageBox(GetMainHwnd(),
               __LOCALIZE("Ничего не найдено для экспорта.", "sws_DLG_152"),
               __LOCALIZE("ReNotes", "sws_DLG_152"),
               MB_OK);
    return;
  }
  char fn[SNM_MAX_PATH] = "";
  if (BrowseForSaveFile(
    __LOCALIZE("ReNotes - Экспорт ролей", "sws_DLG_152"),
    g_lastRolesFn,
    NULL,
    SNM_ROLES_EXT_LIST,
    fn,
    sizeof(fn))) {
    lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
    if (!ExportRolesFile(fn))
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Не удалось экспортировать роли.", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                 MB_OK);
  }
}

/******************************************************************************
* ReaScript export #755                                                       *
******************************************************************************/
const char *NF_GetSWSTrackNotes(MediaTrack *track) {
  SNM_TrackNotes *notes = SNM_TrackNotes::find(track);
  return notes ? notes->GetNotes() : "";
}

void NF_SetSWSTrackNotes(MediaTrack *track, const char *buf) {
  if (!track)
    return;

  MarkProjectDirty(NULL);

  if (SNM_TrackNotes *notes = SNM_TrackNotes::find(track)) {
    notes->SetNotes(buf);

    // update displayed text if Notes window is visible and notes for set track are displayed
    NotesWnd *w = g_notesWndMgr.Get();
    if (w && w->IsWndVisible() && g_notesType == SNM_NOTES_TRACK && g_trNote == track)
      w->SetText(buf);
    return;
  }

  // tracknote for the track doesn't exist yet, add new one
  g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(nullptr, TrackToGuid(track), buf));
}

const char *NFDoGetSWSMarkerRegionSub(int mkrRgnIdxNumberIn) {
  // maybe more safe version, but below also seems to work and is faster
  /*
  int idx = 0, num;

  while ((idx = EnumProjectMarkers2(NULL, idx, NULL, NULL, NULL, NULL, &num)))
  {
      // mkrRgn exists in project
      if (idx-1 == mkrRgnIdxNumberIn)
      {
          int mkrRgnId = GetMarkerRegionIdFromIndex(NULL, idx-1); // takes zero-based idx
          for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++)
          {
              // regionSub exists
              if (mkrRgnId == g_pRegionSubs.Get()->Get(i)->GetId())
              {
                  mkrRgnSubOut->Set(g_pRegionSubs.Get()->Get(i)->GetNotes());
                  return mkrRgnSubOut->Get();
              }
          }
      }
  }
  */

  int mkrRgnId = GetMarkerRegionIdFromIndex(NULL, mkrRgnIdxNumberIn); // takes zero-based idx
  if (mkrRgnId == -1) return "";

  for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
    // mkrRgn sub exists
    if (mkrRgnId == g_pRegionSubs.Get()->Get(i)->GetId()) {
      return g_pRegionSubs.Get()->Get(i)->GetNotes();
    }
  }

  return "";
}

bool NFDoSetSWSMarkerRegionSub(const char *mkrRgnSubIn, int mkrRgnIdxNumberIn) {
  int idx = 0;
  bool mkrRgnExists = false;

  while ((idx = EnumProjectMarkers2(NULL, idx, NULL, NULL, NULL, NULL, NULL))) {
    if (idx - 1 == mkrRgnIdxNumberIn) // mkrRegion exists in project
    {
      mkrRgnExists = true;

      int mkrRgnId = GetMarkerRegionIdFromIndex(NULL, idx - 1); // takes zero-based idx
      for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
        if (mkrRgnId == g_pRegionSubs.Get()->Get(i)->GetId()) // mkrRgn sub exists, update it
        {
          g_pRegionSubs.Get()->Get(i)->SetNotes(mkrRgnSubIn);
          return true;
        }
      }

      // mkrRgn sub doesn't exist but marker/region is present in project, add new mkrRgn sub
      if (mkrRgnExists) {
        g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(nullptr, mkrRgnId, mkrRgnSubIn));
        return true;
      } else // mkrRgn isn't present in project
        return false;
    }
  }
  return false; // appease compilers
}

void NF_DoUpdateSWSMarkerRegionSubWindow() {
  if (NotesWnd *w = g_notesWndMgr.Get()) {
    if (w->IsWndVisible() && g_notesType == SNM_NOTES_RGN_SUB)
      w->ForceUpdateRgnSub();
  }
}

const char *JB_GetSWSExtraProjectNotes(ReaProject *project) {
  return g_prjNotes.Get(project)->Get();
}

void JB_SetSWSExtraProjectNotes(ReaProject *project, const char *buf) {
  MarkProjectDirty(project);

  g_prjNotes.Get(project)->Set(buf);

  // update displayed text if the project is frontmost, the Notes window is visible and notes for project extra are displayed
  if (g_prjNotes.Get(project) == g_prjNotes.Get(NULL)) {
    if (NotesWnd *w = g_notesWndMgr.Get()) {
      if (w->IsWndVisible() && g_notesType == SNM_NOTES_PROJECT_EXTRA) {
        w->SetText(buf);
      }
    }
  }
  return;
}
