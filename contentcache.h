// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "vieweroptions.h"
#include "filetype.h"
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

class FileLineIter
{
public:
    enum Outcome
    {
        Exhausted,              // Reached end of data buffer.
        BreakNewline,           // Break because newline character.
        BreakMax,               // Break because max line length was reached.
        BreakWrap,              // Break because wrapping width was reached.
        BreakWrapSkip,          // Wrapping break, and then skip whitespace.
        BreakWrapResync,        // Wrapping break, and trigger resync (caller
                                // clears data buffer and loads a new block).
    };

                    FileLineIter(const ViewerOptions& options);
                    ~FileLineIter();
    void            Reset();
    void            SetEncoding(FileDataType type, UINT codepage);
    void            SetWrapWidth(uint32 wrap_width);
    void            SetBytes(FileOffset offset, const BYTE* bytes, size_t available);
    bool            More() const { return m_count > 0; }
    Outcome         Next(const BYTE*& bytes, uint32& length, uint32& width);
    bool            SkipWhitespace(uint32 curr_len, uint32& skipped);
    bool            IsBinaryFile() const { return m_binary_file; }

private:
    const ViewerOptions& m_options;
    uint32          m_wrap = 80;
    UINT            m_codepage = 0;
    bool            m_binary_file = true;
    FileOffset      m_offset = 0;
    const BYTE*     m_bytes = nullptr;
    size_t          m_count = 0;
    size_t          m_available = 0;
    std::unique_ptr<IDecoder> m_decoder;
    wcwidth_iter    m_iter;
    uint32          m_pending_length = 0;       // Length in bytes.
    uint32          m_pending_width = 0;        // Width in character cells.
    uint32          m_pending_wrap_length = 0;  // Candidate length for word wrap.
    uint32          m_pending_wrap_width = 0;   // Candidate width for word wrap.
};

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
    size_t          GetLineNumber(size_t index) const;
    size_t          FriendlyLineNumberToIndex(size_t line) const;
    bool            IsBinaryFile() const { return m_line_iter.IsBinaryFile(); }
    bool            IsUTF8Compatible() const;
    bool            IsUnicodeEncoding() const { return m_is_unicode_encoding; }
    UINT            GetCodePage() const { return m_codepage; }
    const WCHAR*    GetEncodingName(bool raw=false) const;

private:
    // const ViewerOptions& m_options;
    unsigned        m_wrap = 0;

    std::vector<FileOffset> m_lines;
    std::vector<size_t> m_line_numbers;
    UINT            m_codepage = 0;
    StrW            m_encoding_name;
    size_t          m_current_line_number = 1;
    FileOffset      m_processed = 0;
    FileOffset      m_pending_begin = 0;
    FileLineIter    m_line_iter;
    uint8           m_skip_whitespace = 0;
    bool            m_wrapped_current_line = false;
    bool            m_is_unicode_encoding = false;
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
    size_t          GetLineNunber(size_t index) const { return m_map.GetLineNumber(index); }
    size_t          FriendlyLineNumberToIndex(size_t index) const { return m_map.FriendlyLineNumberToIndex(index); }
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

