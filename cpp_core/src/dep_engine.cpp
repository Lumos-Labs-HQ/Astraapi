#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

// ══════════════════════════════════════════════════════════════════════════════
// Pre-compiled dependency plan registry
// ══════════════════════════════════════════════════════════════════════════════

struct DepNode {
    std::string name;
    PyObject* metadata;  // strong ref — the original Python dict
    std::vector<int> dependencies;  // indices into the plan's node list
};

struct DepPlan {
    std::vector<DepNode> nodes;
    std::vector<int> execution_order;  // topological order (indices)
};

static std::unordered_map<uint64_t, DepPlan> g_dep_plans;
static std::shared_mutex g_dep_mutex;

// ══════════════════════════════════════════════════════════════════════════════
// compile_dep_plan(route_id: u64, nodes: PyList) → None
//
// Each node in nodes is a dict: {name, depends_on: [str], ...metadata}
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_compile_dep_plan(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"route_id", "nodes", nullptr};
    unsigned long long route_id;
    PyObject* nodes_list;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "KO", (char**)kwlist,
            &route_id, &nodes_list)) return nullptr;

    if (!PyList_Check(nodes_list)) {
        PyErr_SetString(PyExc_TypeError, "nodes must be a list");
        return nullptr;
    }

    DepPlan plan;
    Py_ssize_t n = PyList_GET_SIZE(nodes_list);

    // Build name → index map
    std::unordered_map<std::string, int> name_to_idx;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* node_dict = PyList_GET_ITEM(nodes_list, i);
        if (!PyDict_Check(node_dict)) continue;

        DepNode node;
        PyObject* name_obj = PyDict_GetItemString(node_dict, "name");
        node.name = name_obj ? PyUnicode_AsUTF8(name_obj) : "";
        Py_INCREF(node_dict);
        node.metadata = node_dict;

        name_to_idx[node.name] = (int)plan.nodes.size();
        plan.nodes.push_back(std::move(node));
    }

    // Resolve dependencies
    for (size_t i = 0; i < plan.nodes.size(); i++) {
        PyObject* deps = PyDict_GetItemString(plan.nodes[i].metadata, "depends_on");
        if (deps && PyList_Check(deps)) {
            Py_ssize_t ndeps = PyList_GET_SIZE(deps);
            for (Py_ssize_t j = 0; j < ndeps; j++) {
                const char* dep_name = PyUnicode_AsUTF8(PyList_GET_ITEM(deps, j));
                if (dep_name) {
                    auto it = name_to_idx.find(dep_name);
                    if (it != name_to_idx.end()) {
                        plan.nodes[i].dependencies.push_back(it->second);
                    }
                }
            }
        }
    }

    // Topological sort (Kahn's algorithm)
    size_t num_nodes = plan.nodes.size();
    std::vector<int> in_degree(num_nodes, 0);
    std::vector<std::vector<int>> adj(num_nodes);

    for (size_t i = 0; i < num_nodes; i++) {
        for (int dep : plan.nodes[i].dependencies) {
            adj[dep].push_back((int)i);
            in_degree[i]++;
        }
    }

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

    plan.execution_order = std::move(order);

    {
        std::unique_lock lock(g_dep_mutex);
        // Clean up old plan
        auto it = g_dep_plans.find(route_id);
        if (it != g_dep_plans.end()) {
            for (auto& node : it->second.nodes) {
                Py_XDECREF(node.metadata);
            }
        }
        g_dep_plans[route_id] = std::move(plan);
    }

    Py_RETURN_NONE;
}

// ══════════════════════════════════════════════════════════════════════════════
// unregister_dep_plan(route_id: u64) → None
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_unregister_dep_plan(PyObject* self, PyObject* arg) {
    unsigned long long route_id = PyLong_AsUnsignedLongLong(arg);
    if (route_id == (unsigned long long)-1 && PyErr_Occurred()) return nullptr;

    std::unique_lock lock(g_dep_mutex);
    auto it = g_dep_plans.find(route_id);
    if (it != g_dep_plans.end()) {
        for (auto& node : it->second.nodes) {
            Py_XDECREF(node.metadata);
        }
        g_dep_plans.erase(it);
    }

    Py_RETURN_NONE;
}

// ══════════════════════════════════════════════════════════════════════════════
// get_dep_plan(route_id: u64) → Optional[PyList]
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_get_dep_plan(PyObject* self, PyObject* arg) {
    unsigned long long route_id = PyLong_AsUnsignedLongLong(arg);
    if (route_id == (unsigned long long)-1 && PyErr_Occurred()) return nullptr;

    std::shared_lock lock(g_dep_mutex);
    auto it = g_dep_plans.find(route_id);
    if (it == g_dep_plans.end()) Py_RETURN_NONE;

    const auto& plan = it->second;
    PyRef result(PyList_New((Py_ssize_t)plan.nodes.size()));
    if (!result) return nullptr;

    for (size_t i = 0; i < plan.nodes.size(); i++) {
        Py_INCREF(plan.nodes[i].metadata);
        PyList_SET_ITEM(result.get(), (Py_ssize_t)i, plan.nodes[i].metadata);
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// prepare_dep_execution(route_id, request, response, background_tasks, dependency_cache)
// → PyList of execution step dicts
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_prepare_dep_execution(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "route_id", "request", "response", "background_tasks", "dependency_cache", nullptr
    };

    unsigned long long route_id;
    PyObject* request;
    PyObject* response;
    PyObject* background_tasks;
    PyObject* dep_cache;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "KOOOO", (char**)kwlist,
            &route_id, &request, &response, &background_tasks, &dep_cache)) {
        return nullptr;
    }

    std::shared_lock lock(g_dep_mutex);
    auto it = g_dep_plans.find(route_id);
    if (it == g_dep_plans.end()) {
        lock.unlock();
        return PyList_New(0);
    }

    const auto& plan = it->second;

    PyRef result(PyList_New(0));
    if (!result) return nullptr;

    // Emit execution steps in topological order
    for (int idx : plan.execution_order) {
        if (idx < 0 || idx >= (int)plan.nodes.size()) continue;
        const auto& node = plan.nodes[idx];

        PyRef step(PyDict_New());
        if (!step) return nullptr;

        // Copy metadata
        Py_INCREF(node.metadata);
        PyRef meta_copy(node.metadata);

        PyRef py_name(PyUnicode_FromString(node.name.c_str()));
        PyDict_SetItemString(step.get(), "name", py_name.get());
        PyDict_SetItemString(step.get(), "metadata", meta_copy.get());

        // Add context objects
        PyDict_SetItemString(step.get(), "request", request);
        PyDict_SetItemString(step.get(), "response", response);
        PyDict_SetItemString(step.get(), "background_tasks", background_tasks);
        PyDict_SetItemString(step.get(), "dependency_cache", dep_cache);

        PyList_Append(result.get(), step.get());
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// dep_plan_count() → int
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_dep_plan_count(PyObject* self, PyObject* args) {
    std::shared_lock lock(g_dep_mutex);
    return PyLong_FromSsize_t((Py_ssize_t)g_dep_plans.size());
}

// ══════════════════════════════════════════════════════════════════════════════
// Module shutdown: release all PyObject* refs held in the dep plan registry
// ══════════════════════════════════════════════════════════════════════════════

void cleanup_dep_plans() {
    std::unique_lock lock(g_dep_mutex);
    for (auto& [id, plan] : g_dep_plans) {
        for (auto& node : plan.nodes) {
            Py_XDECREF(node.metadata);
            node.metadata = nullptr;
        }
    }
    g_dep_plans.clear();
}
