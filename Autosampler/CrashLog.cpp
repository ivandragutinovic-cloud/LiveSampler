// Zapis pada programa (Windows): kod greske, adresa i stek (modul + RVA) u
// %APPDATA%\LiveSampler\autosampler-crash.txt. Radi i kad JUCE crash handler ne stigne
// (abort / std::terminate / pure virtual / invalid parameter / stack overflow).
// RVA se prevodi u funkciju pomocu .pdb fajla iz istog CI builda.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#pragma comment(lib, "dbghelp.lib")

namespace
{
    wchar_t logPath[MAX_PATH] = {};
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
    volatile LONG written = 0;

    void writeText (const char* s)
    {
        HANDLE h = CreateFileW (logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile (h, s, (DWORD) lstrlenA (s), &w, nullptr);
        CloseHandle (h);
    }

    void writeFrame (int i, DWORD64 addr)
    {
        char line[600];
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        DWORD64 rva = addr;
        if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) addr, &mod) && mod != nullptr)
        {
            GetModuleFileNameA (mod, name, MAX_PATH);
            rva = addr - (DWORD64) mod;
        }
        const char* base = name;
        for (const char* p = name; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
        _snprintf_s (line, sizeof (line), _TRUNCATE, "  %2d  %s + 0x%llx\r\n", i, base, (unsigned long long) rva);
        writeText (line);
    }

    void header (const char* what)
    {
        char line[300];
        SYSTEMTIME t; GetLocalTime (&t);
        _snprintf_s (line, sizeof (line), _TRUNCATE, "\r\n==== %04d-%02d-%02d %02d:%02d:%02d  %s  (thread %lu)\r\n",
                     t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, what, GetCurrentThreadId());
        writeText (line);
    }

    void captureHere (const char* what)
    {
        header (what);
        void* frames[48] = {};
        const USHORT n = RtlCaptureStackBackTrace (1, 48, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) writeFrame (i, (DWORD64) frames[i]);
    }

    LONG WINAPI filter (EXCEPTION_POINTERS* ep)
    {
        if (InterlockedExchange (&written, 1) == 0 && ep != nullptr && ep->ExceptionRecord != nullptr)
        {
            char line[300];
            header ("EXCEPTION");
            const auto* er = ep->ExceptionRecord;
            _snprintf_s (line, sizeof (line), _TRUNCATE, "  code 0x%08lx  flags %lu  params %llx %llx\r\n", er->ExceptionCode, er->ExceptionFlags,
                         (unsigned long long) (er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0),
                         (unsigned long long) (er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0));
            writeText (line);
            writeText ("  at:\r\n");
            writeFrame (0, (DWORD64) er->ExceptionAddress);
            // stek iz konteksta izuzetka
            CONTEXT ctx = *ep->ContextRecord;
            STACKFRAME64 sf = {};
            sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
            HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
            SymInitialize (proc, nullptr, TRUE);
            writeText ("  stack:\r\n");
            for (int i = 0; i < 48; ++i)
            {
                if (! StackWalk64 (IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                if (sf.AddrPC.Offset == 0) break;
                writeFrame (i, sf.AddrPC.Offset);
            }
        }
        return previousFilter != nullptr ? previousFilter (ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    void onSignal (int sig)
    {
        captureHere (sig == SIGABRT ? "abort()" : "signal");
        std::signal (sig, SIG_DFL);
        std::raise (sig);
    }
    void onTerminate()
    {
        const char* what = "std::terminate";
        try { if (auto e = std::current_exception()) std::rethrow_exception (e); }
        catch (const std::exception& e) { captureHere (what); writeText ("  exception: "); writeText (e.what()); writeText ("\r\n"); std::abort(); }
        catch (...) { captureHere ("std::terminate (unknown exception)"); std::abort(); }
        captureHere (what);
        std::abort();
    }
    void onPurecall() { captureHere ("pure virtual call"); std::abort(); }
    void onInvalidParameter (const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) { captureHere ("invalid CRT parameter"); std::abort(); }
}

void installCrashLog (const wchar_t* path)
{
    lstrcpynW (logPath, path, MAX_PATH);
    previousFilter = SetUnhandledExceptionFilter (filter);
    std::signal (SIGABRT, onSignal);
    std::set_terminate (onTerminate);
    _set_purecall_handler (onPurecall);
    _set_invalid_parameter_handler (onInvalidParameter);
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee (&guarantee);    // da zapis radi i kad se prepuni stek
}

void crashLogThreadStart()
{
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee (&guarantee);
}

void crashLogNote (const char* text)
{
    header ("note");
    writeText ("  "); writeText (text); writeText ("\r\n");
}
#else
void installCrashLog (const wchar_t*) {}
void crashLogThreadStart() {}
void crashLogNote (const char*) {}
#endif
