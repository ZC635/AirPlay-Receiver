#include "platform/GStreamerCacheProcess.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static bool put(const QString &path,const QByteArray &data) {
    QFile f(path); return f.open(QIODevice::WriteOnly|QIODevice::Truncate) && f.write(data)==data.size() && f.flush();
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const auto a=app.arguments(); if(a.size()<5) return 70;
    const QString mode=a[1],result=a[2],nonce=a[3],marker=a[4];
    if(mode=="owner") {
        QElapsedTimer clock;clock.start();CacheProcessRunner runner;
        CacheProcessRequest r{a[0],{"tree",result,nonce,marker},QProcessEnvironment::systemEnvironment(),result,nonce};
        if(!runner.start(r,60000,[&clock]{return clock.elapsed();})) return 71;
        return app.exec();
    }
    if(mode=="tree") {
        QString command='"'+a[0]+"\" hang \""+result+"\" \""+nonce+"\" \""+marker+".child\"";
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
        si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        if(!CreateProcessW(nullptr,reinterpret_cast<LPWSTR>(command.data()),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return 72;
        const bool saved=put(marker,QByteArray::number(GetCurrentProcessId())+'\n'+QByteArray::number(pi.dwProcessId));
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);if(!saved)return 73;
        return 0; // A descendant still owns the inherited pipe after its parent exits.
    }
    if(mode=="pipe-held-outside-job" && a.size()>5) {
        HANDLE target=OpenProcess(PROCESS_DUP_HANDLE,FALSE,a[5].toULong());
        if(!target)return 82;
        HANDLE remote=nullptr;
        const BOOL copied=DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_OUTPUT_HANDLE),target,&remote,0,FALSE,DUPLICATE_SAME_ACCESS);
        CloseHandle(target);if(!copied)return 83;
        if(!put(marker,QByteArray::number(GetCurrentProcessId())+'\n'+QByteArray::number(quintptr(remote))))return 84;
        Sleep(INFINITE);
    }
    if(mode=="hang") { if(!put(marker,QByteArray::number(GetCurrentProcessId())))return 74;Sleep(INFINITE); }
    if(mode=="crash") { TerminateProcess(GetCurrentProcess(),0xc0000005);return 75; }
    if(mode=="flood" || mode=="stderr-flood") {
        const QByteArray chunk(65536,'x');DWORD written;
        HANDLE stream=GetStdHandle(mode=="flood"?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
        for(int n=0;n<144;++n) if(!WriteFile(stream,chunk.constData(),DWORD(chunk.size()),&written,nullptr))return 76;
    }
    if(mode=="large-result") {if(!put(result,QByteArray(1048577,'x')))return 77;return 0;}
    if(mode=="large-result-hang") {
        if(!put(marker,QByteArray::number(GetCurrentProcessId())))return 81;
        Sleep(250);
        if(!put(result,QByteArray(1048577,'x')))return 80;
        Sleep(INFINITE);
    }
    if(mode=="inheritance" && a.size()>5) {
        const auto borrowed=reinterpret_cast<HANDLE>(quintptr(a[5].toULongLong()));
        if(SetEvent(borrowed)) return 79;
    }
    const QByteArray json="{\"nonce\":\""+nonce.toUtf8()+"\",\"ok\":true}";
    if(!put(result,json))return 78;
    DWORD written;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),json.constData(),DWORD(json.size()),&written,nullptr);
    return 0;
}
