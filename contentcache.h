// Copyright (c) 2025 by Christopher Antos
// License: http://opensource.org/licenses/MIT

// vim: set et ts=4 sw=4 cino={0s:

#include "vieweroptions.h"
#include "encodings.h"
#include "searcher.h"
#include "wcwidth.h"
#include "wcwidth_iter.h"

#include <vector>
#include <map>

typedef unsigned __int64 FileOffset;

struct FoundOffset
{
                    FoundOffset() { Clear(); }
    bool            Empty() const { return !is_valid; }
    void            Clear();
    void            MarkOffset(FileOffset found_offset);
    void            Found(FileOffset found_offset, unsigned found_len);

    FileOffset      offset;         // Offset where text was found.
    unsigned        len;            // Length of found text in line (NOTE: may extend past end of line if a line break occurred).
    bool            is_valid;       // True when offset is valid, False otherwise.
};

class PatchBlock
{
public:
    static const DWORD c_size = 8;
                    PatchBlock(FileOffset offset);
    bool            IsSet(FileOffset offset) const;
    BYTE            GetByte(FileOffset offset) const;
    void            SetByte(FileOffset offset, BYTE value);
private:
    FileOffset      m_offset;
    BYTE            m_bytes[c_size];
    BYTE            m_mask;
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
    FileLineIter&   operator=(FileLineIter&& other);

    void            Reset();
    void            ClearProcessed();
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
    std::unique_ptr<IDecoder> m_decoder;

    FileOffset      m_offset = 0;
    const BYTE*     m_bytes = nullptr;
    size_t          m_count = 0;
    size_t          m_available = 0;
    character_sequence_state m_width_state;
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
    FileLineMap&    operator=(FileLineMap&& other);

    void            OverrideEncoding(UINT codepage);
    bool            SetWrapWidth(unsigned wrap);
    unsigned        GetWrapWidth() const { return m_wrap; }

    void            Reset();
    void            ClearProcessed();
    FileOffset      Processed() const { return m_processed; }
    void            Next(const BYTE* bytes, size_t count);

    size_t          Count() const { return m_lines.size(); }
    size_t          CountFriendlyLines() const;
    FileOffset      GetOffset(size_t index) const;
    size_t          GetLineNumber(size_t index) const;
    void            GetLineText(const BYTE* p, size_t num_bytes, StrW& out, bool hex_mode=false) const;
    size_t          FriendlyLineNumberToIndex(size_t line) const;
    size_t          OffsetToIndex(FileOffset offset) const;
    bool            IsDetectedBinaryFile() const { return m_detected_type == FileDataType::Binary; }
    bool            IsBinaryFile() const { return m_line_iter.IsBinaryFile(); }
    bool            IsUTF8Compatible() const;
    bool            IsUnicodeEncoding() const { return m_is_unicode_encoding; }
    UINT            GetCodePage(bool hex_mode=false) const;
    UINT            GetDetectedCodePage() const { return m_detected_codepage; }
    const WCHAR*    GetEncodingName(bool hex_mode=false) const;
    const WCHAR*    GetDetectedEncodingName() const;

    void            SetFileType(FileDataType type, UINT codepage, const WCHAR* encoding_name);

private:

private:
    // Content.
    std::vector<FileOffset> m_lines;
    std::vector<size_t> m_line_numbers;

    // Processing.
    size_t          m_current_line_number = 1;
    FileOffset      m_processed = 0;
    FileOffset      m_pending_begin = 0;
    FileLineIter    m_line_iter;
    uint8           m_skip_whitespace = 0;
    bool            m_wrapped_current_line = false;

    // Configuration.
    unsigned        m_wrap = 0;

    // Encoding.
    FileDataType    m_detected_type = FileDataType::Binary;
    UINT            m_detected_codepage = 0;
    UINT            m_codepage = 0;
    StrW            m_detected_encoding_name;
    StrW            m_encoding_name;
    bool            m_is_unicode_encoding = false;
    bool            m_need_type = true;
};

class ContentCache
{
public:
                    ContentCache(const ViewerOptions& options);
                    ~ContentCache() { Close(); }
    ContentCache&   operator=(ContentCache&& other);

    bool            HasContent() const;
    bool            IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }
    bool            IsPipe() const { return m_redirected; }
    bool            IsDetectedBinaryFile() const { return m_map.IsDetectedBinaryFile(); }
    bool            IsBinaryFile() const { return m_map.IsBinaryFile(); }
    UINT            GetCodePage(bool hex_mode=false) const { return m_map.GetCodePage(hex_mode); }
    UINT            GetDetectedCodePage() const { return m_map.GetDetectedCodePage(); }
    const WCHAR*    GetEncodingName(bool hex_mode=false) const { return m_map.GetEncodingName(hex_mode); }
    const WCHAR*    GetDetectedEncodingName() const { return m_map.GetDetectedEncodingName(); }
    bool            SetTextContent(const char* text, Error& e);
    void            SetEncoding(UINT codepage) { m_map.OverrideEncoding(codepage); }
    bool            Open(const WCHAR* name, Error& e);
    void            Close();

    void            ClearProcessed();
    void            SetWrapWidth(unsigned wrap);
    unsigned        GetHexOffsetColumnWidth() const { return m_hex_size_width; }
    unsigned        FormatLineData(size_t line, unsigned left_offset, StrW& s, unsigned max_width, Error& e, const WCHAR* color=nullptr, const FoundOffset* found_line=nullptr, unsigned max_len=-1);
    bool            FormatHexData(FileOffset offset, unsigned row, unsigned hex_bytes, StrW& s, Error& e, const FoundOffset* found_line=nullptr);

    bool            ProcessThrough(size_t line, Error& e, bool cancelable=false);
    bool            ProcessToEnd(Error& e, bool cancelable=false);
    FileOffset      Processed() const { return m_map.Processed(); }
    bool            Completed() const { return m_completed; }
    bool            Eof() const { return m_eof; }

    size_t          Count() const { return m_map.Count(); }
    size_t          CountFriendlyLines() const { return m_map.CountFriendlyLines(); }
    FileOffset      GetFileSize() const { return m_size; }
    FileOffset      GetMaxHexOffset(unsigned hex_width) const;
    FileOffset      GetOffset(size_t index) const { return m_map.GetOffset(index); }
    size_t          GetLineNunber(size_t index) const { return m_map.GetLineNumber(index); }
    size_t          FriendlyLineNumberToIndex(size_t index) const { return m_map.FriendlyLineNumberToIndex(index); }
    unsigned        GetLength(size_t index) const;

    bool            Find(bool next, const std::unique_ptr<Searcher>& searcher, unsigned max_width, FoundOffset& found, unsigned& left_offset, Error& e);
    bool            Find(bool next, const std::unique_ptr<Searcher>& searcher, unsigned hex_width, FoundOffset& found, Error& e);

    FileOffset      GetBufferOffset() const { return m_data_offset; }
    unsigned        GetBufferLength() const { return m_data_length; }

    bool            IsDirty() const { return !m_patch_blocks.empty(); }
    void            SetByte(FileOffset offset, BYTE value, bool high_nybble);
    bool            SaveBytes(Error& e);
    void            DiscardBytes() { m_patch_blocks.clear(); }
    bool            NextEditedByteRow(FileOffset here, FileOffset& there, unsigned hex_width, bool next) const;

private:
    void            SetSize(FileOffset size);
    bool            EnsureDataBuffer(Error& e);
    bool            LoadData(FileOffset offset, DWORD& end_slop, Error& e);
    bool            EnsureFileData(size_t line, Error& e);
    bool            EnsureHexData(FileOffset offset, unsigned length, Error& e);
    bool            IsByteDirty(FileOffset offset, BYTE& value) const;

private:
    const ViewerOptions& m_options;
    HANDLE          m_file = INVALID_HANDLE_VALUE;
    FileOffset      m_size = 0;
    unsigned        m_hex_size_width = 0;

    bool            m_redirected = false;
    PipeChunks      m_chunks;
    const char*     m_text = nullptr;

    FileLineMap     m_map;
    bool            m_completed = false;
    bool            m_eof = false;

    BYTE*           m_data = nullptr;
    FileOffset      m_data_offset = 0;
    DWORD           m_data_length = 0;
    DWORD           m_data_slop = 0;

    std::map<FileOffset, PatchBlock> m_patch_blocks;
};

