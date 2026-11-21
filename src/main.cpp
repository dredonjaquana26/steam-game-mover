// Steam Game Mover — dark Win32 GUI. Move a Steam game to another drive in one click.
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <cwctype>
#include <algorithm>
using std::min; using std::max;
#include <gdiplus.h>
#include <string>
#include <vector>
#include "engine.h"

#pragma comment(lib,"comctl32.lib")

using namespace sgm;

// ---- palette (matches our player) ----
static const COLORREF C_BG    = RGB(14,16,21);
static const COLORREF C_PANEL = RGB(23,26,33);
static const COLORREF C_CARD  = RGB(18,21,27);
static const COLORREF C_ACC   = RGB(37,99,235);
static const COLORREF C_ACC2  = RGB(59,130,246);
static const COLORREF C_LIGHT = RGB(229,231,235);
static const COLORREF C_MUTED = RGB(139,147,167);
static const COLORREF C_LINE  = RGB(42,47,58);

enum { IDC_LIST=1001, IDC_DEST, IDC_BROWSE, IDC_MOVE, IDC_UNDO, IDC_REFRESH, IDC_PROG, IDC_STATUS };
#define WM_APP_PROG (WM_APP+1)
#define WM_APP_DONE (WM_APP+2)

static HINSTANCE g_hInst;
static HWND g_hList, g_hDest, g_hBrowse, g_hMove, g_hUndo, g_hRefresh, g_hProg, g_hStatus, g_hMoveLbl, g_hMain;
static HFONT g_fTitle, g_fSub, g_fUi, g_fBtn;
static HBRUSH g_brBg, g_brPanel;
static std::vector<Game> g_games;
static std::wstring g_dest;
static bool g_demo=false, g_busy=false;
static volatile bool g_cancel=false;

static void EnsureFonts(){
    if(g_fTitle) return;
    g_fTitle=CreateFontW(26,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fSub  =CreateFontW(16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fUi   =CreateFontW(17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fBtn  =CreateFontW(18,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
}

static void RoundFill(HDC dc,int l,int t,int r,int b,int rad,COLORREF fill,bool border,COLORREF bcol){
    HBRUSH br=CreateSolidBrush(fill);
    HPEN pen=border?CreatePen(PS_SOLID,1,bcol):(HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob=SelectObject(dc,br), op=SelectObject(dc,pen);
    RoundRect(dc,l,t,r,b,rad,rad);
    SelectObject(dc,ob); SelectObject(dc,op);
    DeleteObject(br); if(border) DeleteObject(pen);
}

// ---------- self-install ----------
static std::wstring KnownFolder(REFKNOWNFOLDERID id){
    PWSTR p=NULL; std::wstring r;
    if(SUCCEEDED(SHGetKnownFolderPath(id,0,NULL,&p))){ r=p; CoTaskMemFree(p);} return r;
}
static void CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& desc){
    if(PathExists(lnk)) return;
    IShellLinkW* sl=NULL;
    if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,NULL,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&sl))){
        sl->SetPath(target.c_str());
        std::wstring dir=target.substr(0,target.find_last_of(L"\\"));
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetDescription(desc.c_str());
        sl->SetIconLocation(target.c_str(),0);
        IPersistFile* pf=NULL;
        if(SUCCEEDED(sl->QueryInterface(IID_IPersistFile,(void**)&pf))){ pf->Save(lnk.c_str(),TRUE); pf->Release(); }
        sl->Release();
    }
}
static void EnsureInstalled(){
    std::wstring local=KnownFolder(FOLDERID_LocalAppData);
    if(local.empty()) return;
    std::wstring dir=local+L"\\SteamGameMover";
    SHCreateDirectoryExW(NULL,dir.c_str(),NULL);
    wchar_t cur[MAX_PATH]; GetModuleFileNameW(NULL,cur,MAX_PATH);
    std::wstring installed=dir+L"\\SteamGameMover.exe";
    if(_wcsicmp(cur,installed.c_str())!=0){ CopyFileW(cur,installed.c_str(),FALSE); }
    std::wstring tgt=PathExists(installed)?installed:cur;
    CreateShortcut(KnownFolder(FOLDERID_Desktop)+L"\\Steam Game Mover.lnk", tgt, L"Move Steam games to another drive");
    CreateShortcut(KnownFolder(FOLDERID_Programs)+L"\\Steam Game Mover.lnk", tgt, L"Move Steam games to another drive");
}

// ---------- list ----------
static void FillDemo(){
    g_games.clear();
    auto add=[&](const wchar_t* n,const wchar_t* d,uint64_t sz,const wchar_t* fp,bool moved){
        Game g; g.name=n; g.installDir=d; g.sizeOnDisk=sz; g.folderPath=fp; g.moved=moved; g_games.push_back(g);
    };
    add(L"Counter-Strike 2", L"Counter-Strike Global Offensive", 32212254720ull, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive", false);
    add(L"Cyberpunk 2077", L"Cyberpunk 2077", 76235112448ull, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Cyberpunk 2077", false);
    add(L"Dota 2", L"dota 2 beta", 38654705664ull, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta", true);
    add(L"Elden Ring", L"ELDEN RING", 64424509440ull, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ELDEN RING", false);
    add(L"Baldur's Gate 3", L"Baldurs Gate 3", 150323855360ull, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Baldurs Gate 3", false);
}

static void RefreshList(){
    if(!g_demo) g_games=EnumGames();
    SendMessageW(g_hList,LVM_DELETEALLITEMS,0,0);
    for(size_t i=0;i<g_games.size();++i){
        const Game& g=g_games[i];
        LVITEMW it{}; it.mask=LVIF_TEXT|LVIF_PARAM; it.iItem=(int)i; it.lParam=(LPARAM)i;
        std::wstring name=g.name;
        it.pszText=(LPWSTR)name.c_str();
        ListView_InsertItem(g_hList,&it);
        std::wstring drive = g.folderPath.size()>=2 ? g.folderPath.substr(0,2) : L"?";
        if(g.moved){ std::wstring t=ReadJunction(g.folderPath); if(t.size()>=2) drive=g.folderPath.substr(0,2); }
        ListView_SetItemText(g_hList,(int)i,1,(LPWSTR)drive.c_str());
        std::wstring sz=HumanSize(g.sizeOnDisk);
        ListView_SetItemText(g_hList,(int)i,2,(LPWSTR)sz.c_str());
        std::wstring st;
        if(g.moved){ std::wstring t=ReadJunction(g.folderPath); std::wstring td=t.size()>=2?t.substr(0,2):L"?"; st=L"Moved \x2192 "+td; }
        else st=L"On "+drive;
        ListView_SetItemText(g_hList,(int)i,3,(LPWSTR)st.c_str());
    }
    int n=(int)g_games.size();
    std::wstring s = g_demo? L"" : (GetSteamPath().empty()? L"Steam was not found on this PC." :
        (n==0? L"No installed games found." : (std::to_wstring(n)+L" game(s) found.")));
    SetWindowTextW(g_hStatus,s.c_str());
}

static int SelectedIndex(){
    int sel=(int)SendMessageW(g_hList,LVM_GETNEXTITEM,(WPARAM)-1,LVNI_SELECTED);
    if(sel<0) return -1;
    LVITEMW it{}; it.mask=LVIF_PARAM; it.iItem=sel;
    ListView_GetItem(g_hList,&it);
    return (int)it.lParam;
}

// ---------- browse folder ----------
static std::wstring PickFolder(HWND owner){
    std::wstring result;
    IFileOpenDialog* d=NULL;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,IID_IFileOpenDialog,(void**)&d))){
        DWORD opt=0; d->GetOptions(&opt); d->SetOptions(opt|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Choose the destination drive / folder");
        if(SUCCEEDED(d->Show(owner))){
            IShellItem* si=NULL;
            if(SUCCEEDED(d->GetResult(&si))){
                PWSTR p=NULL;
                if(SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH,&p))){ result=p; CoTaskMemFree(p);} si->Release();
            }
        }
        d->Release();
    }
    return result;
}

// ---------- worker ----------
struct Job { std::wstring src, destParent; bool undo; };
static DWORD WINAPI Worker(LPVOID param){
    Job* j=(Job*)param;
    Progress pr; pr.cancel=&g_cancel;
    pr.cb=[&](int p){ PostMessageW(g_hMain,WM_APP_PROG,(WPARAM)p,0); };
    std::wstring err = j->undo ? UndoMove(j->src,&pr) : MoveGame(j->src,j->destParent,&pr);
    wchar_t* heap=NULL;
    if(!err.empty()){ heap=(wchar_t*)LocalAlloc(LPTR,(err.size()+1)*sizeof(wchar_t)); wcscpy(heap,err.c_str()); }
    PostMessageW(g_hMain,WM_APP_DONE,(WPARAM)(err.empty()?1:0),(LPARAM)heap);
    delete j; return 0;
}
static void StartJob(bool undo){
    if(g_busy) return;
    int idx=SelectedIndex();
    if(idx<0){ MessageBoxW(g_hMain,L"Select a game from the list first.",L"Steam Game Mover",MB_ICONINFORMATION); return; }
    const Game& g=g_games[idx];
    if(g_demo){ MessageBoxW(g_hMain,L"Demo mode.",L"Steam Game Mover",MB_ICONINFORMATION); return; }
    if(!undo){
        if(g.moved){ MessageBoxW(g_hMain,L"This game is already moved. Use Undo to bring it back.",L"Steam Game Mover",MB_ICONINFORMATION); return; }
        if(g_dest.empty()){ g_dest=PickFolder(g_hMain); SetWindowTextW(g_hDest,g_dest.c_str()); if(g_dest.empty()) return; }
        std::wstring msg=L"Move \""+g.name+L"\" ("+HumanSize(g.sizeOnDisk)+L") to:\n"+g_dest+L"\n\nSteam will keep working from the new location.";
        if(MessageBoxW(g_hMain,msg.c_str(),L"Confirm move",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return;
    } else {
        if(!g.moved){ MessageBoxW(g_hMain,L"This game is not moved.",L"Steam Game Mover",MB_ICONINFORMATION); return; }
        if(MessageBoxW(g_hMain,(L"Bring \""+g.name+L"\" back to its original drive?").c_str(),L"Confirm undo",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return;
    }
    g_busy=true; g_cancel=false;
    EnableWindow(g_hMove,FALSE); EnableWindow(g_hUndo,FALSE); EnableWindow(g_hBrowse,FALSE); EnableWindow(g_hRefresh,FALSE);
    SendMessageW(g_hProg,PBM_SETPOS,0,0);
    SetWindowTextW(g_hStatus, undo?L"Bringing the game back\x2026":L"Copying\x2026 please wait.");
    Job* j=new Job{ g.folderPath, g_dest, undo };
    CreateThread(NULL,0,Worker,j,0,NULL);
}

// ---------- owner-draw button ----------
static void DrawButton(LPDRAWITEMSTRUCT d){
    bool primary = (d->CtlID==IDC_MOVE);
    bool pressed = (d->itemState & ODS_SELECTED);
    bool disabled= (d->itemState & ODS_DISABLED);
    RECT r=d->rcItem;
    HDC dc=d->hDC;
    COLORREF fill = primary ? (pressed?RGB(29,78,216):C_ACC) : (pressed?RGB(40,46,58):C_PANEL);
    if(disabled) fill = primary?RGB(44,55,90):RGB(28,32,40);
    HBRUSH b=CreateSolidBrush(fill);
    HPEN pen=CreatePen(PS_SOLID,1, primary?fill:C_LINE);
    HGDIOBJ ob=SelectObject(dc,b), op=SelectObject(dc,pen);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,10,10);
    SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(b); DeleteObject(pen);
    wchar_t txt[128]; GetWindowTextW(d->hwndItem,txt,128);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc, disabled?C_MUTED:(primary?RGB(255,255,255):C_LIGHT));
    HGDIOBJ of=SelectObject(dc,g_fBtn);
    DrawTextW(dc,txt,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,of);
}

// ---------- header paint ----------
static void PaintHeader(HWND hwnd){
    PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
    RECT rc; GetClientRect(hwnd,&rc);
    // bg
    HBRUSH bg=CreateSolidBrush(C_BG); FillRect(dc,&rc,bg); DeleteObject(bg);
    // header band
    RECT hb=rc; hb.bottom=70;
    HBRUSH hbr=CreateSolidBrush(C_PANEL); FillRect(dc,&hb,hbr); DeleteObject(hbr);
    RECT ln={0,70,rc.right,71}; HBRUSH lb=CreateSolidBrush(C_LINE); FillRect(dc,&ln,lb); DeleteObject(lb);
    // accent chip
    RECT chip={16,18,52,54}; HBRUSH cb=CreateSolidBrush(C_ACC);
    HPEN np=(HPEN)GetStockObject(NULL_PEN); HGDIOBJ oldp=SelectObject(dc,np); HGDIOBJ oldb=SelectObject(dc,cb);
    RoundRect(dc,chip.left,chip.top,chip.right,chip.bottom,8,8);
    SelectObject(dc,oldb); SelectObject(dc,oldp); DeleteObject(cb);
    // arrow glyph in chip (two chevrons)
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
    HGDIOBJ of=SelectObject(dc,g_fTitle);
    RECT cr=chip; DrawTextW(dc,L"\x00BB",-1,&cr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    // title + subtitle
    SetTextColor(dc,C_LIGHT);
    RECT tr={64,12,rc.right-16,40}; DrawTextW(dc,L"Steam Game Mover",-1,&tr,DT_LEFT|DT_SINGLELINE);
    SelectObject(dc,g_fSub); SetTextColor(dc,C_MUTED);
    RECT sr={64,40,rc.right-16,64}; DrawTextW(dc,L"Move a game to another drive \x2014 Steam keeps working, no re-download.",-1,&sr,DT_LEFT|DT_SINGLELINE);
    SelectObject(dc,of);
    EndPaint(hwnd,&ps);
}

static void Layout(HWND hwnd){
    RECT rc; GetClientRect(hwnd,&rc);
    int W=rc.right, H=rc.bottom;
    int m=16, panelH=128, listTop=82, listBottom=H-panelH;
    MoveWindow(g_hList, m, listTop, W-2*m, listBottom-listTop, TRUE);
    int py=H-panelH+10;
    MoveWindow(g_hMoveLbl, m, py+4, 66, 20, TRUE);
    MoveWindow(g_hDest, m+74, py, W-2*m-74-110-8, 26, TRUE);
    MoveWindow(g_hBrowse, W-m-110, py-1, 110, 28, TRUE);
    int by=py+40;
    MoveWindow(g_hMove, m, by, 230, 38, TRUE);
    MoveWindow(g_hUndo, m+240, by, 150, 38, TRUE);
    MoveWindow(g_hRefresh, m+400, by, 120, 38, TRUE);
    MoveWindow(g_hProg, m, by+48, W-2*m, 16, TRUE);
    MoveWindow(g_hStatus, m, by+70, W-2*m, 20, TRUE);
}

static void CreateControls(HWND hwnd){
    EnsureFonts();

    g_hList=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
        0,0,10,10,hwnd,(HMENU)IDC_LIST,g_hInst,NULL);
    ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(g_hList,L"DarkMode_Explorer",NULL);
    ListView_SetBkColor(g_hList,C_CARD);
    ListView_SetTextBkColor(g_hList,C_CARD);
    ListView_SetTextColor(g_hList,C_LIGHT);
    { HWND hh=ListView_GetHeader(g_hList); if(hh) SetWindowTheme(hh,L"DarkMode_ItemsView",NULL); }
    SendMessageW(g_hList,WM_SETFONT,(WPARAM)g_fUi,TRUE);
    auto col=[&](int i,const wchar_t* t,int w){ LVCOLUMNW c{}; c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM; c.pszText=(LPWSTR)t; c.cx=w; c.iSubItem=i; ListView_InsertColumn(g_hList,i,&c); };
    col(0,L"Game",360); col(1,L"Drive",90); col(2,L"Size",120); col(3,L"Status",200);

    g_hDest=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_READONLY,0,0,10,10,hwnd,(HMENU)IDC_DEST,g_hInst,NULL);
    SendMessageW(g_hDest,WM_SETFONT,(WPARAM)g_fUi,TRUE);
    SendMessageW(g_hDest,EM_SETCUEBANNER,TRUE,(LPARAM)L"Destination drive / folder \x2014 click Browse\x2026");

    g_hBrowse =CreateWindowExW(0,L"BUTTON",L"Browse\x2026",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,10,10,hwnd,(HMENU)IDC_BROWSE,g_hInst,NULL);
    g_hMove   =CreateWindowExW(0,L"BUTTON",L"Move selected game",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,10,10,hwnd,(HMENU)IDC_MOVE,g_hInst,NULL);
    g_hUndo   =CreateWindowExW(0,L"BUTTON",L"Undo move",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,10,10,hwnd,(HMENU)IDC_UNDO,g_hInst,NULL);
    g_hRefresh=CreateWindowExW(0,L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,10,10,hwnd,(HMENU)IDC_REFRESH,g_hInst,NULL);

    g_hProg=CreateWindowExW(0,PROGRESS_CLASSW,L"",WS_CHILD|WS_VISIBLE|PBS_SMOOTH,0,0,10,10,hwnd,(HMENU)IDC_PROG,g_hInst,NULL);
    SetWindowTheme(g_hProg,L"",L"");   // drop theme so our bar color applies
    SendMessageW(g_hProg,PBM_SETRANGE,0,MAKELPARAM(0,100));
    SendMessageW(g_hProg,PBM_SETBARCOLOR,0,(LPARAM)C_ACC);
    SendMessageW(g_hProg,PBM_SETBKCOLOR,0,(LPARAM)C_CARD);

    g_hStatus=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,10,10,hwnd,(HMENU)IDC_STATUS,g_hInst,NULL);
    SendMessageW(g_hStatus,WM_SETFONT,(WPARAM)g_fUi,TRUE);

    g_hMoveLbl=CreateWindowExW(0,L"STATIC",L"Move to:",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,10,10,hwnd,NULL,g_hInst,NULL);
    SendMessageW(g_hMoveLbl,WM_SETFONT,(WPARAM)g_fUi,TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        g_hMain=hwnd;
        BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        CreateControls(hwnd);
        return 0;
    }
    case WM_SIZE: Layout(hwnd); return 0;
    case WM_ERASEBKGND: return 1; // we paint in WM_PAINT
    case WM_PAINT: PaintHeader(hwnd); return 0;
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)wp; SetBkColor(dc,C_BG);
        SetTextColor(dc, (HWND)lp==g_hStatus?C_MUTED:C_LIGHT);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT:{ HDC dc=(HDC)wp; SetBkColor(dc,C_CARD); SetTextColor(dc,C_LIGHT); return (LRESULT)g_brPanel; }
    case WM_DRAWITEM:{ DrawButton((LPDRAWITEMSTRUCT)lp); return TRUE; }
    case WM_COMMAND:{
        switch(LOWORD(wp)){
        case IDC_BROWSE:{ std::wstring f=PickFolder(hwnd); if(!f.empty()){ g_dest=f; SetWindowTextW(g_hDest,f.c_str()); } return 0; }
        case IDC_MOVE: StartJob(false); return 0;
        case IDC_UNDO: StartJob(true); return 0;
        case IDC_REFRESH: RefreshList(); return 0;
        }
        return 0;
    }
    case WM_APP_PROG: SendMessageW(g_hProg,PBM_SETPOS,(WPARAM)wp,0); return 0;
    case WM_APP_DONE:{
        g_busy=false;
        EnableWindow(g_hMove,TRUE); EnableWindow(g_hUndo,TRUE); EnableWindow(g_hBrowse,TRUE); EnableWindow(g_hRefresh,TRUE);
        SendMessageW(g_hProg,PBM_SETPOS,(wp?100:0),0);
        if(wp){ SetWindowTextW(g_hStatus,L"Done. The game now runs from the new drive."); }
        else {
            wchar_t* e=(wchar_t*)lp; std::wstring m=e?e:L"Operation failed.";
            SetWindowTextW(g_hStatus,m.c_str());
            MessageBoxW(hwnd,m.c_str(),L"Steam Game Mover",MB_ICONWARNING);
        }
        if(lp) LocalFree((HLOCAL)lp);
        RefreshList();
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

// ---------- headless: selftest ----------
static int SelfTest(){
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH,tmp);
    std::wstring log=std::wstring(tmp)+L"sgm_selftest.log";
    FILE* f=_wfopen(log.c_str(),L"wb");
    auto out=[&](const std::wstring& s){
        if(!f) return;
        std::wstring line=s+L"\r\n";
        int n=WideCharToMultiByte(CP_UTF8,0,line.c_str(),(int)line.size(),NULL,0,NULL,NULL);
        std::string u(n,0); WideCharToMultiByte(CP_UTF8,0,line.c_str(),(int)line.size(),&u[0],n,NULL,NULL);
        fwrite(u.data(),1,u.size(),f);
    };
    std::wstring steam=GetSteamPath();
    out(L"SteamPath = "+(steam.empty()?L"(not found)":steam));
    auto libs=GetLibraries(steam);
    out(L"libraries = "+std::to_wstring(libs.size()));
    for(auto&l:libs) out(L"  lib: "+l);
    auto games=EnumGames();
    out(L"games = "+std::to_wstring(games.size()));
    int shown=0; for(auto&g:games){ if(shown++>=10) break; out(L"  "+g.name+L"  ["+HumanSize(g.sizeOnDisk)+L"]  "+(g.moved?L"MOVED":L"")+L"  "+g.folderPath); }
    out(L"SELFTEST_OK");
    if(f) fclose(f);
    return 0;
}

// ---------- headless: render PNG ----------
static int GpEncoder(const WCHAR* fmt, CLSID* clsid){
    using namespace Gdiplus;
    UINT num=0,size=0; GetImageEncodersSize(&num,&size); if(!size) return -1;
    std::vector<char> buf(size); auto* info=(ImageCodecInfo*)buf.data();
    GetImageEncoders(num,size,info);
    for(UINT i=0;i<num;i++){ if(wcscmp(info[i].MimeType,fmt)==0){ *clsid=info[i].Clsid; return i; } }
    return -1;
}
static void TextL(HDC dc,int x,int y,const wchar_t* s,HFONT f,COLORREF col){
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,col);
    HGDIOBJ of=SelectObject(dc,f); TextOutW(dc,x,y,s,(int)wcslen(s)); SelectObject(dc,of);
}
// Faithful in-memory paint of the whole interface (reliable headless, no screen needed).
static void PaintUI(HDC dc,int W,int H,int movePct){
    struct Row{ const wchar_t* name; const wchar_t* drive; uint64_t size; bool moved; const wchar_t* to; };
    Row rows[]={
        {L"Counter-Strike 2",   L"C:", 32212254720ull,  false, L""},
        {L"Cyberpunk 2077",     L"C:", 76235112448ull,  false, L""},
        {L"Dota 2",             L"D:", 38654705664ull,  true,  L"D:"},
        {L"Elden Ring",         L"C:", 64424509440ull,  false, L""},
        {L"Baldur's Gate 3",    L"C:", 150323855360ull, false, L""},
    };
    int sel = movePct>=0 ? 1 : 1; // Cyberpunk selected

    HBRUSH bg=CreateSolidBrush(C_BG); RECT all={0,0,W,H}; FillRect(dc,&all,bg); DeleteObject(bg);
    // header
    RECT hb={0,0,W,70}; HBRUSH hbr=CreateSolidBrush(C_PANEL); FillRect(dc,&hb,hbr); DeleteObject(hbr);
    RECT ln={0,70,W,71}; HBRUSH lb=CreateSolidBrush(C_LINE); FillRect(dc,&ln,lb); DeleteObject(lb);
    RoundFill(dc,16,18,52,54,8,C_ACC,false,0);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
    { RECT cr={16,16,52,56}; HGDIOBJ of=SelectObject(dc,g_fTitle); DrawTextW(dc,L"\x00BB",-1,&cr,DT_CENTER|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,of); }
    TextL(dc,64,14,L"Steam Game Mover",g_fTitle,C_LIGHT);
    TextL(dc,64,42,L"Move a game to another drive \x2014 Steam keeps working, no re-download.",g_fSub,C_MUTED);

    // list card
    int lx=16, ly=82, lw=W-32, lh=(H-128)-ly;
    RoundFill(dc,lx,ly,lx+lw,ly+lh,8,C_CARD,true,C_LINE);
    int cGame=lx+18, cDrive=lx+388, cSize=lx+480, cStat=lx+610;
    // column header
    RECT hr={lx+1,ly+1,lx+lw-1,ly+31}; HBRUSH hhr=CreateSolidBrush(C_PANEL); FillRect(dc,&hr,hhr); DeleteObject(hhr);
    TextL(dc,cGame, ly+8,L"GAME", g_fSub,C_MUTED);
    TextL(dc,cDrive,ly+8,L"DRIVE",g_fSub,C_MUTED);
    TextL(dc,cSize, ly+8,L"SIZE", g_fSub,C_MUTED);
    TextL(dc,cStat, ly+8,L"STATUS",g_fSub,C_MUTED);
    RECT sl={lx+1,ly+31,lx+lw-1,ly+32}; HBRUSH slb=CreateSolidBrush(C_LINE); FillRect(dc,&sl,slb); DeleteObject(slb);
    // rows
    int rowTop=ly+32, rowH=44;
    for(int i=0;i<5;i++){
        int ry=rowTop+i*rowH;
        if(i==sel){ RECT rr={lx+1,ry,lx+lw-1,ry+rowH}; HBRUSH b=CreateSolidBrush(RGB(30,41,69)); FillRect(dc,&rr,b); DeleteObject(b);
            RECT ab={lx+1,ry,lx+4,ry+rowH}; HBRUSH a=CreateSolidBrush(C_ACC); FillRect(dc,&ab,a); DeleteObject(a); }
        int ty=ry+rowH/2-11;
        TextL(dc,cGame, ty,rows[i].name, g_fUi,C_LIGHT);
        TextL(dc,cDrive,ty,rows[i].drive,g_fUi,C_MUTED);
        TextL(dc,cSize, ty,HumanSize(rows[i].size).c_str(),g_fUi,C_LIGHT);
        if(rows[i].moved){ std::wstring s=std::wstring(L"Moved \x2192 ")+rows[i].to; TextL(dc,cStat,ty,s.c_str(),g_fUi,C_ACC2); }
        else TextL(dc,cStat,ty,(std::wstring(L"On ")+rows[i].drive).c_str(),g_fUi,C_MUTED);
        if(i<4){ RECT rl={cGame,ry+rowH-1,lx+lw-14,ry+rowH}; HBRUSH rb=CreateSolidBrush(RGB(32,36,45)); FillRect(dc,&rl,rb); DeleteObject(rb); }
    }

    // bottom panel
    int pt=H-128;
    TextL(dc,16,pt+14,L"Move to:",g_fUi,C_MUTED);
    RoundFill(dc,92,pt+8,W-146,pt+38,6,C_CARD,true,C_LINE);
    TextL(dc,104,pt+13,L"D:\\Games",g_fUi,C_LIGHT);
    RoundFill(dc,W-130,pt+8,W-16,pt+38,8,C_PANEL,true,C_LINE);
    { RECT b={W-130,pt+8,W-16,pt+38}; SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_LIGHT); HGDIOBJ of=SelectObject(dc,g_fBtn); DrawTextW(dc,L"Browse\x2026",-1,&b,DT_CENTER|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,of); }
    // action buttons
    auto btn=[&](int x,int w,const wchar_t* t,bool acc){
        RoundFill(dc,x,pt+52,x+w,pt+90,10, acc?C_ACC:C_PANEL, !acc, C_LINE);
        RECT b={x,pt+52,x+w,pt+90}; SetBkMode(dc,TRANSPARENT); SetTextColor(dc, acc?RGB(255,255,255):C_LIGHT);
        HGDIOBJ of=SelectObject(dc,g_fBtn); DrawTextW(dc,t,-1,&b,DT_CENTER|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,of);
    };
    btn(16,230,L"Move selected game",true);
    btn(256,150,L"Undo move",false);
    btn(416,120,L"Refresh",false);
    // status
    if(movePct>=0) TextL(dc,560,pt+62,L"Copying Cyberpunk 2077\x2026",g_fUi,C_LIGHT);
    else           TextL(dc,560,pt+62,L"5 games found.",g_fUi,C_MUTED);
    // progress
    int px=16,pw=W-32,py=pt+102,ph=12;
    RoundFill(dc,px,py,px+pw,py+ph,6,C_CARD,true,C_LINE);
    int pct = movePct>=0?movePct:0;
    if(pct>0){ int fw=(pw-4)*pct/100; RoundFill(dc,px+2,py+2,px+2+fw,py+ph-2,5,C_ACC,false,0); }
}

static int RenderPng(const std::wstring& path, int movePct){
    using namespace Gdiplus;
    GdiplusStartupInput gi; ULONG_PTR tok; GdiplusStartup(&tok,&gi,NULL);
    EnsureFonts();
    int W=984,H=624;
    HDC screen=GetDC(NULL); HDC mem=CreateCompatibleDC(screen);
    HBITMAP bmp=CreateCompatibleBitmap(screen,W,H); HGDIOBJ ob=SelectObject(mem,bmp);
    SetGraphicsMode(mem,GM_ADVANCED);
    PaintUI(mem,W,H,movePct);
    Bitmap* b=Bitmap::FromHBITMAP(bmp,NULL);
    CLSID png; GpEncoder(L"image/png",&png);
    b->Save(path.c_str(),&png,NULL);
    delete b;
    SelectObject(mem,ob); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL,screen);
    GdiplusShutdown(tok);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int nShow){
    g_hInst=hInst;
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES|ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    g_brBg=CreateSolidBrush(C_BG); g_brPanel=CreateSolidBrush(C_CARD);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=L"SGMWindow";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=g_brBg;
    wc.hIcon=LoadIconW(hInst,MAKEINTRESOURCEW(1)); wc.hIconSm=wc.hIcon;
    RegisterClassExW(&wc);

    int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i<argc;i++){
        std::wstring a=argv[i]; for(auto&c:a)c=towlower(c);
        if(a==L"--selftest") return SelfTest();
        if(a==L"--render"){ std::wstring out=(i+1<argc)?argv[i+1]:L"ui.png"; int pct=-1; for(int j=i+1;j<argc;j++){ std::wstring x=argv[j]; for(auto&c:x)c=towlower(c); if(x==L"moving")pct=45;} return RenderPng(out,pct); }
    }

    EnsureInstalled();
    HWND hwnd=CreateWindowExW(0,L"SGMWindow",L"Steam Game Mover",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1000,680,NULL,NULL,hInst,NULL);
    RefreshList();
    ShowWindow(hwnd,nShow); UpdateWindow(hwnd);
    MSG m; while(GetMessageW(&m,NULL,0,0)){ TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
