// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Prepare AST for dynamic scheduler features
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2022 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef _V3DYNAMIC_SCHEDULER_H_
#define _V3DYNAMIC_SCHEDULER_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Error.h"
#include "V3Ast.h"

//============================================================================

class V3DynamicScheduler final {
public:
    static void processes(AstNetlist* nodep);
    static void events(AstNetlist* nodep);
    static void classes(AstNetlist* nodep);
};

#endif  // Guard
