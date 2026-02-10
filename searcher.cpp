// Copyright (c) 2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "searcher.h"
#include "input.h"
#include "output.h"
#include "ecma48.h"
#include "colors.h"
#include "contentcache.h"

#ifdef INCLUDE_RE2
#include "re2/re2.h"
#endif

#include <regex>
#include <memory>

static bool s_regex = false;    // Starts out false in every session.

class Searcher_Literal : public Searcher
{
public:
                    Searcher_Literal(const WCHAR* s, bool caseless, Error& e);
                    ~Searcher_Literal() = default;

    SearcherType    GetSearcherType() const override { return SearcherType::Literal; }
    unsigned        GetNeedleDelta() const override { return m_find.Length(); }

protected:
    bool            DoNext(FileLineMap& map, const BYTE* line, unsigned length, Error& e) override;

private:
    const bool      m_caseless;
    const StrW      m_find;
};

Searcher_Literal::Searcher_Literal(const WCHAR* s, bool caseless, Error& e)
: m_caseless(caseless)
, m_find(s)
{
}

bool Searcher_Literal::DoNext(FileLineMap& map, const BYTE* _line, unsigned _length, Error& e)
{
    // FUTURE:  Boyer-Moore?

    map.GetLineText(_line, _length, m_tmp);
    TrimLineEnding(m_tmp);

    const WCHAR* const line = m_tmp.Text();
    const unsigned length = m_tmp.Length();

    const WCHAR* const end = line + length - (m_find.Length() - 1);
    for (const WCHAR* p = line; p < end; ++p)
    {
        const int n = (m_caseless ?
                       _wcsnicmp(p, m_find.Text(), m_find.Length()) :
                       wcsncmp(p, m_find.Text(), m_find.Length()));
        if (n == 0)
        {
            SetMatch(unsigned(p - line), m_find.Length());
            return true;
        }
    }

    SetExhausted();
    return false;
}

#ifndef INCLUDE_RE2
class Searcher_ECMAScriptRegex : public Searcher
{
public:
                    Searcher_ECMAScriptRegex(const WCHAR* s, bool caseless, Error& e);
                    ~Searcher_ECMAScriptRegex() = default;

    SearcherType    GetSearcherType() const override { return SearcherType::Regex; }

protected:
    bool            DoNext(FileLineMap& map, const BYTE* line, unsigned length, Error& e) override;

private:
    std::regex_constants::syntax_option_type m_syntax;
    std::unique_ptr<std::wregex> m_wregex;
};

Searcher_ECMAScriptRegex::Searcher_ECMAScriptRegex(const WCHAR* s, bool caseless, Error& e)
{
    std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript;
    flags |= std::regex_constants::optimize;
    if (caseless)
        flags |= std::regex_constants::icase;

    try
    {
        std::unique_ptr<std::wregex> r = std::make_unique<std::wregex>(s, flags);
        m_wregex = std::move(r);
    }
    catch (std::regex_error ex)
    {
        StrW what;
        what.SetA(ex.what());
        e.Set(what.Text());
    }
}

bool Searcher_ECMAScriptRegex::DoNext(FileLineMap& map, const BYTE* _line, unsigned _length, Error& e)
{
    map.GetLineText(_line, _length, m_tmp);
    TrimLineEnding(m_tmp);

    const WCHAR* const line = m_tmp.Text();
    const unsigned length = m_tmp.Length();

    std::wcmatch matches;
    try
    {
        const auto flags = IsStarted() ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
        std::regex_search(line, line + length, matches, *m_wregex, flags);
    }
    catch (std::regex_error ex)
    {
        StrW what;
        what.SetA(ex.what());
        e.Set(what.Text());
        SetExhausted();
        return false;
    }

    if (!matches.size())
    {
        SetExhausted();
        return false;
    }

    SetMatch(unsigned(matches.position(0)), unsigned(matches.length(0)));
    return true;
}
#endif

#ifdef INCLUDE_RE2
class Searcher_RE2 : public Searcher
{
public:
                    Searcher_RE2(const WCHAR* s, bool caseless, Error& e);
                    ~Searcher_RE2() { delete m_re2; }

    SearcherType    GetSearcherType() const override { return SearcherType::Regex; }

protected:
    bool            DoNext(FileLineMap& map, const BYTE* line, unsigned length, Error& e) override;

private:
    RE2*            m_re2 = nullptr;
    StrW            m_tmp;
    StrUtf8         m_line;
};

Searcher_RE2::Searcher_RE2(const WCHAR* _s, bool caseless, Error& e)
{
    StrUtf8 s;
    s.SetW(_s);

    RE2::Options options;
    options.set_case_sensitive(!caseless);

    m_re2 = new RE2(s.Text(), options);

    if (!m_re2->ok())
    {
        StrW err;
        err.SetFromCodepage(CP_UTF8, m_re2->error().c_str());
        err.TrimRight();
        e.Set(err.Text());
    }
}

bool Searcher_RE2::DoNext(FileLineMap& map, const BYTE* _line, unsigned length, Error& e)
{
    if (!m_re2)
    {
exhausted:
        SetExhausted();
        return false;
    }

    const UINT cp = map.GetCodePage();
    const char* line;
    if (cp == CP_USASCII || cp == CP_UTF8)
    {
        // If the content is natively UTF8, then use it as-is.
        line = reinterpret_cast<const char*>(_line);
    }
    else
    {
        // Get the content as UTF16 and convert it to UTF8 for RE2.
        map.GetLineText(_line, length, m_tmp);
        m_line.SetW(m_tmp.Text());
        line = m_line.Text();
        length = m_line.Length();
    }

    absl::string_view match;
    absl::string_view sv(line, length);
    if (!m_re2->Match(sv, 0, length, RE2::UNANCHORED, &match, 1))
        goto exhausted;

    const size_t mpos = match.data() - sv.data();
    const size_t mlen = match.size();

    // Translate mpos and mlen from UTF8 to WCHAR.
    // TODO:  MB_ERR_INVALID_CHARS?
    const size_t mpos_begin = MultiByteToWideChar(cp, 0, line, unsigned(mpos), nullptr, 0);
    const size_t mpos_end = MultiByteToWideChar(cp, 0, line, unsigned(mpos + mlen), nullptr, 0);

    SetMatch(unsigned(mpos_begin), unsigned(mpos_end - mpos_begin));
    return true;
}
#endif

std::shared_ptr<Searcher> Searcher::Create(SearcherType type, const WCHAR* s, bool caseless, Error& e)
{
    std::shared_ptr<Searcher> searcher;

    switch (type)
    {
    default:
    case SearcherType::Literal:
        searcher = std::make_shared<Searcher_Literal>(s, caseless, e);
        break;
    case SearcherType::Regex:
#ifdef INCLUDE_RE2
        searcher = std::make_shared<Searcher_RE2>(s, caseless, e);
#else
        searcher = std::make_shared<Searcher_ECMAScriptRegex>(s, caseless, e);
#endif
        break;
    }

    if (e.Test())
        searcher.reset();
    return searcher;
}

bool Searcher::Match(FileLineMap& map, const BYTE* line, unsigned length, Error& e)
{
    m_started = false;
    m_exhausted = false;
    m_line = line;
    m_length = length;
    m_match_index = 0;
    m_match_length = 0;
    m_consumed = 0;
    return Next(map, e);
}

bool Searcher::Next(FileLineMap& map, Error& e)
{
    if (m_exhausted)
        return false;

    if (m_consumed > m_length ||
        !DoNext(map, m_line + m_consumed, m_length - m_consumed, e))
    {
        SetExhausted();
        return false;
    }

    m_consumed = GetMatchStart() + GetMatchLength();
    m_started = true;

    if (!GetMatchLength())
        ++m_consumed;

    return true;
}

void TrimLineEnding(StrW& s)
{
    while (s.Length())
    {
        const WCHAR c = s.Text()[s.Length() - 1];
        if (c != '\r' && c != '\n')
            break;
        s.SetLength(s.Length() - 1);
    }
}

std::shared_ptr<Searcher> ReadSearchInput(unsigned row, unsigned terminal_width, bool caseless, Error& e)
{
    StrW tmp;
    ClickableRow cr;

    enum { ID_HELP, ID_IGNORECASE, ID_REGEXP };

    auto printcontext = [&]()
    {
        cr.Init(row, terminal_width, 20);

        cr.AddKeyName(L"F1", ColorElement::Footer, L"Help", ID_HELP, 79, true);
        cr.Add(nullptr, 2, 79, true);
        cr.AddKeyName(L"^I", ColorElement::Footer, caseless ? L"IgnoreCase" : L"ExactCase ", ID_IGNORECASE, 99, true);
        cr.Add(nullptr, 2, 89, true);
        cr.AddKeyName(L"^X", ColorElement::Footer, s_regex ? L"RegExp " : L"Literal", ID_REGEXP, 89, true);

        tmp.Set(L"\r");
        cr.BuildOutput(tmp, GetColor(ColorElement::Footer));
        tmp.Printf(L"\rSearch%s ", c_prompt_char);
        OutputConsole(tmp.Text(), tmp.Length());
    };

    auto callback = [&](const InputRecord& input, const ReadInputBuffer& /*buffer*/, void* /*cookie*/)
    {
        switch (input.type)
        {
        case InputType::Char:
            switch (input.key_char)
            {
            case 'X'-'@':
                // 'Ctrl-X' toggles regex mode.
toggle_regex:
                s_regex = !s_regex;
                printcontext();
                return 1;
            }
            break;
        case InputType::Key:
            switch (input.key)
            {
            case Key::F1:
                if (input.modifier == Modifier::None)
                {
help_url:
#ifdef INCLUDE_RE2
                    const char* const url = "https://github.com/google/re2/wiki/Syntax";
#else
                    const char* const url = "https://learn.microsoft.com/en-us/cpp/standard-library/regular-expressions-cpp";
#endif
                    AllowSetForegroundWindow(ASFW_ANY);
                    ShellExecuteA(0, nullptr, url, 0, 0, SW_NORMAL);
                }
                break;
            case Key::TAB:
                // 'Ctrl-I' toggles ignore case.
                if (input.modifier == Modifier::CTRL)
                {
toggle_caseless:
                    caseless = !caseless;
                    printcontext();
                }
                return 1;
            }
            break;
        case InputType::Mouse:
            switch (cr.InterpretInput(input))
            {
            case ID_HELP:           goto help_url;
            case ID_IGNORECASE:     goto toggle_caseless;
            case ID_REGEXP:         goto toggle_regex;
            }
            break;
        }
        return 0; // Accept.
    };

    printcontext();

    StrW s;
    const uint16 right_width = cr.GetRightWidth();
    const DWORD max_width = terminal_width - 8 - right_width - (right_width ? 4 : 0);
    ReadInput(s, History::Search, 1024, max_width, callback);

    OutputConsole(c_norm);

    std::shared_ptr<Searcher> searcher;
    if (s.Length())
        searcher = Searcher::Create(s_regex ? SearcherType::Regex : SearcherType::Literal, s.Text(), caseless, e);
    return searcher;
}
