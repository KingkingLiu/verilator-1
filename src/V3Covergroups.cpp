// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Parse module/signal name references
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
// Covergroup TRANSFORMATIONS:
//      Top-down traversal
//          TODO: describe
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Covergroups.h"

#include "V3Ast.h"
#include "V3Config.h"
#include "V3Global.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################

class CovergroupsVisitor final : public VNVisitor {
private:
    // NODE STATE
    // Cleared on netlist

    // TYPES

    // STATE
    AstNodeModule* m_modp = nullptr;
    // METHODS

    // VISITs
    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        {
            m_modp = nodep;
            iterateChildren(nodep);
        }
    }
    void visit(AstCovergroup* nodep) override {
        AstClass* classp = new AstClass{nodep->fileline(), nodep->name()};
        nodep->replaceWith(classp);
    }
    void visit(AstVar* nodep) override {
        // Only covergroup instantions have to be handled in this phase
        if (!VN_IS(nodep->subDTypep(), CovergroupRefDType)) return;
        AstCovergroup* covergroupp = VN_AS(nodep->subDTypep(), CovergroupRefDType)->covergroupp();
        for (AstNode* stmtp = covergroupp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            // AstCoverpoint* pointp = VN_AS(stmtp, Coverpoint);
            FileLine* const fl = nodep->fileline();
            AstBegin* blockp = new AstBegin{fl, nodep->name() + "__incrementation_block", nullptr};

            AstSenTree* sentreep = new AstSenTree{fl, covergroupp->sensesp()->cloneTree(false)};
            AstAlways* alwaysp = new AstAlways{fl, VAlwaysKwd::ALWAYS, sentreep, blockp};
            m_modp->addStmtsp(alwaysp);
        }
        //        nodep->unlinkFrBack()->deleteTree();
    }

    void visit(AstNode* nodep) override {
        // Default: Just iterate
        iterateChildren(nodep);
    }

public:
    // CONSTRUCTORS
    explicit CovergroupsVisitor(AstNetlist* rootp) { iterate(rootp); }
    ~CovergroupsVisitor() override = default;
};

//######################################################################
// Link class functions

void V3Covergroups::covergroups(AstNetlist* rootp) {
    UINFO(4, __FUNCTION__ << ": " << endl);
    { CovergroupsVisitor{rootp}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("covergroups", 0, dumpTree() >= 6);
}
