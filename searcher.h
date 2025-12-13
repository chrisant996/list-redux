// Copyright (c) 2025 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include <windows.h>
#include <memory>

#include "error.h"

enum class SearcherType { Literal, ECMAScriptRegex };

class Searcher : public std::enable_shared_from_this<Searcher>
{
public:
    static std::shared_ptr<Searcher> Create(SearcherType type, const WCHAR* s, bool caseless, bool optimize, Error& e);

                    ~Searcher() = default;

    bool            Match(const WCHAR* line, unsigned len, Error& e);

    unsigned        GetMatchStart() const { return m_match_index; }
    unsigned        GetMatchLength() const { return m_match_length; }

    virtual SearcherType GetSearcherType() const = 0;
    virtual unsigned GetNeedleDelta() const { return 0; }

protected:
                    Searcher() { SetExhausted(); }

    virtual bool    DoNext(const WCHAR* line, unsigned cchLine, Error& e) = 0;

    void            SetExhausted() { m_exhausted = true; m_match_index = 0; m_match_length = 0; }
    bool            IsStarted() const { return m_started; }

    void            SetMatch(unsigned index, unsigned length) { m_match_index = index + m_consumed; m_match_length = length; }

private:
    bool            Next(Error& e);

private:
    bool            m_started;
    bool            m_exhausted;
    const WCHAR*    m_line;
    unsigned        m_length;
    unsigned        m_match_index;
    unsigned        m_match_length;
    unsigned        m_consumed;
};

void TrimLineEnding(StrW& s);

std::shared_ptr<Searcher> ReadSearchInput(unsigned row, unsigned terminal_width, bool caseless, bool regex, Error& e);
