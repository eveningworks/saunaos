// missing16.c -- the Windows calls w16 does NOT answer yet: what each one
// does on Windows 3.1, why w16 does not do it, and which of Windows 3.11's
// own programs and DLLs import it (measured over the 225 binaries of
// Windows for Workgroups 3.11, 2026-10-23). NO CODE: tools/gen_w16_calls.py
// turns these blocks into /usr/share/doc/windows/missing.md, which Retro
// Launcher's What? and `w16 --why` show; w16's build does not compile it.
//
// A block is `// MODULE.ORD Name(args) -> T`, then `Windows:`, `Why:` and
// `By:`, the handlers' shape (gen_w16_calls.py's header). A `Why:` naming
// another call here ("As OpenComm.") is shown with that call's reason.
//
// IMPLEMENTING A CALL MOVES ITS BLOCK onto its new handler, rewritten as
// `Windows:` and `w16:` -- the generator refuses a block here for a call an
// API table answers.

// --- COMMDLG ---------------------------------------------------------------------------------

// COMMDLG.5 ChooseColor(lpcc) -> BOOL
// Windows: Opens the Color dialog -- the 48 basic colours, 16 custom ones and the Define Custom
// Colors half -- and answers the colour chosen.
// Why: Not written yet: w16 draws COMMDLG's Open, Save, Font and Find dialogs itself, and Color is
// the one left.
// By: WINCHAT.EXE, WINMETER.EXE

// --- DDEML -----------------------------------------------------------------------------------

// DDEML.9 DdeQueryConvInfo(hConv, idTransaction, lpConvInfo) -> UINT
// Windows: What DDEML knows of a conversation: its partner, topic, state and the transaction asked
// about.
// Why: A conversation needs another program to talk to, and each program is its own w16
// (DdeConnect's stand-in finds no one) -- the roadmap's "w16: Hearts over the network".
// By: CLIPBRD.EXE, CLIPSRV.EXE, WINCHAT.EXE

// DDEML.10 DdeSetUserHandle(hConv, id, hUser) -> BOOL
// Windows: Stores a value of the program's own with a conversation.
// Why: As DdeQueryConvInfo.
// By: CLIPBRD.EXE, WINCHAT.EXE

// DDEML.12 DdeAbandonTransaction(idInst, hConv, idTransaction) -> BOOL
// Windows: Cancels an asynchronous transaction.
// Why: As DdeQueryConvInfo.
// By: WINCHAT.EXE

// DDEML.15 DdeAddData(hData, pSrc, cb, cbOff) -> HDDEDATA
// Windows: Writes bytes into a DDE data object at an offset, growing it.
// Why: As DdeQueryConvInfo: data objects exist only to travel between programs.
// By: WINCHAT.EXE

// DDEML.17 DdeAccessData(hData, pcbDataSize) -> LPBYTE
// Windows: A pointer to a DDE data object's bytes, and its size.
// Why: As DdeAddData.
// By: CLIPBRD.EXE, CLIPSRV.EXE, WINCHAT.EXE

// DDEML.18 DdeUnaccessData(hData) -> BOOL
// Windows: Ends a DdeAccessData.
// Why: As DdeAddData.
// By: CLIPBRD.EXE, CLIPSRV.EXE, WINCHAT.EXE

// DDEML.19 DdeFreeDataHandle(hData) -> BOOL
// Windows: Frees a DDE data object.
// Why: As DdeAddData.
// By: CLIPBRD.EXE, CLIPSRV.EXE

// DDEML.23 DdeQueryString(idInst, hsz, lpsz, cchMax, codepage) -> DWORD
// Windows: The text of a DDE string handle.
// Why: Not written yet: string handles are DDEML's atoms, which DdeCreateStringHandle already
// makes.
// By: CLIPSRV.EXE, WINCHAT.EXE

// DDEML.24 DdeKeepStringHandle(idInst, hsz) -> BOOL
// Windows: Adds a use to a DDE string handle.
// Why: Not written yet, as DdeQueryString.
// By: CLIPBRD.EXE, CLIPSRV.EXE, WINCHAT.EXE

// DDEML.36 DdeCmpStringHandles(hsz1, hsz2) -> int
// Windows: Compares two DDE string handles' text, ignoring case.
// Why: Not written yet, as DdeQueryString.
// By: CLIPSRV.EXE, WINCHAT.EXE, WINMETER.EXE

// --- GDI -------------------------------------------------------------------------------------

// GDI.40 FillRgn(hDC, hRgn, hBrush) -> BOOL
// Windows: Fills a region with a brush.
// Why: Regions are their bounding rectangles today (the roadmap's "w16: regions as more than their
// bounding rectangle"); with real regions it is a span fill.
// By: FAXVIEW.EXE, FRAMEWRK.DLL, MAIN.CPL, WINTUTOR.EXE

// GDI.41 FrameRgn(hDC, hRgn, hBrush, nWidth, nHeight) -> BOOL
// Windows: Draws a border round a region's edge with a brush, nWidth by nHeight thick.
// Why: As FillRgn.
// By: WINTUTOR.EXE

// GDI.42 InvertRgn(hDC, hRgn) -> BOOL
// Windows: Inverts the colours inside a region.
// Why: As FillRgn.
// By: FRAMEWRK.DLL

// GDI.50 CreateBrushIndirect(lpLogBrush) -> HBRUSH
// Windows: A brush from a LOGBRUSH: solid, hatched, patterned or hollow.
// Why: Not written yet: the brushes it describes are the ones CreateSolidBrush, CreateHatchBrush
// and CreatePatternBrush already make.
// By: POWER.DRV, SCHDPLUS.EXE, USER.EXE

// GDI.65 CreateRectRgnIndirect(lpRect) -> HRGN
// Windows: A rectangular region from a RECT.
// Why: Not written yet: CreateRectRgn from a structure -- one line.
// By: CARDFILE.EXE, FRAMEWRK.DLL, MAIN.CPL, TERMINAL.EXE, USER.EXE, WINMETER.EXE

// GDI.76 GetBkMode(hDC) -> int
// Windows: Whether text and hatches fill their background (OPAQUE) or leave it (TRANSPARENT).
// Why: Not written yet: the DC keeps it for SetBkMode.
// By: RASPHONE.EXE, USER.EXE

// GDI.78 GetCurrentPosition(hDC) -> DWORD
// Windows: Where MoveTo left the pen, x in the low word.
// Why: Not written yet: the DC keeps it for LineTo.
// By: USER.EXE, WINTUTOR.EXE

// GDI.84 GetPolyFillMode(hDC) -> int
// Windows: Whether Polygon fills by ALTERNATE or WINDING.
// Why: Not written yet: the DC keeps it for SetPolyFillMode.
// By: RASPHONE.EXE, WFWNET.DRV

// GDI.98 IntersectVisRect(hDC, X1, Y1, X2, Y2) -> int
// Windows: Narrows a DC's visible region to a rectangle -- USER's, for painting a window's own
// part.
// Why: Windows' own: USER's. w16's DCs are clipped by w16 itself.
// By: USER.EXE

// GDI.101 OffsetRgn(hRgn, nXOffset, nYOffset) -> int
// Windows: Moves a region.
// Why: Not written yet: a region is a rectangle in w16 today, so this is a few lines, and it will
// follow real regions.
// By: FRAMEWRK.DLL, USER.EXE

// GDI.105 SelectVisRgn(hDC, hRgn) -> int
// Windows: Sets a DC's visible region -- USER's, when it hands out a window's DC.
// Why: Windows' own, as IntersectVisRect.
// By: USER.EXE

// GDI.117 SetDCOrg(hDC, X, Y) -> DWORD
// Windows: Moves a DC's origin on the screen -- USER's, when a window moves.
// Why: Windows' own, as IntersectVisRect.
// By: USER.EXE

// GDI.119 AddFontResource(lpFilename) -> int
// Windows: Adds a font file (.FON, .FOT) to what every program may use.
// Why: Not written yet: w16 reads C:'s fonts at start; one added later would join them for this
// program.
// By: MAIN.CPL, USER.EXE, VFORMS.DLL, WINSETUP.EXE

// GDI.121 Death(hDC) -> BOOL
// Windows: Disables the display driver while a DOS program runs full screen.
// Why: Windows' own: WINOLDAP's.
// By: USER.EXE

// GDI.122 Resurrection(hDC, w1, w2, w3, w4, w5, w6) -> void
// Windows: Re-enables the display driver after Death.
// Why: As Death.
// By: USER.EXE

// GDI.124 GetMetaFile(lpFilename) -> HMETAFILE
// Windows: A metafile handle on a .WMF file on disk.
// Why: Not written yet: w16 plays and records metafiles in memory; reading one from a file is a
// small addition.
// By: PRINTMAN.EXE

// GDI.129 SaveVisRgn(hDC) -> HRGN
// Windows: Saves a DC's visible region -- USER's.
// Why: Windows' own, as IntersectVisRect.
// By: USER.EXE

// GDI.130 RestoreVisRgn(hDC) -> int
// Windows: Puts back the visible region SaveVisRgn saved -- USER's.
// Why: Windows' own, as IntersectVisRect.
// By: USER.EXE

// GDI.131 InquireVisRgn(hDC) -> HRGN
// Windows: A DC's visible region -- USER's.
// Why: Windows' own, as IntersectVisRect.
// By: USER.EXE

// GDI.132 SetEnvironment(lpPortName, lpEnviron, nCount) -> int
// Windows: Stores a printer's environment (its DEVMODE) for a port.
// Why: No printing (the roadmap's "w16: printing").
// By: GENDRV.DLL, HPPCL5E.DRV, HPPLOT.DRV, PSCRIPT.DRV, TTY.DRV, UNIDRV.DLL

// GDI.133 GetEnvironment(lpPortName, lpEnviron, nMaxCount) -> int
// Windows: A port's stored printer environment.
// Why: As SetEnvironment.
// By: GENDRV.DLL, HPPCL5E.DRV, HPPLOT.DRV, PSCRIPT.DRV, TTY.DRV, UNIDRV.DLL

// GDI.134 GetRgnBox(hRgn, lpRect) -> int
// Windows: A region's bounding rectangle, and whether it is empty, simple or complex.
// Why: Not written yet: in w16 today the rectangle is the region.
// By: USER.EXE

// GDI.136 RemoveFontResource(lpFilename) -> BOOL
// Windows: Takes a font AddFontResource added away again.
// Why: As AddFontResource.
// By: MAIN.CPL, VFORMS.DLL

// GDI.151 CopyMetaFile(hMF, lpFilename) -> HMETAFILE
// Windows: A copy of a metafile, in memory or written to a file.
// Why: Not written yet, as GetMetaFile.
// By: DDEML.DLL, OLECLI.DLL, OLESVR.DLL, PACKAGER.EXE, STORE.DLL, VFORMS.DLL

// GDI.155 QueryAbort(hDC, nReserved) -> BOOL
// Windows: Asks a printer DC's abort procedure whether to go on.
// Why: As SetEnvironment.
// By: EFAXDRV.DRV, GENDRV.DLL, UNIDRV.DLL

// GDI.161 PtInRegion(hRgn, X, Y) -> BOOL
// Windows: Whether a point lies inside a region.
// Why: Not written yet: in w16 today, inside the region's rectangle; it will follow real regions.
// By: MAIN.CPL

// GDI.173 GetClipRgn(hDC) -> HRGN
// Windows: The DC's clipping region (the handle itself, which the program must not delete).
// Why: Not written yet: w16's DC keeps a clip rectangle.
// By: USER.EXE

// GDI.175 EnumMetaFile(hDC, hMF, lpCallbackFunc, lParam) -> BOOL
// Windows: Calls a procedure for each record in a metafile, with the handle table it builds.
// Why: Not written yet: w16's metafile decoder (lib/uwmf) walks the records already.
// By: CLIPBRD.EXE, OLECLI.DLL, PRINTMAN.EXE

// GDI.176 PlayMetaFileRecord(hDC, lpHandletable, lpMetaRecord, nHandles) -> void
// Windows: Plays one record of a metafile -- EnumMetaFile's partner.
// Why: Not written yet, as EnumMetaFile.
// By: CLIPBRD.EXE, OLECLI.DLL, PRINTMAN.EXE

// GDI.179 GetDCState(hDC) -> HDC
// Windows: A copy of a DC's state, for SetDCState -- USER's.
// Why: Windows' own: USER's.
// By: USER.EXE

// GDI.180 SetDCState(hDC, hSavedDC) -> void
// Windows: Puts back a DC's state from GetDCState's copy.
// Why: Windows' own, as GetDCState.
// By: USER.EXE

// GDI.181 RectInRegionOld(hRgn, lpRect) -> BOOL
// Windows: RectInRegion's older entry: whether any of a rectangle lies inside a region.
// Why: Not written yet, nor RectInRegion itself, as PtInRegion.
// By: USER.EXE

// GDI.190 SetDCHook(hDC, lpfnHook, dwHookData) -> BOOL
// Windows: Hooks a DC so USER hears when it is reset or its region changes.
// Why: Windows' own: USER's.
// By: USER.EXE

// GDI.192 SetHookFlags(hDC, wFlags) -> WORD
// Windows: Marks a hooked DC dirty or clean -- USER's.
// Why: Windows' own, as SetDCHook.
// By: USER.EXE

// GDI.193 SetBoundsRect(hDC, lprcBounds, flags) -> UINT
// Windows: Starts, stops or resets accumulating the rectangle a DC's drawing touches.
// Why: Not written yet: w16 tracks what a window's drawing touched for its own presents, and could
// keep it per DC.
// By: USER.EXE

// GDI.194 GetBoundsRect(hDC, lprcBounds, flags) -> UINT
// Windows: The rectangle a DC's drawing has touched since SetBoundsRect.
// Why: Not written yet, as SetBoundsRect.
// By: USER.EXE

// GDI.201 dmBitBlt() -> void
// Windows: The default bit-block transfer a display driver may hand its work to -- one of GDI's
// dm* "brute" routines for drivers.
// Why: A display driver's: w16 carries no Windows display driver and draws everything itself.
// By: HPPCL5E2.DLL, PSCRIPT.DRV, UNIDRV.DLL

// GDI.202 dmColorInfo() -> void
// Windows: A driver's default colour matching, as dmBitBlt.
// Why: As dmBitBlt.
// By: GENDRV.DLL, HPPCL5E2.DLL, PSCRIPT.DRV, TTY.DRV, UNIDRV.DLL

// GDI.206 dmEnumDFonts() -> void
// Windows: A driver's default font enumeration, as dmBitBlt.
// Why: As dmBitBlt.
// By: UNIDRV.DLL

// GDI.207 dmEnumObj() -> void
// Windows: A driver's default pen and brush enumeration, as dmBitBlt.
// Why: As dmBitBlt.
// By: GENDRV.DLL, UNIDRV.DLL

// GDI.208 dmOutput() -> void
// Windows: A driver's default line and shape output, as dmBitBlt.
// Why: As dmBitBlt.
// By: HPPCL5E2.DLL, PSCRIPT.DRV, UNIDRV.DLL

// GDI.209 dmPixel() -> void
// Windows: A driver's default pixel reading and setting, as dmBitBlt.
// Why: As dmBitBlt.
// By: HPPCL5E2.DLL, PSCRIPT.DRV, UNIDRV.DLL

// GDI.210 dmRealizeObject() -> void
// Windows: A driver's default making of pens and brushes, as dmBitBlt.
// Why: As dmBitBlt.
// By: HPPCL5E2.DLL, PSCRIPT.DRV, UNIDRV.DLL

// GDI.211 dmStrBlt() -> void
// Windows: A driver's default text output, as dmBitBlt.
// Why: As dmBitBlt.
// By: GENDRV.DLL, PSCRIPT.DRV, UNIDRV.DLL

// GDI.212 dmScanLR() -> void
// Windows: A driver's default scan for a colour along a row (flood fills), as dmBitBlt.
// Why: As dmBitBlt.
// By: HPPCL5E2.DLL, PSCRIPT.DRV, UNIDRV.DLL

// GDI.214 dmExtTextOut() -> void
// Windows: A driver's default ExtTextOut, as dmBitBlt.
// Why: As dmBitBlt.
// By: HPPCL5E2.DLL

// GDI.220 dmTranspose() -> void
// Windows: A driver's default bitmap transposition, as dmBitBlt.
// Why: As dmBitBlt.
// By: GENDRV.DLL

// GDI.230 CreatePQ(size) -> HPQ
// Windows: A priority queue -- GDI's, kept for the print spooler.
// Why: No printing, as SetEnvironment: the spooler's.
// By: GENDRV.DLL, TTY.DRV

// GDI.231 MinPQ(hPQ) -> int
// Windows: The lowest key in a priority queue.
// Why: As CreatePQ.
// By: GENDRV.DLL

// GDI.232 ExtractPQ(hPQ) -> int
// Windows: Takes the lowest key out of a priority queue.
// Why: As CreatePQ.
// By: GENDRV.DLL, TTY.DRV

// GDI.233 InsertPQ(hPQ, tag, key) -> int
// Windows: Adds a key to a priority queue.
// Why: As CreatePQ.
// By: GENDRV.DLL, TTY.DRV

// GDI.234 SizePQ(hPQ, sizechange) -> int
// Windows: Grows or shrinks a priority queue.
// Why: As CreatePQ.
// By: GENDRV.DLL, TTY.DRV

// GDI.235 DeletePQ(hPQ) -> int
// Windows: Frees a priority queue.
// Why: As CreatePQ.
// By: GENDRV.DLL, TTY.DRV

// GDI.240 OpenJob(lpOutput, lpTitle, hDC) -> HPJOB
// Windows: Opens a spool job for a printer port -- printer drivers'.
// Why: As SetEnvironment.
// By: FINSTALL.DLL, GENDRV.DLL, HPPCL5E.DRV, HPPLOT.DRV, PSCRIPT.DRV, TTY.DRV, and 1 more

// GDI.241 WriteSpool(hJob, lpData, cch) -> int
// Windows: Writes bytes to a spool job.
// Why: As SetEnvironment.
// By: FINSTALL.DLL, GENDRV.DLL, HPPCL5E.DRV, HPPLOT.DRV, PSCRIPT.DRV, TTY.DRV, and 1 more

// GDI.242 WriteDialog(hJob, lpMsg, cch) -> int
// Windows: Shows the spooler's message box for a job.
// Why: As SetEnvironment.
// By: HPPLOT.DRV, TTY.DRV, UNIDRV.DLL

// GDI.243 CloseJob(hJob) -> int
// Windows: Closes a spool job, handing it to Print Manager.
// Why: As SetEnvironment.
// By: FINSTALL.DLL, GENDRV.DLL, HPPCL5E.DRV, HPPCL5E2.DLL, HPPLOT.DRV, PSCRIPT.DRV, and 2 more

// GDI.244 DeleteJob(hJob, wDummy) -> int
// Windows: Throws a spool job away.
// Why: As SetEnvironment.
// By: GENDRV.DLL, HPPCL5E.DRV, HPPCL5E2.DLL, PSCRIPT.DRV, TTY.DRV, UNIDRV.DLL

// GDI.245 GetSpoolJob(wOption, lpBuf) -> DWORD
// Windows: Print Manager's view of a queued job.
// Why: As SetEnvironment.
// By: MAIN.CPL, PRINTMAN.EXE, WFWNET.DRV

// GDI.246 StartSpoolPage(hJob) -> int
// Windows: Begins a page of a spool job.
// Why: As SetEnvironment.
// By: FINSTALL.DLL, GENDRV.DLL, HPPCL5E.DRV, HPPCL5E2.DLL, HPPLOT.DRV, IBM4019.DRV, and 5 more

// GDI.247 EndSpoolPage(hJob) -> int
// Windows: Ends a page of a spool job.
// Why: As SetEnvironment.
// By: FINSTALL.DLL, GENDRV.DLL, HPPCL5E.DRV, HPPCL5E2.DLL, HPPLOT.DRV, IBM4019.DRV, and 5 more

// GDI.300 EngineEnumerateFont(lpFaceName, lpfnCallback, fu) -> DWORD
// Windows: Lists the TrueType rasterizer's fonts for a printer driver.
// Why: A driver's: w16 rasterizes TrueType itself.
// By: GENDRV.DLL, HPPCL5E3.DLL, PSCRIPT.DRV

// GDI.301 EngineDeleteFont(lpFontInfo) -> int
// Windows: Frees a font the rasterizer realized for a driver.
// Why: As EngineEnumerateFont.
// By: GENDRV.DLL, HPPCL5E3.DLL, PSCRIPT.DRV

// GDI.302 EngineRealizeFont(lpLogFont, lpTextXform, lpFontInfo) -> int
// Windows: Makes a TrueType font at a size for a driver.
// Why: As EngineEnumerateFont.
// By: GENDRV.DLL, HPPCL5E3.DLL, PSCRIPT.DRV

// GDI.303 EngineGetCharWidth(lpFontInfo, bFirst, bLast, lpWidths) -> int
// Windows: A realized TrueType font's widths, for a driver.
// Why: As EngineEnumerateFont.
// By: GENDRV.DLL, HPPCL5E3.DLL, PSCRIPT.DRV

// GDI.304 EngineSetFontContext(lpFontInfo, data) -> int
// Windows: Points the rasterizer at a font for a driver.
// Why: As EngineEnumerateFont.
// By: GENDRV.DLL, HPPCL5E3.DLL, IBM4019.DRV, PSCRIPT.DRV

// GDI.305 EngineGetGlyphBMP(wFontInfo, lpFontInfo, wChar, wFormat, lpMetrics, cbBuffer, lpBuffer, lpMat2) -> DWORD
// Windows: One glyph's bitmap from the rasterizer, for a driver.
// Why: As EngineEnumerateFont.
// By: GENDRV.DLL, HPPCL5E3.DLL, IBM4019.DRV, PSCRIPT.DRV

// GDI.310 CreateScalableFontResource(fHidden, lpszResourceFile, lpszFontFile, lpszCurrentPath) -> BOOL
// Windows: Writes a .FOT file naming a TrueType .TTF, so it can be installed.
// Why: Not written yet: Control Panel's Fonts, not a running program's -- w16 reads C:'s .TTF
// files directly.
// By: MAIN.CPL

// GDI.311 GetFontData(hDC, dwTable, dwOffset, lpvBuffer, cbData) -> DWORD
// Windows: Bytes from a TrueType font's file: one table, or the whole file.
// Why: Not written yet: w16 has the font's file open for its rasterizer; handing a program the
// bytes is small.
// By: PSCRIPT.DRV

// GDI.313 GetRasterizerCaps(lpraststat, cb) -> BOOL
// Windows: Whether TrueType is installed and enabled.
// Why: Not written yet: w16 would answer that it is, both.
// By: HPPCL5E3.DLL, MAIN.CPL

// GDI.354 ShrinkGDIHeap() -> void
// Windows: Compacts GDI's heap.
// Why: Windows' own: USER's, under low memory.
// By: USER.EXE

// GDI.361 GDISelectPalette(hDC, hPal, bForceBackground) -> HPALETTE
// Windows: SelectPalette's GDI half -- USER's.
// Why: Windows' own: USER's SelectPalette is built on it.
// By: USER.EXE

// GDI.362 GDIRealizePalette(hDC) -> UINT
// Windows: RealizePalette's GDI half -- USER's.
// Why: Windows' own, as GDISelectPalette.
// By: USER.EXE

// GDI.364 SetPaletteEntries(hPal, wStartIndex, wNumEntries, lpPalEntries) -> UINT
// Windows: Changes colours of a logical palette.
// Why: Not written yet: w16 keeps a palette's entries for GetPaletteEntries, so setting them is
// small; no palette is realized (UpdateColors).
// By: DDEML.DLL

// GDI.365 RealizeDefaultPalette(hDC) -> void
// Windows: Puts the system palette back to its 20 static colours -- USER's.
// Why: Windows' own: USER's.
// By: USER.EXE

// GDI.374 GetSystemPaletteUse(hDC) -> UINT
// Windows: Whether the system palette keeps its static colours (SYSPAL_STATIC).
// Why: Not written yet: w16 would answer SYSPAL_STATIC -- it has no palette to give up.
// By: USER.EXE

// GDI.377 StartDoc(hDC, lpdi) -> int
// Windows: Begins a print job on a printer DC.
// Why: No printing: PrintDlg answers that there is no printer, so no program gets a printer DC
// (the roadmap's "w16: printing").
// By: PRINTMAN.EXE

// GDI.378 EndDoc(hDC) -> int
// Windows: Ends a print job.
// Why: As StartDoc.
// By: FAXVIEW.EXE, PRINTMAN.EXE

// GDI.379 StartPage(hDC) -> int
// Windows: Begins a printed page.
// Why: As StartDoc.
// By: PRINTMAN.EXE

// GDI.380 EndPage(hDC) -> int
// Windows: Ends a printed page and sends it.
// Why: As StartDoc.
// By: PRINTMAN.EXE

// GDI.381 SetAbortProc(hDC, lpAbortProc) -> int
// Windows: Names the procedure a print job calls to let its program cancel.
// Why: As StartDoc.
// By: FAXCOVER.DLL, PRINTMAN.EXE

// GDI.382 AbortDoc(hDC) -> int
// Windows: Cancels a print job.
// Why: As StartDoc.
// By: FAXCOVER.DLL, PRINTMAN.EXE

// GDI.400 FastWindowFrame(hDC, lpRect, xWidth, yWidth, rop) -> BOOL
// Windows: Draws a window's sizing frame quickly -- USER's, for a drag outline.
// Why: Windows' own: USER's. toywm draws drag outlines.
// By: USER.EXE

// GDI.405 FinalGdiInit(hbrDefault) -> void
// Windows: GDI's last start-up step, called by USER.
// Why: Windows' own: USER's.
// By: USER.EXE

// GDI.407 CreateUserBitmap(nWidth, nHeight, cPlanes, cBitsPerPixel, lpBits) -> HBITMAP
// Windows: A bitmap owned by USER, not by the task that asked -- USER's.
// Why: Windows' own: USER's.
// By: USER.EXE

// GDI.409 CreateUserDiscardableBitmap(hDC, nWidth, nHeight) -> HBITMAP
// Windows: A discardable bitmap owned by USER -- USER's.
// Why: Windows' own, as CreateUserBitmap.
// By: USER.EXE

// GDI.410 IsValidMetaFile(hmf) -> BOOL
// Windows: Whether a handle is a valid metafile.
// Why: Not written yet: w16 knows its metafile handles.
// By: USER.EXE

// GDI.411 GetCurLogFont(hDC) -> HFONT
// Windows: The font selected into a DC -- USER's.
// Why: Windows' own: USER's; w16 keeps it per DC and could answer.
// By: USER.EXE

// GDI.412 IsDCCurrentPalette(hDC) -> BOOL
// Windows: Whether a DC's palette is the one in the foreground -- USER's.
// Why: Windows' own, as GDISelectPalette.
// By: USER.EXE

// GDI.460 GdiTaskTermination(hTask) -> void
// Windows: Frees what GDI kept for a task that ended -- USER's.
// Why: Windows' own: USER's. A w16 program's GDI objects go with its process.
// By: USER.EXE

// GDI.462 IsGDIObject(hObj) -> int
// Windows: What kind of GDI object a handle is (a pen, a brush, a font...), or 0.
// Why: Not written yet: w16's object table knows each handle's kind.
// By: USER.EXE

// GDI.463 MakeObjectPrivate(hObj, bPrivate) -> void
// Windows: Marks a GDI object as one task's -- USER's.
// Why: Windows' own: USER's.
// By: USER.EXE

// GDI.464 FixupBogusPublisherMetafile(hmf, dwVersion) -> void
// Windows: Repairs metafiles an early Microsoft Publisher wrote wrongly.
// Why: Windows' own: a fix for one program's files, which no program here needs.
// By: USER.EXE

// GDI.467 UnicodeToAnsi(lpUnicode, lpAnsi) -> void
// Windows: Converts a Unicode string to the code page -- an internal of GDI's TrueType code.
// Why: Windows' own: GDI's.
// By: MAIN.CPL

// GDI.471 GetTextExtentPoint(hDC, lpString, cbString, lpSize) -> BOOL
// Windows: The width and height a string would take, in a SIZE structure (Windows 3.1's form of
// GetTextExtent).
// Why: Not written yet: GetTextExtent's answer in a structure -- a few lines.
// By: RASMON.EXE

// GDI.483 MoveToEx(hDC, X, Y, lpPoint) -> BOOL
// Windows: Moves the pen to a point, answering the old one in a POINT.
// Why: Not written yet: MoveTo with a structure -- a few lines.
// By: RASPHONE.EXE

// --- KERNEL ----------------------------------------------------------------------------------

// KERNEL.2 ExitKernel() -> void
// Windows: Shuts KERNEL down: the last step of leaving Windows, made by USER once every program
// has closed.
// Why: Windows' own: only USER calls it. A program leaves through ExitWindows, and a w16 program's
// end is its own process ending.
// By: USER.EXE

// KERNEL.28 GlobalMasterHandle() -> DWORD
// Windows: The handle and selector of KERNEL's table of every global block -- the burgermaster --
// which TOOLHELP walks to list memory.
// Why: A debugger's view of Windows' own heap; w16 keeps no such table for a program to walk.
// By: TOOLHELP.DLL

// KERNEL.29 Yield() -> void
// Windows: Gives the CPU to the next task that has a message waiting, and returns when this one is
// scheduled again.
// Why: Not written yet: with one program per w16 there is no other task to yield to, so it would
// return at once -- a small handler waiting for its first caller.
// By: DDEML.DLL, EFAXPUMP.DLL, FAXSTUB.DLL, FAXVIEW.EXE, MCISEQ.DRV, MCIWAVE.DRV, and 8 more

// KERNEL.31 PostEvent(hTask) -> void
// Windows: Wakes a task that is waiting in WaitEvent, as USER does when it posts a message to it.
// Why: Windows' own: USER's way of waking a task. w16's queue wakes its one task itself.
// By: USER.EXE

// KERNEL.33 LockCurrentTask(fLock) -> HANDLE
// Windows: Stops (or allows) task switching, so the current task keeps the CPU -- what a modal
// system dialog does.
// Why: Windows' own: USER's, for system-modal state. w16 runs one task.
// By: USER.EXE

// KERNEL.34 SetTaskQueue(hTask, hQueue) -> HANDLE
// Windows: Gives a task its message queue: USER makes the queue, KERNEL records it.
// Why: Windows' own: USER and KERNEL sharing the task database. w16 keeps its queue itself.
// By: USER.EXE

// KERNEL.35 GetTaskQueue(hTask) -> HANDLE
// Windows: The message queue a task was given.
// Why: Windows' own, as SetTaskQueue.
// By: USER.EXE

// KERNEL.38 SetTaskSignalProc(hTask, lpfnProc) -> FARPROC
// Windows: Names the procedure KERNEL calls to tell a task of events such as its end; USER
// installs its own.
// Why: Windows' own: USER's hook into task deaths.
// By: USER.EXE

// KERNEL.45 LoadModule(lpModuleName, lpParameterBlock) -> HINSTANCE
// Windows: Loads and starts a program or DLL, given its file name and a block of command line,
// environment and show state; WinExec is built on it.
// Why: Starting a second program is the roadmap's "w16: one program starting or finding another":
// each program is its own w16 today.
// By: IFKERNEL.DLL, MMSYSTEM.DLL, OLECLI.DLL, SCHEDMSG.DLL

// KERNEL.46 FreeModule(hModule) -> BOOL
// Windows: Drops one use of a module; at none it is unloaded (a DLL's WEP is called first).
// Why: Not written yet: w16 loads a program's DLLs once and keeps them for its life, so there is
// no use count to drop.
// By: GDI.EXE, OLECLI.DLL

// KERNEL.67 SetResourceHandler(hInst, lpType, lpfnHandler) -> FARPROC
// Windows: Installs the procedure that loads one type of resource, as USER does for icons and
// cursors.
// Why: Windows' own: USER's. w16 loads every resource type itself.
// By: USER.EXE

// KERNEL.68 InitAtomTable(nSize) -> BOOL
// Windows: Sizes the task's local atom table (a hash of nSize buckets) before its first AddAtom.
// Why: Not written yet: w16's atom table grows as it is used, so it has nothing to size --
// answering TRUE would be whole.
// By: GDI.EXE, USER.EXE

// KERNEL.69 FindAtom(lpString) -> ATOM
// Windows: The atom a string was added as, or 0, without adding it.
// Why: Not written yet: no program run so far has looked up an atom without adding it.
// By: GDI.EXE, USER.EXE

// KERNEL.73 GetAtomHandle(nAtom) -> HANDLE
// Windows: The local-heap handle of an atom's entry.
// Why: Windows' own: USER's, which reads atom entries in place. w16's atoms are not local-heap
// blocks.
// By: USER.EXE

// KERNEL.76 DeletePathname(lpPathName) -> int
// Windows: Deletes a file -- DOS's INT 21h function 41h, exported for Windows' own components.
// Why: Not written yet: the same as OpenFile with OF_DELETE, which w16 answers.
// By: GDI.EXE, PRINTMAN.EXE

// KERNEL.77 Reserved1(lpString) -> LPSTR
// Windows: An undocumented KERNEL export Windows' components use; the 3.1 SDK lists it only by
// number.
// Why: Undocumented: what it answers is not established well enough to answer it.
// By: GDI.EXE

// KERNEL.78 Reserved2(lpDest, lpSrc) -> LPSTR
// Windows: An undocumented KERNEL export Windows' components use; the 3.1 SDK lists it only by
// number.
// Why: Undocumented, as Reserved1.
// By: GDI.EXE

// KERNEL.79 Reserved3(lpString) -> LPSTR
// Windows: An undocumented KERNEL export Windows' components use; the 3.1 SDK lists it only by
// number.
// Why: Undocumented, as Reserved1.
// By: CAN_ADF.EXE, SF4019.EXE

// KERNEL.87 Reserved5(lpString1, lpString2) -> int
// Windows: An undocumented KERNEL export Windows' components use; the 3.1 SDK lists it only by
// number.
// Why: Undocumented, as Reserved1.
// By: CAN_ADF.EXE, EFAXPUMP.DLL, FAXCOVER.DLL, FAXOPT.DLL, GDI.EXE, LINEARIZ.DLL, and 2 more

// KERNEL.101 NoHookDosCall() -> void
// Windows: DOS's INT 21h, called with registers set, past any hook a debugger or TSR put on it.
// Why: Not written yet: w16 answers INT 21h itself and has no hooks to step past, so it would be
// DOS3Call under another name.
// By: GDI.EXE, HPSYSTEM.DRV, SYSTEM.DRV

// KERNEL.103 NetBIOSCall() -> void
// Windows: The NetBIOS interrupt (INT 5Ch), called with ES:BX at a network control block.
// Why: toy-os's network is not NetBIOS; a network driver's call.
// By: MSNET.DRV, MSSFS.DLL, NDDENB.DLL

// KERNEL.104 GetCodeInfo(lpProc, lpSegInfo) -> BOOL
// Windows: Fills a block with what the module's segment table says of the segment a procedure
// lives in: its offset in the file, its size and flags.
// Why: Not written yet: a driver's call; w16 has the segment table and could answer.
// By: MMSYSTEM.DLL, USER.EXE

// KERNEL.105 GetExeVersion() -> WORD
// Windows: The Windows version the current task's executable was linked for (3.0 is 0x0300).
// Why: Not written yet: the NE header's expected-version field, which w16 reads already.
// By: GDI.EXE, USER.EXE

// KERNEL.106 SetSwapAreaSize(rsSize) -> LONG
// Windows: Asks for code-swap space of rsSize paragraphs, real mode's way of keeping code in
// memory.
// Why: Not written yet: w16 never swaps code, so it would answer the size asked for.
// By: TERMINAL.EXE

// KERNEL.111 GlobalWire(hMem) -> LPVOID
// Windows: Moves a block low in memory and locks it there, for one that stays locked a long time.
// Why: Not written yet: w16's blocks never move, so it would be GlobalLock.
// By: 8514.DRV, GDI.EXE, MIDIMAP.DRV, RECORDER.DLL, TERMINAL.EXE, TIMER.DRV, and 1 more

// KERNEL.112 GlobalUnWire(hMem) -> BOOL
// Windows: Unlocks a block GlobalWire locked.
// Why: Not written yet, as GlobalWire.
// By: GDI.EXE, MIDIMAP.DRV, TERMINAL.EXE

// KERNEL.117 OldYield() -> void
// Windows: Yield's older form, kept for USER's own use.
// Why: Windows' own: USER's.
// By: USER.EXE

// KERNEL.118 GetTaskQueueDS() -> void
// Windows: Loads DS with the current task's queue segment, for USER.
// Why: Windows' own: USER's.
// By: USER.EXE

// KERNEL.119 GetTaskQueueES() -> void
// Windows: Loads ES with the current task's queue segment, for USER.
// Why: Windows' own: USER's.
// By: USER.EXE

// KERNEL.122 IsTaskLocked() -> HANDLE
// Windows: The task LockCurrentTask locked to, or 0.
// Why: Windows' own, as LockCurrentTask.
// By: MMSYSTEM.DLL

// KERNEL.123 KbdRst() -> void
// Windows: Resets the keyboard controller -- an export kept for the keyboard drivers.
// Why: A driver's call: w16 carries no Windows drivers.
// By: KBDHP.DRV, KEYBOARD.DRV

// KERNEL.130 FileCDR(lpFunc) -> FARPROC
// Windows: Registers the procedure KERNEL calls when a file is created, renamed or deleted -- File
// Manager's way of hearing of changes.
// Why: Windows' own: USER and File Manager's. A program run under w16 is told nothing of other
// programs' files.
// By: USER.EXE, WINFILE.EXE

// KERNEL.131 GetDOSEnvironment() -> LPSTR
// Windows: A pointer to the task's DOS environment: NAME=value strings, an empty one ending them.
// Why: Not written yet, and wanted: a program reading PATH or TEMP stops here. w16 would answer a
// block built from the settings it gives drive C:.
// By: AWCAS.DLL, DEMILAYR.DLL, EFAXDRV.DRV, EFAXPUMP.DLL, FAXCODEC.DLL, FAXCOVER.DLL, and 14 more

// KERNEL.133 GetExePtr(hInstance) -> HMODULE
// Windows: The module a handle belongs to -- an instance, a task or a module handle.
// Why: Not written yet: w16 knows which module each instance is (GetModuleHandle).
// By: 8514.DRV, SUPERVGA.DRV, USER.EXE, VGA.DRV

// KERNEL.138 GetHeapSpaces(hModule) -> DWORD
// Windows: The free and the total size of a module's local heap, for USER's and GDI's resource
// meter.
// Why: Windows' own: the "system resources" figure. w16's heaps grow as they need.
// By: USER.EXE

// KERNEL.139 DoSignal() -> void
// Windows: Sends a task the signal its signal procedure hears.
// Why: Windows' own, as SetTaskSignalProc.
// By: KBDHP.DRV, KEYBOARD.DRV

// KERNEL.150 DirectedYield(hTask) -> void
// Windows: Yields to one named task, which runs next.
// Why: Not written yet: with no other task, it would return at once, as Yield.
// By: TOOLHELP.DLL, USER.EXE

// KERNEL.152 GetNumTasks() -> int
// Windows: How many tasks are running.
// Why: Not written yet: w16 would answer 1 -- each program is its own w16.
// By: PROGMAN.EXE, WINFILE.EXE, WINSETUP.EXE

// KERNEL.155 GetTaskDS() -> WORD
// Windows: The data segment of the current task.
// Why: Windows' own: USER's.
// By: USER.EXE

// KERNEL.158 IsWinOldApTask(hTask) -> BOOL
// Windows: Whether a task is a DOS program run in a window (WINOLDAP).
// Why: Not written yet: w16 would answer FALSE -- it runs Windows programs only.
// By: TASKMAN.EXE, USER.EXE

// KERNEL.159 GlobalHandleNoRIP(wSelector) -> DWORD
// Windows: GlobalHandle without the debug build's error report for a bad selector.
// Why: Not written yet: GlobalHandle under another name for w16, which has no debug build.
// By: MMSYSTEM.DLL

// KERNEL.161 LocalCountFree() -> WORD
// Windows: The free bytes in the current local heap.
// Why: Not written yet: w16's local heap knows its free blocks.
// By: GDI.EXE, MMSYSTEM.DLL, USER.EXE

// KERNEL.162 LocalHeapSize() -> WORD
// Windows: The current local heap's size in bytes.
// Why: Not written yet, as LocalCountFree.
// By: GDI.EXE, MMSYSTEM.DLL, USER.EXE

// KERNEL.167 GetExpWinVer(hInstance) -> WORD
// Windows: The Windows version a module was linked for, from its NE header.
// Why: Not written yet: as GetExeVersion, for any module.
// By: GDI.EXE, USER.EXE

// KERNEL.170 AllocCStoDSAlias(wSelector) -> WORD
// Windows: A data selector for a code segment, so code can be read or patched through it.
// Why: Not written yet: w16's CPU has selectors and could alias one.
// By: 8514.DRV, KBDHP.DRV, KEYBOARD.DRV, OLECLI.DLL, SUPERVGA.DRV, SVGA256.DRV, and 4 more

// KERNEL.171 AllocDStoCSAlias(wSelector) -> WORD
// Windows: A code selector for a data segment, so generated code there can run -- how a driver
// builds a thunk.
// Why: Not written yet, as AllocCStoDSAlias.
// By: CPWIN386.CPL, LMOUSE.DRV, MOUSE.DRV, MSC3BC2.DRV, MSCMOUSE.DRV, NCDW.DLL, and 1 more

// KERNEL.173 __ROMBIOS = selector
// Windows: KERNEL's constant: a selector to the PC's ROM BIOS at F000:0000.
// Why: A driver's: w16 has no BIOS to show a program.
// By: HPSYSTEM.DRV, KBDBE.DLL, KBDBR.DLL, KBDCA.DLL, KBDDA.DLL, KBDFC.DLL, and 18 more

// KERNEL.174 __A000H = selector
// Windows: KERNEL's constant: a selector to the VGA's video memory at A000:0000.
// Why: A display driver's: w16 draws through toywm, with no video memory to map.
// By: SUPERVGA.DRV, SVGA256.DRV, V7VGA.DRV, VGA.DRV, XGA.DRV

// KERNEL.175 AllocSelector(wSelector) -> WORD
// Windows: A new selector -- a copy of one, or a blank one when wSelector is 0.
// Why: Not written yet: w16's CPU keeps a table of selectors and could hand one out.
// By: 8514.DRV, CPWIN386.CPL, DMCOLOR.DLL, GDI.EXE, NDDENB.DLL, SUPERVGA.DRV, and 8 more

// KERNEL.176 FreeSelector(wSelector) -> WORD
// Windows: Gives back a selector AllocSelector made.
// Why: Not written yet, as AllocSelector.
// By: CPWIN386.CPL, DMCOLOR.DLL, GDI.EXE, KBDHP.DRV, KEYBOARD.DRV, NCDW.DLL, and 12 more

// KERNEL.177 PrestoChangoSelector(wSrc, wDst) -> WORD
// Windows: Makes wDst the code or data alias of wSrc's segment.
// Why: Not written yet, as AllocCStoDSAlias.
// By: 8514.DRV, DMCOLOR.DLL, GDI.EXE, SUPERVGA.DRV, SVGA256.DRV, V7VGA.DRV, and 2 more

// KERNEL.180 LongPtrAdd(dwLongPtr, dwAdd) -> void
// Windows: Adds to a far pointer, moving its selector when the offset passes 64 KB.
// Why: Windows' own: GDI's. A program steps a huge pointer with __AHINCR, which w16 answers.
// By: GDI.EXE, WINFILE.EXE

// KERNEL.184 GlobalDOSAlloc(dwBytes) -> DWORD
// Windows: A block below 1 MB, with both a selector and a real-mode segment, for passing to DOS or
// the BIOS.
// Why: w16 has no real mode: there is no DOS for such a block to reach.
// By: AWCAS.DLL, CPWIN386.CPL, MCICDA.DRV, MSSFS.DLL, NETAPI.DLL, PMSPL.DLL, and 9 more

// KERNEL.185 GlobalDOSFree(wSelector) -> WORD
// Windows: Frees a GlobalDOSAlloc block.
// Why: As GlobalDOSAlloc.
// By: AWCAS.DLL, CPWIN386.CPL, MCICDA.DRV, MSSFS.DLL, NETAPI.DLL, PMSPL.DLL, and 6 more

// KERNEL.186 GetSelectorBase(wSelector) -> DWORD
// Windows: The linear address a selector starts at.
// Why: Not written yet: w16's selectors have bases, but host addresses, not 32-bit linear ones a
// program could use.
// By: GDI.EXE, SNDBLST.DRV, SNDBLST2.DRV, TIMER.DRV, VER.DLL

// KERNEL.187 SetSelectorBase(wSelector, dwBase) -> WORD
// Windows: Points a selector at a linear address.
// Why: As GetSelectorBase: a program's linear address names nothing in w16.
// By: CPWIN386.CPL, GDI.EXE, NDDENB.DLL, VER.DLL, WFWSETUP.DLL, WINSETUP.EXE

// KERNEL.189 SetSelectorLimit(wSelector, dwLimit) -> WORD
// Windows: Sets the last byte a selector reaches.
// Why: Not written yet: with the base unchangeable, a limit alone means little.
// By: CPWIN386.CPL, VER.DLL, WFWSETUP.DLL, WINSETUP.EXE

// KERNEL.191 GlobalPageLock(hSelector) -> WORD
// Windows: Locks a block's pages in physical memory, so an interrupt handler can reach it while
// paging is off-limits.
// Why: A driver's: w16 pages nothing.
// By: CPWIN386.CPL, GDI.EXE, MIDIMAP.DRV, MMSYSTEM.DLL, MSADLIB.DRV, SNDBLST.DRV, and 4 more

// KERNEL.192 GlobalPageUnlock(hSelector) -> WORD
// Windows: Undoes GlobalPageLock.
// Why: As GlobalPageLock.
// By: CPWIN386.CPL, MIDIMAP.DRV, MMSYSTEM.DLL, MSADLIB.DRV, SNDBLST.DRV, SNDBLST2.DRV, and 2 more

// KERNEL.193 __0040H = selector
// Windows: KERNEL's constant: a selector to the BIOS data area at 0040:0000.
// Why: A driver's: w16 has no BIOS data area.
// By: COMM.DRV, HPSYSTEM.DRV, SYSTEM.DRV

// KERNEL.197 GlobalFix(hMem) -> void
// Windows: Fixes a block in linear memory so it does not move.
// Why: Not written yet: w16's blocks never move, so it would do nothing.
// By: 8514.DRV, GDI.EXE, MMSYSTEM.DLL, USER.EXE, WFWNET.DRV

// KERNEL.198 GlobalUnfix(hMem) -> BOOL
// Windows: Undoes GlobalFix.
// Why: As GlobalFix.
// By: GDI.EXE, MMSYSTEM.DLL, WFWNET.DRV

// KERNEL.199 SetHandleCount(wNumber) -> WORD
// Windows: Asks for room for more open files than the default 20.
// Why: Not written yet: w16's files are not counted against a DOS table, so it would answer the
// number asked for.
// By: DEMILAYR.DLL, WINFILE.EXE

// KERNEL.202 RegisterPtrace(lpfnPtrace) -> void
// Windows: Installs a debugger's notification procedure.
// Why: A debugger's: w16 has no 16-bit debugger interface (toy-os's strace watches w16 from
// outside).
// By: TOOLHELP.DLL

// KERNEL.205 CVWBreak() -> void
// Windows: CodeView for Windows' break into the debugger.
// Why: A debugger's, as RegisterPtrace.
// By: KBDHP.DRV, KEYBOARD.DRV

// KERNEL.315 WriteOutProfiles() -> void
// Windows: Writes every cached .INI change to disk.
// Why: Not written yet: w16 writes WIN.INI and private profiles at once, so it would do nothing.
// By: USER.EXE

// KERNEL.319 FlushCachedFileHandle(hModule) -> void
// Windows: Closes the file handle KERNEL keeps open on a module's executable.
// Why: Windows' own: Setup's. w16 holds no module file open.
// By: WINSETUP.EXE

// KERNEL.320 IsTask(hTask) -> BOOL
// Windows: Whether a handle is a running task's.
// Why: Not written yet: w16 would answer TRUE for its one task.
// By: MMSYSTEM.DLL

// KERNEL.323 IsRomModule(hModule) -> BOOL
// Windows: Whether a module runs from ROM, as on ROM-Windows machines.
// Why: Not written yet: FALSE, always.
// By: PROGMAN.EXE

// KERNEL.324 LogError(wError, lpInfo) -> void
// Windows: Reports an error in the debug build of Windows; the retail build does nothing.
// Why: Not written yet: the retail behaviour, nothing, would be whole.
// By: GDI.EXE, USER.EXE

// KERNEL.325 LogParamError(wError, lpfn, lpValue) -> void
// Windows: Reports a bad argument in the debug build; the retail build does nothing.
// Why: Not written yet, as LogError.
// By: MMSYSTEM.DLL

// KERNEL.327 K327() -> void
// Windows: An undocumented KERNEL export Windows' components use.
// Why: Undocumented, as Reserved1.
// By: GDI.EXE, USER.EXE

// KERNEL.328 _DebugOutput(wFlags, lpFormat, ...) -> void
// Windows: Formats a message for the debug terminal (the debug build's OutputDebugString with
// arguments).
// Why: Not written yet: it would go to toy-os's log, as w16's trace does.
// By: COMMCTRL.DLL

// KERNEL.334 IsBadReadPtr(lp, cb) -> BOOL
// Windows: Whether cb bytes at a far pointer can be read: the selector valid and its limit far
// enough.
// Why: Not written yet: w16's selector table answers it exactly.
// By: GDI.EXE, USER.EXE, WFWSETUP.DLL

// KERNEL.335 IsBadWritePtr(lp, cb) -> BOOL
// Windows: Whether cb bytes at a far pointer can be written.
// Why: Not written yet, as IsBadReadPtr.
// By: GDI.EXE, MMSYSTEM.DLL, USER.EXE

// KERNEL.336 IsBadCodePtr(lpfn) -> BOOL
// Windows: Whether a far pointer lands in a code segment it may run.
// Why: Not written yet, as IsBadReadPtr.
// By: MMSYSTEM.DLL, USER.EXE

// KERNEL.337 IsBadStringPtr(lpsz, cchMax) -> BOOL
// Windows: Whether a string can be read up to its end or cchMax bytes.
// Why: Not written yet, as IsBadReadPtr.
// By: EFAXPUMP.DLL, MMSYSTEM.DLL, USER.EXE

// KERNEL.340 DiagOutput(lpString) -> void
// Windows: Writes a string to the diagnostic output of the debug build.
// Why: Windows' own: USER's debug build.
// By: USER.EXE

// KERNEL.346 IsBadHugeReadPtr(lp, cb) -> BOOL
// Windows: IsBadReadPtr across 64 KB segments, for a huge block.
// Why: Not written yet, as IsBadReadPtr.
// By: GDI.EXE, MMSYSTEM.DLL

// KERNEL.347 IsBadHugeWritePtr(lp, cb) -> BOOL
// Windows: IsBadWritePtr across 64 KB segments.
// Why: Not written yet, as IsBadReadPtr.
// By: GDI.EXE, MMSYSTEM.DLL

// KERNEL.348 hmemcpy(lpDest, lpSrc, cb) -> void
// Windows: Copies bytes between huge blocks, across segment boundaries.
// Why: Not written yet, and simple: w16's huge blocks are runs of selectors __AHINCR apart.
// By: NETDDE.EXE

// KERNEL.349 _hread(hFile, lpBuffer, cb) -> LONG
// Windows: Reads more than 64 KB from a file into a huge block.
// Why: Not written yet: _lread past 64 KB, over w16's huge blocks.
// By: MCIWAVE.DRV, MMSYSTEM.DLL, USER.EXE

// KERNEL.350 _hwrite(hFile, lpBuffer, cb) -> LONG
// Windows: Writes more than 64 KB from a huge block.
// Why: Not written yet, as _hread.
// By: MCIWAVE.DRV, MMSYSTEM.DLL, RASMXS.DLL

// KERNEL.351 BUNNY_351() -> void
// Windows: An undocumented KERNEL export only USER calls.
// Why: Windows' own and undocumented.
// By: USER.EXE

// KERNEL.353 lstrcpyn(lpDest, lpSrc, cchMax) -> LPSTR
// Windows: Copies at most cchMax - 1 bytes of a string and always ends it.
// Why: Not written yet: lstrcpy with a bound, a few lines.
// By: NDDEAPI.DLL, NETDDE.EXE, PRINTMAN.EXE, RASCONF.DLL, USER.EXE, WFWNET.DRV, and 2 more

// KERNEL.354 GetAppCompatFlags(hTask) -> DWORD
// Windows: The compatibility bits Windows keeps for known old programs, from WIN.INI's
// [Compatibility].
// Why: Not written yet: w16 would answer 0, no workarounds asked for.
// By: COMM.DRV, GDI.EXE, PSCRIPT.DRV, USER.EXE

// --- KEYBOARD --------------------------------------------------------------------------------

// KEYBOARD.1 Inquire(lpKBInfo) -> WORD
// Windows: Fills a KBINFO with the keyboard's type and function keys -- the keyboard driver's own
// entry.
// Why: A driver's: USER calls it, and w16 is its own USER, with toywm's keyboard behind it.
// By: USER.EXE

// KEYBOARD.2 Enable(lpEventProc, lpKeyState) -> void
// Windows: Starts the keyboard driver, naming USER's procedure for key events.
// Why: As Inquire.
// By: USER.EXE

// KEYBOARD.3 Disable() -> void
// Windows: Stops the keyboard driver.
// Why: As Inquire.
// By: USER.EXE

// KEYBOARD.4 ToAscii(wVirtKey, wScanCode, lpKeyState, lpChar, wFlags) -> int
// Windows: The character a key makes with the key state given -- shifted, with dead keys.
// Why: Not written yet: toywm sends w16 characters already made, so w16 would need the layout in
// reverse; GetKeyboardState first.
// By: USER.EXE

// KEYBOARD.7 SetSpeed(nSpeed) -> int
// Windows: Sets the keyboard's repeat rate.
// Why: toy-os's own setting, not a program's to change.
// By: USER.EXE

// KEYBOARD.100 ScreenSwitchEnable(fEnable) -> void
// Windows: Allows or holds off Alt+Tab-style switches to a full-screen DOS session.
// Why: A driver's: there are no DOS sessions.
// By: SUPERVGA.DRV, V7VGA.DRV, VGA.DRV

// KEYBOARD.132 GetKBCodePage() -> int
// Windows: The code page the keyboard's layout is for (437, 850...).
// Why: Not written yet: w16 would answer the OEM code page it reports elsewhere.
// By: DDEML.DLL

// KEYBOARD.133 GetKeyNameText(lParam, lpBuffer, nSize) -> int
// Windows: A key's name (Ctrl, Enter, Num 5) from a keyboard message's lParam.
// Why: Not written yet: a table of key names by scan code, as the keyboard drivers carried.
// By: PIFEDIT.EXE, PROGMAN.EXE

// KEYBOARD.134 AnsiToOemBuff(lpAnsiStr, lpOemStr, nLength) -> void
// Windows: Converts a buffer of the given length from the Windows code page to the DOS one.
// Why: Not written yet: AnsiToOem is there; this is the same with a length.
// By: FRAMEWRK.DLL, USER.EXE, WFWSETUP.DLL

// KEYBOARD.135 OemToAnsiBuff(lpOemStr, lpAnsiStr, nLength) -> void
// Windows: Converts a buffer of the given length from the DOS code page to the Windows one.
// Why: Not written yet, as AnsiToOemBuff.
// By: FRAMEWRK.DLL, MSSCHED.DLL, NDDEAPI.DLL, USER.EXE, WFWNET.DRV, WFWSETUP.DLL

// --- LZEXPAND --------------------------------------------------------------------------------

// LZEXPAND.1 LZCopy(hfSource, hfDest) -> LONG
// Windows: Copies a file, expanding it on the way if it was compressed with COMPRESS.EXE.
// Why: LZEXPAND is not answered yet: its compressed format (SZDD) is a small LZSS.
// By: DRIVERS.CPL, VER.DLL, WFWSETUP.DLL, WINSETUP.EXE

// LZEXPAND.2 LZOpenFile(lpFileName, lpReOpenBuf, wStyle) -> int
// Windows: Opens a file for LZRead, expanded or not.
// Why: As LZCopy.
// By: MAIN.CPL, VER.DLL, WFWSETUP.DLL, WINSETUP.EXE

// LZEXPAND.4 LZSeek(hFile, lOffset, nOrigin) -> LONG
// Windows: Moves the read position in an expanded file.
// Why: As LZCopy.
// By: MAIN.CPL, VER.DLL

// LZEXPAND.5 LZRead(hFile, lpBuffer, cbRead) -> int
// Windows: Reads expanded bytes from a file LZOpenFile opened.
// Why: As LZCopy.
// By: MAIN.CPL, VER.DLL

// LZEXPAND.6 LZClose(hFile) -> void
// Windows: Closes a file LZOpenFile opened.
// Why: As LZCopy.
// By: MAIN.CPL, VER.DLL, WFWSETUP.DLL, WINSETUP.EXE

// LZEXPAND.7 LZStart() -> int
// Windows: Prepares for a run of CopyLZFile calls.
// Why: As LZCopy.
// By: WFWSETUP.DLL, WINSETUP.EXE

// LZEXPAND.9 LZDone() -> void
// Windows: Ends a run LZStart began.
// Why: As LZCopy.
// By: WFWSETUP.DLL, WINSETUP.EXE

// LZEXPAND.10 GetExpandedName(lpszSource, lpszBuffer) -> int
// Windows: The name a compressed file will have once expanded (README.TX_ to README.TXT).
// Why: As LZCopy.
// By: MAIN.CPL

// --- MMSYSTEM --------------------------------------------------------------------------------

// MMSYSTEM.31 DriverCallback(dwCallback, uFlags, hDevice, uMessage, dwUser, dwParam1, dwParam2) -> BOOL
// Windows: How a multimedia driver tells a program a buffer is done: a window message, a task
// signal or a call.
// Why: A driver's: w16 carries no multimedia drivers; its sound goes to toy-os's soundd.
// By: MIDIMAP.DRV, MPU401.DRV, MSADLIB.DRV, SNDBLST.DRV, SNDBLST2.DRV, TIMER.DRV

// MMSYSTEM.32 StackEnter() -> void
// Windows: Switches to MMSYSTEM's own stack inside an interrupt-time callback.
// Why: As DriverCallback.
// By: MPU401.DRV, SNDBLST.DRV, SNDBLST2.DRV, TIMER.DRV

// MMSYSTEM.33 StackLeave() -> void
// Windows: Switches back from StackEnter's stack.
// Why: As DriverCallback.
// By: MPU401.DRV, SNDBLST.DRV, SNDBLST2.DRV, TIMER.DRV

// MMSYSTEM.201 midiOutGetNumDevs() -> UINT
// Windows: How many MIDI output devices there are.
// Why: MIDI is not answered yet: toy-os has a General MIDI synthesizer (lib/usnd_synth.c) that a
// program's MIDI could play through.
// By: MCISEQ.DRV, MIDIMAP.DRV, MPLAYER.EXE

// MMSYSTEM.202 midiOutGetDevCaps(uDeviceID, lpCaps, uSize) -> UINT
// Windows: A MIDI output device's name, kind and voices.
// Why: As midiOutGetNumDevs.
// By: MIDIMAP.DRV

// MMSYSTEM.204 midiOutOpen(lphMidiOut, uDeviceID, dwCallback, dwInstance, dwFlags) -> UINT
// Windows: Opens a MIDI output device.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.205 midiOutClose(hMidiOut) -> UINT
// Windows: Closes a MIDI output device.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.206 midiOutPrepareHeader(hMidiOut, lpMidiOutHdr, uSize) -> UINT
// Windows: Readies a buffer of system-exclusive bytes for midiOutLongMsg.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.207 midiOutUnprepareHeader(hMidiOut, lpMidiOutHdr, uSize) -> UINT
// Windows: Undoes midiOutPrepareHeader once the buffer is sent.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.208 midiOutShortMsg(hMidiOut, dwMsg) -> UINT
// Windows: Sends one MIDI message -- a note on, a note off, a program change.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.209 midiOutLongMsg(hMidiOut, lpMidiOutHdr, uSize) -> UINT
// Windows: Sends a system-exclusive message from a prepared buffer.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.210 midiOutReset(hMidiOut) -> UINT
// Windows: Turns every note off on every channel.
// Why: As midiOutGetNumDevs.
// By: MIDIMAP.DRV

// MMSYSTEM.213 midiOutCachePatches(hMidiOut, uBank, lpPatchArray, uFlags) -> UINT
// Windows: Asks an internal synthesizer to load the instruments named.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.214 midiOutCacheDrumPatches(hMidiOut, uPatch, lpKeyArray, uFlags) -> UINT
// Windows: Asks an internal synthesizer to load the drum sounds named.
// Why: As midiOutGetNumDevs.
// By: MCISEQ.DRV, MIDIMAP.DRV

// MMSYSTEM.216 midiOutMessage(hMidiOut, msg, dw1, dw2) -> DWORD
// Windows: Sends a MIDI output driver a message of its own.
// Why: As DriverCallback.
// By: MIDIMAP.DRV

// MMSYSTEM.301 midiInGetNumDevs() -> UINT
// Windows: How many MIDI input devices there are.
// Why: Not written yet: w16 would answer 0 -- toy-os has no MIDI input.
// By: MPLAYER.EXE

// MMSYSTEM.350 auxGetNumDevs() -> UINT
// Windows: How many auxiliary audio devices (a CD's audio line, a mixer input) there are.
// Why: Not written yet: w16 would answer 0.
// By: MPLAYER.EXE

// MMSYSTEM.402 waveOutGetDevCaps(uDeviceID, lpCaps, uSize) -> UINT
// Windows: A wave output device's name and the formats it plays.
// Why: waveOut is not answered yet: sndPlaySound and MessageBeep play through soundd; a program's
// own stream of buffers is the next step.
// By: MCIWAVE.DRV

// MMSYSTEM.404 waveOutOpen(lphWaveOut, uDeviceID, lpFormat, dwCallback, dwInstance, dwFlags) -> UINT
// Windows: Opens a wave output device for a PCM format, with a callback for each buffer played.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.405 waveOutClose(hWaveOut) -> UINT
// Windows: Closes a wave output device.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.406 waveOutPrepareHeader(hWaveOut, lpWaveOutHdr, uSize) -> UINT
// Windows: Readies a buffer for waveOutWrite.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.407 waveOutUnprepareHeader(hWaveOut, lpWaveOutHdr, uSize) -> UINT
// Windows: Undoes waveOutPrepareHeader once the buffer has played.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.408 waveOutWrite(hWaveOut, lpWaveOutHdr, uSize) -> UINT
// Windows: Queues a buffer to play; the callback says when it is done.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.409 waveOutPause(hWaveOut) -> UINT
// Windows: Pauses playback.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV

// MMSYSTEM.410 waveOutRestart(hWaveOut) -> UINT
// Windows: Resumes paused playback.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV

// MMSYSTEM.411 waveOutReset(hWaveOut) -> UINT
// Windows: Stops playback and hands every queued buffer back.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.412 waveOutGetPosition(hWaveOut, lpInfo, uSize) -> UINT
// Windows: How far playback has got, in samples, bytes or time.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.420 waveOutGetID(hWaveOut, lpuDeviceID) -> UINT
// Windows: The device number an open output was opened on.
// Why: As waveOutGetDevCaps.
// By: MCIWAVE.DRV

// MMSYSTEM.501 waveInGetNumDevs() -> UINT
// Windows: How many wave input (recording) devices there are.
// Why: Recording is not answered: soundd plays and does not record, so w16 would answer 0.
// By: MCIWAVE.DRV, MPLAYER.EXE, SOUNDREC.EXE

// MMSYSTEM.502 waveInGetDevCaps(uDeviceID, lpCaps, uSize) -> UINT
// Windows: A recording device's name and formats.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV

// MMSYSTEM.504 waveInOpen(lphWaveIn, uDeviceID, lpFormat, dwCallback, dwInstance, dwFlags) -> UINT
// Windows: Opens a recording device for a PCM format.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.505 waveInClose(hWaveIn) -> UINT
// Windows: Closes a recording device.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.506 waveInPrepareHeader(hWaveIn, lpWaveInHdr, uSize) -> UINT
// Windows: Readies a buffer for recording into.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.507 waveInUnprepareHeader(hWaveIn, lpWaveInHdr, uSize) -> UINT
// Windows: Undoes waveInPrepareHeader.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.508 waveInAddBuffer(hWaveIn, lpWaveInHdr, uSize) -> UINT
// Windows: Queues a buffer to record into.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.509 waveInStart(hWaveIn) -> UINT
// Windows: Starts recording.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.510 waveInStop(hWaveIn) -> UINT
// Windows: Stops recording.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV

// MMSYSTEM.511 waveInReset(hWaveIn) -> UINT
// Windows: Stops recording and hands every queued buffer back.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.512 waveInGetPosition(hWaveIn, lpInfo, uSize) -> UINT
// Windows: How far recording has got.
// Why: As waveInGetNumDevs.
// By: SOUNDREC.EXE

// MMSYSTEM.513 waveInGetID(hWaveIn, lpuDeviceID) -> UINT
// Windows: The device number an open input was opened on.
// Why: As waveInGetNumDevs.
// By: MCIWAVE.DRV

// MMSYSTEM.602 timeSetEvent(uDelay, uResolution, lpFunction, dwUser, uFlags) -> UINT
// Windows: Calls a procedure after a delay, once or periodically, at millisecond resolution --
// outside the message queue.
// Why: Not written yet: w16's SetTimer posts WM_TIMER; a callback run between instructions is the
// next step.
// By: MCISEQ.DRV

// MMSYSTEM.603 timeKillEvent(uTimerID) -> UINT
// Windows: Cancels a timeSetEvent timer.
// Why: As timeSetEvent.
// By: MCISEQ.DRV

// MMSYSTEM.605 timeBeginPeriod(uPeriod) -> UINT
// Windows: Asks for timers this many milliseconds fine.
// Why: Not written yet: w16 would answer TIMERR_NOERROR, its timers being as fine as toy-os's.
// By: MCISEQ.DRV

// MMSYSTEM.606 timeEndPeriod(uPeriod) -> UINT
// Windows: Ends a timeBeginPeriod.
// Why: As timeBeginPeriod.
// By: MCISEQ.DRV

// MMSYSTEM.607 timeGetTime() -> DWORD
// Windows: Milliseconds since Windows started.
// Why: Not written yet: GetTickCount's answer, which w16 gives -- one line.
// By: MCISEQ.DRV, MPU401.DRV, SNDBLST.DRV, SNDBLST2.DRV

// MMSYSTEM.701 mciSendCommand(wDeviceID, wMessage, dwParam1, dwParam2) -> DWORD
// Windows: Sends a Media Control Interface command (MCI_OPEN, MCI_PLAY...) to a device: a CD, a
// sequencer, a wave file.
// Why: MCI is not answered: its devices are drivers (MCIWAVE, MCISEQ, MCICDA) w16 does not run;
// Media Player and Sound Recorder would want them.
// By: MPLAYER.EXE

// MMSYSTEM.702 mciSendString(lpstrCommand, lpstrReturnString, uReturnLength, hwndCallback) -> DWORD
// Windows: An MCI command as text ("play cdaudio"), its answer as text.
// Why: As mciSendCommand.
// By: MPLAYER.EXE

// MMSYSTEM.705 mciLoadCommandResource(hInstance, lpResName, uType) -> UINT
// Windows: Loads an MCI driver's table of the commands it adds.
// Why: As mciSendCommand: an MCI driver's.
// By: MCIWAVE.DRV

// MMSYSTEM.706 mciGetErrorString(dwError, lpstrBuffer, uLength) -> BOOL
// Windows: The text of an MCI error.
// Why: As mciSendCommand.
// By: MPLAYER.EXE

// MMSYSTEM.707 mciSetDriverData(uDeviceID, dwData) -> BOOL
// Windows: Stores an MCI driver's own data with a device.
// Why: As mciLoadCommandResource.
// By: MCICDA.DRV, MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.708 mciGetDriverData(uDeviceID) -> DWORD
// Windows: What mciSetDriverData stored.
// Why: As mciLoadCommandResource.
// By: MCICDA.DRV, MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.710 mciDriverYield(uDeviceID) -> UINT
// Windows: Lets an MCI driver check for a break key while it waits.
// Why: As mciLoadCommandResource.
// By: MCICDA.DRV, MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.711 mciDriverNotify(hwndCallback, uDeviceID, uStatus) -> BOOL
// Windows: Sends a program MM_MCINOTIFY when its MCI command finishes -- the driver's.
// Why: As mciLoadCommandResource.
// By: MCICDA.DRV, MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.713 mciFreeCommandResource(uTable) -> BOOL
// Windows: Frees a table mciLoadCommandResource loaded.
// Why: As mciLoadCommandResource.
// By: MCIWAVE.DRV

// MMSYSTEM.717 mciGetCreatorTask(uDeviceID) -> HTASK
// Windows: The task that opened an MCI device.
// Why: As mciSendCommand.
// By: MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.900 mmTaskCreate(lpfnTask, lphTask, dwInst) -> UINT
// Windows: Starts a background task of MMSYSTEM's own (MCI drivers play from one).
// Why: One program per w16, and no second task inside it (the roadmap's "w16: one program starting
// or finding another").
// By: MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.902 mmTaskBlock(hTask) -> void
// Windows: Sleeps an mmTask until signalled.
// Why: As mmTaskCreate.
// By: MCIWAVE.DRV

// MMSYSTEM.903 mmTaskSignal(hTask) -> BOOL
// Windows: Wakes a blocked mmTask.
// Why: As mmTaskCreate.
// By: MCIWAVE.DRV

// MMSYSTEM.904 mmGetCurrentTask() -> HTASK
// Windows: The current task, for an mmTask.
// Why: As mmTaskCreate.
// By: MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.905 mmTaskYield() -> void
// Windows: Yields from an mmTask.
// Why: As mmTaskCreate.
// By: MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.1210 mmioOpen(szFilename, lpmmioinfo, dwOpenFlags) -> HMMIO
// Windows: Opens a file for buffered reads and writes, with RIFF chunk support -- how WAV and AVI
// files are read.
// Why: Not written yet: w16 reads WAV files itself for sndPlaySound; handing a program the same
// reader is the next step after waveOut.
// By: MCISEQ.DRV, MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1211 mmioClose(hmmio, wFlags) -> UINT
// Windows: Closes an mmio file.
// Why: As mmioOpen.
// By: MCISEQ.DRV, MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1212 mmioRead(hmmio, pch, cch) -> LONG
// Windows: Reads from an mmio file.
// Why: As mmioOpen.
// By: MCISEQ.DRV, MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1213 mmioWrite(hmmio, pch, cch) -> LONG
// Windows: Writes to an mmio file.
// Why: As mmioOpen.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1214 mmioSeek(hmmio, lOffset, iOrigin) -> LONG
// Windows: Moves an mmio file's position.
// Why: As mmioOpen.
// By: MCISEQ.DRV, MCIWAVE.DRV

// MMSYSTEM.1215 mmioGetInfo(hmmio, lpmmioinfo, wFlags) -> UINT
// Windows: An mmio file's state and buffer, for reading it directly.
// Why: As mmioOpen.
// By: SOUNDREC.EXE

// MMSYSTEM.1223 mmioDescend(hmmio, lpck, lpckParent, wFlags) -> UINT
// Windows: Steps into a RIFF chunk, or finds one by its id.
// Why: As mmioOpen.
// By: MCISEQ.DRV, MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1224 mmioAscend(hmmio, lpck, wFlags) -> UINT
// Windows: Steps out of a RIFF chunk, writing its size if it was created.
// Why: As mmioOpen.
// By: MCISEQ.DRV, MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1225 mmioCreateChunk(hmmio, lpck, wFlags) -> UINT
// Windows: Starts a new RIFF chunk in an mmio file being written.
// Why: As mmioOpen.
// By: MCIWAVE.DRV, SOUNDREC.EXE

// MMSYSTEM.1226 mmioRename(szFilename, szNewFilename, lpmmioinfo, dwRenameFlags) -> UINT
// Windows: Renames a file through mmio's I/O procedures.
// Why: As mmioOpen.
// By: MCIWAVE.DRV

// --- OLECLI ----------------------------------------------------------------------------------

// OLECLI.17 OleSetBounds(lpObject, lprcBound) -> OLESTATUS
// Windows: Tells an embedded object's server the size the object should have.
// Why: No program can edit an object (OleActivate's stand-in): there is no server to tell.
// By: VFORMS.DLL

// OLECLI.23 OleReconnect(lpObject) -> OLESTATUS
// Windows: Reconnects a linked object to its server's open document.
// Why: No server: nothing can be linked (OleCreateLinkFromClip's stand-in).
// By: CARDFILE.EXE, PACKAGER.EXE

// OLECLI.46 OleRename(lpObject, lpszNewName) -> OLESTATUS
// Windows: Changes an object's name in its client document.
// Why: Not written yet: w16's objects have names a client could change; nothing has asked.
// By: CARDFILE.EXE, PACKAGER.EXE

// --- SHELL -----------------------------------------------------------------------------------

// SHELL.4 RegDeleteKey(hKey, lpszSubKey) -> LONG
// Windows: Deletes a key, and everything under it, from the registration database.
// Why: The registration database is a stand-in today, empty and read-only; a real one is on the
// list after this round of Paintbrush's stand-ins.
// By: REGEDIT.EXE, WINFILE.EXE

// SHELL.13 DragQueryPoint(hDrop, lppt) -> BOOL
// Windows: Where in a window the files of a WM_DROPFILES were dropped.
// Why: Nothing is dropped on a program yet (the roadmap's "w16: a file dropped on a program
// (WM_DROPFILES)").
// By: FRAMEWRK.DLL, PROGMAN.EXE

// SHELL.20 ShellExecute(hwnd, lpOperation, lpFile, lpParameters, lpDirectory, iShowCmd) -> HINSTANCE
// Windows: Opens or prints a file with the program registered for it, or runs a program.
// Why: A program starting another is the roadmap's "w16: one program starting or finding another";
// toy-os's own open-with could answer the files it knows.
// By: EFAXPUMP.DLL, PACKAGER.EXE, PROGMAN.EXE, SIGVIEW.EXE, VFORMS.DLL, WINFILE.EXE

// SHELL.21 FindExecutable(lpFile, lpDirectory, lpResult) -> HINSTANCE
// Windows: The program registered to open a file.
// Why: As ShellExecute.
// By: PROGMAN.EXE, VFORMS.DLL

// SHELL.34 ExtractIcon(hInst, lpszExeFileName, nIconIndex) -> HICON
// Windows: An icon from a program, DLL or .ICO file, or how many it has.
// Why: Not written yet: w16 reads another program's icons for Retro Launcher already (w16
// --check).
// By: PACKAGER.EXE, PROGMAN.EXE, STORE.DLL

// SHELL.36 ExtractAssociatedIcon(hInst, lpIconPath, lpiIcon) -> HICON
// Windows: The icon of a file, or of the program registered for it.
// Why: As ExtractIcon, and the registration database after.
// By: PACKAGER.EXE, PROGMAN.EXE

// SHELL.37 DoEnvironmentSubst(szString, cbString) -> DWORD
// Windows: Replaces %NAME% in a string with the DOS environment's values.
// Why: As GetDOSEnvironment: w16 gives a program no environment yet.
// By: PROGMAN.EXE

// SHELL.38 FindEnvironmentString(szEnvVar) -> LPSTR
// Windows: The value of a DOS environment variable.
// Why: As DoEnvironmentSubst.
// By: WINFILE.EXE

// SHELL.39 InternalExtractIcon(hInst, lpszExeName, nIconIndex, nIcons) -> HGLOBAL
// Windows: Several icons from a file at once -- Program Manager's.
// Why: Windows' own: Program Manager's form of ExtractIcon.
// By: PROGMAN.EXE

// SHELL.102 RegisterShellHook(hWnd, fInstall) -> BOOL
// Windows: Asks to be told when top-level windows are made and destroyed -- a shell's own call.
// Why: The shell is toy-os's desktop, not a w16 program.
// By: PROGMAN.EXE

// --- SOUND -----------------------------------------------------------------------------------

// SOUND.3 SetVoiceQueueSize(nVoice, nBytes) -> int
// Windows: Sizes a voice's note queue for the PC speaker's sound driver.
// Why: Not written yet: w16 plays SOUND.DRV's notes through soundd, and its queues grow, so it
// would answer success.
// By: RASMON.EXE

// SOUND.17 DoBeep() -> int
// Windows: Sounds the PC speaker's beep.
// Why: Not written yet: MessageBeep plays the system sound; this would play the same.
// By: USER.EXE

// --- SYSTEM ----------------------------------------------------------------------------------

// SYSTEM.1 InquireSystem(wHow, wWhich) -> DWORD
// Windows: The system driver's facts: the timer's resolution, a drive's kind.
// Why: A driver's: USER and KERNEL call it, and w16 is its own USER and KERNEL.
// By: USER.EXE

// SYSTEM.2 CreateSystemTimer(wRate, lpfnCallback) -> WORD
// Windows: A hardware timer calling a procedure at interrupt time -- drivers' and MMSYSTEM's.
// Why: As InquireSystem.
// By: USER.EXE

// SYSTEM.3 KillSystemTimer(wTimer) -> WORD
// Windows: Stops a system timer.
// Why: As InquireSystem.
// By: USER.EXE

// SYSTEM.4 EnableSystemTimers() -> void
// Windows: Starts the system timers -- KERNEL's.
// Why: As InquireSystem.
// By: USER.EXE

// SYSTEM.5 DisableSystemTimers() -> void
// Windows: Stops every system timer -- KERNEL's.
// Why: As InquireSystem.
// By: USER.EXE

// SYSTEM.6 GetSystemMSecCount() -> DWORD
// Windows: Milliseconds since Windows started, from the system driver.
// Why: Not written yet: GetTickCount's answer.
// By: MOUSE.DRV, USER.EXE

// --- TOOLHELP --------------------------------------------------------------------------------

// TOOLHELP.63 TaskFirst(lpte) -> BOOL
// Windows: Starts a walk of the running tasks, filling a TASKENTRY.
// Why: Not written yet: one task to list -- the program's own.
// By: EFAXPUMP.DLL, IFKERNEL.DLL

// TOOLHELP.64 TaskNext(lpte) -> BOOL
// Windows: The next task in a walk.
// Why: Not written yet, as TaskFirst: FALSE after the one.
// By: EFAXPUMP.DLL, IFKERNEL.DLL

// TOOLHELP.65 TaskFindHandle(lpte, hTask) -> BOOL
// Windows: Fills a TASKENTRY for one task.
// Why: Not written yet, as TaskFirst.
// By: EFAXDRV.DRV

// TOOLHELP.73 NotifyRegister(hTask, lpfnCallback, wFlags) -> BOOL
// Windows: Asks to be told of system events: tasks and modules starting and ending, debug strings.
// Why: Not written yet: a debugger's or a task list's; with one task there is little to tell.
// By: PACKAGER.EXE

// TOOLHELP.74 NotifyUnregister(hTask) -> BOOL
// Windows: Stops NotifyRegister's notices.
// Why: As NotifyRegister.
// By: PACKAGER.EXE

// TOOLHELP.77 TerminateApp(hTask, wFlags) -> void
// Windows: Ends a task -- the current one, or another a debugger watches.
// Why: Not written yet: ending the program's own task would end its w16.
// By: IFKERNEL.DLL

// --- USER ------------------------------------------------------------------------------------

// USER.7 ExitWindows(dwReturnCode, wReserved) -> BOOL
// Windows: Asks every program whether Windows may end (WM_QUERYENDSESSION) and, if all agree, ends
// Windows -- or restarts it, or reboots, by the code.
// Why: The desktop is toy-os's: a w16 program may not log toy-os out. It would end that program's
// own w16 at most.
// By: CPWIN386.CPL, DRIVERS.CPL, MAILSPL.EXE, MAIN.CPL, MSMAIL.EXE, PROGMAN.EXE, and 4 more

// USER.20 SetDoubleClickTime(wCount) -> void
// Windows: Sets the milliseconds two clicks may be apart and still be one double click, for every
// program.
// Why: toy-os's own setting (Settings > Mouse) is the desktop's, not a w16 program's to change.
// By: MAIN.CPL

// USER.21 GetDoubleClickTime() -> WORD
// Windows: The milliseconds two clicks may be apart and still be one double click.
// Why: Not written yet: w16 already reads toy-os's double-click setting for its own CS_DBLCLKS --
// a few lines.
// By: RECORDER.DLL, WINFILE.EXE

// USER.24 RemoveProp(hWnd, lpString) -> HANDLE
// Windows: Takes a named property off a window and answers its value.
// Why: Not written yet: window properties (a name and a word per window) are a small table w16
// does not keep.
// By: COMMCTRL.DLL, COMMDLG.DLL, FAXOPT.DLL, MSREMIND.EXE, OLECLI.DLL, OLESVR.DLL, and 5 more

// USER.25 GetProp(hWnd, lpString) -> HANDLE
// Windows: A named property's value on a window, or 0.
// Why: Not written yet, as RemoveProp.
// By: COMMCTRL.DLL, COMMDLG.DLL, FAXOPT.DLL, MSREMIND.EXE, OLECLI.DLL, OLESVR.DLL, and 5 more

// USER.26 SetProp(hWnd, lpString, hData) -> BOOL
// Windows: Puts a named property on a window: a word, found again by its name (or atom).
// Why: Not written yet, as RemoveProp.
// By: COMMCTRL.DLL, COMMDLG.DLL, FAXOPT.DLL, MSREMIND.EXE, OLECLI.DLL, OLESVR.DLL, and 5 more

// USER.27 EnumProps(hWnd, lpEnumFunc) -> int
// Windows: Calls a procedure for each property on a window.
// Why: Not written yet, as RemoveProp.
// By: OLESVR.DLL

// USER.112 WaitMessage() -> void
// Windows: Sleeps until a message arrives in the queue, leaving it there.
// Why: Not written yet: w16's GetMessage already sleeps the same way; this is the same wait
// without the take.
// By: FAXVIEW.EXE, FRAMEWRK.DLL, MAILSPL.EXE

// USER.115 ReplyMessage(lResult) -> void
// Windows: Answers a message another task SENT, so the sender carries on while this one keeps
// working.
// Why: Not written yet: with one task, nothing is ever sent from another, so it would do nothing.
// By: PRINTMAN.EXE

// USER.116 PostAppMessage(hTask, wMsg, wParam, lParam) -> BOOL
// Windows: Posts a message to a task's queue rather than to a window (hwnd 0).
// Why: Not written yet: w16 would post it to its one queue.
// By: MCISEQ.DRV, MMSYSTEM.DLL

// USER.126 InvalidateRgn(hWnd, hRgn, fErase) -> void
// Windows: Adds a region of a window to what its next WM_PAINT repaints.
// Why: w16's invalid areas are whole windows today (the roadmap's "w16: invalid regions as
// rectangles"); with regions it would follow.
// By: FRAMEWRK.DLL, PROGMAN.EXE, WINMETER.EXE

// USER.128 ValidateRgn(hWnd, hRgn) -> void
// Windows: Takes a region of a window out of what needs repainting.
// Why: As InvalidateRgn.
// By: FRAMEWRK.DLL

// USER.131 GetClassLong(hWnd, nIndex) -> LONG
// Windows: A long of a window's class: its window procedure (GCL_WNDPROC) or menu name
// (GCL_MENUNAME).
// Why: Not written yet: GetClassWord and SetClassWord are, and the class keeps its procedure.
// By: COMMDLG.DLL

// USER.143 CountClipboardFormats() -> int
// Windows: How many formats the clipboard holds now.
// Why: Not written yet: toy-os's clipboard knows what it holds; a Clipboard Viewer call.
// By: CLIPBRD.EXE, CLIPSRV.EXE

// USER.146 GetClipboardFormatName(wFormat, lpszFormatName, cchMax) -> int
// Windows: The name a registered clipboard format was registered under.
// Why: Not written yet: the names RegisterClipboardFormat was given are w16's to answer.
// By: CLIPBRD.EXE, CLIPSRV.EXE, NETDDE.EXE, OLECLI.DLL

// USER.147 SetClipboardViewer(hWnd) -> HWND
// Windows: Joins the chain of windows told (WM_DRAWCLIPBOARD) whenever the clipboard changes;
// answers the next window in the chain.
// Why: Not written yet: toy-os's clipboard daemon tells w16 of changes, which w16 could pass on as
// WM_DRAWCLIPBOARD.
// By: CLIPBRD.EXE

// USER.149 ChangeClipboardChain(hWndRemove, hWndNext) -> BOOL
// Windows: Leaves the clipboard-viewer chain, linking the windows either side.
// Why: Not written yet, as SetClipboardViewer.
// By: CLIPBRD.EXE

// USER.161 GetMenuString(hMenu, wIDItem, lpString, nMaxCount, wFlag) -> int
// Windows: Copies a menu item's text, picked by its id or position (MF_BYCOMMAND, MF_BYPOSITION);
// answers its length.
// Why: Not written yet: w16's menus keep each item's text, so it is a small handler.
// By: CLIPBRD.EXE, HPPCL5E.DRV, WINFILE.EXE

// USER.170 ArrangeIconicWindows(hWnd) -> WORD
// Windows: Lines up a window's minimized children, or the desktop's icons, along the bottom.
// Why: The desktop is toywm's, which arranges nothing a program asks; MDI children would come with
// DefFrameProc.
// By: TASKMAN.EXE

// USER.172 SwitchToThisWindow(hWnd, fRestore) -> void
// Windows: Brings a window to the front and activates it, restoring it if minimized -- Task List's
// Switch To.
// Why: A program cannot reach another's window: each program is its own w16.
// By: TASKMAN.EXE

// USER.181 SetSysColors(cDspElements, lpnDspElements, lpdwRgbValues) -> void
// Windows: Changes system colours (COLOR_*) for every program, and tells them all with
// WM_SYSCOLORCHANGE.
// Why: w16's colours are 3.1's defaults for its own windows; a program changing them for everyone
// is Control Panel's job, and toy-os's theme is not its to change.
// By: MAIN.CPL, WINTUTOR.EXE

// USER.185 GrayString(hDC, hBrush, lpfnOutput, lpData, nCount, X, Y, nWidth, nHeight) -> BOOL
// Windows: Draws text (or what an output procedure draws) dimmed, as a disabled control's label.
// Why: Not written yet: w16 draws its own disabled labels in 3.1's grey; a program's own call is
// not there yet.
// By: COMMDLG.DLL, FRAMEWRK.DLL, MPLAYER.EXE, PROGMAN.EXE, SOUNDREC.EXE, TERMINAL.EXE

// USER.186 SwapMouseButton(fSwap) -> BOOL
// Windows: Swaps the left and right buttons, for every program.
// Why: toy-os's own setting (Settings > Mouse), not a w16 program's to change.
// By: MAIN.CPL, WINTUTOR.EXE

// USER.188 SetSysModalWindow(hWnd) -> HWND
// Windows: Makes a window system-modal: no other window takes input until it closes.
// Why: toy-os's desktop may not be held by a w16 program; at most its own windows would be.
// By: FRAMEWRK.DLL, PROGMAN.EXE

// USER.189 GetSysModalWindow() -> HWND
// Windows: The system-modal window, or 0.
// Why: Not written yet: w16 would answer 0.
// By: COMMDLG.DLL, FRAMEWRK.DLL

// USER.191 ChildWindowFromPoint(hWndParent, pt) -> HWND
// Windows: The child of a window under a point in its client area, or the window itself.
// Why: Not written yet: w16 finds the window under the pointer already (WindowFromPoint), for any
// parent.
// By: COMMCTRL.DLL, COMMDLG.DLL, FRAMEWRK.DLL, MAIN.CPL

// USER.195 DlgDirListComboBox(hDlg, lpPathSpec, nIDComboBox, nIDStaticPath, wFiletype) -> int
// Windows: Fills a dialog's combo box with the files and drives of a directory.
// Why: As DlgDirList, a stand-in today: listing files into a list needs the DOS view of drives.
// By: CPWIN386.CPL

// USER.197 GetTabbedTextExtent(hDC, lpString, nCount, nTabPositions, lpnTabStopPositions) -> DWORD
// Windows: The width and height a string with tabs would take, the tabs expanded to the stops
// given.
// Why: Not written yet: TabbedTextOut's measure without the drawing.
// By: FRAMEWRK.DLL

// USER.198 CascadeChildWindows(hWndParent, wAction) -> void
// Windows: Overlaps a window's children (MDI or the desktop's) down and to the right.
// Why: toywm places top-level windows; MDI children would come with DefFrameProc.
// By: TASKMAN.EXE

// USER.199 TileChildWindows(hWndParent, wAction) -> void
// Windows: Tiles a window's children side by side.
// Why: As CascadeChildWindows.
// By: TASKMAN.EXE

// USER.200 OpenComm(lpComName, cbInQueue, cbOutQueue) -> int
// Windows: Opens a serial port (COM1:) or the parallel port (LPT1:) with queues of the sizes
// given; answers its handle.
// Why: w16 gives a program no serial ports: toy-os's serial lines are its debug console and log. A
// port given through would need its own setting.
// By: CARDFILE.EXE, MAIN.CPL, PRINTMAN.EXE, RASSER.DLL, TERMINAL.EXE

// USER.201 SetCommState(lpDCB) -> int
// Windows: Sets an open port's speed, bits, parity and handshaking from a DCB.
// Why: As OpenComm.
// By: CARDFILE.EXE, PRINTMAN.EXE, RASSER.DLL, TERMINAL.EXE

// USER.202 GetCommState(nCid, lpDCB) -> int
// Windows: Fills a DCB with an open port's settings.
// Why: As OpenComm.
// By: CARDFILE.EXE, PRINTMAN.EXE, RASSER.DLL, TERMINAL.EXE

// USER.203 GetCommError(nCid, lpStat) -> int
// Windows: An open port's error bits since last asked, and how much its queues hold.
// Why: As OpenComm.
// By: CARDFILE.EXE, PRINTMAN.EXE, RASMXS.DLL, RASSER.DLL, TERMINAL.EXE

// USER.204 ReadComm(nCid, lpBuf, nSize) -> int
// Windows: Reads what a port has received.
// Why: As OpenComm.
// By: RASMXS.DLL, RASSER.DLL, TERMINAL.EXE

// USER.205 WriteComm(nCid, lpBuf, nSize) -> int
// Windows: Queues bytes for a port to send.
// Why: As OpenComm.
// By: CARDFILE.EXE, PRINTMAN.EXE, RASMXS.DLL, RASSER.DLL, TERMINAL.EXE

// USER.207 CloseComm(nCid) -> int
// Windows: Closes a port OpenComm opened.
// Why: As OpenComm.
// By: CARDFILE.EXE, MAIN.CPL, PRINTMAN.EXE, RASSER.DLL, TERMINAL.EXE

// USER.208 SetCommEventMask(nCid, nEvtMask) -> LPWORD
// Windows: Chooses which port events (a character in, a break, a line change) are recorded.
// Why: As OpenComm.
// By: RASSER.DLL, TERMINAL.EXE

// USER.209 GetCommEventMask(nCid, nEvtMask) -> WORD
// Windows: The port events recorded since asked, clearing those named.
// Why: As OpenComm.
// By: RASSER.DLL

// USER.210 SetCommBreak(nCid) -> int
// Windows: Holds a port's line in the break state.
// Why: As OpenComm.
// By: TERMINAL.EXE

// USER.211 ClearCommBreak(nCid) -> int
// Windows: Ends a break SetCommBreak began.
// Why: As OpenComm.
// By: TERMINAL.EXE

// USER.214 EscapeCommFunction(nCid, nFunc) -> LONG
// Windows: Drives a port's control lines (DTR, RTS) or other functions by number.
// Why: As OpenComm.
// By: MAIN.CPL, PRINTMAN.EXE, RASSER.DLL, TERMINAL.EXE

// USER.215 FlushComm(nCid, nQueue) -> int
// Windows: Empties a port's send or receive queue.
// Why: As OpenComm.
// By: CARDFILE.EXE, PRINTMAN.EXE, RASMXS.DLL, RASSER.DLL, TERMINAL.EXE

// USER.222 GetKeyboardState(lpKeyState) -> void
// Windows: Fills 256 bytes with every virtual key's state: down in the top bit, toggled in the low
// one.
// Why: GetKeyState knows only Shift, Ctrl and Alt today (the roadmap's "w16: GetKeyState for every
// key and toggle"); this would follow it.
// By: PRINTMAN.EXE, RECORDER.DLL, RECORDER.EXE, TERMINAL.EXE

// USER.223 SetKeyboardState(lpKeyState) -> void
// Windows: Replaces the 256-byte key state, as Recorder does when it plays keys back.
// Why: As GetKeyboardState.
// By: RECORDER.DLL, RECORDER.EXE, TERMINAL.EXE

// USER.224 GetWindowTask(hWnd) -> HANDLE
// Windows: The task a window belongs to.
// Why: Not written yet: every window is the one task's, so it is one line.
// By: DDEML.DLL, EFAXPUMP.DLL, IFKERNEL.DLL, MAILSPL.EXE, MCISEQ.DRV, MMSYSTEM.DLL, and 8 more

// USER.229 GetTopWindow(hWnd) -> HWND
// Windows: The first child of a window in Z order (the topmost), or 0.
// Why: Not written yet: w16 keeps each window's children in order.
// By: FRAMEWRK.DLL, PACKAGER.EXE, TERMINAL.EXE

// USER.233 SetParent(hWndChild, hWndNewParent) -> HWND
// Windows: Moves a child window under another parent; answers the old one.
// Why: Not written yet: w16's window tree is fixed at CreateWindow today.
// By: DDEML.DLL, FRAMEWRK.DLL, SCHDPLUS.EXE

// USER.240 DialogBoxIndirectParam(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam) -> int
// Windows: DialogBoxParam from a template in memory rather than a resource.
// Why: Not written yet: w16 builds dialogs from the same template bytes either way, so it is a
// small handler.
// By: COMMDLG.DLL

// USER.242 CreateDialogIndirectParam(hInstance, lpDialogTemplate, hWndParent, lpDialogFunc, dwInitParam) -> HWND
// Windows: A modeless dialog from a template in memory.
// Why: Not written yet, as DialogBoxIndirectParam.
// By: COMMDLG.DLL

// USER.245 EnableCommNotification(nCid, hWnd, nInTrigger, nOutTrigger) -> BOOL
// Windows: Asks for WM_COMMNOTIFY when a port has received or sent enough.
// Why: As OpenComm.
// By: RASMXS.DLL, RASSER.DLL

// USER.247 GetCursor() -> HCURSOR
// Windows: The cursor shown now.
// Why: Not written yet: w16 keeps it (SetCursor's answer), so it is a few lines.
// By: WFWSETUP.DLL

// USER.251 SendDriverMessage(hDriver, wMessage, lParam1, lParam2) -> LONG
// Windows: Sends an installable driver a message (DRV_*) and answers its reply.
// Why: Installable drivers (MCI, audio codecs) are Windows' own DLLs, not run under w16;
// MMSYSTEM's sound is toy-os's.
// By: DRIVERS.CPL, MMSYSTEM.DLL, POWER.DRV

// USER.252 OpenDriver(lpDriverName, lpSectionName, lParam) -> HDRVR
// Windows: Loads an installable driver named in SYSTEM.INI and opens it.
// Why: As SendDriverMessage.
// By: DRIVERS.CPL, MMSYSTEM.DLL, WINFILE.EXE

// USER.253 CloseDriver(hDriver, lParam1, lParam2) -> LONG
// Windows: Closes a driver OpenDriver opened, unloading it at its last use.
// Why: As SendDriverMessage.
// By: DRIVERS.CPL, MMSYSTEM.DLL, WINFILE.EXE

// USER.254 GetDriverModuleHandle(hDriver) -> HINSTANCE
// Windows: The module an open driver was loaded from.
// Why: As SendDriverMessage.
// By: CONTROL.EXE, MMSYSTEM.DLL

// USER.255 DefDriverProc(dwDriverIdentifier, hDriver, wMessage, lParam1, lParam2) -> LONG
// Windows: The default answers to the driver messages a driver does not handle itself.
// Why: As SendDriverMessage.
// By: MCICDA.DRV, MCISEQ.DRV, MCIWAVE.DRV, MIDIMAP.DRV, MMSYSTEM.DLL, MPU401.DRV, and 4 more

// USER.257 GetNextDriver(hDriver, dwFlags) -> HDRVR
// Windows: Steps through the open installable drivers.
// Why: As SendDriverMessage.
// By: CONTROL.EXE, POWER.DRV

// USER.259 BeginDeferWindowPos(nNumWindows) -> HDWP
// Windows: Starts a batch of window moves made all at once by EndDeferWindowPos.
// Why: Not written yet: a batch of SetWindowPos calls, which w16 answers one at a time.
// By: FRAMEWRK.DLL

// USER.260 DeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, wFlags) -> HDWP
// Windows: Adds one window's move to a batch.
// Why: Not written yet, as BeginDeferWindowPos.
// By: FRAMEWRK.DLL

// USER.261 EndDeferWindowPos(hWinPosInfo) -> void
// Windows: Makes every move in a batch at once.
// Why: Not written yet, as BeginDeferWindowPos.
// By: FRAMEWRK.DLL

// USER.270 GlobalFindAtom(lpString) -> ATOM
// Windows: The global atom a string was added as, or 0, without adding it.
// Why: Not written yet: the global atom table is w16's own, as FindAtom.
// By: DDEML.DLL, EFAXPUMP.DLL, FAXSTUB.DLL, HPPCL5MS.DRV, MAILSPL.EXE, MSMAIL.EXE, and 8 more

// USER.284 GetFreeSystemResources(fuSysResource) -> UINT
// Windows: The percentage of USER's or GDI's heap still free -- About boxes' "System Resources".
// Why: Not written yet: w16's heaps grow, so it would answer a constant high figure.
// By: SHELL.DLL

// USER.290 RedrawWindow(hWnd, lprcUpdate, hrgnUpdate, flags) -> BOOL
// Windows: Invalidates, validates or repaints part of a window, with flags for erasing and
// children.
// Why: Not written yet: w16 paints whole windows; this would map onto its InvalidateRect and
// UpdateWindow.
// By: WINPOPUP.EXE

// USER.319 ScrollWindowEx(hWnd, dx, dy, prcScroll, prcClip, hrgnUpdate, prcUpdate, flags) -> int
// Windows: ScrollWindow with a region and rectangle back for what was uncovered, and flags for
// children and erasing.
// Why: As ScrollWindow, a stand-in today: the pixels are not moved.
// By: WINMETER.EXE

// USER.368 CopyIcon(hInstance, hIcon) -> HICON
// Windows: A copy of an icon, owned by the module named.
// Why: Not written yet: w16's icons are handles onto a program's resources, and a copy would be
// one more.
// By: POWER.DRV, PROGMAN.EXE

// USER.370 GetWindowPlacement(hWnd, lpwndpl) -> BOOL
// Windows: A window's show state and its normal, minimized and maximized positions.
// Why: Not written yet: toywm knows a w16 window's state and its normal rectangle.
// By: CLIPBRD.EXE, CONTROL.EXE, MPLAYER.EXE, NETWATCH.EXE, PRINTMAN.EXE, PROGMAN.EXE, and 2 more

// USER.371 SetWindowPlacement(hWnd, lpwndpl) -> BOOL
// Windows: Sets a window's show state and positions, as a program restoring where it was.
// Why: Not written yet, as GetWindowPlacement.
// By: CLIPBRD.EXE, NETWATCH.EXE, PROGMAN.EXE, WINCHAT.EXE, WINFILE.EXE

// USER.372 GetInternalIconHeader(lpIcon, lpDestIcon) -> void
// Windows: Copies USER's internal header of an icon -- Program Manager's way of reading icons it
// shows.
// Why: Windows' own, undocumented: Program Manager's.
// By: PROGMAN.EXE

// USER.373 SubtractRect(lprcDest, lprcSrc1, lprcSrc2) -> BOOL
// Windows: The first rectangle less the second, where the result is still a rectangle.
// Why: Not written yet: a few lines beside UnionRect and IntersectRect.
// By: COMMCTRL.DLL

// USER.402 GetPriorityClipboardFormat(lpPriorityList, cEntries) -> int
// Windows: The first format in a list that the clipboard holds, -1 if none does.
// Why: Not written yet: toy-os's clipboard knows what it holds.
// By: FRAMEWRK.DLL

// USER.403 UnregisterClass(lpClassName, hInstance) -> BOOL
// Windows: Takes a window class away; its windows must be gone first.
// Why: Not written yet: w16's classes go when the program ends, which every program run so far
// waited for.
// By: CLIPSRV.EXE, MIDIMAP.DRV, MMSYSTEM.DLL, MSREMIND.EXE, RASPHONE.EXE, SL.DLL, and 1 more

// USER.412 RemoveMenu(hMenu, nPosition, wFlags) -> BOOL
// Windows: Takes an item out of a menu without destroying the popup it opens.
// Why: Not written yet: DeleteMenu is there; this is the same without the destroy.
// By: MSMAIL.EXE, RASPHONE.EXE

// USER.433 IsCharAlpha(cChar) -> BOOL
// Windows: Whether a character is a letter in the code page -- é and ö too.
// Why: Not written yet: w16's AnsiUpper and AnsiLower already know the code page's letters.
// By: DEMILAYR.DLL, PROGMAN.EXE, WGPOMGR.DLL, WINSETUP.EXE

// USER.434 IsCharAlphaNumeric(cChar) -> BOOL
// Windows: Whether a character is a letter or a digit in the code page.
// Why: Not written yet, as IsCharAlpha.
// By: DEMILAYR.DLL, PROGMAN.EXE

// USER.435 IsCharUpper(cChar) -> BOOL
// Windows: Whether a character is an upper-case letter in the code page.
// Why: Not written yet, as IsCharAlpha.
// By: DEMILAYR.DLL, FINSTALL.DLL, LZEXPAND.DLL

// USER.436 IsCharLower(cChar) -> BOOL
// Windows: Whether a character is a lower-case letter in the code page.
// Why: Not written yet, as IsCharAlpha.
// By: DEMILAYR.DLL, FRAMEWRK.DLL, LZEXPAND.DLL

// USER.438 AnsiLowerBuff(lpString, nLength) -> UINT
// Windows: Lower-cases a buffer of the length given, in the code page.
// Why: Not written yet, as IsCharAlpha.
// By: DEMILAYR.DLL, FRAMEWRK.DLL

// USER.445 DefFrameProc(hWnd, hWndMDIClient, wMsg, wParam, lParam) -> LONG
// Windows: The default window procedure of an MDI frame: its Window menu, its children's maximized
// state and their keys.
// Why: MDI is not built: a frame with child documents inside it (Clipboard Viewer, File Manager,
// Program Manager) needs the MDICLIENT class and these three calls together.
// By: CLIPBRD.EXE, FRAMEWRK.DLL, PROGMAN.EXE, SYSEDIT.EXE, WINFILE.EXE

// USER.447 DefMDIChildProc(hWnd, wMsg, wParam, lParam) -> LONG
// Windows: The default window procedure of an MDI child window.
// Why: As DefFrameProc.
// By: CLIPBRD.EXE, FRAMEWRK.DLL, PROGMAN.EXE, SYSEDIT.EXE, WINFILE.EXE

// USER.451 TranslateMDISysAccel(hWndClient, lpMsg) -> BOOL
// Windows: Turns an MDI child's keys (Ctrl+F4, Ctrl+F6) into its system commands.
// Why: As DefFrameProc.
// By: CLIPBRD.EXE, FRAMEWRK.DLL, MSMAIL.EXE, PROGMAN.EXE, SYSEDIT.EXE, WINFILE.EXE

// USER.454 AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle) -> void
// Windows: The window rectangle a client rectangle needs, with an extended style.
// Why: Not written yet: AdjustWindowRect is the same without the extended style.
// By: FRAMEWRK.DLL

// USER.455 GetIconID(hRes, lpType) -> WORD
// Windows: The icon of a group that best fits the display -- what LoadIcon chooses.
// Why: Windows' own: SHELL's.
// By: SHELL.DLL

// USER.457 DestroyIcon(hIcon) -> BOOL
// Windows: Frees an icon CreateIcon or CopyIcon made.
// Why: Not written yet: w16's icons are resource handles with nothing to free, so it would answer
// TRUE.
// By: FRAMEWRK.DLL, MAILSPL.EXE, MSMAIL.EXE, PACKAGER.EXE, POWER.DRV, PROGMAN.EXE, and 3 more

// USER.458 DestroyCursor(hCursor) -> BOOL
// Windows: Frees a cursor CreateCursor made.
// Why: Not written yet: a program's own cursor is a global block w16 could free.
// By: RASMON.EXE, WFWSETUP.DLL

// USER.459 DumpIcon(lpIcon, lpcbHeader, lplpXorBits, lplpAndBits) -> DWORD
// Windows: Finds the parts of an icon in memory -- Program Manager's.
// Why: Windows' own, undocumented: Program Manager's.
// By: PROGMAN.EXE

// USER.462 CalcChildScroll(hWnd, wScroll) -> void
// Windows: Recalculates an MDI client's scroll bars from where its children are.
// Why: As DefFrameProc.
// By: PROGMAN.EXE

// USER.464 DragObject(hWndParent, hWndFrom, wObj, dwData, hCursor) -> DWORD
// Windows: Runs a drag of an object between windows (Program Manager's icons, File Manager's
// files), with WM_QUERYDROPOBJECT along the way.
// Why: Drags between programs are the roadmap's "w16: a file dropped on a program (WM_DROPFILES)"
// first; this is the older, wider protocol.
// By: PROGMAN.EXE, WINFILE.EXE

// USER.499 WNetErrorText(wError, lpBuffer, nBufferSize) -> WORD
// Windows: The text of a network error.
// Why: The WNet calls are the network's: toy-os's SMB mounts are drives to w16, with no network
// API behind them.
// By: WFWNET.DRV, WINFILE.EXE

// USER.501 WNetOpenJob(lpQueue, lpTitle, nCopies, lpfh) -> WORD
// Windows: Opens a print job on a network printer queue.
// Why: No printing, and no network queues (the roadmap's "w16: printing").
// By: PRINTMAN.EXE

// USER.502 WNetCloseJob(fh, lpJobID, lpQueue) -> WORD
// Windows: Closes a network print job.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.504 WNetHoldJob(lpQueue, wJobID) -> WORD
// Windows: Holds a job in a network print queue.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.505 WNetReleaseJob(lpQueue, wJobID) -> WORD
// Windows: Releases a held network print job.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.506 WNetCancelJob(lpQueue, wJobID) -> WORD
// Windows: Cancels a network print job.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.508 WNetWatchQueue(hWnd, lpLocal, lpUser, nQueue) -> WORD
// Windows: Asks to be told when a network print queue changes.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.509 WNetUnwatchQueue(lpLocal) -> WORD
// Windows: Stops watching a network print queue.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.510 WNetLockQueueData(lpQueue, lpUser, lplpQueueStruct) -> WORD
// Windows: Locks a network print queue's job list for reading.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.511 WNetUnlockQueueData(lpQueue) -> WORD
// Windows: Unlocks a queue's job list.
// Why: As WNetOpenJob.
// By: PRINTMAN.EXE

// USER.512 WNetGetConnection(lpLocalName, lpRemoteName, cbBufferSize) -> WORD
// Windows: The network share a drive letter or port is connected to.
// Why: As WNetErrorText. It would answer "not a network drive" for every letter -- drive C: and
// the rest are toy-os's folders.
// By: COMMDLG.DLL, CPWIN386.CPL, MAIN.CPL, MSSFS.DLL, NETFAX.DLL, OLECLI.DLL, and 5 more

// USER.514 WNetDeviceMode(hWndParent) -> WORD
// Windows: Opens the network driver's own setup dialog.
// Why: As WNetErrorText.
// By: MAIN.CPL

// USER.515 WNetBrowseDialog(hWndParent, nType, szPath) -> WORD
// Windows: Opens the network's browse dialog for a share or printer.
// Why: As WNetErrorText.
// By: MAIN.CPL, WGPOMGR.DLL, WINFILE.EXE

// USER.516 WNetGetUser(szUser, nBufferSize) -> WORD
// Windows: The user name the network knows.
// Why: As WNetErrorText.
// By: PRINTMAN.EXE, WGPOMGR.DLL

// USER.517 WNetAddConnection(lpNetPath, lpPassword, lpLocalName) -> WORD
// Windows: Connects a drive letter or port to a network share.
// Why: As WNetErrorText.
// By: DEMILAYR.DLL, MAIN.CPL, MSSFS.DLL, OLECLI.DLL, TRNSCHED.DLL, WGPOMGR.DLL, and 1 more

// USER.518 WNetCancelConnection(lpName, bForce) -> WORD
// Windows: Disconnects a drive letter from its share.
// Why: As WNetErrorText.
// By: DEMILAYR.DLL, MAIN.CPL, MSSFS.DLL, TRNSCHED.DLL, WGPOMGR.DLL, WINFILE.EXE

// USER.519 WNetGetError(nError) -> WORD
// Windows: The network driver's own error for the last call.
// Why: As WNetErrorText.
// By: MAIN.CPL, PRINTMAN.EXE

// USER.520 WNetGetErrorText(nError, lpBuffer, nBufferSize) -> WORD
// Windows: The text of a network driver's own error.
// Why: As WNetErrorText.
// By: MAIN.CPL, OLECLI.DLL, PRINTMAN.EXE

// USER.523 WNetRestoreConnection(hwndOwner, lpszDevice) -> WORD
// Windows: Reconnects the shares remembered from the last session.
// Why: As WNetErrorText.
// By: PRINTMAN.EXE, WINFILE.EXE

// USER.525 WNetConnectDialog(hWndParent, iType) -> WORD
// Windows: Opens the network's Connect Network Drive (or Printer) dialog.
// Why: As WNetErrorText.
// By: COMMDLG.DLL, NETFAX.DLL, PRINTMAN.EXE, WINFILE.EXE

// USER.526 WNetDisconnectDialog(hWndParent, iType) -> WORD
// Windows: Opens the network's Disconnect dialog.
// Why: As WNetErrorText.
// By: PRINTMAN.EXE, WINFILE.EXE

// USER.527 WNetConnectionDialog(hWndParent, iType) -> WORD
// Windows: Opens the network's Connections dialog.
// Why: As WNetErrorText.
// By: COMMDLG.DLL, MAIN.CPL, WINFILE.EXE

// USER.529 WNetPropertyDialog(hwndParent, iButton, nPropSel, lpszName, nType) -> WORD
// Windows: Opens a network property sheet File Manager offers for a shared directory.
// Why: As WNetErrorText.
// By: WINFILE.EXE

// USER.530 WNetGetDirectoryType(lpName, lpType) -> WORD
// Windows: Whether a directory is on the network, and its kind.
// Why: As WNetErrorText.
// By: WINFILE.EXE

// USER.531 WNetDirectoryNotify(hwnd, lpDir, wOper) -> WORD
// Windows: Tells the network a directory is being made, removed or renamed.
// Why: As WNetErrorText.
// By: WINFILE.EXE

// USER.532 WNetGetPropertyText(iButton, nPropSel, lpszName, lpszButtonName, cbButtonName, nType) -> WORD
// Windows: The label of a network property button File Manager shows.
// Why: As WNetErrorText.
// By: WINFILE.EXE

// --- VER -------------------------------------------------------------------------------------

// VER.3 GetFileResource(lpszFileName, lpszResType, lpszResID, dwFileOffset, dwResLen, lpvData) -> BOOL
// Windows: Reads one resource straight from an executable file.
// Why: VER is not answered yet: w16 reads executables' resources already, so the version calls are
// small.
// By: MAIN.CPL

// VER.6 GetFileVersionInfoSize(lpszFileName, lpdwHandle) -> DWORD
// Windows: How big a file's VERSIONINFO resource is.
// Why: As GetFileResource.
// By: DRIVERS.CPL, WFWSETUP.DLL, WINFILE.EXE, WINSETUP.EXE

// VER.7 GetFileVersionInfo(lpszFileName, dwHandle, cbBuf, lpvData) -> BOOL
// Windows: Reads a file's VERSIONINFO resource.
// Why: As GetFileResource.
// By: DRIVERS.CPL, WFWSETUP.DLL, WINFILE.EXE, WINSETUP.EXE

// VER.9 VerInstallFile(fuFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen) -> DWORD
// Windows: Copies a file into place if it is newer, expanding it on the way.
// Why: As GetFileResource, and LZEXPAND's.
// By: DRIVERS.CPL, MAIN.CPL, WFWSETUP.DLL, WINSETUP.EXE

// VER.10 VerLanguageName(uLang, szLang, cbLang) -> UINT
// Windows: The name of a language from its id (0x0409 is English (United States)).
// Why: Not written yet: a table of 3.1's language names.
// By: WINFILE.EXE

// VER.11 VerQueryValue(lpvBlock, lpszSubBlock, lplpBuffer, lpcb) -> BOOL
// Windows: One value from a VERSIONINFO block -- its fixed part, or a string such as CompanyName.
// Why: As GetFileResource.
// By: WFWSETUP.DLL, WINFILE.EXE, WINSETUP.EXE
