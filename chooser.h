// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "str.h"
#include "fileinfo.h"
#include "columns.h"
#include "input.h"

#include <vector>
#include <unordered_set>

class MarkedList
{
public:
                    MarkedList() = default;

    void            Clear() { m_set.clear(); m_reverse = false; }
    void            MarkAll() { m_set.clear(); m_reverse = true; }
    void            Reverse() { m_reverse = !m_reverse; }
    void            Mark(intptr_t index, int tag);  // -1=unmark, 0=toggle, 1=mark
    bool            IsMarked(intptr_t index) const;
    bool            AnyMarked() const;
    bool            AllMarked() const;

private:
    std::unordered_set<intptr_t> m_set;
    bool            m_reverse = false;
};

enum class ChooserOutcome { CONTINUE, VIEWONE, VIEWTAGGED, EXITAPP };

class Chooser
{
public:
                    Chooser();
                    ~Chooser() = default;

    void            Navigate(const WCHAR* dir, std::vector<FileInfo>&& files);
    void            Navigate(const WCHAR* dir, Error& e);
    bool            EverNavigated() const { return m_ever_navigated; }
    ChooserOutcome  Go(Error& e);
    StrW            GetSelectedFile() const;
    std::vector<StrW> GetTaggedFiles() const;

private:
    void            Reset();
    void            ForceUpdateAll();
    void            UpdateDisplay();
    void            Relayout();
    void            EnsureColumnWidths();
    ChooserOutcome  HandleInput(const InputRecord& input, Error &e);
    void            SetIndex(intptr_t index);
    void            SetTop(intptr_t top);
    void            EnsureTop();
    void            RefreshDirectoryListing(Error& e);

private:
    const HANDLE    m_hout;
    unsigned        m_terminal_width = 0;
    unsigned        m_terminal_height = 0;
    const unsigned  m_padding = 2;

    bool            m_ever_navigated = false;
    int             m_details = 1;

    StrW            m_dir;
    std::vector<FileInfo> m_files;
    ColumnWidths    m_col_widths;
    unsigned        m_max_size_width;
    intptr_t        m_count;
    intptr_t        m_num_rows;
    intptr_t        m_num_per_row;
    intptr_t        m_visible_rows;
    StrW            m_feedback;

    intptr_t        m_top;
    intptr_t        m_index;
    MarkedList      m_tagged;
    InputRecord     m_prev_input;
    bool            m_prev_latched;

    MarkedList      m_dirty;
    bool            m_dirty_header = false;
    bool            m_dirty_footer = false;
    intptr_t        m_prev_visible_rows = 0;
    StrW            m_last_feedback;
};

