#!/usr/bin/env python3
#
# Copyright 2022 by Antmicro. Verilator is free software; you
# can redistribute it and/or modify it under the terms of either the GNU Lesser
# General Public License Version 3 or the Apache License 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Apache-2.0
#
# Based on the work of (Guillaume "Vermeille" Sanchez)[https://github.com/Vermeille/clang-callgraph], licensed under Apache 2.0

from pprint import pprint
from clang.cindex import CursorKind, Index, CompilationDatabase
from collections import defaultdict
import sys
import json
import os
from termcolor import colored

INDEX = None
CLANG_ARGS = []
TRANSLATE_UNITS = {}
PRINTED = []

# by definition constructor is MT safe,
# but functions that constructor call are not!
MT_SAFE_KINDS = [CursorKind.CONSTRUCTOR]
UNSAFE_CALLS = []

def get_diag_info(diag):
    return {
        'severity': diag.severity,
        'location': diag.location,
        'spelling': diag.spelling,
        'ranges': list(diag.ranges),
        'fixits': list(diag.fixits)
    }


def print_node(node, level):
    if fully_qualified_pretty(node) not in PRINTED:
        annotations = get_annotations(node)
        if "MT_SAFE" in annotations or node.kind in MT_SAFE_KINDS:
            color = "green"
        else:
            color = "red"
        print(colored('%s %-35s %-100s %s [%s:%s - %s:%s] \t %s  %s' % (' ' * level,
                    node.kind, fully_qualified_pretty(node), ' ' * (20 - level), node.extent.start.line, node.extent.start.column,
                    node.extent.end.line, node.extent.end.column, node.location.file, annotations), color))
        PRINTED.append(fully_qualified_pretty(node))


def get_annotations(node):
    return [c.displayname for c in node.get_children()
            if c.kind == CursorKind.ANNOTATE_ATTR]


def fully_qualified(c):
    if c is None:
        return ''
    elif c.kind == CursorKind.TRANSLATION_UNIT:
        return ''
    else:
        res = fully_qualified(c.semantic_parent)
        if res != '':
            return res + '::' + c.spelling
        return c.spelling


def fully_qualified_pretty(c):
    if c is None:
        return ''
    elif c.kind == CursorKind.TRANSLATION_UNIT:
        return ''
    else:
        res = fully_qualified(c.semantic_parent)
        if res != '':
            return res + '::' + c.displayname
        return c.displayname


def is_excluded(node, xfiles, xprefs):
    if not node.extent.start.file:
        return False

    for xf in xfiles:
        if node.extent.start.file.name.startswith(xf):
            return True

    fqp = fully_qualified_pretty(node)

    for xp in xprefs:
        if fqp.startswith(xp):
            return True

    return False


def get_call_expr(node, pr=False):
    call_expr = []
    for c in node.get_children():
        call_expr.extend(get_call_expr(c, pr))
    if node.kind == CursorKind.CALL_EXPR:
        call_expr.append(node)
    return call_expr


def find_usr(file, usr, xfiles, xprefs, layer):
    file_cpp = str(file).replace(".h", ".cpp")
    try:
        tu = TRANSLATE_UNITS[file_cpp]
    except KeyError:
        tu = INDEX.parse(file_cpp, CLANG_ARGS)
        for d in tu.diagnostics:
            if d.severity == d.Error or d.severity == d.Fatal:
                pprint(('diags', list(map(get_diag_info, tu.diagnostics))))
                return
        TRANSLATE_UNITS[file_cpp] = tu
    for c in tu.cursor.get_children():
        if c.get_usr() == usr:
            return c
    return None

def find_funcs(node, xfiles, xprefs, level=1):
    funcs = get_call_expr(node)
    for c in funcs:
        if not c.referenced:
            continue
        if is_excluded(c.referenced, xfiles, xprefs):
            continue
        call = c.referenced
        # Don't recurse into already printed nodes
        # it some cases in increases printing nodes by a lot
        if fully_qualified_pretty(call) in PRINTED:
            continue
        if str(call.location.file).endswith(".h"):
            if os.path.exists(str(call.location.file).replace(".h", ".cpp")):
                tu_call = find_usr(call.location.file, call.get_usr(), xfiles, xprefs, level+1)
                if tu_call is not None:
                    call = tu_call

        if call is not None:
            if fully_qualified_pretty(call) not in PRINTED:
                PRINTED.append(fully_qualified_pretty(call))

            if "MT_SAFE" not in get_annotations(call) and call.kind not in MT_SAFE_KINDS:
                return False
            if call.kind == CursorKind.CXX_METHOD or call.kind == CursorKind.CONSTRUCTOR or call.kind == CursorKind.FUNCTION_DECL:
                return find_funcs(call, xfiles, xprefs, level+1)
    return True

def print_funcs(node, xfiles, xprefs, level=1):
    funcs = get_call_expr(node)
    for c in funcs:
        if not c.referenced:
            continue
        if is_excluded(c.referenced, xfiles, xprefs):
            continue
        call = c.referenced
        # Don't recurse into already printed nodes
        # it some cases in increases printing nodes by a lot
        if fully_qualified_pretty(call) in PRINTED:
            continue
        if str(call.location.file).endswith(".h"):
            if os.path.exists(str(call.location.file).replace(".h", ".cpp")):
                tu_call = find_usr(call.location.file, call.get_usr(), xfiles, xprefs, level+1)
                if tu_call is not None:
                    call = tu_call

        if call is not None:
            print_node(call, level)
            if call.kind == CursorKind.CXX_METHOD or call.kind == CursorKind.CONSTRUCTOR or call.kind == CursorKind.FUNCTION_DECL:
                print_funcs(call, xfiles, xprefs, level+1)

def show_info(node, xfiles, xprefs, level=1):
    if node.kind == CursorKind.CXX_METHOD:
        if "MT_SAFE" in get_annotations(node):
            if not find_funcs(node, xfiles, xprefs, level+1):
                global UNSAFE_CALLS
                UNSAFE_CALLS.append(node)
    for c in node.get_children():
        show_info(c, xfiles, xprefs, level+1)

def read_compile_commands(filename):
    if filename.endswith('.json'):
        with open(filename) as compdb:
            return json.load(compdb)
    else:
        return [{'command': '', 'file': filename}]


def read_args(args):
    db = None
    clang_args = []
    excluded_prefixes = []
    excluded_paths = ['/usr']
    search_attributes = []
    edit = False
    i = 0
    while i < len(args):
        if args[i] == '-x':
            i += 1
            excluded_prefixes = args[i].split(',')
        elif args[i] == '-p':
            i += 1
            excluded_paths = args[i].split(',')
        elif args[i][0] == '-':
            clang_args.append(args[i])
        else:
            db = args[i]
        i += 1
    return {
        'db': db,
        'clang_args': clang_args,
        'excluded_prefixes': excluded_prefixes,
        'excluded_paths': excluded_paths,
        'edit': edit,
        'search_attributes': search_attributes
    }

def show_ast(cursor, level=0):
    '''pretty print cursor AST'''
    print_node(cursor, level)
    for c in cursor.get_children():
        show_ast(c, level+1)

def main():
    if len(sys.argv) < 2:
        print('usage: ' + sys.argv[0] + ' file.cpp|compile_database.json '
              '[extra clang args...]')
        return

    cfg = read_args(sys.argv)

    global INDEX
    global CLANG_ARGS
    global TRANSLATE_UNITS
    CLANG_ARGS = cfg['clang_args']

    print('reading source files...')
    files_read = []
    for cmd in read_compile_commands(cfg['db']):
        INDEX = Index.create()
        tu = INDEX.parse(cmd['file'], CLANG_ARGS)
        TRANSLATE_UNITS[cmd['file']] = tu
        if cmd['file'] not in files_read:
            files_read.append(cmd['file'])
        else:
            continue
        print(cmd['file'])
        if not tu:
            parser.error("unable to load input")

        for d in tu.diagnostics:
            if d.severity == d.Error or d.severity == d.Fatal:
                pprint(('diags', list(map(get_diag_info, tu.diagnostics))))
                return
        show_info(tu.cursor, cfg['excluded_paths'], cfg['excluded_prefixes'])

    global UNSAFE_CALLS, PRINTED
    for n in UNSAFE_CALLS:
        PRINTED.clear()
        print_node(n, 0)
        print_funcs(n, cfg['excluded_paths'], cfg['excluded_prefixes'], 1)
        print("--------------")
    print(f"Unsafe len: {len(UNSAFE_CALLS)}")


if __name__ == '__main__':
    main()
