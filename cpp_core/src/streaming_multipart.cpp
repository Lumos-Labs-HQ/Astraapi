#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "streaming_multipart.hpp"
#include "pyref.hpp"
#include <cstring>
#include <algorithm>
#include <cstdio>

// ── Static Python object cache ────────────────────────────────────────────────
PyObject* StreamingMultipartParser::s_bytes_io_cls_   = nullptr;
PyObject* StreamingMultipartParser::s_upload_file_cls_ = nullptr;
PyObject* StreamingMultipartParser::s_headers_cls_    = nullptr;
PyObject* StreamingMultipartParser::s_seek_str_       = nullptr;
bool      StreamingMultipartParser::s_initialized_    = false;

bool StreamingMultipartParser::init_python_refs() {
    if (s_initialized_) return true;

    // io.BytesIO
    if (!s_bytes_io_cls_) {
        PyRef io(PyImport_ImportModule("io"));
        if (!io) { PyErr_Clear(); return false; }
        s_bytes_io_cls_ = PyObject_GetAttrString(io.get(), "BytesIO");
        if (!s_bytes_io_cls_) { PyErr_Clear(); return false; }
    }
    // astraapi._datastructures_impl.UploadFile + Headers
    if (!s_upload_file_cls_ || !s_headers_cls_) {
        PyRef mod(PyImport_ImportModule("astraapi._datastructures_impl"));
        if (!mod) { PyErr_Clear(); return false; }
        if (!s_upload_file_cls_) {
            s_upload_file_cls_ = PyObject_GetAttrString(mod.get(), "UploadFile");
            if (!s_upload_file_cls_) { PyErr_Clear(); return false; }
        }
        if (!s_headers_cls_) {
            s_headers_cls_ = PyObject_GetAttrString(mod.get(), "Headers");
            if (!s_headers_cls_) { PyErr_Clear(); return false; }
        }
    }
    if (!s_seek_str_) s_seek_str_ = PyUnicode_InternFromString("seek");

    s_initialized_ = true;
    return true;
}

// ── Constructor ───────────────────────────────────────────────────────────────
StreamingMultipartParser::StreamingMultipartParser(
    const std::string& boundary, size_t max_body_size)
    : max_body_size_(max_body_size)
{
    boundary_       = "--" + boundary;
    final_boundary_ = "--" + boundary + "--";
    build_bmh_table();
    files.reserve(8);
    lookahead_.reserve(boundary_.size() + 8);
}

// ── Boyer-Moore-Horspool ──────────────────────────────────────────────────────
void StreamingMultipartParser::build_bmh_table() {
    const size_t m = boundary_.size();
    for (int i = 0; i < 256; i++) bmh_skip_[i] = (uint8_t)std::min(m, (size_t)255);
    for (size_t i = 0; i < m - 1; i++) {
        size_t skip = m - 1 - i;
        bmh_skip_[(uint8_t)boundary_[i]] = (uint8_t)std::min(skip, (size_t)255);
    }
}

size_t StreamingMultipartParser::bmh_search(const uint8_t* data, size_t len) const {
    const size_t m = boundary_.size();
    if (len < m) return npos;
    const uint8_t* pat = (const uint8_t*)boundary_.data();
    size_t i = m - 1;
    while (i < len) {
        size_t j = m - 1, k = i;
        while (j < m && data[k] == pat[j]) {
            if (j == 0) return k;
            --j; --k;
        }
        i += bmh_skip_[data[i]];
    }
    return npos;
}

// ── Main feed() ───────────────────────────────────────────────────────────────
FeedResult StreamingMultipartParser::feed(const uint8_t* data, size_t len) {
    if (state_ == MultipartState::DONE)  return FeedResult::DONE;
    if (state_ == MultipartState::ERROR) return FeedResult::ERROR;

    if (max_body_size_ > 0) {
        total_bytes_ += len;
        if (total_bytes_ > max_body_size_) return FeedResult::SIZE_EXCEEDED;
    }

    std::vector<uint8_t> buf;
    if (!lookahead_.empty()) {
        buf.reserve(lookahead_.size() + len);
        buf.insert(buf.end(), lookahead_.begin(), lookahead_.end());
        buf.insert(buf.end(), data, data + len);
        lookahead_.clear();
        data = buf.data();
        len  = buf.size();
    }

    size_t pos = 0;
    while (pos < len) {
        size_t consumed = 0;
        FeedResult r = FeedResult::NEED_MORE;
        switch (state_) {
        case MultipartState::PREAMBLE:    r = process_preamble(data+pos, len-pos, consumed); break;
        case MultipartState::PART_HEADER: r = process_header(data+pos, len-pos, consumed);   break;
        case MultipartState::PART_DATA:   r = process_data(data+pos, len-pos, consumed);     break;
        default: return FeedResult::DONE;
        }
        if (consumed == 0 && r == FeedResult::NEED_MORE) {
            lookahead_.assign(data+pos, data+len);
            return FeedResult::NEED_MORE;
        }
        pos += consumed;
        if (r != FeedResult::NEED_MORE) return r;
    }
    return (state_ == MultipartState::DONE) ? FeedResult::DONE : FeedResult::NEED_MORE;
}

// ── PREAMBLE ──────────────────────────────────────────────────────────────────
FeedResult StreamingMultipartParser::process_preamble(
    const uint8_t* data, size_t len, size_t& consumed)
{
    size_t pos = bmh_search(data, len);
    if (pos == npos) {
        size_t keep = std::min(len, boundary_.size() - 1);
        lookahead_.assign(data + len - keep, data + len);
        consumed = len;
        return FeedResult::NEED_MORE;
    }
    size_t after = pos + boundary_.size();
    if (after + 2 > len) {
        lookahead_.assign(data + pos, data + len);
        consumed = pos;
        return FeedResult::NEED_MORE;
    }
    if (data[after] == '-' && data[after+1] == '-') {
        state_ = MultipartState::DONE;
        consumed = after + 2;
        return FeedResult::DONE;
    }
    if (data[after] == '\r') after++;
    if (after < len && data[after] == '\n') after++;
    state_ = MultipartState::PART_HEADER;
    cur_header_buf_.clear();
    consumed = after;
    return FeedResult::NEED_MORE;
}

// ── PART_HEADER ───────────────────────────────────────────────────────────────
FeedResult StreamingMultipartParser::process_header(
    const uint8_t* data, size_t len, size_t& consumed)
{
    const char* p = (const char*)data;
    const char* end = p + len;
    const char* found = std::search(p, end, "\r\n\r\n", "\r\n\r\n" + 4);
    if (found == end) {
        if (cur_header_buf_.size() + len > 65536) return FeedResult::ERROR;
        cur_header_buf_.append(p, len);
        consumed = len;
        return FeedResult::NEED_MORE;
    }
    size_t hdr_end = (found - p) + 4;
    cur_header_buf_.append(p, found - p);
    parse_part_headers();
    cur_header_buf_.clear();

    if (cur_is_file_) {
        // Allocate a new file part — C++ owns the data buffer
        files.emplace_back();
        MultipartFile& f = files.back();
        f.name         = cur_name_;
        f.filename     = cur_filename_;
        f.content_type = cur_content_type_;
        // Reserve reasonable initial capacity
        f.data_buf.reserve(65536);
        cur_file_ = &f;
    } else {
        cur_field_data_.clear();
    }

    state_ = MultipartState::PART_DATA;
    consumed = hdr_end;
    return FeedResult::NEED_MORE;
}

// ── PART_DATA ─────────────────────────────────────────────────────────────────
FeedResult StreamingMultipartParser::process_data(
    const uint8_t* data, size_t len, size_t& consumed)
{
    // Search for "\r\n" + boundary_ using memchr acceleration
    std::string delim = "\r\n" + boundary_;
    const uint8_t* delim_data = (const uint8_t*)delim.data();
    size_t delim_len = delim.size();

    size_t pos = npos;
    if (len >= delim_len) {
        const uint8_t first = delim_data[0];
        const uint8_t* p = data;
        const uint8_t* end = data + len - delim_len + 1;
        while (p < end) {
            p = (const uint8_t*)memchr(p, first, end - p);
            if (!p) break;
            if (memcmp(p, delim_data, delim_len) == 0) { pos = p - data; break; }
            p++;
        }
    }

    if (pos == npos) {
        // Append safe bytes to buffer (keep last delim_len-1 as lookahead)
        size_t safe = (len >= delim_len - 1) ? len - (delim_len - 1) : 0;
        if (safe > 0) {
            if (cur_is_file_ && cur_file_) {
                cur_file_->data_buf.insert(cur_file_->data_buf.end(), data, data + safe);
            } else {
                cur_field_data_.append((const char*)data, safe);
            }
        }
        lookahead_.assign(data + safe, data + len);
        consumed = len;
        return FeedResult::NEED_MORE;
    }

    // Found delimiter — append data up to pos
    if (pos > 0) {
        if (cur_is_file_ && cur_file_) {
            cur_file_->data_buf.insert(cur_file_->data_buf.end(), data, data + pos);
        } else {
            cur_field_data_.append((const char*)data, pos);
        }
    }

    // Finalize current part
    if (cur_is_file_ && cur_file_) {
        cur_file_ = nullptr;
    } else {
        fields.push_back({cur_name_, cur_field_data_});
    }

    // Advance past delimiter
    size_t after = pos + delim_len;
    if (after + 2 > len) {
        lookahead_.assign(data + pos, data + len);
        consumed = pos;
        return FeedResult::NEED_MORE;
    }
    if (data[after] == '-' && data[after+1] == '-') {
        state_ = MultipartState::DONE;
        consumed = after + 2;
        return FeedResult::DONE;
    }
    if (data[after] == '\r') after++;
    if (after < len && data[after] == '\n') after++;
    state_ = MultipartState::PART_HEADER;
    cur_header_buf_.clear();
    consumed = after;
    return FeedResult::NEED_MORE;
}

// ── Header parsing ────────────────────────────────────────────────────────────
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void StreamingMultipartParser::parse_part_headers() {
    cur_name_.clear(); cur_filename_.clear();
    cur_content_type_ = "application/octet-stream";
    cur_is_file_ = false;

    const std::string& hdr = cur_header_buf_;
    size_t pos = 0;
    while (pos < hdr.size()) {
        size_t nl = hdr.find("\r\n", pos);
        if (nl == std::string::npos) nl = hdr.size();
        std::string line = hdr.substr(pos, nl - pos);
        pos = nl + 2;
        if (line.empty()) break;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string hname = line.substr(0, colon);
        for (auto& c : hname) if (c >= 'A' && c <= 'Z') c += 32;
        hname = trim(hname);
        std::string hval = trim(line.substr(colon + 1));

        if (hname == "content-disposition") {
            size_t p2 = 0;
            while (p2 < hval.size()) {
                size_t sc = hval.find(';', p2);
                if (sc == std::string::npos) sc = hval.size();
                std::string seg = trim(hval.substr(p2, sc - p2));
                p2 = sc + 1;
                if (seg.rfind("name=", 0) == 0) {
                    cur_name_ = trim(seg.substr(5));
                    if (!cur_name_.empty() && cur_name_.front() == '"')
                        cur_name_ = cur_name_.substr(1, cur_name_.size() - 2);
                } else if (seg.rfind("filename=", 0) == 0) {
                    cur_filename_ = trim(seg.substr(9));
                    if (!cur_filename_.empty() && cur_filename_.front() == '"')
                        cur_filename_ = cur_filename_.substr(1, cur_filename_.size() - 2);
                    cur_is_file_ = true;
                }
            }
        } else if (hname == "content-type") {
            cur_content_type_ = hval;
        }
    }
}

// ── Build Python kwargs dict directly ────────────────────────────────────────
// This is the key optimization: C++ builds the kwargs dict directly,
// no FormData, no intermediate Python objects during parsing.
// Python endpoint just receives UploadFile objects — zero overhead.
PyObject* StreamingMultipartParser::build_kwargs(PyObject* existing_kwargs) const {
    if (!init_python_refs()) return nullptr;

    // Use existing kwargs dict or create new one
    PyObject* kwargs = existing_kwargs ? existing_kwargs : PyDict_New();
    if (!kwargs) return nullptr;

    // Static interned key strings
    static PyObject* s_fn_key  = nullptr;
    static PyObject* s_fl_key  = nullptr;
    static PyObject* s_ct_key  = nullptr;
    static PyObject* s_hdr_key = nullptr;
    static PyObject* s_sz_key  = nullptr;
    if (!s_fn_key) {
        s_fn_key  = PyUnicode_InternFromString("filename");
        s_fl_key  = PyUnicode_InternFromString("file");
        s_ct_key  = PyUnicode_InternFromString("content_type");
        s_hdr_key = PyUnicode_InternFromString("headers");
        s_sz_key  = PyUnicode_InternFromString("size");
    }

    // Empty Headers object (reused for all files)
    PyRef empty_tuple(PyTuple_New(0));
    PyRef raw_list(PyList_New(0));
    PyRef hdr_kw(PyDict_New());
    if (!empty_tuple || !raw_list || !hdr_kw) {
        if (!existing_kwargs) Py_DECREF(kwargs);
        return nullptr;
    }
    PyDict_SetItemString(hdr_kw.get(), "raw", raw_list.get());
    PyRef headers(PyObject_Call(s_headers_cls_, empty_tuple.get(), hdr_kw.get()));
    if (!headers) { PyErr_Clear(); if (!existing_kwargs) Py_DECREF(kwargs); return nullptr; }

    // Inject form fields as strings
    for (const auto& field : fields) {
        PyRef key(PyUnicode_FromStringAndSize(field.name.data(), field.name.size()));
        PyRef val(PyUnicode_FromStringAndSize(field.value.data(), field.value.size()));
        if (key && val) PyDict_SetItem(kwargs, key.get(), val.get());
    }

    // Inject file fields as UploadFile objects
    for (const auto& file : files) {
        // Create file-like object from C++ buffer:
        // Small files (< 1MB): io.BytesIO — zero-copy from C++ buffer
        // Large files (>= 1MB): write to real tempfile via C fwrite, wrap as Python file
        PyObject* file_obj = nullptr;
        Py_ssize_t file_size = (Py_ssize_t)file.data_buf.size();

        if (file_size < 1048576) {
            // Small file: BytesIO — single PyBytes allocation, then BytesIO wraps it
            PyRef data_bytes(PyBytes_FromStringAndSize(
                (const char*)file.data_buf.data(), file_size));
            if (!data_bytes) { if (!existing_kwargs) Py_DECREF(kwargs); return nullptr; }
            file_obj = PyObject_CallOneArg(s_bytes_io_cls_, data_bytes.get());
        } else {
            // Large file: write to real tempfile via C fwrite (no Python overhead)
            // Then open it as a Python file object
            char tmppath[256];
            snprintf(tmppath, sizeof(tmppath), "/tmp/astraapi_upload_XXXXXX");
            int fd = mkstemp(tmppath);
            if (fd >= 0) {
                // Write all data via C write() — no Python GIL overhead
                const uint8_t* ptr = file.data_buf.data();
                size_t remaining = file.data_buf.size();
                while (remaining > 0) {
                    ssize_t written = write(fd, ptr, remaining);
                    if (written <= 0) break;
                    ptr += written;
                    remaining -= written;
                }
                close(fd);
                // Open as Python file object (seeked to start)
                PyRef path_str(PyUnicode_FromString(tmppath));
                PyRef mode_str(PyUnicode_FromString("r+b"));
                if (path_str && mode_str) {
                    file_obj = PyObject_CallFunctionObjArgs(
                        (PyObject*)&PyBaseObject_Type, nullptr);  // placeholder
                    // Use builtins.open()
                    PyRef builtins(PyImport_ImportModule("builtins"));
                    if (builtins) {
                        PyRef open_fn(PyObject_GetAttrString(builtins.get(), "open"));
                        if (open_fn) {
                            file_obj = PyObject_CallFunctionObjArgs(
                                open_fn.get(), path_str.get(), mode_str.get(), nullptr);
                        }
                    }
                    if (!file_obj) PyErr_Clear();
                }
                // Schedule file deletion on close via a wrapper? 
                // For now: use Python's tempfile.NamedTemporaryFile delete=True behavior
                // by registering an atexit or using os.unlink after close.
                // Simplest: use Python's tempfile module to get auto-delete
                if (!file_obj) {
                    // Fallback: BytesIO (memory)
                    PyRef data_bytes(PyBytes_FromStringAndSize(
                        (const char*)file.data_buf.data(), file_size));
                    if (data_bytes)
                        file_obj = PyObject_CallOneArg(s_bytes_io_cls_, data_bytes.get());
                }
            } else {
                // mkstemp failed: fallback to BytesIO
                PyRef data_bytes(PyBytes_FromStringAndSize(
                    (const char*)file.data_buf.data(), file_size));
                if (data_bytes)
                    file_obj = PyObject_CallOneArg(s_bytes_io_cls_, data_bytes.get());
            }
        }

        if (!file_obj) { if (!existing_kwargs) Py_DECREF(kwargs); return nullptr; }

        // Build UploadFile(filename=..., file=file_obj, content_type=..., headers=..., size=...)
        PyRef kw(PyDict_New());
        if (!kw) { Py_DECREF(file_obj); if (!existing_kwargs) Py_DECREF(kwargs); return nullptr; }

        PyRef fn(PyUnicode_FromStringAndSize(file.filename.data(), file.filename.size()));
        PyRef ct(PyUnicode_FromStringAndSize(file.content_type.data(), file.content_type.size()));
        PyRef sz(PyLong_FromSsize_t(file_size));
        if (!fn || !ct || !sz) {
            Py_DECREF(file_obj);
            if (!existing_kwargs) Py_DECREF(kwargs);
            return nullptr;
        }

        PyDict_SetItem(kw.get(), s_fn_key,  fn.get());
        PyDict_SetItem(kw.get(), s_fl_key,  file_obj);
        PyDict_SetItem(kw.get(), s_ct_key,  ct.get());
        PyDict_SetItem(kw.get(), s_hdr_key, headers.get());
        PyDict_SetItem(kw.get(), s_sz_key,  sz.get());
        Py_DECREF(file_obj);

        PyRef uf(PyObject_Call(s_upload_file_cls_, empty_tuple.get(), kw.get()));
        if (!uf) { PyErr_Clear(); if (!existing_kwargs) Py_DECREF(kwargs); return nullptr; }

        PyRef name_key(PyUnicode_FromStringAndSize(file.name.data(), file.name.size()));
        if (!name_key) { if (!existing_kwargs) Py_DECREF(kwargs); return nullptr; }

        // Handle multi-value: if key exists, convert to list
        PyObject* existing = PyDict_GetItem(kwargs, name_key.get());
        if (existing == nullptr) {
            PyDict_SetItem(kwargs, name_key.get(), uf.get());
        } else if (PyList_Check(existing)) {
            PyList_Append(existing, uf.get());
        } else {
            PyRef lst(PyList_New(2));
            if (lst) {
                Py_INCREF(existing);
                PyList_SET_ITEM(lst.get(), 0, existing);
                Py_INCREF(uf.get());
                PyList_SET_ITEM(lst.get(), 1, uf.get());
                PyDict_SetItem(kwargs, name_key.get(), lst.get());
            }
        }
    }

    return kwargs;
}

// Keep build_form_data for backward compat (used by routing.py path)
PyObject* StreamingMultipartParser::build_form_data() const {
    // Build kwargs first, then wrap in FormData
    PyRef kw(build_kwargs(nullptr));
    if (!kw) return nullptr;

    // Import FormData
    static PyObject* s_fd_cls = nullptr;
    if (!s_fd_cls) {
        PyRef mod(PyImport_ImportModule("astraapi._datastructures_impl"));
        if (!mod) { PyErr_Clear(); return nullptr; }
        s_fd_cls = PyObject_GetAttrString(mod.get(), "FormData");
        if (!s_fd_cls) { PyErr_Clear(); return nullptr; }
    }

    // Build list of (name, value) pairs from kwargs
    PyRef items(PyList_New(0));
    if (!items) return nullptr;
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(kw.get(), &pos, &k, &v)) {
        PyRef tup(PyTuple_Pack(2, k, v));
        if (tup) PyList_Append(items.get(), tup.get());
    }
    PyRef args(PyTuple_Pack(1, items.get()));
    if (!args) return nullptr;
    return PyObject_Call(s_fd_cls, args.get(), nullptr);
}

// ── Python-callable factory functions ────────────────────────────────────────
static void streaming_parser_destructor(PyObject* cap) {
    auto* p = static_cast<StreamingMultipartParser*>(
        PyCapsule_GetPointer(cap, "streaming_multipart_parser"));
    delete p;
}

PyObject* py_create_streaming_multipart_parser(PyObject* /*self*/, PyObject* args) {
    const char* boundary; Py_ssize_t blen; Py_ssize_t max_body = 0;
    if (!PyArg_ParseTuple(args, "s#|n", &boundary, &blen, &max_body)) return nullptr;
    auto* parser = new StreamingMultipartParser(
        std::string(boundary, blen), (size_t)std::max((Py_ssize_t)0, max_body));
    return PyCapsule_New(parser, "streaming_multipart_parser", streaming_parser_destructor);
}

PyObject* py_feed_streaming_multipart(PyObject* /*self*/, PyObject* args) {
    PyObject* cap; Py_buffer buf;
    if (!PyArg_ParseTuple(args, "Oy*", &cap, &buf)) return nullptr;
    auto* parser = static_cast<StreamingMultipartParser*>(
        PyCapsule_GetPointer(cap, "streaming_multipart_parser"));
    if (!parser) { PyBuffer_Release(&buf); return nullptr; }
    FeedResult r = parser->feed((const uint8_t*)buf.buf, (size_t)buf.len);
    PyBuffer_Release(&buf);
    return PyLong_FromLong((long)r);
}

PyObject* py_get_streaming_multipart_form_data(PyObject* /*self*/, PyObject* cap) {
    auto* parser = static_cast<StreamingMultipartParser*>(
        PyCapsule_GetPointer(cap, "streaming_multipart_parser"));
    if (!parser) return nullptr;
    PyObject* fd = parser->build_form_data();
    if (!fd) { PyErr_SetString(PyExc_RuntimeError, "Failed to build FormData"); return nullptr; }
    return fd;
}
