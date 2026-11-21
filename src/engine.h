// Steam Game Mover — engine: Steam discovery + safe move (copy/verify/delete/junction/rollback).
// Junctions via DeviceIoControl — NO admin required. Proven by derisk (7/7).
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cwctype>

#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT 0x000900A4
#endif
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x000900A8
#endif
#define SGM_TAG_MOUNT_POINT 0xA0000003
#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE 16384
#endif

typedef struct _SGM_REPARSE_DATA_BUFFER {
    DWORD  ReparseTag;
    WORD   ReparseDataLength;
    WORD   Reserved;
    WORD   SubstituteNameOffset;
    WORD   SubstituteNameLength;
    WORD   PrintNameOffset;
    WORD   PrintNameLength;
    WCHAR  PathBuffer[1];
} SGM_REPARSE_DATA_BUFFER;

namespace sgm {

struct Game {
    std::wstring name;        // display name
    std::wstring installDir;  // folder leaf under steamapps/common
    std::wstring folderPath;  // full path to the game folder
    std::wstring library;     // library root
    uint64_t     sizeOnDisk = 0;
    bool         moved = false; // folder is already a junction
    unsigned     appid = 0;
};

struct Progress {
    uint64_t total = 0, done = 0;
    uint64_t fileBase = 0;
    std::function<void(int)> cb;   // percent 0..100
    volatile bool* cancel = nullptr;
};

// ---------- small helpers ----------
inline std::wstring BaseName(const std::wstring& p){
    size_t s = p.find_last_of(L"\\/");
    return s==std::wstring::npos ? p : p.substr(s+1);
}
inline std::wstring Utf8ToW(const std::string& s){
    if(s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),NULL,0);
    std::wstring w(n,0);
    MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],n);
    return w;
}
inline bool IsReparse(const std::wstring& p){
    DWORD a = GetFileAttributesW(p.c_str());
    return a!=INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
inline bool PathExists(const std::wstring& p){
    return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;
}

// ---------- tree ops ----------
inline void TreeStats(const std::wstring& p, uint64_t& files, uint64_t& bytes){
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p+L"\\*").c_str(), &fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        std::wstring n=fd.cFileName;
        if(n==L"."||n==L"..") continue;
        std::wstring c=p+L"\\"+n;
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) TreeStats(c,files,bytes);
        else { files++; bytes += ((uint64_t)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; }
    } while(FindNextFileW(h,&fd));
    FindClose(h);
}

inline DWORD CALLBACK CopyCb(LARGE_INTEGER, LARGE_INTEGER bytesXfer, LARGE_INTEGER,
    LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data){
    Progress* pr = (Progress*)data;
    if(pr->cancel && *pr->cancel) return PROGRESS_CANCEL;
    uint64_t g = pr->fileBase + (uint64_t)bytesXfer.QuadPart;
    if(pr->total && pr->cb) pr->cb((int)(g*100/pr->total));
    return PROGRESS_CONTINUE;
}

inline bool CopyTree(const std::wstring& src, const std::wstring& dst, Progress* pr){
    if(!CreateDirectoryW(dst.c_str(), NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src+L"\\*").c_str(), &fd);
    if(h==INVALID_HANDLE_VALUE) return true;
    bool ok=true;
    do{
        std::wstring n=fd.cFileName;
        if(n==L"."||n==L"..") continue;
        std::wstring s=src+L"\\"+n, d=dst+L"\\"+n;
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if(!CopyTree(s,d,pr)){ ok=false; break; }
        } else {
            uint64_t fsz = ((uint64_t)fd.nFileSizeHigh<<32)|fd.nFileSizeLow;
            if(pr) pr->fileBase = pr->done;
            BOOL cancelFlag = FALSE;
            if(!CopyFileExW(s.c_str(), d.c_str(), pr?CopyCb:NULL, pr, &cancelFlag, 0)){ ok=false; break; }
            if(pr){ pr->done += fsz; if(pr->cb && pr->total) pr->cb((int)(pr->done*100/pr->total)); }
        }
    } while(FindNextFileW(h,&fd));
    FindClose(h);
    return ok;
}

inline bool DeleteTree(const std::wstring& p){
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p+L"\\*").c_str(), &fd);
    if(h!=INVALID_HANDLE_VALUE){
        do{
            std::wstring n=fd.cFileName;
            if(n==L"."||n==L"..") continue;
            std::wstring c=p+L"\\"+n;
            if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
                if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(c.c_str());
                else DeleteTree(c);
            } else {
                SetFileAttributesW(c.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(c.c_str());
            }
        } while(FindNextFileW(h,&fd));
        FindClose(h);
    }
    return RemoveDirectoryW(p.c_str())!=0;
}

// ---------- junction ----------
inline bool CreateJunction(const std::wstring& link, const std::wstring& target){
    if(!CreateDirectoryW(link.c_str(), NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if(h==INVALID_HANDLE_VALUE) return false;
    std::wstring sub = L"\\??\\" + target, prn = target;
    size_t subB=sub.size()*2, prnB=prn.size()*2;
    size_t pathB = subB+2 + prnB+2;
    size_t hdr = offsetof(SGM_REPARSE_DATA_BUFFER, PathBuffer);
    size_t total = hdr + pathB;
    std::vector<char> buf(total,0);
    auto* rb=(SGM_REPARSE_DATA_BUFFER*)buf.data();
    rb->ReparseTag = SGM_TAG_MOUNT_POINT;
    rb->SubstituteNameOffset = 0;
    rb->SubstituteNameLength = (WORD)subB;
    rb->PrintNameOffset = (WORD)(subB+2);
    rb->PrintNameLength = (WORD)prnB;
    char* pb=(char*)rb->PathBuffer;
    memcpy(pb+rb->SubstituteNameOffset, sub.c_str(), subB+2);
    memcpy(pb+rb->PrintNameOffset, prn.c_str(), prnB+2);
    rb->ReparseDataLength = (WORD)(hdr-8 + pathB);
    DWORD ret=0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, rb, (DWORD)total, NULL,0,&ret,NULL);
    CloseHandle(h);
    if(!ok){ RemoveDirectoryW(link.c_str()); return false; }
    return true;
}

inline std::wstring ReadJunction(const std::wstring& link){
    HANDLE h = CreateFileW(link.c_str(), 0,0,NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if(h==INVALID_HANDLE_VALUE) return L"";
    std::vector<char> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD ret=0;
    BOOL ok=DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL,0, buf.data(),(DWORD)buf.size(),&ret,NULL);
    CloseHandle(h);
    if(!ok) return L"";
    auto* rb=(SGM_REPARSE_DATA_BUFFER*)buf.data();
    if(rb->ReparseTag!=SGM_TAG_MOUNT_POINT) return L"";
    char* pb=(char*)rb->PathBuffer;
    return std::wstring((WCHAR*)(pb+rb->PrintNameOffset), rb->PrintNameLength/2);
}

// ---------- move / undo ----------
// Returns empty string on success; otherwise an error message.
inline std::wstring MoveGame(const std::wstring& srcDir, const std::wstring& destParent, Progress* pr){
    std::wstring destDir = destParent + L"\\" + BaseName(srcDir);
    DWORD sa = GetFileAttributesW(srcDir.c_str());
    if(sa==INVALID_FILE_ATTRIBUTES) return L"Source folder not found.";
    if(sa & FILE_ATTRIBUTE_REPARSE_POINT) return L"This game is already moved (it is a junction).";
    if(PathExists(destDir)) return L"A folder with the same name already exists at the destination.";

    // free space check
    uint64_t need=0, nf=0; TreeStats(srcDir, nf, need);
    ULARGE_INTEGER freeB{};
    if(GetDiskFreeSpaceExW(destParent.substr(0,3).c_str(), &freeB, NULL, NULL)){
        if(freeB.QuadPart < need + (64ull<<20)) return L"Not enough free space on the destination drive.";
    }
    if(pr){ pr->total = need?need:1; pr->done=0; }

    SHCreateDirectoryExW(NULL, destParent.c_str(), NULL);
    if(!CopyTree(srcDir, destDir, pr)){ DeleteTree(destDir); return L"Copy failed (disk full, in use, or permission denied)."; }

    uint64_t sf=0,sb=0,df=0,db=0; TreeStats(srcDir,sf,sb); TreeStats(destDir,df,db);
    if(sf!=df || sb!=db){ DeleteTree(destDir); return L"Verification failed — the copy does not match. Nothing was changed."; }

    if(!DeleteTree(srcDir)){ DeleteTree(destDir); return L"Could not remove the original folder (is the game running?)."; }
    if(!CreateJunction(srcDir, destDir)){ CopyTree(destDir, srcDir, nullptr); DeleteTree(destDir); return L"Could not create the link. The game was restored to its place."; }
    return L"";
}

// Undo: folder is a junction we made -> copy back and remove the junction+moved copy.
inline std::wstring UndoMove(const std::wstring& folderPath, Progress* pr){
    if(!IsReparse(folderPath)) return L"This game is not moved.";
    std::wstring target = ReadJunction(folderPath);
    if(target.empty() || !PathExists(target)) return L"Cannot find the moved data.";
    uint64_t need=0,nf=0; TreeStats(target,nf,need);
    if(pr){ pr->total=need?need:1; pr->done=0; }
    if(!RemoveDirectoryW(folderPath.c_str())) return L"Could not remove the link.";
    if(!CopyTree(target, folderPath, pr)){ CreateJunction(folderPath, target); return L"Copy back failed. The link was restored."; }
    uint64_t sf=0,sb=0,df=0,db=0; TreeStats(target,sf,sb); TreeStats(folderPath,df,db);
    if(sf!=df||sb!=db){ DeleteTree(folderPath); CreateJunction(folderPath, target); return L"Verification failed on undo. The link was restored."; }
    DeleteTree(target);
    return L"";
}

// ---------- Steam discovery ----------
inline std::wstring GetSteamPath(){
    wchar_t buf[MAX_PATH]; DWORD sz=sizeof(buf);
    if(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
        RRF_RT_REG_SZ, NULL, buf, &sz)==ERROR_SUCCESS){
        std::wstring p=buf; for(auto&c:p) if(c==L'/') c=L'\\'; return p;
    }
    if(RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath",
        RRF_RT_REG_SZ, NULL, buf, &sz)==ERROR_SUCCESS){
        std::wstring p=buf; for(auto&c:p) if(c==L'/') c=L'\\'; return p;
    }
    return L"";
}

inline std::string ReadFileUtf8(const std::wstring& path){
    HANDLE h=CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(h==INVALID_HANDLE_VALUE) return "";
    std::string out; char b[8192]; DWORD r=0;
    while(ReadFile(h,b,sizeof(b),&r,NULL)&&r) out.append(b,r);
    CloseHandle(h); return out;
}

// extract the quoted value following "key" from a VDF/ACF text (first match from pos)
inline std::string VdfValue(const std::string& s, const std::string& key, size_t from=0){
    std::string k="\""+key+"\"";
    size_t p=s.find(k, from); if(p==std::string::npos) return "";
    p+=k.size();
    p=s.find('"',p); if(p==std::string::npos) return ""; p++;
    size_t e=s.find('"',p); if(e==std::string::npos) return "";
    return s.substr(p,e-p);
}

inline std::vector<std::wstring> GetLibraries(const std::wstring& steam){
    std::vector<std::wstring> libs;
    auto add=[&](std::wstring p){
        for(auto&c:p) if(c==L'/') c=L'\\';
        while(!p.empty() && (p.back()==L'\\')) p.pop_back();
        if(p.empty()) return;
        for(auto& e:libs){ std::wstring a=e,b=p; for(auto&c:a)c=towlower(c); for(auto&c:b)c=towlower(c); if(a==b) return; }
        libs.push_back(p);
    };
    if(!steam.empty()) add(steam);
    std::string vdf = ReadFileUtf8(steam+L"\\steamapps\\libraryfolders.vdf");
    if(vdf.empty()) vdf = ReadFileUtf8(steam+L"\\config\\libraryfolders.vdf");
    size_t pos=0;
    while(true){
        std::string k="\"path\"";
        size_t p=vdf.find(k,pos); if(p==std::string::npos) break;
        std::string v=VdfValue(vdf,"path",p);
        pos=p+k.size();
        // un-escape doubled backslashes
        std::string u; for(size_t i=0;i<v.size();++i){ if(v[i]=='\\'&&i+1<v.size()&&v[i+1]=='\\'){u+='\\';++i;} else u+=v[i]; }
        add(Utf8ToW(u));
    }
    return libs;
}

inline std::vector<Game> EnumGames(){
    std::vector<Game> games;
    std::wstring steam=GetSteamPath();
    if(steam.empty()) return games;
    for(const auto& lib : GetLibraries(steam)){
        std::wstring apps = lib+L"\\steamapps";
        WIN32_FIND_DATAW fd;
        HANDLE h=FindFirstFileW((apps+L"\\appmanifest_*.acf").c_str(), &fd);
        if(h==INVALID_HANDLE_VALUE) continue;
        do{
            std::string acf=ReadFileUtf8(apps+L"\\"+fd.cFileName);
            if(acf.empty()) continue;
            std::wstring installdir=Utf8ToW(VdfValue(acf,"installdir"));
            if(installdir.empty()) continue;
            Game g;
            g.installDir=installdir;
            g.name=Utf8ToW(VdfValue(acf,"name")); if(g.name.empty()) g.name=installdir;
            g.library=lib;
            g.folderPath=apps+L"\\common\\"+installdir;
            try{ g.sizeOnDisk=_strtoui64(VdfValue(acf,"SizeOnDisk").c_str(),NULL,10);}catch(...){}
            try{ g.appid=(unsigned)strtoul(VdfValue(acf,"appid").c_str(),NULL,10);}catch(...){}
            if(!PathExists(g.folderPath)) continue;
            g.moved=IsReparse(g.folderPath);
            games.push_back(g);
        } while(FindNextFileW(h,&fd));
        FindClose(h);
    }
    return games;
}

inline std::wstring HumanSize(uint64_t b){
    const wchar_t* u[]={L"B",L"KB",L"MB",L"GB",L"TB"};
    double v=(double)b; int i=0; while(v>=1024.0&&i<4){v/=1024.0;++i;}
    wchar_t out[64]; swprintf(out,64, (i==0)?L"%.0f %ls":L"%.1f %ls", v, u[i]); return out;
}

} // namespace sgm
