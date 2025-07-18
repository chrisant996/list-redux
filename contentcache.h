// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "vieweroptions.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"

#include <vector>

typedef unsigned __int64 FileOffset;

struct FoundLine
{
                    FoundLine() { Clear(); }
    bool            Empty() const { return !is_valid; }
    void            Clear();
    void            MarkLine(size_t found_line);
    void            MarkOffset(FileOffset found_offset);
    void            Found(size_t found_line, unsigned found_offset, unsigned found_len);
    void            Found(FileOffset found_offset, unsigned found_len);

    size_t          line;           // Line number index where text was found.
    FileOffset      offset;         // Offset where text was found (may be line-relative, or file-absolute).
    unsigned        len;            // Length of found text in line (NOTE: may extend past end of line if a line break occurred).
    bool            is_valid;       // True when line or offset are valid, False otherwise.
    bool            is_line;        // True for found line, False for absolute file offset.
};

class Utf8Accumulator
{
public:
                    Utf8Accumulator() { Reset(); }
    void            Reset() { ZeroMemory(this, sizeof(*this)); }
    int32           Build(char c);      // 0=building, 1=completed, -1=invalid.
    bool            Ready() const { return m_length == m_expected; }
    void            ClearInvalid();
    uint32          Codepoint() const { assert(Ready()); return m_ax; }
    const char*     Bytes() const { assert(Ready()); return m_buffer; }
    uint32          Length() const { assert(Ready()); return m_length; }
private:
    uint32          m_ax;
    uint8           m_expected;         // Number of bytes expected.
    uint8           m_length;           // Number of bytes accumulated.
    char            m_buffer[4 + 1];    // Bytes accumulated.
    int8            m_invalid;          // An invalid data state occurred.
};

struct PipeChunk
{
public:
                    PipeChunk();
                    PipeChunk(const PipeChunk&) = delete;
                    PipeChunk(PipeChunk&& other);
                    ~PipeChunk();
    PipeChunk&      operator=(const PipeChunk& other) = delete;
    PipeChunk&      operator=(PipeChunk&& other);
    const BYTE*     Bytes() const { return m_bytes; }
    DWORD           Capacity() const;
    DWORD           Used() const { return m_used; }
    DWORD           Available() const { return Capacity() - Used(); }
    BYTE*           WritePtr() { return m_bytes + Used(); }
    void            Wrote(DWORD wrote);
private:
    void            Move(PipeChunk&& other);
private:
    BYTE*           m_bytes;
    DWORD           m_used = 0;
    // Note:  There is one DWORD of implicit padding here, which is available
    // for future use.
};

typedef std::vector<PipeChunk> PipeChunks;

class FileLineMap
{
public:
                    FileLineMap(const ViewerOptions& options);
                    ~FileLineMap() = default;

    bool            SetWrapWidth(unsigned wrap);
    unsigned        GetWrapWidth() const { return m_wrap; }

    void            Clear();
    FileOffset      Processed() const { return m_processed; }
    void            Next(const BYTE* bytes, size_t count);

    size_t          Count() const { return m_lines.size(); }
    FileOffset      GetOffset(size_t index) const;
    bool            IsBinaryFile() const { return m_binary_file; }
    UINT            GetCodePage() const { return m_codepage; }
    const WCHAR*    GetEncodingName(bool raw=false) const;

private:
    const ViewerOptions& m_options;
    unsigned        m_wrap = 0;

    std::vector<FileOffset> m_lines;
    UINT            m_codepage = 0;
    StrW            m_encoding_name;
    FileOffset      m_processed = 0;
    bool            m_binary_file = true;
    bool            m_continue_last_line = false;
    unsigned        m_last_length = 0;
    unsigned        m_last_visible_width = 0;

#ifdef DEBUG
    bool            m_skipped_empty_line = false;
#endif
};

class ContentCache
{
public:
                    ContentCache(const ViewerOptions& options);
                    ~ContentCache() { Close(); }

    bool            HasContent() const;
    bool            IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }
    bool            IsPipe() const { return m_redirected; }
    bool            IsBinaryFile() const { return m_map.IsBinaryFile(); }
    UINT            GetCodePage() const { return m_map.GetCodePage(); }
    const WCHAR*    GetEncodingName(bool raw=false) const { return m_map.GetEncodingName(raw); }
    bool            SetTextContent(const char* text, Error& e);
    bool            Open(const WCHAR* name, Error& e);
    void            Close();

    void            Reset();
    void            SetWrapWidth(unsigned wrap);
    unsigned        FormatLineData(size_t line, unsigned left_offset, StrW& s, unsigned max_width, Error& e, const WCHAR* color=nullptr, const FoundLine* found_line=nullptr);
    bool            FormatHexData(FileOffset offset, unsigned row, unsigned hex_bytes, StrW& s, Error& e, const FoundLine* found_line=nullptr);

    bool            ProcessThrough(size_t line, Error& e);
    bool            ProcessToEnd(Error& e);
    FileOffset      Processed() const { return m_map.Processed(); }
    bool            Completed() const { return m_completed; }
    bool            Eof() const { return m_eof; }

    size_t          Count() const { return m_map.Count(); }
    FileOffset      GetFileSize() const { return m_size; }
    FileOffset      GetMaxHexOffset(unsigned hex_width) const;
    FileOffset      GetOffset(size_t index) const { return m_map.GetOffset(index); }
    unsigned        GetLength(size_t index) const;

    bool            Find(bool next, const WCHAR* needle, FoundLine& found, bool caseless);
    bool            Find(bool next, const WCHAR* needle, unsigned hex_width, FoundLine& found, bool caseless);

    FileOffset      GetBufferOffset() const { return m_data_offset; }
    unsigned        GetBufferLength() const { return m_data_length; }

private:
    bool            EnsureDataBuffer(Error& e);
    bool            LoadData(FileOffset offset, Error& e);
    bool            EnsureFileData(size_t line, Error& e);
    bool            EnsureHexData(FileOffset offset, unsigned length, Error& e);

private:
    const ViewerOptions& m_options;
    HANDLE          m_file = INVALID_HANDLE_VALUE;
    FileOffset      m_size = 0;

    bool            m_redirected = false;
    PipeChunks      m_chunks;
    const char*     m_text = nullptr;

    FileLineMap     m_map;
    bool            m_completed = false;
    bool            m_eof = false;

    BYTE*           m_data = nullptr;
    FileOffset      m_data_offset = 0;
    DWORD           m_data_length = 0;
};

