/******************************************************************************
/ SnM_Notes.h
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

//#pragma once

#ifndef _SNM_NOTES_H_
#define _SNM_NOTES_H_

#include "SnM_Marker.h"
#include "SnM_VWnd.h"
#include "../sws_wnd.h"

#define NOTES_UPDATE_FREQ		150

// note types (g_notesType)
enum {
  SNM_NOTES_TRACK = 0,
  SNM_NOTES_ITEM,
  SNM_NOTES_PROJECT,
  SNM_NOTES_PROJECT_EXTRA,
  SNM_NOTES_GLOBAL,
  SNM_NOTES_RGN_SUB
};

class SNM_TrackNotes {
  public:
    static SNM_TrackNotes *find(MediaTrack *);

    SNM_TrackNotes(ReaProject *project, const GUID *guid, const char *notes)
      : m_project{project}, m_guid{*guid}, m_notes{notes} {
      // The current project (project == NULL) doen't always match
      // the notes' project when saving in SaveExtensionConfig [#1141]

      if (!project)
        m_project = EnumProjects(-1, nullptr, 0);
    }

    MediaTrack *GetTrack() { return GuidToTrack(m_project, &m_guid); }
    const GUID *GetGUID() { return &m_guid; }
    const char *GetNotes() const { return m_notes.Get(); }
    int GetNotesLength() const { return m_notes.GetLength(); }
    void SetNotes(const char *notes) { m_notes.Set(notes); }

  private:
    ReaProject *m_project;
    GUID m_guid;
    WDL_FastString m_notes;
};

class SNM_RegionSubtitle {
  public:
    SNM_RegionSubtitle(ReaProject *project, const int id, const char *notes, const char *actor = "")
      : m_project{project}, m_id{id}, m_notes{notes}, m_actor{actor}, m_startTime{0.0},
        m_endTime{0.0} {
      if (!project)
        m_project = EnumProjects(-1, nullptr, 0);
    }

    int GetId() const { return m_id; }
    void SetId(int id) { m_id = id; }
    bool IsValid() const { return GetMarkerRegionIndexFromId(m_project, m_id) >= 0; }
    const char *GetNotes() const { return m_notes.Get(); }
    int GetNotesLength() const { return m_notes.GetLength(); }
    void SetNotes(const char *notes) { m_notes.Set(notes); }
    const char *GetActor() const { return m_actor.Get(); }
    void SetActor(const char *actor) { m_actor.Set(actor); }
    double GetStartTime() const { return m_startTime; }
    double GetEndTime() const { return m_endTime; }
    void SetTimes(double start, double end) {
      m_startTime = start;
      m_endTime = end;
    }

  private:
    ReaProject *m_project;
    int m_id;
    WDL_FastString m_notes;
    WDL_FastString m_actor;
    double m_startTime, m_endTime;
};

class SNM_Actor {
  public:
    SNM_Actor(const char *name, int color)
      : m_name{name}, m_color{color}, m_enabled{true}, m_hasCustomColor{false} {
    }

    const char *GetName() const { return m_name.Get(); }
    int GetColor() const { return m_color; }
    void SetColor(int color) { m_color = color; }
    int GetEffectiveColor() const;
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool HasCustomColor() const { return m_hasCustomColor; }
    void SetHasCustomColor(bool v) { m_hasCustomColor = v; }
    const char *GetLinkedActorName() const { return m_linkedActorName.Get(); }
    bool HasLinkedActor() const { return m_linkedActorName.GetLength() > 0; }
    void SetLinkedActorName(const char *name) { m_linkedActorName.Set(name); }

  private:
    WDL_FastString m_name;
    int m_color;
    bool m_enabled;
    bool m_hasCustomColor;
    WDL_FastString m_linkedActorName;
};

struct ActorListItem {
  SNM_Actor *actor;
  ActorListItem(SNM_Actor *a) : actor(a) {
  }
};

class ActorListView: public SWS_ListView {
  public:
    ActorListView(HWND hwndList, HWND hwndEdit);
    bool HideGridLines() override { return true; }
    bool CustomDrawItem(HDC hdc, SWS_ListItem *item, int iCol, RECT *r, bool selected);
    bool GetCustomColumnColor(SWS_ListItem *item,
                              int iCol,
                              bool selected,
                              COLORREF *textColor) override;

  protected:
    void GetItemText(SWS_ListItem *item, int iCol, char *str, int iStrMax);
    void OnItemClk(SWS_ListItem *item, int iCol, int iKeyState);
    void GetItemList(SWS_ListItemList *pList);
    int OnItemSort(SWS_ListItem *item1, SWS_ListItem *item2);
};

class NotesUpdateJob: public ScheduledJob {
  public:
    NotesUpdateJob(int _approxMs) : ScheduledJob(SNM_SCHEDJOB_NOTES_UPDATE, _approxMs) {
    }

  protected:
    void Perform();
};

#ifdef _SNM_SWELL_ISSUES
class OSXForceTxtChangeJob: public ScheduledJob {
  public:
    OSXForceTxtChangeJob() : ScheduledJob(SNM_SCHEDJOB_OSX_FIX, 50) {
    } // ~fast enough to follow key strokes 
  protected:
    void Perform();
};
#endif

class NotesMarkerRegionListener: public SNM_MarkerRegionListener {
  public:
    NotesMarkerRegionListener() : SNM_MarkerRegionListener() {
    }
    void NotifyMarkerRegionUpdate(int _updateFlags);
};

class NotesWnd: public SWS_DockWnd {
  public:
    NotesWnd();
    ~NotesWnd();

    void SetType(int _type);
    void SetText(const char *_str, bool _addRN = true);
    void RefreshGUI();
    void SetFontsizeFrominiFile(); // looks for Fontsize key in S&M.ini > [Notes] section
    void SetWrapText(bool wrap);
    void OnCommand(WPARAM wParam, LPARAM lParam);
    void GetMinSize(int *_w, int *_h) {
      *_w = MIN_DOCKWND_WIDTH;
      *_h = 140;
    }
    void ToggleLock();
    void RefreshActorList();

    void SaveCurrentText(int _type, bool _wantUndo = true);
    void SaveCurrentProjectNotes(bool _wantUndo = true);
    void SaveCurrentExtraProjectNotes(bool _wantUndo = true);
    void SaveCurrentItemNotes(bool _wantUndo = true);
    void SaveCurrentTrackNotes(bool _wantUndo = true);
    void SaveCurrentGlobalNotes(bool _wantUndo = true);
    void SaveCurrentRgnSub(bool _wantUndo = true);
    void Update(bool _force = false);
    int UpdateItemNotes();
    int UpdateTrackNotes();
    int UpdateRgnSub();
    void ForceUpdateRgnSub();
    HWND GetEditControl() const { return m_edit; }

  protected:
    void OnInitDlg();
    void OnDestroy();
    void OnDroppedFiles(HDROP h);
    bool IsActive(bool bWantEdit = false);
    bool ReprocessContextMenu() override { return false; }
    HMENU OnContextMenu(int x, int y, bool *wantDefaultItems);
    int OnKey(MSG *msg, int iKeyState);
    void OnTimer(WPARAM wParam = 0);
    void OnResize();
    void DrawControls(LICE_IBitmap *_bm, const RECT *_r, int *_tooltipHeight = NULL);
    bool GetToolTipString(int _xpos, int _ypos, char *_bufOut, int _bufOutSz);

  private:
    SNM_VirtualComboBox m_cbType;
    SNM_VirtualComboBox m_cbRegion;
    WDL_VirtualIconButton m_btnLock;
    SNM_ToolbarButton m_btnImportSub, m_btnClearSubs;
    WDL_VirtualStaticText m_txtLabel;
    SNM_DynSizedText m_bigNotes;

    NotesMarkerRegionListener m_mkrRgnListener;
    HWND m_edit;
    bool m_settingText;
    ActorListView *m_actorListView;
    SNM_Actor *m_contextMenuActor;
    WDL_TypedBuf<int> m_overlappingRegionIds;
};

bool GetStringFromNotesChunk(WDL_FastString *_notes, char *_buf, int _bufMaxSize);
bool GetNotesChunkFromString(const char *_buf,
                             WDL_FastString *_notes,
                             const char *_startLine = NULL);

void NotesSetTrackTitle();
void NotesSetTrackListChange();

int NotesInit();
void NotesExit();
void ImportSubTitleFile(COMMAND_T *);
void OpenNotes(COMMAND_T *);
int IsNotesDisplayed(COMMAND_T *);
void ToggleNotesLock(COMMAND_T *);
int IsNotesLocked(COMMAND_T *);
void ToggleColoredRegions(COMMAND_T *);
int IsColoredRegions(COMMAND_T *);
void ClearAllSubtitlesAction(COMMAND_T *);
void EnableAllActors(COMMAND_T *);
void DisableAllActors(COMMAND_T *);
void ImportRolesAction(COMMAND_T *);
void ExportRolesAction(COMMAND_T *);
void ToggleHideRegions(COMMAND_T *);
int IsHideRegions(COMMAND_T *);
void ToggleHideActorList(COMMAND_T *);
int IsHideActorList(COMMAND_T *);
void ToggleDisplayActorInPrefix(COMMAND_T *);
int IsDisplayActorInPrefix(COMMAND_T *);
void CopyMarkersAction(COMMAND_T *);
void CopyRoleDistributionAction(COMMAND_T *);
void WriteGlobalNotesToFile();

extern bool g_hideRegions;
extern bool g_hideActorList;

// ReaScript export
const char *NF_GetSWSTrackNotes(MediaTrack *);
void NF_SetSWSTrackNotes(MediaTrack *, const char *buf);

const char *NFDoGetSWSMarkerRegionSub(int mkrRgnIdx);
bool NFDoSetSWSMarkerRegionSub(const char *mkrRgnSubIn, int mkrRgnIdx);
void NF_DoUpdateSWSMarkerRegionSubWindow();

const char *JB_GetSWSExtraProjectNotes(ReaProject * project);
void JB_SetSWSExtraProjectNotes(ReaProject *project, const char *buf);

#endif