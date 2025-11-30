// Copyright (c) 2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "searcher.h"
#include "input.h"
#include "output.h"
#include "colors.h"

#include <regex>
#include <memory>

class Searcher_Literal : public Searcher
{
public:
                    Searcher_Literal(const WCHAR* s, bool caseless, bool /*optimize*/, Error& e);
                    ~Searcher_Literal() = default;

    SearcherType    GetSearcherType() const override { return SearcherType::Literal; }
    unsigned        GetNeedleDelta() const override { return m_find.Length(); }

protected:
    bool            DoNext(const WCHAR* line, unsigned length, Error& e) override;

private:
    const bool      m_caseless;
    const StrW      m_find;
};

Searcher_Literal::Searcher_Literal(const WCHAR* s, bool caseless, bool /*optimize*/, Error& e)
: m_caseless(caseless)
, m_find(s)
{
}

bool Searcher_Literal::DoNext(const WCHAR* line, unsigned length, Error& e)
{
    // FUTURE:  Boyer-Moore?

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

class Searcher_ECMAScriptRegex : public Searcher
{
public:
                    Searcher_ECMAScriptRegex(const WCHAR* s, bool caseless, bool optimize, Error& e);
                    ~Searcher_ECMAScriptRegex() = default;

    SearcherType    GetSearcherType() const override { return SearcherType::ECMAScriptRegex; }

protected:
    bool            DoNext(const WCHAR* line, unsigned length, Error& e) override;

private:
    std::regex_constants::syntax_option_type m_syntax;
    std::unique_ptr<std::wregex> m_wregex;
};

Searcher_ECMAScriptRegex::Searcher_ECMAScriptRegex(const WCHAR* s, bool caseless, bool optimize, Error& e)
{
    std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript;
    if (caseless)
        flags |= std::regex_constants::icase;
    if (optimize)
        flags |= std::regex_constants::optimize;

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

bool Searcher_ECMAScriptRegex::DoNext(const WCHAR* line, unsigned length, Error& e)
{
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

std::unique_ptr<Searcher> Searcher::Create(SearcherType type, const WCHAR* s, bool caseless, bool optimize, Error& e)
{
    std::unique_ptr<Searcher> searcher;

    switch (type)
    {
    default:
    case SearcherType::Literal:
        searcher = std::make_unique<Searcher_Literal>(s, caseless, optimize, e);
        break;
    case SearcherType::ECMAScriptRegex:
        searcher = std::make_unique<Searcher_ECMAScriptRegex>(s, caseless, optimize, e);
        break;
    }

    if (e.Test())
        searcher.release();
    return searcher;
}

bool Searcher::Match(const WCHAR* line, unsigned length, Error& e)
{
    m_started = false;
    m_exhausted = false;
    m_line = line;
    m_length = length;
    m_match_index = 0;
    m_match_length = 0;
    m_consumed = 0;
    return Next(e);
}

bool Searcher::Next(Error& e)
{
    if (m_exhausted)
        return false;

    if (m_consumed > m_length ||
        !DoNext(m_line + m_consumed, m_length - m_consumed, e))
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

std::unique_ptr<Searcher> ReadSearchInput(unsigned terminal_width, bool caseless, bool regex, Error& e)
{
    StrW s;
    bool done = false;

    auto callback = [&](const InputRecord& input)
    {
        switch (input.type)
        {
        case InputType::Char:
            switch (input.key_char)
            {
            case 'X'-'@':
                // 'Ctrl-X' toggles regex mode.
                regex = !regex;
                done = false;
                return -1;
            }
            break;
        case InputType::Key:
            switch (input.key)
            {
            case Key::TAB:
                // 'Ctrl-I' toggles ignore case.
                if (input.modifier == Modifier::CTRL)
                    caseless = !caseless;
                done = false;
                return -1;
            }
            break;
        }
        return 0; // Accept.
    };

    StrW right;
    while (!done)
    {
        right.Clear();
        right.Append(caseless ?
            L"IgnoreCase (^I)" :
            L" ExactCase (^I)");
        right.Append(regex ?
            L"    RegExp (^X)" :
            L"   Literal (^X)");

        s.Clear();
        s.AppendColor(GetColor(ColorElement::Command));
        s.Printf(L"\r\x1b[K\x1b[%uG%s\rSearch%s ", terminal_width + 1 - right.Length(), right.Text(), c_prompt_char);
        OutputConsole(s.Text(), s.Length());

        done = true;
        ReadInput(s, History::Search, 1024, 32, callback);

        OutputConsole(c_norm);
    }

    std::unique_ptr<Searcher> searcher;
    if (s.Length())
        searcher = Searcher::Create(regex ? SearcherType::ECMAScriptRegex : SearcherType::Literal, s.Text(), caseless, true, e);
    return searcher;
}
