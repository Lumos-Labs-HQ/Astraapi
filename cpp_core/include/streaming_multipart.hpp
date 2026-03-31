#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <cstdint>
#include <string>
#include <vector>

// ── StreamingMultipartParser ─────────────────────────────────────────────────
// RFC 2046 multipart/form-data streaming parser.
// Boyer-Moore-Horspool boundary search: O(n/m) average.
// C++ accumulates file data in std::vector<uint8_t> — zero Python calls during parsing.
// At the end, creates UploadFile objects directly and injects into kwargs dict.
// Python endpoint just receives UploadFile — no SpooledTemporaryFile, no FormData.
// Small files (< 1MB): io.BytesIO. Large files (>= 1MB): real tmpfile via C fwrite.

enum class MultipartState : uint8_t { PREAMBLE, PART_HEADER, PART_DATA, DONE, ERROR };
enum class FeedResult      : uint8_t { NEED_MORE, DONE, ERROR, SIZE_EXCEEDED };

struct MultipartField { std::string name, value; };

struct MultipartFile {
    std::string name, filename, content_type;
    std::vector<uint8_t> data_buf;  // C++ owns the data — no Python calls during upload
};

class StreamingMultipartParser {
public:
    explicit StreamingMultipartParser(const std::string& boundary, size_t max_body_size = 0);

    // Feed a chunk. Call multiple times. Returns DONE when all parts parsed.
    FeedResult feed(const uint8_t* data, size_t len);

    // Build Python kwargs dict with UploadFile objects injected directly.
    // existing_kwargs: if non-null, inject into it; else create new dict.
    // Returns borrowed ref to existing_kwargs or new ref — caller owns new ref.
    PyObject* build_kwargs(PyObject* existing_kwargs) const;

    // Build FormData (for routing.py path backward compat)
    PyObject* build_form_data() const;

    std::vector<MultipartField> fields;
    std::vector<MultipartFile>  files;

private:
    void build_bmh_table();
    static constexpr size_t npos = SIZE_MAX;
    size_t bmh_search(const uint8_t* data, size_t len) const;

    FeedResult process_preamble(const uint8_t* data, size_t len, size_t& consumed);
    FeedResult process_header(const uint8_t* data, size_t len, size_t& consumed);
    FeedResult process_data(const uint8_t* data, size_t len, size_t& consumed);
    void parse_part_headers();

    std::string boundary_, final_boundary_;
    uint8_t     bmh_skip_[256];
    MultipartState state_ = MultipartState::PREAMBLE;
    size_t total_bytes_ = 0, max_body_size_;

    std::vector<uint8_t> lookahead_;
    std::string cur_header_buf_, cur_name_, cur_filename_, cur_content_type_, cur_field_data_;
    bool cur_is_file_ = false;
    MultipartFile* cur_file_ = nullptr;

    static PyObject* s_bytes_io_cls_;
    static PyObject* s_upload_file_cls_;
    static PyObject* s_headers_cls_;
    static PyObject* s_seek_str_;
    static bool      s_initialized_;
    static bool init_python_refs();
};
