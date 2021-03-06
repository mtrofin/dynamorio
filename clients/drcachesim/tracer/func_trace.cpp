/* **********************************************************
 * Copyright (c) 2016-2018 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

// func_trace.cpp: module for recording function traces

#include <string>
#include <vector>
#include <set>
#include "dr_api.h"
#include "drsyms.h"
#include "drwrap.h"
#include "drmgr.h"
#include "drvector.h"
#include "trace_entry.h"
#include "../common/options.h"
#include "func_trace.h"

// The expected pattern for a single_op_value is:
//     function_name|function_id|arguments_num
// where function_name can contain spaces (for instance, C++ namespace prefix)
#define PATTERN_SEPARATOR "|"

#define NOTIFY(level, ...)                     \
    do {                                       \
        if (op_verbose.get_value() >= (level)) \
            dr_fprintf(STDERR, __VA_ARGS__);   \
    } while (0)

static int func_trace_init_count;

static func_trace_append_entry_t append_entry;
// Should always be called after appending a consecutive number of entries
// in case if the buffer met the redzone after appending entries.
static func_trace_memtrace_if_redzone_t memtrace_if_redzone;
static drvector_t funcs;
static std::string funcs_str, funcs_str_sep;

typedef struct {
    char name[2048]; // probably the maximum length of C/C++ symbol
    int id;
    int arg_num;
} func_metadata_t;

static func_metadata_t *
create_func_metadata(std::string name, int id, int arg_num)
{
    func_metadata_t *f = (func_metadata_t *)dr_global_alloc(sizeof(func_metadata_t));
    strncpy(f->name, name.c_str(), BUFFER_SIZE_ELEMENTS(f->name));
    f->id = id;
    f->arg_num = arg_num;
    return f;
}

static void
delete_func_metadata(func_metadata_t *f)
{
    dr_global_free((void *)f, sizeof(func_metadata_t));
}

static void
free_func_entry(void *entry)
{
    delete_func_metadata((func_metadata_t *)entry);
}

// NOTE: try to avoid invoking any code that could be traced by func_pre_hook
//       (e.g., STL, libc, etc.)
static void
func_pre_hook(void *wrapcxt, INOUT void **user_data)
{
    void *drcontext = drwrap_get_drcontext(wrapcxt);
    if (drcontext == NULL)
        return;

    size_t idx = (size_t)*user_data;
    func_metadata_t *f = (func_metadata_t *)drvector_get_entry(&funcs, (uint)idx);
    app_pc retaddr = drwrap_get_retaddr(wrapcxt);
    append_entry(drcontext, TRACE_MARKER_TYPE_FUNC_ID, (uintptr_t)f->id);
    append_entry(drcontext, TRACE_MARKER_TYPE_FUNC_RETADDR, (uintptr_t)retaddr);
    for (int i = 0; i < f->arg_num; i++) {
        uintptr_t arg_i = (uintptr_t)drwrap_get_arg(wrapcxt, i);
        append_entry(drcontext, TRACE_MARKER_TYPE_FUNC_ARG, arg_i);
    }
    memtrace_if_redzone(drcontext);
}

// NOTE: try to avoid invoking any code that could be traced by func_post_hook
//       (e.g., STL, libc, etc.)
static void
func_post_hook(void *wrapcxt, void *user_data)
{
    void *drcontext = drwrap_get_drcontext(wrapcxt);
    if (drcontext == NULL)
        return;

    size_t idx = (size_t)user_data;
    func_metadata_t *f = (func_metadata_t *)drvector_get_entry(&funcs, (uint)idx);
    uintptr_t retval = (uintptr_t)drwrap_get_retval(wrapcxt);
    append_entry(drcontext, TRACE_MARKER_TYPE_FUNC_ID, (uintptr_t)f->id);
    append_entry(drcontext, TRACE_MARKER_TYPE_FUNC_RETVAL, retval);
    memtrace_if_redzone(drcontext);
}

static app_pc
get_pc_by_symbol(const module_data_t *mod, const char *symbol)
{
    if (mod == NULL || symbol == NULL)
        return NULL;

    // Try to find the symbol in the dynamic symbol table.
    app_pc pc = (app_pc)dr_get_proc_address(mod->handle, symbol);
    if (pc != NULL) {
        NOTIFY(1, "dr_get_proc_address found symbol %s at pc=" PFX "\n", symbol, pc);
        return pc;
    } else {
        // If failed to find the symbol in the dynamic symbol table, then we try to find
        // it in the module loaded by reading the module file in mod->full_path.
        // NOTE: mod->full_path could be invalid in the case where the original
        // module file is remapped and deleted (e.g. hugepage_text).
        // FIXME: find a way to find the PC of the symbol even if the original module file
        // is deleted.
        size_t offset;
        drsym_error_t err =
            drsym_lookup_symbol(mod->full_path, symbol, &offset, DRSYM_DEMANGLE);
        if (err == DRSYM_SUCCESS) {
            pc = mod->start + offset;
            NOTIFY(1, "drsym_lookup_symbol found symbol %s at pc=" PFX "\n", symbol, pc);
            return pc;
        } else {
            NOTIFY(1, "Failed to find symbol %s, drsym_error_t=%d\n", symbol, err);
            return NULL;
        }
    }
}

static void
instru_funcs_module_load(void *drcontext, const module_data_t *mod, bool loaded)
{
    if (drcontext == NULL || mod == NULL)
        return;

    NOTIFY(1, "instru_funcs_module_load, mod->full_path=%s\n",
           mod->full_path == NULL ? "" : mod->full_path);
    for (size_t i = 0; i < funcs.entries; i++) {
        func_metadata_t *f = (func_metadata_t *)drvector_get_entry(&funcs, (uint)i);
        app_pc f_pc = get_pc_by_symbol(mod, f->name);
        if (f_pc != NULL) {
            if (drwrap_wrap_ex(f_pc, func_pre_hook, func_post_hook, (void *)i, 0)) {
                NOTIFY(1, "Inserted hooks for function %s\n", f->name);
            } else {
                NOTIFY(1, "Failed to insert hooks for function %s\n", f->name);
            }
        }
    }
}

static std::vector<std::string>
split_by(std::string s, std::string sep)
{
    size_t pos;
    std::vector<std::string> vec;
    do {
        pos = s.find(sep);
        vec.push_back(s.substr(0, pos));
        s.erase(0, pos + sep.length());
    } while (pos != std::string::npos);
    return vec;
}

static void
init_funcs_str_and_sep()
{
    if (op_record_heap.get_value())
        funcs_str = op_record_heap_value.get_value();
    else
        funcs_str = "";
    funcs_str_sep = op_record_function.get_value_separator();
    DR_ASSERT(funcs_str_sep == op_record_heap_value.get_value_separator());
    std::string op_value = op_record_function.get_value();
    if (!funcs_str.empty() && !op_value.empty())
        funcs_str += funcs_str_sep;
    funcs_str += op_value;
}

bool
func_trace_init(func_trace_append_entry_t append_entry_,
                func_trace_memtrace_if_redzone_t memtrace_if_redzone_)
{
    if (dr_atomic_add32_return_sum(&func_trace_init_count, 1) > 1)
        return true;

    init_funcs_str_and_sep();
    // If there is no function specified to trace,
    // then the whole func_trace module doesn't have to do anything.
    if (funcs_str.empty())
        return true;

    auto op_values = split_by(funcs_str, funcs_str_sep);
    std::set<int> existing_ids;
    if (!drvector_init(&funcs, (uint)op_values.size(), false, free_func_entry)) {
        DR_ASSERT(false);
        goto failed;
    }
    append_entry = append_entry_;
    memtrace_if_redzone = memtrace_if_redzone_;

    for (auto &single_op_value : op_values) {
        auto items = split_by(single_op_value, PATTERN_SEPARATOR);
        if (items.size() != 3) {
            NOTIFY(0,
                   "Warning: -record_function or -record_heap_value was not"
                   " passed a triplet, input=%s\n",
                   funcs_str.c_str());
            continue;
        }
        std::string name = items[0];
        int id = atoi(items[1].c_str());
        int arg_num = atoi(items[2].c_str());
        if (name.empty()) {
            NOTIFY(0, "Warning: -record_function name should not be empty");
            continue;
        }
        if (existing_ids.find(id) != existing_ids.end()) {
            NOTIFY(0,
                   "Warning: duplicated function id in -record_function or"
                   " -record_heap_value, input=%s\n",
                   funcs_str.c_str());
            continue;
        }

        dr_log(NULL, DR_LOG_ALL, 1, "Trace func name=%s, id=%d, arg_num=%d\n",
               name.c_str(), id, arg_num);
        existing_ids.insert(id);
        drvector_append(&funcs, create_func_metadata(name, id, arg_num));
    }

    if (!(drsym_init(0) == DRSYM_SUCCESS)) {
        DR_ASSERT(false);
        goto failed;
    }
    if (!drwrap_init()) {
        DR_ASSERT(false);
        goto failed;
    }
    if (!drmgr_register_module_load_event(instru_funcs_module_load)) {
        DR_ASSERT(false);
        goto failed;
    }

    return true;
failed:
    func_trace_exit();
    return false;
}

void
func_trace_exit()
{
    if (dr_atomic_add32_return_sum(&func_trace_init_count, -1) != 0)
        return;

    if (funcs_str.empty())
        return;
    if (!drvector_delete(&funcs))
        DR_ASSERT(false);
    if (!drmgr_unregister_module_load_event(instru_funcs_module_load) ||
        !(drsym_exit() == DRSYM_SUCCESS))
        DR_ASSERT(false);
    drwrap_exit();
}
