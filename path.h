// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include "str.h"

template<class T, class Base>
class Path : public Base
{
public:
                        Path() = default;
                        Path(const WCHAR* p) { Set(p); }
                        Path(const Base& s) { Set(s); }
                        Path(Path<T,Base>&& s) { Set(std::move(s)); }

    void                SetMaybeRooted(const T* root, const T* component);
    void                JoinComponent(const T* component);
    bool                AppendComponent(const T* component);
    bool                AppendComponent(const Base& component);
    bool                ToParent(Base* file=nullptr);

    void                EnsureTrailingSlash();
    void                MaybeStripTrailingSlash();  // Doesn't climb past a root.
    void                StripTrailingSlash();       // Unconditionally strips trailing slash.

protected:
    static inline bool  IsSlash(T ch) { return ch == '/' || ch == '\\'; }
    static bool         IsUnder(const T*& path, size_t& len, const T* under);
    static const T*     FindEndOfRoot(const T* start);
};

template<class T, class Base>
bool Path<T,Base>::IsUnder(const T*& path, size_t& len, const T* under)
{
    assert(under && *under );   // Guarantees under[-1] won't underrun.

    const T* walk = path;
    while (*walk && (::ToLower(*walk) == ::ToLower(*under) ||
                     IsSlash(*walk) && IsSlash(*under)))
        ++walk, ++under;
    if (*under || !IsSlash(under[-1]) && *walk && !IsSlash(*walk++))
        return false;

    // Move past initial substring 'under'.
    const size_t moved = (walk - path);
    len -= moved;
    path += moved;
    return true;
}

template<class T, class Base>
const T* Path<T,Base>::FindEndOfRoot(const T* start)
{
    // Don't climb above a drive spec (C:) or root (\ or C:\).
    // Don't climb above a UNC root (\\server\share).

// TODO: \\?\etc syntax...

    if (start[0] && start[1] == ':')
    {
        start += 2;
        if (IsSlash(start[0]))
            ++start;
    }
    else if (start[0] == '\\' && start[1] == '\\')
    {
        start += 2;
        while (*start && !IsSlash(*start))
            start++;
        while (*start && IsSlash(*start))
            start++;
        while (*start && !IsSlash(*start))
            start++;
    }

    return start;
}

template<class T, class Base>
void Path<T,Base>::SetMaybeRooted(const T* root, const T* component)
{
    if (!component || !*component)
    {
        Set(root);
        return;
    }
    else if (!root || !*root)
    {
        Set(component);
        return;
    }

    // The goal here is to sensibly append 'root' and 'component' to form a
    // path.  The paths must be in Windows NT path syntax.

    size_t len = StrLen(component);

    Base _root;         // In case 'root' is from 'this'.
    _root.Set(root);
    root = _root.Text();

    Clear();

    // Use device spec from 'component' if given.
    // If 'component' is UNC, don't adjust it.
    // Otherwise use device spec from 'root' if given.
    // Strip the device spec from 'component', if used.

    if (len >= 2 && component[1] == ':')
    {
        Set(component, 2);
        component += 2;
        len -= 2;
    }
    else if (len >= 2 && component[0] == '\\' && component[1] == '\\' )
    {
        // Local is UNC.  Don't use root's drive.
    }
    else if( root[0] && root[1] == ':' )
    {
        Set( root, 2 );
        root += 2;
    }

    // If 'component' is an absolute path (after the device spec has been
    // stripped and used), then use only 'component'.
    if (len && IsSlash(component[0]))
    {
        Append(component, len);
        return;
    }

    const size_t spec_len = Length();
    Append(root);

    // Consume '..' and '.' relative path components, manipulating the root as
    // appropriate.
    const T dotdot[3] = { '.', '.' };
    const T dot[2] = { '.' };
    for (;;)
    {
        if (IsUnder(component, len, dotdot)) ToParent();
        else if (!IsUnder(component, len, dot)) break;
    }

    // Ensure 'component' (if any) is separated from 'root' by a backslash.
    if (Length() <= spec_len || (len && (!IsSlash(Text()[Length() - 1]))))
        Append('\\');

    Append(component, len);
}

template<class T, class Base>
void Path<T,Base>::JoinComponent(const T* component)
{
    SetMaybeRooted(Text(), component);
}

template<class T, class Base>
bool Path<T,Base>::AppendComponent(const T* component)
{
    Assert(component);
    Assert(*component);
    Assert(!IsSlash(component[0]));
    Assert(component[1] != ':');

    if (!Length())
        return false;

    EnsureTrailingSlash();
    Append(component);

    return true;
}

template<class T, class Base>
bool Path<T,Base>::AppendComponent(const Base& component)
{
    Assert(component.Length());
    Assert(!IsSlash(component.Text()[0]));
    Assert(component.Text()[1] != ':');

    if (!Length())
        return false;

    EnsureTrailingSlash();
    Append(component);

    return true;
}

template<class T, class Base>
void Path<T,Base>::EnsureTrailingSlash()
{
    if (Length() && !IsSlash(Text()[Length() - 1]))
        Append('\\');
}

template<class T, class Base>
void Path<T,Base>::MaybeStripTrailingSlash()
{
    const T* start = FindEndOfRoot(Text());
    const size_t min_len = start - Text();

    while (Length() > min_len && IsSlash(Text()[Length() - 1]))
        SetLength(Length() - 1);
}

template<class T, class Base>
void Path<T,Base>::StripTrailingSlash()
{
    while (Length() && IsSlash(Text()[Length() - 1]))
        SetLength(Length() - 1);
}

template<class T, class Base>
bool Path<T,Base>::ToParent(Base* file)
{
    const T* start = Text();
    const T* end = start + Length();
    const T* const old_end = end;

    // Don't allow climbing above a root (C: or C:\ or \ or \\server\share).
    start = FindEndOfRoot(start);

    // Consume last path component.
    if (end > start && IsSlash(end[-1]))
        --end;
    while (end > start && !IsSlash(end[-1]))
        --end;

    // Put last path component in 'file'.
    if (file)
        file->Set(end, old_end - end);

    // Consume trailing slash (unless protected).
    if (end > start && IsSlash(end[-1]))
        --end;

    SetEnd(end);
    return end != old_end;
}

typedef Path<char,StrA> PathA;
typedef Path<WCHAR,StrW> PathW;

#ifdef UNICODE
typedef PathW PathT;
#else
typedef PathA PathT;
#endif

