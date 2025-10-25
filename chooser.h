// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#pragma once

#include <windows.h>
#include "str.h"
#include "fileinfo.h"
#include "columns.h"
#include "input.h"
#include "output.h"

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

enum class ReportErrorFlags { NONE, CANABORT, INLINE };
DEFINE_ENUM_FLAG_OPERATORS(ReportErrorFlags);

class Chooser
{
public:
                    Chooser(const Interactive* interactive);
                    ~Chooser() = default;

    void            Navigate(const WCHAR* dir, std::vector<FileInfo>&& files);
    void            Navigate(const WCHAR* dir, Error& e);
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

    bool            AskForConfirmation(const WCHAR* msg);
    bool            ReportError(Error& e, ReportErrorFlags flags=ReportErrorFlags::NONE);
    void            WaitToContinue(bool erase_after=false, bool new_line=false);

    void            NewFileMask(Error& e);
    void            ChangeAttributes(Error& e);
    void            NewDirectory(Error& e);
    void            RenameEntry(Error& e);
    void            DeleteEntries(Error& e);
    void            RunFile(bool edit, Error& e);
    void            SweepFiles(Error& e);
    void            ShowFileList();

private:
    const HANDLE    m_hout;
    const Interactive* const m_interactive;
    unsigned        m_terminal_width = 0;
    unsigned        m_terminal_height = 0;
    const unsigned  m_padding = 2;

    int             m_details = 1;

    StrW            m_dir;
    std::vector<FileInfo> m_files;
    ColumnWidths    m_col_widths;
    unsigned        m_max_size_width = 0;
    intptr_t        m_count = 0;
    intptr_t        m_num_rows = 0;
    int32           m_num_per_row = 0;
    int32           m_visible_rows = 0;
    int32           m_vert_scroll_car = 0;
    int32           m_vert_scroll_column = 0;
    StrW            m_feedback;

    intptr_t        m_top = 0;
    intptr_t        m_index = 0;
    MarkedList      m_tagged;
    InputRecord     m_prev_input;
    bool            m_prev_latched = false;

    MarkedList      m_dirty;
    bool            m_dirty_header = false;
    bool            m_dirty_footer = false;
    intptr_t        m_prev_visible_rows = 0;
    StrW            m_last_feedback;
};

