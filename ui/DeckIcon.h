// DeckIcon.h - resource ids of AHKDeck.exe (application icon + About QR codes).
//
// The icon is the AutoHotkey "H" from the pinned AutoHotkey v2.0.25 source
// (GPL-2.0, same license as this product; attribution in
// THIRD-PARTY-NOTICES.md). Sharing one id between the .rc and the C++ side
// keeps LoadImageW() and WM_SETICON from drifting away from the resource.
// The QR ids name the donation SVGs embedded as RCDATA by version_info.rc.in
// (m079); they are the author's own donation codes, not third-party material.
#pragma once

#define IDI_AHKDECK 101
#define ID_QR_WECHAT 102
#define ID_QR_ALIPAY 103
