// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "filesys.h"

FileType GetFileType(const WCHAR* p)
{
    WIN32_FIND_DATA fd;
    SHFind h = FindFirstFile(p, &fd);
    if (h.Empty())
        return FileType::Invalid;
    if (fd.dwFileAttributes == DWORD(-1))
        return FileType::Invalid;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)
        return FileType::Device;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return FileType::Dir;
    return FileType::File;
}

static class delay_load_shell32
{
public:
                        delay_load_shell32();
    bool                init();
    int                 SHFileOperationW(LPSHFILEOPSTRUCTW pshfileop);
private:
    bool                m_initialized = false;
    bool                m_ok = false;
    union
    {
        FARPROC         proc[1];
        struct {
            int (STDAPICALLTYPE* SHFileOperationW)(LPSHFILEOPSTRUCTW pshfileop);
        };
    } m_procs;
} s_shell32;

delay_load_shell32::delay_load_shell32()
{
    ZeroMemory(&m_procs, sizeof(m_procs));
}

bool delay_load_shell32::init()
{
    if (!m_initialized)
    {
        m_initialized = true;
        HMODULE hlib = LoadLibrary(L"shell32.dll");
        if (hlib)
        {
            size_t c = 0;
            m_procs.proc[c++] = GetProcAddress(hlib, "SHFileOperationW");
            assert(_countof(m_procs.proc) == c);
        }

        m_ok = true;
        static_assert(sizeof(m_procs.proc) == sizeof(m_procs), "proc[] dimension is too small");
        for (auto const& proc : m_procs.proc)
        {
            if (!proc)
            {
                m_ok = false;
                break;
            }
        }
    }

assert(m_ok);
    return m_ok;
}

int delay_load_shell32::SHFileOperationW(LPSHFILEOPSTRUCTW pshfileop)
{
    if (!init())
        return ERROR_INVALID_FUNCTION;
    return m_procs.SHFileOperationW(pshfileop);
}

int Recycle(const std::vector<StrW>& names, Error& e)
{
    if (names.empty() || !s_shell32.init())
        return -1;

    SHFILEOPSTRUCTW shfileop = { 0 };

    // pFrom needs double null termination.
    size_t alloc_len = 1;
    for (auto& name : names)
        alloc_len += name.Length() + 1;
    WCHAR* buffer = new WCHAR[alloc_len];
    if (!buffer)
    {
        e.Sys(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    WCHAR* p = buffer;
    for (auto& name : names)
    {
        const size_t name_len = name.Length() + 1;
        memcpy(p, name.Text(), name_len * sizeof(*name.Text()));
        p += name_len;
    }
    *(p++) = 0;
    assert(p - buffer == alloc_len);

    // FOF_NO_CONNECTED_ELEMENTS is documented by MSDN to be only available
    // starting with Version 5.0 of shell32.dll.  Platforms without version
    // 5.0 are WinNT, Win95, and Win98.
    shfileop.pFrom = buffer;
    shfileop.wFunc = FO_DELETE;
    shfileop.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS;

    // The undocumented return values of SHFileOperation() usually map to
    // Win32 errors but not always.
    const int err = s_shell32.SHFileOperationW(&shfileop);
    delete [] buffer;

    if (err && err != ERROR_FILE_NOT_FOUND)
    {
        StrW s;
        s.Printf(L"Error 0x%08.8x recycling ", err);
        if (names.size() == 1)
            s.Printf(L"'%s'", names[0].Text());
        else
            s.Append(L"the items.");
        e.Set(s.Text());
        return 0;
    }

    return 1;
}
