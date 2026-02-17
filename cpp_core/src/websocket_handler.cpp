#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "json_parser.hpp"
#include "json_writer.hpp"
#include "buffer_pool.hpp"
#include "ws_frame_parser.hpp"
#include "pyref.hpp"
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// ws_parse_json(data: bytes|str) → PyAny  (yyjson — zero Python calls)
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_parse_json(PyObject* self, PyObject* arg) {
    const char* data = nullptr;
    Py_ssize_t len = 0;

    if (PyBytes_Check(arg)) {
        data = PyBytes_AS_STRING(arg);
        len = PyBytes_GET_SIZE(arg);
    } else if (PyUnicode_Check(arg)) {
        data = PyUnicode_AsUTF8AndSize(arg, &len);
        if (!data) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected bytes or str");
        return nullptr;
    }

    if (len <= 0) {
        PyErr_SetString(PyExc_ValueError, "Empty JSON input");
        return nullptr;
    }

    return yyjson_parse_to_pyobject(data, static_cast<size_t>(len));
}

// ══════════════════════════════════════════════════════════════════════════════
// ws_serialize_json(obj: PyAny) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_serialize_json(PyObject* self, PyObject* arg) {
    return serialize_to_json_pybytes(arg);
}

// ══════════════════════════════════════════════════════════════════════════════
// ws_batch_parse(messages: PyList) → PyList
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_batch_parse(PyObject* self, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list");
        return nullptr;
    }

    Py_ssize_t n = PyList_GET_SIZE(arg);
    PyRef result(PyList_New(n));
    if (!result) return nullptr;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* msg = PyList_GET_ITEM(arg, i);  // borrowed
        const char* data = nullptr;
        Py_ssize_t len = 0;
        PyObject* parsed = nullptr;

        if (PyBytes_Check(msg)) {
            data = PyBytes_AS_STRING(msg);
            len = PyBytes_GET_SIZE(msg);
        } else if (PyUnicode_Check(msg)) {
            data = PyUnicode_AsUTF8AndSize(msg, &len);
        }

        if (data && len > 0) {
            parsed = yyjson_parse_to_pyobject(data, static_cast<size_t>(len));
        }

        if (!parsed) {
            // On parse error, keep original
            PyErr_Clear();
            Py_INCREF(msg);
            parsed = msg;
        }
        PyList_SET_ITEM(result.get(), i, parsed);  // steals ref
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// ws_build_json_frame(obj: PyAny, opcode: int) → PyBytes
// Combined JSON serialize + WebSocket frame build in single allocation.
// Eliminates intermediate PyBytes from separate serialize + frame_build calls.
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_build_json_frame(PyObject* /*self*/, PyObject* args) {
    PyObject* obj;
    int opcode;
    if (!PyArg_ParseTuple(args, "Oi", &obj, &opcode)) return nullptr;

    // Serialize Python object to JSON using buffer pool
    auto buf = acquire_buffer();
    if (write_json(obj, buf, 0) < 0) {
        release_buffer(std::move(buf));
        return nullptr;
    }

    size_t json_len = buf.size();
    size_t hdr_size = ws_frame_header_size(json_len);
    size_t total = hdr_size + json_len;

    // Single allocation: frame header + JSON payload
    PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total);
    if (!result) {
        release_buffer(std::move(buf));
        return nullptr;
    }

    uint8_t* out = (uint8_t*)PyBytes_AS_STRING(result);
    ws_write_frame_header(out, (WsOpcode)opcode, json_len);
    memcpy(out + hdr_size, buf.data(), json_len);

    release_buffer(std::move(buf));
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// ws_parse_frames_json(buffer: bytearray) -> (consumed, [(opcode, parsed_obj|bytes), ...], pong_bytes|None)
// Parses WS frames and for TEXT frames, directly parses JSON payload via yyjson.
// For BINARY frames, returns raw bytes. For close, returns close payload as bytes.
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_parse_frames_json(PyObject* /*self*/, PyObject* arg) {
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;
    size_t total_consumed = 0;

    PyRef frames(PyList_New(0));
    if (!frames) { PyBuffer_Release(&buf); return nullptr; }

    std::vector<uint8_t> pong_buf;

    while (total_consumed < data_len) {
        WsFrame frame;
        int consumed = ws_parse_frame(data + total_consumed, data_len - total_consumed, &frame);
        if (consumed <= 0) break;

        if (frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }

        uint8_t opcode = (uint8_t)frame.opcode;

        if (opcode == WS_PING) {
            size_t pong_hdr = ws_frame_header_size((size_t)frame.payload_len);
            size_t old_size = pong_buf.size();
            pong_buf.resize(old_size + pong_hdr + (size_t)frame.payload_len);
            ws_write_frame_header(pong_buf.data() + old_size, WS_PONG, (size_t)frame.payload_len);
            if (frame.payload_len > 0) {
                memcpy(pong_buf.data() + old_size + pong_hdr, frame.payload, (size_t)frame.payload_len);
            }
            total_consumed += (size_t)consumed;
            continue;
        }

        if (opcode == WS_PONG) {
            total_consumed += (size_t)consumed;
            continue;
        }

        // For TEXT frames, try to parse JSON directly
        PyObject* payload_obj;
        if (opcode == WS_TEXT && frame.payload_len > 0) {
            payload_obj = yyjson_parse_to_pyobject(
                (const char*)frame.payload, (size_t)frame.payload_len);
            if (!payload_obj) {
                // JSON parse failed — fallback to str
                PyErr_Clear();
                payload_obj = PyUnicode_DecodeUTF8(
                    (const char*)frame.payload, (Py_ssize_t)frame.payload_len, "surrogateescape");
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(
                (const char*)frame.payload, (Py_ssize_t)frame.payload_len);
        }
        if (!payload_obj) { PyBuffer_Release(&buf); return nullptr; }

        PyRef payload_ref(payload_obj);
        PyRef opcode_obj(PyLong_FromLong(opcode));
        if (!opcode_obj) { PyBuffer_Release(&buf); return nullptr; }
        PyRef tuple(PyTuple_Pack(2, opcode_obj.get(), payload_ref.get()));
        if (!tuple) { PyBuffer_Release(&buf); return nullptr; }

        if (PyList_Append(frames.get(), tuple.get()) < 0) {
            PyBuffer_Release(&buf);
            return nullptr;
        }

        total_consumed += (size_t)consumed;
        if (opcode == WS_CLOSE) break;
    }

    PyBuffer_Release(&buf);

    PyRef py_consumed(PyLong_FromSize_t(total_consumed));
    PyObject* py_pong;
    if (!pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize((const char*)pong_buf.data(), (Py_ssize_t)pong_buf.size());
        if (!py_pong) return nullptr;
    } else {
        Py_INCREF(Py_None);
        py_pong = Py_None;
    }

    return PyTuple_Pack(3, py_consumed.get(), frames.get(), py_pong);
}
