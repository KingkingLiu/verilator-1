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
    //   *::user1p  -> pointer to AstClass from AstCovergroup and from AstCovergroup to AstClass
    // Cleared on netlist

    // TYPES

    // STATE
    AstNodeModule* m_modp = nullptr;

    // METHODS
    std::string getMemberNameOfConvertedCoverpoint(AstCoverpoint* nodep) {
        // 19.5 section of IEEE Std 1800-2017 describes names of coverpoints
        // Then "__values_occurred" suffix is added
        std::string coverpointName = nodep->stmtp()->name();
        return coverpointName + "__values_occurred";
    }
    void makeImplicitNew(AstClass* nodep) {
        // This function is called when a covergroup is converted into a class.
        // Original classes have constructor added in V3LinkDot.cpp.
        AstFunc* const newp = new AstFunc{nodep->fileline(), "new", nullptr, nullptr};
        newp->isConstructor(true);
        nodep->addMembersp(newp);
        UINFO(8, "Made implicit new for " << nodep->name() << ": " << nodep << endl);
    }
    void makeGetInstCoverage(AstClass* nodep) {
        AstFunc* const getInstCoverage
            = new AstFunc{nodep->fileline(), "get_inst_coverage", nullptr, nullptr};
    }
    AstClass* getConvertClassFromCovergroup(AstCovergroup* nodep) {
        // Convert covergroup into class
        AstClass* classp = new AstClass{nodep->fileline(), nodep->name()};
        makeImplicitNew(classp);
        // Change coverpoints into class fields to remember which values already occurred
        for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstCoverpoint* pointp = VN_AS(stmtp, Coverpoint);
            FileLine* fl = pointp->fileline();
            AstNodeDType* fieldDTypep = new AstPackArrayDType{
                fl, VFlagChildDType{}, new AstBasicDType{fl, VBasicDTypeKwd::BIT},
                new AstRange{fl, new AstConst{fl, 15},
                             new AstConst{fl, 0}}};  // change the first constant to function call
                                                     // that returns the proper size
            AstVar* fieldp
                = new AstVar{fl, VVarType::MEMBER, getMemberNameOfConvertedCoverpoint(pointp),
                             VFlagChildDType{}, fieldDTypep};
            classp->addMembersp(fieldp);
        }
        nodep->user1p(classp);
        classp->user1p(nodep);
        nodep->replaceWith(classp);
        return classp;
    }

    // VISITs
    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        {
            m_modp = nodep;
            iterateChildren(nodep);
        }
    }
    void visit(AstCovergroup* nodep) override { getConvertClassFromCovergroup(nodep); }
    void visit(AstVar* nodep) override {
        // Only covergroup instantions have to be handled in this phase
        if (!VN_IS(nodep->subDTypep(), CovergroupRefDType)) return;
        AstCovergroup* covergroupp = VN_AS(nodep->subDTypep(), CovergroupRefDType)->covergroupp();
        AstClass* classp = VN_AS(covergroupp->user1p(), Class);
        // Convert covergroup into class if it hasn't been done yet
        if (!classp) { classp = getConvertClassFromCovergroup(covergroupp); }
        // Convert covergroup instance into class instance
        AstClassRefDType* classRefp = new AstClassRefDType{nodep->fileline(), classp, nullptr};
        nodep->subDTypep()->replaceWith(classRefp);

        // Add block to mark which values occurred
        for (AstNode* stmtp = covergroupp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstCoverpoint* pointp = VN_AS(stmtp, Coverpoint);
            FileLine* const fl = nodep->fileline();
            AstVar* classFieldp
                = VN_AS(classp->findMember(getMemberNameOfConvertedCoverpoint(pointp)), Var);
            AstAssign* markOccurrencep
                = new AstAssign{fl,
                                new AstSelBit{fl, new AstVarRef{fl, classFieldp, VAccess::WRITE},
                                              pointp->stmtp()->cloneTree(false)},
                                new AstConst{fl, AstConst::BitTrue{}}};
            AstBegin* blockp
                = new AstBegin{fl, nodep->name() + "__incrementation_block", markOccurrencep};

            AstSenTree* sentreep = new AstSenTree{fl, covergroupp->sensesp()->cloneTree(false)};
            AstAlways* alwaysp = new AstAlways{fl, VAlwaysKwd::ALWAYS, sentreep, blockp};
            m_modp->addStmtsp(alwaysp);
        }
    }
    void visit(AstMethodCall* nodep) override {
        // Only covergroup methods have to be handled in this phase, but it is needed to iterate
        // through whole subtree anyway, because one method call may have other method calls in its
        // arguments
        iterateChildren(nodep);

        // Convert covergroup method to a class method
        AstVarRef* varrefp = VN_CAST(nodep->fromp(), VarRef);
        if (!varrefp) return;
        AstCovergroup* covergroupp;
        AstClass* classp;
        if (AstCovergroupRefDType* covergroupRefp
            = VN_CAST(varrefp->varp()->subDTypep(), CovergroupRefDType)) {
            // Instance hasn't been converted yet, it will be done when it's node is visited
            covergroupp = covergroupRefp->covergroupp();
            classp = VN_AS(covergroupp->user1p(), Class);
            if (!classp) {
                // Covergroup hasn't been converted yet, so it will be done now
                classp = getConvertClassFromCovergroup(covergroupp);
            }
        } else if (AstClassRefDType* classRefp
                   = VN_CAST(varrefp->varp()->subDTypep(), ClassRefDType)) {
            classp = classRefp->classp();
            // Class instance may be class instance from the beginning or it may be converted
            // covergroup instance. If it is the converted, it has user1p.
            covergroupp = VN_AS(classp->user1p(), Covergroup);
        }
        if (!covergroupp)  // The method is not covergroup method
            return;
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
    V3Global::dumpCheckGlobalTree("covergroups", 0, dumpTree() >= 3);
}
