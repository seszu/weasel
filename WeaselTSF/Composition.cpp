#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

namespace {

HMONITOR MonitorFromInputRect(const RECT& inputRect, DWORD flags) {
  RECT rc = inputRect;
  if (rc.right <= rc.left)
    rc.right = rc.left + 1;
  if (rc.bottom <= rc.top)
    rc.bottom = rc.top + 1;
  return ::MonitorFromRect(&rc, flags);
}

bool TryGetCaretRectOnScreen(HWND referenceWindow, RECT* rc) {
  if (!referenceWindow || !rc)
    return false;

  DWORD threadId = ::GetWindowThreadProcessId(referenceWindow, nullptr);
  if (!threadId)
    return false;

  GUITHREADINFO gui = {};
  gui.cbSize = sizeof(gui);
  if (!::GetGUIThreadInfo(threadId, &gui) || !gui.hwndCaret)
    return false;

  // GUITHREADINFO::rcCaret is relative to hwndCaret. Convert it to screen
  // coordinates explicitly instead of adding a top-level window origin.
  *rc = gui.rcCaret;
  ::SetLastError(ERROR_SUCCESS);
  if (::MapWindowPoints(gui.hwndCaret, HWND_DESKTOP,
                        reinterpret_cast<LPPOINT>(rc), 2) == 0 &&
      ::GetLastError() != ERROR_SUCCESS) {
    return false;
  }

  if (rc->right <= rc->left)
    rc->right = rc->left + 1;
  if (rc->bottom <= rc->top)
    rc->bottom = rc->top + 1;

  return MonitorFromInputRect(*rc, MONITOR_DEFAULTTONULL) != nullptr;
}

void ClampInputRectToMonitor(RECT* rc, HMONITOR monitor) {
  if (!rc || !monitor)
    return;

  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (!::GetMonitorInfo(monitor, &info))
    return;

  LONG width = rc->right - rc->left;
  LONG height = rc->bottom - rc->top;
  if (width < 1)
    width = 1;
  if (height < 1)
    height = 1;

  LONG maxLeft = info.rcWork.right - width;
  LONG maxTop = info.rcWork.bottom - height;
  if (maxLeft < info.rcWork.left)
    maxLeft = info.rcWork.left;
  if (maxTop < info.rcWork.top)
    maxTop = info.rcWork.top;

  LONG left = rc->left;
  LONG top = rc->top;
  if (left < info.rcWork.left)
    left = info.rcWork.left;
  else if (left > maxLeft)
    left = maxLeft;
  if (top < info.rcWork.top)
    top = info.rcWork.top;
  else if (top > maxTop)
    top = maxTop;

  rc->left = left;
  rc->top = top;
  rc->right = left + width;
  rc->bottom = top + height;
}


}  // namespace

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled)
      : CEditSession(pTextService, pContext) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
};

STDMETHODIMP CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  if ((pContextComposition->StartComposition(
           ec, pRangeComposition, _pTextService, &pComposition) == S_OK) &&
      (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* set selection */
    TF_SELECTION tfSelection;
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);

    // The old composition's range is still visible while its asynchronous
    // end session is pending. Position only after the new composition has
    // actually been created, not from the response handler's stale range.
    _pTextService->_UpdateCompositionWindow(_pContext);
  }

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(
      new CStartCompositionEditSession(this, pContext, fCUASWorkaroundEnabled));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE)
      : CEditSession(pTextService, pContext), _clear(clear) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
};

STDMETHODIMP CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  // Drop ownership before EndComposition(). Some applications notify
  // OnCompositionTerminated synchronously while the old composition ends.
  // Keeping it as the current composition makes that normal notification
  // look like an external abort and can clear a new Rime composition during
  // auto-commit.
  if (_pTextService && _pTextService->_IsCurrentComposition(_pComposition))
    _pTextService->_FinalizeComposition();
  _pComposition->EndComposition(ec);
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext,
                                BOOL clear,
                                BOOL endUI) {
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;
  com_ptr<ITfComposition> pComposition = _pComposition;

  if (endUI)
    _cand->EndUI();
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, pComposition, clear)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
};

STDMETHODIMP CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc = {};
  BOOL fClipped = FALSE;
  TF_SELECTION selection;
  ULONG nSelection;

  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection)))
    return E_FAIL;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                     &nSelection)))
    return E_FAIL;

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end / no usable composition range
    pRange = selection.range;
  }

  HWND hwndView = nullptr;
  _pContextView->GetWnd(&hwndView);
  HWND referenceWindow = hwndView ? hwndView : ::GetForegroundWindow();

  RECT rcReference = {};
  const bool hasReferenceRect =
      referenceWindow && ::GetWindowRect(referenceWindow, &rcReference);

  const HRESULT textExtHr =
      _pContextView->GetTextExt(ec, pRange, &rc, &fClipped);

  bool needFallback = FAILED(textExtHr);

  // Some custom text hosts (AutoCAD is a confirmed example) briefly return
  // S_OK with a zero-height sentinel rectangle outside the active text view.
  // A valid TSF rectangle normally follows shortly afterwards, but on rare
  // frames that delay can be much longer than 150 ms.
  //
  // For that specific transient signature, prefer:
  //   1. a valid Win32 caret in the same text view;
  //   2. the most recent trustworthy TSF rectangle for the same text window;
  //   3. short deferral;
  //   4. only then the generic monitor fallback.
  //
  // This avoids flashing at a screen/window corner while preserving the
  // "candidate must eventually remain visible" guarantee.
  static thread_local ULONGLONG transientBadRectSince = 0;
  static thread_local HWND transientBadRectWindow = nullptr;
  static thread_local RECT lastGoodTsfRect = {};
  static thread_local ULONGLONG lastGoodTsfTick = 0;
  static thread_local HWND lastGoodTsfWindow = nullptr;

  bool outsideReferenceWindow = false;
  bool usableReferenceRect = false;
  bool transientLayoutRect = false;

  if (SUCCEEDED(textExtHr)) {
    const bool zeroRect =
        rc.left == 0 && rc.top == 0 && rc.right == 0 && rc.bottom == 0;
    const bool offscreen =
        MonitorFromInputRect(rc, MONITOR_DEFAULTTONULL) == nullptr;

    usableReferenceRect =
        hasReferenceRect && rcReference.right > rcReference.left &&
        rcReference.bottom > rcReference.top;

    if (usableReferenceRect) {
      outsideReferenceWindow =
          rc.left < rcReference.left || rc.left >= rcReference.right ||
          rc.top < rcReference.top || rc.top >= rcReference.bottom;
    }

    const bool zeroHeight = rc.bottom <= rc.top;
    transientLayoutRect =
        usableReferenceRect && outsideReferenceWindow && zeroHeight;

    if (transientLayoutRect) {
      const ULONGLONG now = ::GetTickCount64();

      if (transientBadRectSince == 0 ||
          transientBadRectWindow != referenceWindow) {
        transientBadRectSince = now;
        transientBadRectWindow = referenceWindow;
      }

      // First choice: if the host exposes a genuine Win32 caret, use it.
      RECT rcCaret = {};
      if (TryGetCaretRectOnScreen(referenceWindow, &rcCaret)) {
        const bool caretInsideReference =
            !usableReferenceRect ||
            (rcCaret.left >= rcReference.left &&
             rcCaret.left < rcReference.right &&
             rcCaret.top >= rcReference.top &&
             rcCaret.top < rcReference.bottom);

        if (caretInsideReference) {
          rc = rcCaret;
          _pTextService->_SetCompositionPosition(rc);
          return S_OK;
        }
      }

      // Second choice: reuse the last trustworthy TSF position from this same
      // text window. In AutoCAD this is normally the immediately preceding
      // character position, so the candidate remains close to the real caret
      // until the new layout rectangle arrives.
      bool lastGoodUsable =
          lastGoodTsfWindow == referenceWindow && lastGoodTsfTick != 0 &&
          now - lastGoodTsfTick <= 10000 &&
          MonitorFromInputRect(lastGoodTsfRect, MONITOR_DEFAULTTONULL) !=
              nullptr;

      if (lastGoodUsable && usableReferenceRect) {
        lastGoodUsable =
            lastGoodTsfRect.left >= rcReference.left &&
            lastGoodTsfRect.left < rcReference.right &&
            lastGoodTsfRect.top >= rcReference.top &&
            lastGoodTsfRect.top < rcReference.bottom;
      }

      if (lastGoodUsable) {
        rc = lastGoodTsfRect;
        _pTextService->_SetCompositionPosition(rc);
        return S_OK;
      }

      // There is no safe position to reuse yet. Give the host considerably
      // longer than the old 150 ms before falling back to a screen anchor.
      if (now - transientBadRectSince < 2000) {
        return S_OK;
      }

      needFallback = true;
    } else {
      transientBadRectSince = 0;
      transientBadRectWindow = nullptr;

      needFallback =
          zeroRect || offscreen ||
          (_enhancedPosition && outsideReferenceWindow);

      // Cache only a genuinely accepted TSF rectangle. Do not contaminate
      // this cache with caret/fallback coordinates.
      if (!needFallback) {
        lastGoodTsfRect = rc;
        lastGoodTsfTick = ::GetTickCount64();
        lastGoodTsfWindow = referenceWindow;
      }
    }
  }

  if (needFallback) {
    RECT rcCaret = {};
    if (TryGetCaretRectOnScreen(referenceWindow, &rcCaret)) {
      rc = rcCaret;
    } else {

      LONG width = 1;
      LONG height = 1;
      if (SUCCEEDED(textExtHr)) {
        if (rc.right > rc.left)
          width = rc.right - rc.left;
        if (rc.bottom > rc.top)
          height = rc.bottom - rc.top;
      }

      if (hasReferenceRect) {
        rc.left = rcReference.left;
        rc.top = rcReference.top;
        rc.right = rc.left + width;
        rc.bottom = rc.top + height;
      } else {
        rc = {0, 0, width, height};
      }

      HMONITOR monitor = nullptr;
      if (referenceWindow)
        monitor = ::MonitorFromWindow(referenceWindow, MONITOR_DEFAULTTONEAREST);
      if (!monitor)
        monitor = MonitorFromInputRect(rc, MONITOR_DEFAULTTONEAREST);
      ClampInputRectToMonitor(&rc, monitor);
    }
  }


  _pTextService->_SetCompositionPosition(rc);
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDMETHODIMP CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
}

/* Composition State */
STDMETHODIMP WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                                ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  // EndComposition() may generate this callback for the composition we just
  // closed. Only an active, matching composition is an external termination.
  if (!_IsCurrentComposition(pComposition))
    return S_OK;

  // A host may terminate the empty TSF composition used for a non-inline
  // preedit. Keep Rime's composing state; the next key will create a fresh
  // TSF composition. Only an inactive Rime session should be aborted here.
  if (_status.composing) {
    _FinalizeComposition();
    return S_OK;
  }

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  m_client.ClearComposition();
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}

BOOL WeaselTSF::_IsCurrentComposition(ITfComposition* pComposition) {
  return _pComposition != nullptr && _pComposition == pComposition;
}
