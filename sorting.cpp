// Copyright (c) 2024 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "pch.h"
#include "sorting.h"

static DWORD s_dwCmpStrFlags = SORT_DIGITSASNUMBERS;

// Longest possible sort order string is "-d-e-g-n-s".
static const WCHAR s_sort_order[16] = L"g";
static const bool s_explicit_extension = false;

static LONG ParseNum(const WCHAR*& p)
{
    LONG num = 0;

    while (*p <= '9' && *p >= '0')
    {
        num *= 10;
        num += (*p) - '0';
        p++;
    }

    return num;
}

int Sorting::CmpStrN(const WCHAR* p1, int len1, const WCHAR* p2, int len2)
{
    const int n = CompareStringW(LOCALE_USER_DEFAULT, s_dwCmpStrFlags, p1, len1, p2, len2);
    if (!n)
    {
        assert(false);
        return 0;
    }
    return n - 2;
}

int Sorting::CmpStrNI(const WCHAR* p1, int len1, const WCHAR* p2, int len2)
{
    const int n = CompareStringW(LOCALE_USER_DEFAULT, s_dwCmpStrFlags|NORM_IGNORECASE, p1, len1, p2, len2);
    if (!n)
    {
        assert(false);
        return 0;
    }
    return n - 2;
}

class NameSplitter
{
public:
    NameSplitter(const WCHAR* p, unsigned index) : m_p(const_cast<WCHAR*>(p) + index), m_ch(p[index]) { *m_p = 0; }
    ~NameSplitter() { *m_p = m_ch; }

private:
    WCHAR* const    m_p;
    const WCHAR     m_ch;
};

bool CmpFileInfo(const FileInfo& fi1, const FileInfo& fi2)
{
    const FileInfo* const pfi1 = &fi1;
    const FileInfo* const pfi2 = &fi2;
    const bool is_file1 = !(pfi1->GetAttributes() & FILE_ATTRIBUTE_DIRECTORY);
    const bool is_file2 = !(pfi2->GetAttributes() & FILE_ATTRIBUTE_DIRECTORY);

    const WCHAR* const name1 = pfi1->GetName().Text();
    const WCHAR* const name2 = pfi2->GetName().Text();
    const WCHAR* const _ext1 = FindExtension(name1);
    const WCHAR* const _ext2 = FindExtension(name2);
    const unsigned name_len1 = _ext1 ? unsigned(_ext1 - name1) : unsigned(pfi1->GetName().Length());
    const unsigned name_len2 = _ext2 ? unsigned(_ext2 - name2) : unsigned(pfi2->GetName().Length());
    const WCHAR* const ext1 = _ext1 ? _ext1 : L"";
    const WCHAR* const ext2 = _ext2 ? _ext2 : L"";

    int n = 0;
    for (const WCHAR* order = s_sort_order; !n && *order; order++)
    {
        bool reverse = (*order == '-');

        if (reverse)
        {
            order++;
            if (!*order)
                break;
        }

        switch (*order)
        {
        case 'g':
            if (is_file1 != is_file2)
                n = is_file1 ? 1 : -1;
            break;
        case 'n':
            if (s_explicit_extension)
            {
                NameSplitter split1(name1, name_len1);
                NameSplitter split2(name2, name_len2);
                n = Sorting::CmpStrI(name1, name2);
            }
            else
            {
                n = Sorting::CmpStrI(name1, name2);
            }
            break;
        case 'e':
            n = Sorting::CmpStrI(ext1, ext2);
            break;
        case 's':
            {
                const auto cb1 = pfi1->GetSize();
                const auto cb2 = pfi2->GetSize();
                if (cb1 < cb2)
                    n = -1;
                else if (cb1 > cb2)
                    n = 1;
            }
            break;
        case 'd':
            {
                const FILETIME* const pft1 = &pfi1->GetModifiedTime();
                const FILETIME* const pft2 = &pfi2->GetModifiedTime();
                n = CompareFileTime(pft1, pft2);
            }
            break;
        }

        if (reverse)
            n = -n;
    }

    return n < 0;
}

