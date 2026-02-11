#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// ══════════════════════════════════════════════════════════════════════════════
// compute_dependency_order(dependencies: List[Tuple[str, List[str]]]) → List[str]
//
// Topological sort using Kahn's algorithm.
// Input: list of (node_name, [dependency_names])
// Output: list of node names in execution order
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_compute_dependency_order(PyObject* self, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list of (str, list) tuples");
        return nullptr;
    }

    Py_ssize_t n = PyList_GET_SIZE(arg);

    // Build graph
    std::unordered_map<std::string, int> name_to_idx;
    std::vector<std::string> names;
    std::vector<std::vector<int>> adj;
    std::vector<int> in_degree;

    // First pass: collect all node names
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = PyList_GET_ITEM(arg, i);
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) < 2) continue;

        PyObject* name_obj = PyTuple_GET_ITEM(item, 0);
        const char* name = PyUnicode_AsUTF8(name_obj);
        if (!name) continue;

        if (name_to_idx.find(name) == name_to_idx.end()) {
            name_to_idx[name] = (int)names.size();
            names.emplace_back(name);
        }
    }

    size_t num_nodes = names.size();
    adj.resize(num_nodes);
    in_degree.resize(num_nodes, 0);

    // Second pass: build edges
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = PyList_GET_ITEM(arg, i);
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) < 2) continue;

        const char* name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(item, 0));
        PyObject* deps = PyTuple_GET_ITEM(item, 1);
        if (!name || !PyList_Check(deps)) continue;

        auto it = name_to_idx.find(name);
        if (it == name_to_idx.end()) continue;
        int node_idx = it->second;

        Py_ssize_t ndeps = PyList_GET_SIZE(deps);
        for (Py_ssize_t j = 0; j < ndeps; j++) {
            const char* dep_name = PyUnicode_AsUTF8(PyList_GET_ITEM(deps, j));
            if (!dep_name) continue;
            auto dep_it = name_to_idx.find(dep_name);
            if (dep_it != name_to_idx.end()) {
                adj[dep_it->second].push_back(node_idx);
                in_degree[node_idx]++;
            }
        }
    }

    // Kahn's algorithm
    std::vector<int> queue;
    for (size_t i = 0; i < num_nodes; i++) {
        if (in_degree[i] == 0) queue.push_back((int)i);
    }

    std::vector<int> order;
    size_t qi = 0;
    while (qi < queue.size()) {
        int node = queue[qi++];
        order.push_back(node);
        for (int neighbor : adj[node]) {
            if (--in_degree[neighbor] == 0) {
                queue.push_back(neighbor);
            }
        }
    }

    if (order.size() != num_nodes) {
        PyErr_SetString(PyExc_ValueError, "Circular dependency detected");
        return nullptr;
    }

    // Build result list
    PyRef result(PyList_New((Py_ssize_t)order.size()));
    if (!result) return nullptr;

    for (size_t i = 0; i < order.size(); i++) {
        PyObject* name_str = PyUnicode_FromString(names[order[i]].c_str());
        if (!name_str) return nullptr;
        PyList_SET_ITEM(result.get(), (Py_ssize_t)i, name_str);
    }

    return result.release();
}
