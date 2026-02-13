/******************************************************************************
/ SnM_VWnd.cpp
/
/ Copyright (c) 2012 and later Jeffos
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

#include "stdafx.h"
#include "SnM.h"
#include "SnM_Dlg.h"
#include "SnM_Util.h"
#include "SnM_VWnd.h"


static void SNM_LICE_FillRoundRect(LICE_IBitmap *drawbm,
                                   int x,
                                   int y,
                                   int w,
                                   int h,
                                   int cr,
                                   LICE_pixel col,
                                   float alpha,
                                   int mode) {
  if (cr > w / 2) cr = w / 2;
  if (cr > h / 2) cr = h / 2;
  if (cr < 1) {
    LICE_FillRect(drawbm, x, y, w, h, col, alpha, mode);
    return;
  }
  LICE_FillRect(drawbm, x + cr, y, w - 2 * cr, h, col, alpha, mode);
  LICE_FillRect(drawbm, x, y + cr, cr, h - 2 * cr, col, alpha, mode);
  LICE_FillRect(drawbm, x + w - cr, y + cr, cr, h - 2 * cr, col, alpha, mode);
  LICE_FillCircle(drawbm, (float) (x + cr), (float) (y + cr), (float) cr, col, alpha, mode, true);
  LICE_FillCircle(drawbm,
                  (float) (x + w - cr - 1),
                  (float) (y + cr),
                  (float) cr,
                  col,
                  alpha,
                  mode,
                  true);
  LICE_FillCircle(drawbm,
                  (float) (x + cr),
                  (float) (y + h - cr - 1),
                  (float) cr,
                  col,
                  alpha,
                  mode,
                  true);
  LICE_FillCircle(drawbm,
                  (float) (x + w - cr - 1),
                  (float) (y + h - cr - 1),
                  (float) cr,
                  col,
                  alpha,
                  mode,
                  true);
}


///////////////////////////////////////////////////////////////////////////////
// SNM_DynSizedText
///////////////////////////////////////////////////////////////////////////////

// split the text into lines and store them (not to do that in OnPaint()),
// empty lines are ignored
// _col: 0 to use the default theme text color
void SNM_DynSizedText::SetText(const char *_txt, int _col, unsigned char _alpha) {
  if (m_col == _col && m_alpha == _alpha && !strcmp(m_lastText.Get(), _txt ? _txt : ""))
    return;

  m_lastText.Set(_txt ? _txt : "");
  m_lines.Empty(true);
  m_maxLineIdx = -1;
  m_alpha = _alpha;
  m_col = _col ? LICE_RGBA_FROMNATIVE(_col, m_alpha) : 0;

  if (_txt && *_txt) {
    int maxLineLen = 0, len;
    const char *p = _txt, *p2 = NULL;
    while ((p2 = FindFirstRN(p, true))) // \r or \n in any order (for OSX..)
    {
      if ((len = (int) (p2 - p))) {
        if (len > maxLineLen) {
          maxLineLen = len;
          m_maxLineIdx = m_lines.GetSize();
        }

        WDL_FastString *line = new WDL_FastString;
        line->Append(p, len);
        m_lines.Add(line);
        p = p2 + 1;
      }

      while (*p && (*p == '\r' || *p == '\n')) p++;

      if (!*p) break;
    }
    if (p && *p && !p2) {
      WDL_FastString *s = new WDL_FastString(p);
      if (s->GetLength() > maxLineLen)
        m_maxLineIdx = m_lines.GetSize();
      m_lines.Add(s);
    }
  }
  m_lastFontH = -1; // will force font refresh
  m_lastW = m_lastH = 0;
  RequestRedraw(NULL);
}

void SNM_DynSizedText::AddActorColor(const char *name, int color) {
  m_actorColors.Insert(name, LICE_RGBA_FROMNATIVE(color & 0xFFFFFF, m_alpha));
}

void SNM_DynSizedText::DrawLines(LICE_IBitmap *_drawbm,
                                 RECT *_r,
                                 int _fontHeight,
                                 int _defaultCol) {
  if (!m_font.GetHFont() || m_lastFontH <= 0)
    return;

  RECT tr;
  tr.top = _r->top + int(
    (_r->bottom - _r->top) / 2.0 - (_fontHeight * m_lines.GetSize()) / 2.0 + 0.5);
  tr.left = _r->left;
  tr.right = _r->right;

  UINT baseFlags = m_alpha < 255
                     ? (DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM | LICE_DT_USEFGALPHA |
                       LICE_DT_NEEDALPHA)
                     : DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM;
  UINT flags = m_align | baseFlags;

  for (int i = 0; i < m_lines.GetSize(); i++) {
    tr.bottom = tr.top + _fontHeight;
#ifdef _SNM_DYN_FONT_DEBUG
    LICE_DrawRect(_drawbm, tr.left, tr.top, tr.right, tr.bottom, LICE_RGBA(255, 255, 255, 255));
#endif
    const char *line = m_lines.Get(i)->Get();
    bool drewActorPrefix = false;

    if (m_actorColors.GetSize() > 0) {
      const char *openP = NULL;
      for (const char *s = line; *s; s++) {
        if (*s == '(') {
          openP = s;
          break;
        }
      }
      if (openP) {
        int depth = 1;
        const char *p = openP + 1;
        while (*p && depth > 0) {
          if (*p == '(') depth++;
          else if (*p == ')') depth--;
          p++;
        }
        const char *closeP = (depth == 0) ? (p - 1) : NULL;
        if (closeP && *(closeP + 1) == ' ') {
          int actorLen = (int) (closeP - openP - 1);
          char actorName[256];
          if (actorLen > 0 && actorLen < (int) sizeof(actorName)) {
            memcpy(actorName, openP + 1, actorLen);
            actorName[actorLen] = 0;

            int *actorCol = m_actorColors.GetPtr(actorName);
            if (actorCol) {
              int prefixLen = (int) (closeP - line + 2);

              RECT totalRect = {0, 0, 0, 0};
              m_font.DrawText(NULL,
                              line,
                              -1,
                              &totalRect,
                              DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
              int totalWidth = totalRect.right - totalRect.left;

              RECT prefixRect = {0, 0, 0, 0};
              m_font.DrawText(NULL,
                              line,
                              prefixLen,
                              &prefixRect,
                              DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
              int prefixWidth = prefixRect.right - prefixRect.left;

              int rectWidth = tr.right - tr.left;
              int startX = tr.left;
              if (m_align & DT_CENTER)
                startX = tr.left + (rectWidth - totalWidth) / 2;
              else if (m_align & DT_RIGHT)
                startX = tr.right - totalWidth;

              RECT actorRect = tr;
              actorRect.left = startX;
              actorRect.right = startX + prefixWidth;

              m_font.SetTextColor(*actorCol);
              m_font.DrawText(_drawbm, line, prefixLen, &actorRect, DT_LEFT | baseFlags);

              RECT textRect = tr;
              textRect.left = startX + prefixWidth;
              m_font.SetTextColor(_defaultCol);
              m_font.DrawText(_drawbm, line + prefixLen, -1, &textRect, DT_LEFT | baseFlags);

              drewActorPrefix = true;
            }
          }
        }
      }
    }

    if (!drewActorPrefix) {
      m_font.SetTextColor(_defaultCol);
      m_font.DrawText(_drawbm, line, -1, &tr, flags);
    }

    tr.top = tr.bottom;
  }
}

void SNM_DynSizedText::SetTitle(const char *_txt) {
  if (!strcmp(m_title.Get(), _txt ? _txt : ""))
    return;
  m_title.Set(_txt ? _txt : "");
  RequestRedraw(NULL);
}

int SNM_DynSizedText::GetTitleLaneHeight() {
  // it is here because of some .h hassle..
  return SNM_FONT_HEIGHT + 2;
}

bool SNM_DynSizedText::HasTitleLane() {
  return (m_visible && m_title.GetLength() && (m_position.bottom - m_position.top) > 4 *
    GetTitleLaneHeight());
}

void SNM_DynSizedText::OnPaint(LICE_IBitmap *drawbm,
                               int origin_x,
                               int origin_y,
                               RECT *cliprect,
                               int rscale) {
  RECT r = m_position;
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  int h = r.bottom - r.top;
  int w = r.right - r.left;

  ColorTheme *ct = SNM_GetColorTheme();
  int col = m_col;
  if (!col)
    col = ct ? LICE_RGBA_FROMNATIVE(ct->main_text, m_alpha) : LICE_RGBA(255, 255, 255, 255);

  if (m_wantBorder)
    LICE_DrawRect(drawbm, r.left, r.top, w, h, col, 0.2f);

  // title lane
  int laneHeight = GetTitleLaneHeight();
  if (WantTitleLane() && HasTitleLane()) {
    if (m_wantBorder)
      LICE_Line(drawbm, r.left, r.top + laneHeight - 1, r.right, r.top + laneHeight - 1, col, 0.2f);

    // title's band coloring (works for all themes)
    LICE_FillRect(drawbm,
                  r.left,
                  r.top,
                  r.right - r.left,
                  laneHeight,
                  col,
                  1.0f,
                  LICE_BLIT_MODE_OVERLAY);
    LICE_FillRect(drawbm,
                  r.left,
                  r.top,
                  r.right - r.left,
                  laneHeight,
                  col,
                  1.0f,
                  LICE_BLIT_MODE_OVERLAY);

    static LICE_CachedFont sFont;
    if (!sFont.GetHFont()) // single lazy init..
    {
      LOGFONT lf = {
        SNM_FONT_HEIGHT, 0, 0, 0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,SNM_FONT_NAME
      };
      sFont.SetFromHFont(CreateFontIndirect(&lf),
                         LICE_FONT_FLAG_OWNS_HFONT | LICE_FONT_FLAG_FORCE_NATIVE);
      // others props are set on demand (support theme switches)
    }
    sFont.SetBkMode(TRANSPARENT);
    sFont.SetTextColor(LICE_RGBA_FROMNATIVE(WDL_STYLE_GetSysColor(COLOR_WINDOW), 255));
    // "negative" color

    {
      RECT tr = {r.left, r.top, r.right, r.top + laneHeight};
      char buf[64] = "";
      snprintf(buf, sizeof(buf), " %s ", m_title.Get());
      // trick for better display when left/right align
      sFont.DrawText(drawbm, buf, -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | m_titleAlign);
    }

    // resize draw rect: take band into account
    r.top += laneHeight;
    h = r.bottom - r.top;
  }


  // ok, now the meat: render lines with a dynamic sized text
  if (!m_lines.GetSize())
    return;

  if (m_lastFontH > 0 && m_lastW == w && m_lastH == h) {
    m_font.SetTextColor(col);
    DrawLines(drawbm, &r, m_lastFontH, col);
    return;
  }

  int maxCap = h;
  if (maxCap > 128)
    maxCap = 128;
  int availW = w - int(w * 0.1 + 0.5);

  int lo = 8, hi = maxCap, bestH = 8;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    HFONT lf = CreateFont(mid,
                          0,
                          0,
                          0,
                          FW_NORMAL,
                          FALSE,
                          FALSE,
                          FALSE,
                          DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY,
                          DEFAULT_PITCH,
                          m_fontName.Get());
    m_font.SetFromHFont(lf, LICE_FONT_FLAG_OWNS_HFONT | LICE_FONT_FLAG_FORCE_NATIVE);
    m_font.SetBkMode(TRANSPARENT);

    bool fits = (m_lines.GetSize() * mid) <= h;
    if (fits) {
      for (int j = 0; j < m_lines.GetSize(); j++) {
        RECT lr = {0, 0, 0, 0};
        m_font.DrawText(NULL,
                        m_lines.Get(j)->Get(),
                        -1,
                        &lr,
                        DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        if ((lr.right - lr.left) > availW) {
          fits = false;
          break;
        }
      }
    }
    if (fits) {
      bestH = mid;
      lo = mid + 1;
    } else { hi = mid - 1; }

    m_font.SetFromHFont(NULL, LICE_FONT_FLAG_OWNS_HFONT);
    DeleteObject(lf);
  }

  {
    HFONT lf = CreateFont(bestH,
                          0,
                          0,
                          0,
                          FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY,
                          DEFAULT_PITCH,
                          m_fontName.Get());
    m_font.SetFromHFont(lf, LICE_FONT_FLAG_OWNS_HFONT | LICE_FONT_FLAG_FORCE_NATIVE);
    m_font.SetBkMode(TRANSPARENT);
  }

  m_lastFontH = bestH;
  m_lastW = w;
  m_lastH = h;
  m_font.SetTextColor(col);
  DrawLines(drawbm, &r, m_lastFontH, col);
}


///////////////////////////////////////////////////////////////////////////////
// SNM_FiveMonitors
///////////////////////////////////////////////////////////////////////////////

// default alignements = just the ones I need ATM..
void SNM_FiveMonitors::AddMonitors(SNM_DynSizedText *_m0,
                                   SNM_DynSizedText *_m1,
                                   SNM_DynSizedText *_m2,
                                   SNM_DynSizedText *_m3,
                                   SNM_DynSizedText *_m4) {
  _m0->SetFontName(SNM_DYN_FONT_NAME);
  AddChild(_m0);

  _m1->SetAlign(DT_CENTER, DT_LEFT);
  _m1->SetFontName(SNM_DYN_FONT_NAME);
  _m1->SetBorder(true);
  AddChild(_m1);

  _m2->SetAlign(DT_CENTER, DT_RIGHT);
  _m2->SetFontName(SNM_DYN_FONT_NAME);
  _m2->SetBorder(true);
  AddChild(_m2);

  _m3->SetAlign(DT_CENTER, DT_LEFT);
  _m3->SetFontName(SNM_DYN_FONT_NAME);
  _m3->SetBorder(true);
  AddChild(_m3);

  _m4->SetAlign(DT_CENTER, DT_RIGHT);
  _m4->SetFontName(SNM_DYN_FONT_NAME);
  _m4->SetBorder(true);
  AddChild(_m4);
}

// all 5 monitors were added?
bool SNM_FiveMonitors::HasValidChildren() {
  int ok = 0;
  if (m_children && m_children->GetSize() == 5)
    for (int i = 0; i < 5; i++)
      if (WDL_VWnd *vwnd = m_children->Get(i))
        if (!strcmp(vwnd->GetType(), "SNM_DynSizedText"))
          ok++;
  return (ok == 5);
}

void SNM_FiveMonitors::SetText(int _num, const char *_txt, int _col, unsigned char _alpha) {
  if (_num >= 0 && _num < 5 && HasValidChildren())
    ((SNM_DynSizedText *) m_children->Get(_num))->
        SetText(_txt, _col, _alpha);
}

void SNM_FiveMonitors::SetTitles(const char *_title1,
                                 const char *_title2,
                                 const char *_title3,
                                 const char *_title4) {
  if (!HasValidChildren()) return; // no NULL check required thanks to this
  ((SNM_DynSizedText *) m_children->Get(1))->SetTitle(_title1);
  ((SNM_DynSizedText *) m_children->Get(2))->SetTitle(_title2);
  ((SNM_DynSizedText *) m_children->Get(3))->SetTitle(_title3);
  ((SNM_DynSizedText *) m_children->Get(4))->SetTitle(_title4);
}

void SNM_FiveMonitors::SetPosition(const RECT *_r) {
  m_position = *_r;

  if (!HasValidChildren()) return; // no NULL check required thanks to this

  RECT r = *_r;
  int monHeight = r.bottom - r.top;
  int monWidth = r.right - r.left;

  m_children->Get(0)->SetPosition(&r);
  if (m_nbRows <= 0) return;

  // show all 4 monitors if enough room
  if (monWidth > 400) {
    r.bottom = r.top + int((m_nbRows >= 2 ? 0.6 : 1.0) * monHeight + 0.5);
    r.right = r.left + int(0.25 * monWidth + 0.5);
    m_children->Get(1)->SetPosition(&r);

    r.left = r.right;
    r.right = _r->right - 1;
    m_children->Get(2)->SetPosition(&r);

    if (m_nbRows >= 2) {
      r.top = r.bottom;
      r.left = _r->left;
      r.bottom = _r->bottom - 1;
      r.right = r.left + int(0.25 * monWidth + 0.5);
      m_children->Get(3)->SetPosition(&r);

      r.left = r.right;
      r.right = _r->right - 1;
      m_children->Get(4)->SetPosition(&r);
    }
  }
  // 2 monitors otherwise
  else {
    RECT r0 = {0, 0, 0, 0};
    m_children->Get(2)->SetPosition(&r0);
    m_children->Get(4)->SetPosition(&r0);

    // vertical display
    if (2 * monHeight > monWidth) {
      r.bottom = r.top + int((m_nbRows >= 2 ? 0.6 : 1.0) * monHeight + 0.5);
      m_children->Get(1)->SetPosition(&r);
      if (m_nbRows >= 2) {
        r.top = r.bottom;
        r.bottom = r.top + int(0.4 * monHeight + 0.5);
        m_children->Get(3)->SetPosition(&r);
      }
    }
    // horizontal display
    else {
      r.right = r.left + int((m_nbRows >= 2 ? 0.6 : 1.0) * monWidth + 0.5);
      m_children->Get(1)->SetPosition(&r);
      if (m_nbRows >= 2) {
        r.left = r.right;
        r.right = r.left + int(0.4 * monWidth + 0.5);
        m_children->Get(3)->SetPosition(&r);
      }
    }
  }
}

// show/hide title lane for all monitors *together* (assumes mon3 and mon4 are smaller than mon1 and mon2)
void SNM_FiveMonitors::OnPaint(LICE_IBitmap *_drawbm,
                               int _origin_x,
                               int _origin_y,
                               RECT *_cliprect,
                               int rscale) {
  if (m_nbRows >= 2) {
    if (!HasValidChildren()) return; // no NULL check required thanks to this
    bool smallerHasLane = (((SNM_DynSizedText *) m_children->Get(3))->HasTitleLane() || ((
      SNM_DynSizedText *) m_children->Get(4))->HasTitleLane());
    ((SNM_DynSizedText *) m_children->Get(1))->SetWantTitleLane(smallerHasLane);
    ((SNM_DynSizedText *) m_children->Get(2))->SetWantTitleLane(smallerHasLane);
  }
  WDL_VWnd::OnPaint(_drawbm, _origin_x, _origin_y, _cliprect, rscale);
}

void SNM_FiveMonitors::SetFontName(const char *_fontName) {
  if (!HasValidChildren()) return; // no NULL check required thanks to this
  for (int i = 0; i < 5; i++) ((SNM_DynSizedText *) m_children->Get(i))->SetFontName(_fontName);
}

void SNM_FiveMonitors::SetRows(int _nbRows) {
  if (m_nbRows != _nbRows) {
    m_nbRows = _nbRows;
    if (HasValidChildren()) {
      ((SNM_DynSizedText *) m_children->Get(1))->SetVisible(m_nbRows >= 1);
      ((SNM_DynSizedText *) m_children->Get(2))->SetVisible(m_nbRows >= 1);
      ((SNM_DynSizedText *) m_children->Get(3))->SetVisible(m_nbRows >= 2);
      ((SNM_DynSizedText *) m_children->Get(4))->SetVisible(m_nbRows >= 2);
    }
    RequestRedraw(NULL);
  }
}


///////////////////////////////////////////////////////////////////////////////
// SNM_TwoTinyButtons
///////////////////////////////////////////////////////////////////////////////

void SNM_TwoTinyButtons::SetPosition(const RECT *_r) {
  m_position = *_r;
  if (m_children) {
    if (WDL_VWnd *a = m_children->Get(0)) {
      RECT r = {0, 0, 0, 0};
      r.right = _r->right - _r->left;
      r.bottom = int((_r->bottom - _r->top) / 2); // no rounding!
      a->SetPosition(&r);
    }
    if (WDL_VWnd *b = m_children->Get(1)) {
      RECT r = {0, 0, 0, 0};
      r.right = _r->right - _r->left;
      r.top = int((_r->bottom - _r->top) / 2) + 1; // no rounding!
      r.bottom = _r->bottom - _r->top;
      b->SetPosition(&r);
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// SNM_ToolbarButton
///////////////////////////////////////////////////////////////////////////////

// overrides WDL_VirtualIconButton::SetGrayed() which paints grayed stuff with 0.25f alpha
// => look bad with some themes, e.g. not readable with the default v4 theme
// => overrides to use 0.45f instead
void SNM_ToolbarButton::SetGrayed(bool _grayed) {
  SetEnabled(!_grayed);
  m_alpha = _grayed ? 0.45f : 1.0f;
#ifdef _SNM_OVERLAYS
  // prevent stuck overlay when graying the button
  if (_grayed)
    m_pressed = 0;
#endif
}

void SNM_ToolbarButton::OnPaint(LICE_IBitmap *drawbm,
                                int origin_x,
                                int origin_y,
                                RECT *cliprect,
                                int rscale) {
  float alpha = (m_grayed ? 0.25f : 1.0f) * m_alpha;
  bool isdown = !!(m_pressed & 1);
  bool ishover = !!(m_pressed & 2);

  RECT r = m_position;
  ScaleRect(&r, rscale);
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  int w = r.right - r.left;
  int h = r.bottom - r.top;
  int cr = 3;

  int col;
  if (WDL_STYLE_WantGlobalButtonBackground(&col)) {
    SNM_LICE_FillRoundRect(drawbm,
                           r.left,
                           r.top,
                           w,
                           h,
                           cr,
                           LICE_RGBA_FROMNATIVE(col, 255),
                           alpha,
                           LICE_BLIT_MODE_COPY);
  }

  if (ishover || m_forceborder || WDL_STYLE_WantGlobalButtonBorders()) {
    int pencol = LICE_RGBA_FROMNATIVE(GSC(COLOR_3DSHADOW), 255);
    LICE_RoundRect(drawbm,
                   (float) r.left,
                   (float) r.top,
                   (float) (w - 1),
                   (float) (h - 1),
                   cr,
                   pencol,
                   alpha,
                   LICE_BLIT_MODE_COPY,
                   true);
  }

  if (m_textfont && m_textlbl.Get()[0]) {
    int fgc = m_forcetext_color ? m_forcetext_color : LICE_RGBA_FROMNATIVE(GSC(COLOR_BTNTEXT), 255);
    m_textfont->SetBkMode(TRANSPARENT);
    m_textfont->SetTextColor(fgc);

    RECT r2 = r;
    if (isdown && ishover) {
      r2.left++;
      r2.top++;
    }
    m_textfont->DrawText(drawbm,
                         m_textlbl.Get(),
                         -1,
                         &r2,
                         DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
  }
}


///////////////////////////////////////////////////////////////////////////////
// SNM_ImageVWnd
///////////////////////////////////////////////////////////////////////////////

int SNM_ImageVWnd::GetWidth() {
  if (m_img) return m_img->getWidth();
  return 0;
}

int SNM_ImageVWnd::GetHeight() {
  if (m_img) return m_img->getHeight();
  return 0;
}

void SNM_ImageVWnd::SetImage(const char *_fn) {
  if (_fn && *_fn)
    if ((m_img = LICE_LoadPNG(_fn, NULL))) {
      m_fn.Set(_fn);
      return;
    }
  DELETE_NULL(m_img);
  m_fn.Set("");
}


void SNM_ImageVWnd::OnPaint(LICE_IBitmap *drawbm,
                            int origin_x,
                            int origin_y,
                            RECT *cliprect,
                            int rscale) {
  if (m_img) {
    LICE_ScaledBlit(drawbm,
                    m_img,
                    m_position.left + origin_x,
                    m_position.top + origin_y,
                    m_position.right - m_position.left,
                    m_position.bottom - m_position.top,
                    0.0f,
                    0.0f,
                    (float) GetWidth(),
                    (float) GetHeight(),
                    1.0f,
                    LICE_BLIT_MODE_COPY | LICE_BLIT_USE_ALPHA);
  }
}


///////////////////////////////////////////////////////////////////////////////
// SNM_Logo
///////////////////////////////////////////////////////////////////////////////

// just avoids some include in the .h
SNM_Logo::SNM_Logo() : SNM_ImageVWnd(SNM_GetThemeLogo()) {
}


///////////////////////////////////////////////////////////////////////////////
// SNM_TinyButton
///////////////////////////////////////////////////////////////////////////////

void OnPaintPlusOrMinus(RECT *_position,
                        bool _en,
                        bool _plus,
                        LICE_IBitmap *_drawbm,
                        int _origin_x,
                        int _origin_y,
                        RECT *_cliprect) {
  RECT r = *_position;
  r.left += _origin_x;
  r.right += _origin_x;
  r.top += _origin_y;
  r.bottom += _origin_y;

  ColorTheme *ct = SNM_GetColorTheme();
  int col = ct ? LICE_RGBA_FROMNATIVE(ct->main_text, 255) : LICE_RGBA(255, 255, 255, 255);
  float alpha = _en ? 0.8f : 0.4f;

  // border
  LICE_Line(_drawbm, r.left, r.bottom - 1, r.left, r.top, col, alpha, 0, false);
  LICE_Line(_drawbm, r.left, r.top, r.right - 1, r.top, col, alpha, 0, false);
  LICE_Line(_drawbm, r.right - 1, r.top, r.right - 1, r.bottom - 1, col, alpha, 0, false);
  LICE_Line(_drawbm, r.left, r.bottom - 1, r.right - 1, r.bottom - 1, col, alpha, 0, false);

  // + or -
  int delta = _plus ? 2 : 3;
  LICE_Line(_drawbm,
            r.left + delta,
            int(r.top + ((r.bottom - r.top) / 2) + 0.5),
            r.right - (delta + 1),
            int(r.top + ((r.bottom - r.top) / 2) + 0.5),
            col,
            alpha,
            0,
            false);
  if (_plus)
    LICE_Line(_drawbm,
              int(r.left + ((r.right - r.left) / 2) + 0.5),
              r.top + delta,
              int(r.left + ((r.right - r.left) / 2) + 0.5),
              r.bottom - (delta + 1),
              col,
              alpha,
              0,
              false);
}

void SNM_TinyPlusButton::OnPaint(LICE_IBitmap *drawbm,
                                 int origin_x,
                                 int origin_y,
                                 RECT *cliprect,
                                 int rscale) {
  OnPaintPlusOrMinus(&m_position, m_en, true, drawbm, origin_x, origin_y, cliprect);
}

void SNM_TinyMinusButton::OnPaint(LICE_IBitmap *drawbm,
                                  int origin_x,
                                  int origin_y,
                                  RECT *cliprect,
                                  int rscale) {
  OnPaintPlusOrMinus(&m_position, m_en, false, drawbm, origin_x, origin_y, cliprect);
}

void SNM_TinyRightButton::OnPaint(LICE_IBitmap *drawbm,
                                  int origin_x,
                                  int origin_y,
                                  RECT *cliprect,
                                  int rscale) {
  RECT r = m_position;
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  ColorTheme *ct = SNM_GetColorTheme();
  int col = ct ? LICE_RGBA_FROMNATIVE(ct->main_text, 255) : LICE_RGBA(255, 255, 255, 255);
  float alpha = m_en ? 0.8f : 0.4f;

  LICE_FillTriangle(drawbm,
                    r.left,
                    r.top,
                    r.right - 1,
                    r.top + int((r.bottom - r.top) / 2 + 0.5),
                    r.left,
                    r.bottom - 1,
                    col,
                    alpha);
  // borders needed (the above ^^ draws w/o aa..)
  LICE_Line(drawbm,
            r.left,
            r.top,
            r.right - 1,
            r.top + int((r.bottom - r.top) / 2 + 0.5),
            col,
            alpha,
            0,
            true);
  LICE_Line(drawbm,
            r.right - 1,
            r.top + int((r.bottom - r.top) / 2 + 0.5),
            r.left,
            r.bottom - 1,
            col,
            alpha,
            0,
            true);
  LICE_Line(drawbm, r.left, r.bottom - 1, r.left, r.top, col, alpha, 0, true);
}

void SNM_TinyLeftButton::OnPaint(LICE_IBitmap *drawbm,
                                 int origin_x,
                                 int origin_y,
                                 RECT *cliprect,
                                 int rscale) {
  RECT r = m_position;
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  ColorTheme *ct = SNM_GetColorTheme();
  int col = ct ? LICE_RGBA_FROMNATIVE(ct->main_text, 255) : LICE_RGBA(255, 255, 255, 255);
  float alpha = m_en ? 0.8f : 0.4f;

  LICE_FillTriangle(drawbm,
                    r.right - 1,
                    r.top,
                    r.left,
                    r.top + int((r.bottom - r.top) / 2 + 0.5),
                    r.right - 1,
                    r.bottom - 1,
                    col,
                    alpha);
  // borders needed (the above ^^ draws w/o aa)
  LICE_Line(drawbm,
            r.right - 1,
            r.top,
            r.left,
            r.top + int((r.bottom - r.top) / 2 + 0.5),
            col,
            alpha,
            0,
            true);
  LICE_Line(drawbm,
            r.left,
            r.top + int((r.bottom - r.top) / 2 + 0.5),
            r.right - 1,
            r.bottom - 1,
            col,
            alpha,
            0,
            true);
  LICE_Line(drawbm, r.right - 1, r.bottom - 1, r.right - 1, r.top, col, alpha, 0, true);
}

void SNM_TinyTickBox::OnPaint(LICE_IBitmap *drawbm,
                              int origin_x,
                              int origin_y,
                              RECT *cliprect,
                              int rscale) {
  RECT r = m_position;
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  ColorTheme *ct = SNM_GetColorTheme();
  int col = ct ? LICE_RGBA_FROMNATIVE(ct->main_text, 255) : LICE_RGBA(255, 255, 255, 255);
  float alpha = m_en ? 0.8f : 0.4f;

  // border
  LICE_Line(drawbm, r.left, r.bottom - 1, r.left, r.top, col, alpha, 0, false);
  LICE_Line(drawbm, r.left, r.top, r.right - 1, r.top, col, alpha, 0, false);
  LICE_Line(drawbm, r.right - 1, r.top, r.right - 1, r.bottom - 1, col, alpha, 0, false);
  LICE_Line(drawbm, r.left, r.bottom - 1, r.right - 1, r.bottom - 1, col, alpha, 0, false);

  if (m_checkstate)
    LICE_FillRect(drawbm,
                  r.left + 2,
                  r.top + 2,
                  r.right - r.left - 4,
                  r.bottom - r.top - 4,
                  col,
                  alpha);
}

void SNM_TinyTickBox::SetCheckState(char _state) {
  if (_state != m_checkstate) {
    m_checkstate = _state;
    RequestRedraw(NULL);
  }
}


///////////////////////////////////////////////////////////////////////////////
// SNM_KnobCaption
///////////////////////////////////////////////////////////////////////////////

void SNM_KnobCaption::SetPosition(const RECT *_r) {
  m_position = *_r;
  if (m_children) {
    WDL_VWnd *knob = m_children ? m_children->Get(0) : NULL;
    if (knob && !strcmp(knob->GetType(), "SNM_Knob")) {
      // fixed knob size atm..
      RECT r = {0, 0, 0, 0};
      r.top = int(0.5 + (m_position.bottom - m_position.top) / 2 - SNM_GUI_W_H_KNOB / 2); // valign
      r.right = SNM_GUI_W_H_KNOB;
      r.bottom = r.top + SNM_GUI_W_H_KNOB;
      knob->SetPosition(&r);
    }
  }
}

void SNM_KnobCaption::SetValue(int _value) {
  if (m_value != _value) {
    m_value = _value;
    WDL_VWnd *knob = m_children ? m_children->Get(0) : NULL;
    if (knob && !strcmp(knob->GetType(), "SNM_Knob"))
      ((SNM_Knob *) knob)->SetSliderPosition(_value);
    RequestRedraw(NULL);
  }
}

LICE_CachedFont *SNM_KnobCaption::GetFont() {
  return
#ifdef _SNM_SWELL_ISSUES
      SNM_GetFont(0); // SWELL issue: native font rendering won't draw multiple lines
#else
      SNM_GetThemeFont(); // i.e. SNM_GetFont(1)
#endif
}

// factorization for _dtFlags..
void SNM_KnobCaption::DrawText(LICE_IBitmap *_drawbm, RECT *_rect, UINT _dtFlags) {
  if (LICE_CachedFont *font = GetFont()) {
    char buf[64] = "";
    if (m_value || !m_zeroTxt.GetLength())
      snprintf(buf, sizeof(buf), "%s\n%d %s", m_title.Get(), m_value, m_suffix.Get());
    else
      snprintf(buf, sizeof(buf), "%s\n%s", m_title.Get(), m_zeroTxt.Get());
    font->DrawText(_drawbm, buf, -1, _rect, _dtFlags);
  }
}

void SNM_KnobCaption::OnPaint(LICE_IBitmap *drawbm,
                              int origin_x,
                              int origin_y,
                              RECT *cliprect,
                              int rscale) {
  WDL_VWnd::OnPaint(drawbm, origin_x, origin_y, cliprect, rscale);

  RECT r = m_position;
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  RECT tr = {r.left + SNM_GUI_W_H_KNOB + 2, r.top, r.right, r.bottom}; // +2: room w/ knob
  DrawText(drawbm, &tr, DT_NOPREFIX | DT_VCENTER);
}

void SNM_VirtualComboBox::OnPaint(LICE_IBitmap *drawbm,
                                  int origin_x,
                                  int origin_y,
                                  RECT *cliprect,
                                  int rscale) {
  if (m_font) m_font->SetBkMode(TRANSPARENT);

  RECT r;
  GetPositionPaintExtent(&r, rscale);
  r.left += origin_x;
  r.right += origin_x;
  r.top += origin_y;
  r.bottom += origin_y;

  int w = r.right - r.left;
  int h = r.bottom - r.top;
  int cr = 3;

  int col = GSC(COLOR_WINDOW);
  col = LICE_RGBA_FROMNATIVE(col, 255);
  SNM_LICE_FillRoundRect(drawbm, r.left, r.top, w, h, cr, col, 1.0f, LICE_BLIT_MODE_COPY);

  int tcol = GSC(COLOR_BTNTEXT);
  tcol = LICE_RGBA_FROMNATIVE(tcol, 255);
  if (m_font && m_items.Get(m_curitem) && m_items.Get(m_curitem)[0]) {
    RECT tr = r;
    tr.left += 8;
    tr.right -= 16;
    m_font->SetTextColor(tcol);
    if (m_align == 0) {
      RECT r2 = {0,};
      m_font->DrawText(drawbm,
                       m_items.Get(m_curitem),
                       -1,
                       &r2,
                       DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
      m_font->DrawText(drawbm,
                       m_items.Get(m_curitem),
                       -1,
                       &tr,
                       DT_SINGLELINE | DT_VCENTER | (r2.right < tr.right - tr.left
                                                       ? DT_CENTER
                                                       : DT_LEFT) | DT_NOPREFIX);
    } else {
      m_font->DrawText(drawbm,
                       m_items.Get(m_curitem),
                       -1,
                       &tr,
                       DT_SINGLELINE | DT_VCENTER | (m_align < 0 ? DT_LEFT : DT_RIGHT) |
                       DT_NOPREFIX);
    }
  }

  int pencol = GSC(COLOR_3DSHADOW);
  pencol = LICE_RGBA_FROMNATIVE(pencol, 255);
  LICE_RoundRect(drawbm,
                 (float) r.left,
                 (float) r.top,
                 (float) (w - 1),
                 (float) (h - 1),
                 cr,
                 pencol,
                 1.0f,
                 LICE_BLIT_MODE_COPY,
                 true);

  int cx = r.right - h / 2;
  int cy = r.top + h / 2;
  int sz = 4;
  LICE_Line(drawbm, cx - sz, cy - sz / 2, cx, cy + sz / 2, tcol, 1.0f, LICE_BLIT_MODE_COPY, true);
  LICE_Line(drawbm, cx, cy + sz / 2, cx + sz, cy - sz / 2, tcol, 1.0f, LICE_BLIT_MODE_COPY, true);
}


///////////////////////////////////////////////////////////////////////////////
// VWnd helpers
///////////////////////////////////////////////////////////////////////////////

void SNM_SkinButton(WDL_VirtualIconButton *_btn,
                    WDL_VirtualIconButton_SkinConfig *_skin,
                    const char *_text) {
  if (_skin && _skin->image) {
    _btn->SetIcon(_skin);
    _btn->SetForceBorder(false);
  } else {
    _btn->SetIcon(NULL);
    _btn->SetTextLabel(_text, 0, SNM_GetThemeFont());
    _btn->SetForceBorder(true);
    if (ColorTheme *ct = SNM_GetColorTheme())
      _btn->SetForceText(true, LICE_RGBA_FROMNATIVE(ct->main_text, 255));
  }
}

void SNM_SkinToolbarButton(SNM_ToolbarButton *_btn, const char *_text) {
  _btn->SetIcon(NULL);
  _btn->SetTextLabel(_text, 0, SNM_GetToolbarFont());
  _btn->SetForceBorder(true);
  if (ColorTheme *ct = SNM_GetColorTheme())
    _btn->SetForceText(true, LICE_RGBA_FROMNATIVE(ct->main_text, 255));
}

void SNM_SkinKnob(SNM_Knob *_knob) {
  static WDL_VirtualSlider_SkinConfig sSkin;
  IconTheme *it = SNM_GetIconTheme(true); // true: knobs are ~recent (v4)
  if (it && it->knob.bgimage) {
    WDL_VirtualSlider_PreprocessSkinConfig(&sSkin);
    _knob->SetSkinImageInfo(&sSkin, &it->knob, &it->knob_sm);
  } else
    _knob->SetSkinImageInfo(NULL); // important: would crash when switching theme..
}

bool SNM_AddLogo(LICE_IBitmap *_bm, const RECT *_r, int _x, int _h) {
  LICE_IBitmap *logo = SNM_GetThemeLogo();
  if (_bm && logo && _r) {
    // top right display (if no overlap with left controls)
    if (_x >= 0 && _h >= 0) {
      if ((_x + logo->getWidth()) < (_r->right + SNM_GUI_X_MARGIN_LOGO)) {
        int y = _r->top + int(_h / 2 - logo->getHeight() / 2 + 0.5);
        LICE_Blit(_bm,
                  logo,
                  _r->right - (logo->getWidth() + SNM_GUI_X_MARGIN_LOGO),
                  y,
                  NULL,
                  0.125f,
                  LICE_BLIT_MODE_ADD | LICE_BLIT_USE_ALPHA);
        return true;
      }
    }
    // bottom right display
    else if (((_r->right - _r->left) - SNM_GUI_X_MARGIN_LOGO) > logo->getWidth() && (_r->bottom - _r
      ->top - SNM_GUI_Y_MARGIN_LOGO) > logo->getHeight()) {
      LICE_Blit(_bm,
                logo,
                _r->right - (logo->getWidth() + SNM_GUI_X_MARGIN_LOGO),
                _r->bottom - _r->top - logo->getHeight() - SNM_GUI_Y_MARGIN_LOGO,
                NULL,
                0.125f,
                LICE_BLIT_MODE_ADD | LICE_BLIT_USE_ALPHA);
      return true;
    }
  }
  return false;
}

bool SNM_HasLeftVWnd(WDL_VWnd *_comp, int _left, int _top, int _bottom) {
  if (_comp) {
    int x = 0, leftWithRoom = _left - SNM_DEF_VWND_X_STEP - 1;
    while (WDL_VWnd *w = _comp->GetParent()->EnumChildren(x++))
      if (w != _comp && w->IsVisible() && w->GetUserData() == DT_LEFT) {
        RECT r;
        w->GetPosition(&r);
        if (r.top >= _top && r.bottom <= _bottom && (r.right >= _left || (leftWithRoom > 0
            ? r.right >= leftWithRoom
            : true)))
          return true;
      }
  }
  return false;
}

// auto position a WDL_VWnd instance
// assumes all components are hidden by default, see WM_PAINT in sws_wnd.cpp
// _align: only DT_LEFT and DT_RIGHT are supported atm
//         note: right aligned comps have lower priority
//         (i.e. hidden if a left aligned comp is already there)
// _x:     in/out param, modified for the next component to be displayed
// _h:     height of the destination panel
// returns false if hidden
// TODO? WDL_VWnd inheritance rather than checking for inherited types
//       e.g. some kind of getPreferedWidthHeight(int* _width, int* _height)?
bool SNM_AutoVWndPosition(UINT _align,
                          WDL_VWnd *_comp,
                          WDL_VWnd *_tiedComp,
                          const RECT *_r,
                          int *_x,
                          int _y,
                          int _h,
                          int _xRoom) {
  if (_comp && _h && abs(_r->bottom - _r->top) >= _h) {
    // *** compute needed width/height ***
    int width = 0, height = 0;
    if (!strcmp(_comp->GetType(), "vwnd_statictext")) {
      WDL_VirtualStaticText *txt = (WDL_VirtualStaticText *) _comp;
      RECT tr = {0, 0, 0, 0};
      txt->GetFont()->DrawText(NULL,
                               txt->GetText(),
                               -1,
                               &tr,
                               DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
      width = tr.right;
      height = tr.bottom;
    } else if (!strcmp(_comp->GetType(), "vwnd_combobox")) {
      WDL_VirtualComboBox *cb = (WDL_VirtualComboBox *) _comp;
      //JFB cb->GetCurSel()?
      for (int i = 0; i < cb->GetCount(); i++) {
        RECT tr = {0, 0, 0, 0};
        cb->GetFont()->DrawText(NULL,
                                cb->GetItem(i),
                                -1,
                                &tr,
                                DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        if (tr.right > width)
          width = tr.right;
        if (tr.bottom > height)
          height = tr.bottom;
      }
      height = height + int(height * 3 / 4 + 0.5);
#ifdef __APPLE__
      height -= 2;
#endif
      width += 2 * height; // 2*height for the arrow zone (square)
    } else if (!strcmp(_comp->GetType(), "vwnd_iconbutton") || !strcmp(
      _comp->GetType(),
      "SNM_ToolbarButton")) {
      WDL_VirtualIconButton *btn = (WDL_VirtualIconButton *) _comp;
      WDL_VirtualIconButton_SkinConfig *skin = btn->GetIcon();
      if (skin && skin->image) {
        width = skin->image->getWidth() / 3;
        height = skin->image->getHeight();
        if (!strcmp(_comp->GetType(), "SNM_ToolbarButton")) {
          width = int(2.3 * width); // larger toolbar buttons!
          height = int(0.75 * height + 0.5) + 1; // +1 for text vertical alignment
        }
        // .. can be changed below! (i.e. adapt to larger text)
      }
      if (btn->GetFont()) {
        RECT tr = {0, 0, 0, 0};
        btn->GetFont()->DrawText(NULL,
                                 btn->GetTextLabel(),
                                 -1,
                                 &tr,
                                 DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        if (tr.bottom > height)
          height = int(tr.bottom + tr.bottom * 3 / 4 + 0.5);
        if ((tr.right + int(height * 3 / 4 + 0.5)) > width)
          width = tr.right + int(height * 3 / 4 + 0.5); // +height*3/4 for some room

        if (btn->GetCheckState() == -1) // basic button
          width = width < 55 ? 55 : width; // ensure a min width
        else {
          // tick box
          width += tr.bottom; // for the tick zone
          height -= 2;
        }
      }
    } else if (!strcmp(_comp->GetType(), "SNM_KnobCaption")) {
      SNM_KnobCaption *knobCap = (SNM_KnobCaption *) _comp;
      RECT tr = {0, 0, 0, 0};
      knobCap->DrawText(NULL, &tr, DT_NOPREFIX | DT_CALCRECT); // multi-line
      width = SNM_GUI_W_H_KNOB + 2 + tr.right;
      // +2 room between knob & caption, see SNM_KnobCaption::OnPaint()
      height = tr.bottom;
    } else if (!strcmp(_comp->GetType(), "SNM_TwoTinyButtons")) {
      width = 9;
      height = 9 * 2 + 1;
    } else if (!strcmp(_comp->GetType(), "SNM_Knob"))
      width = height = SNM_GUI_W_H_KNOB;


    // *** set position/visibility ***

    // vertical alignment not manget yet (always vcenter)
    _y += int(_h / 2 - height / 2 + 0.5);

    // horizontal alignment
    switch (_align) {
      case DT_LEFT: {
        _comp->SetUserData(DT_LEFT);
        if (*_x + width > _r->right - SNM_GUI_X_MARGIN) // enough horizontal room?
        {
          if (*_x + 25 > (_r->right - SNM_GUI_X_MARGIN)) // ensures a minimum width (25 pix)
          {
            if (_tiedComp && _tiedComp->IsVisible())
              _tiedComp->SetVisible(false);
            return false;
          }
          width = _r->right - SNM_GUI_X_MARGIN - *_x; // force width
        }
        RECT tr = {*_x, _y, *_x + width, _y + height};
        _comp->SetPosition(&tr);
        *_x = tr.right + _xRoom;
        break;
      }
      case DT_RIGHT: {
        _comp->SetUserData(DT_RIGHT);
        if ((*_x - width) > (_r->left + SNM_GUI_X_MARGIN) &&
          /*JFB does not *always* work, replaced with SNM_HasLeftVWnd()
                              !_comp->GetParent()->VirtWndFromPoint(*_x-width, int((_y+_h)/2+0.5))
          */
          !SNM_HasLeftVWnd(_comp, *_x - width, _y, _y + _h)) {
          RECT tr = {*_x - width, _y, *_x, _y + height};
          _comp->SetPosition(&tr);
          *_x = tr.left - _xRoom;
        } else {
          if (_tiedComp && _tiedComp->IsVisible())
            _tiedComp->SetVisible(false);
          return false;
        }
        break;
      }
    }
    _comp->SetVisible(true);
    return true;
  }

  if (_tiedComp && _tiedComp->IsVisible())
    _tiedComp->SetVisible(false);
  return false;
}
