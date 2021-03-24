// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Prepare AST for dynamic scheduler features
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2021 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3DynamicScheduler.h"
#include "V3Ast.h"

#include <deque>
#include <map>

//######################################################################

class DynamicSchedulerVisitor final : public AstNVisitor {
private:
    // NODE STATE
    // AstAssignDly::user1()      -> bool.  Set true if already processed
    AstUser1InUse m_inuser1;

    // STATE
    AstCFunc* m_cfuncp = nullptr;  // Current public C Function
    typedef std::map<const std::pair<AstNodeModule*, string>, AstVar*> VarMap;
    VarMap m_modVarMap;  // Table of new var names created under module
    typedef std::unordered_map<const AstVarScope*, int> ScopeVecMap;
    ScopeVecMap m_scopeVecMap;  // Next var number for each scope

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    AstVarScope* createVarSc(AstVarScope* oldvarscp, const string& name,
                             int width /*0==fromoldvar*/, AstNodeDType* newdtypep) {
        // Because we've already scoped it, we may need to add both the AstVar and the AstVarScope
        UASSERT_OBJ(oldvarscp->scopep(), oldvarscp, "Var unscoped");
        AstVar* varp;
        AstNodeModule* addmodp = oldvarscp->scopep()->modp();
        // We need a new AstVar, but only one for all scopes, to match the new AstVarScope
        const auto it = m_modVarMap.find(make_pair(addmodp, name));
        if (it != m_modVarMap.end()) {
            // Created module's AstVar earlier under some other scope
            varp = it->second;
        } else {
            if (newdtypep) {
                varp = new AstVar(oldvarscp->fileline(), AstVarType::BLOCKTEMP, name, newdtypep);
            } else if (width == 0) {
                varp = new AstVar(oldvarscp->fileline(), AstVarType::BLOCKTEMP, name,
                                  oldvarscp->varp());
                varp->dtypeFrom(oldvarscp);
            } else {  // Used for vset and dimensions, so can zero init
                varp = new AstVar(oldvarscp->fileline(), AstVarType::BLOCKTEMP, name,
                                  VFlagBitPacked(), width);
            }
            addmodp->addStmtp(varp);
            m_modVarMap.emplace(make_pair(addmodp, name), varp);
        }

        AstVarScope* varscp = new AstVarScope(oldvarscp->fileline(), oldvarscp->scopep(), varp);
        oldvarscp->scopep()->addVarp(varscp);
        return varscp;
    }

    AstNode* createDlyArray(AstAssignDly* nodep) {
        // Create delayed assignment
        // Find selects
        auto* lhsp = nodep->lhsp()->unlinkFrBack();
        AstNode* newlhsp = nullptr;  // nullptr = unlink old assign
        AstSel* bitselp = nullptr;
        AstArraySel* arrayselp = nullptr;
        if (VN_IS(lhsp, Sel)) {
            bitselp = VN_CAST(lhsp, Sel);
            arrayselp = VN_CAST(bitselp->fromp(), ArraySel);
        } else {
            arrayselp = VN_CAST(lhsp, ArraySel);
        }
        UINFO(4, "AssignDlyArray: " << nodep << endl);
        //
        //=== Dimensions: __Vdlyvdim__
        std::deque<AstNode*> dimvalp;  // Assignment value for each dimension of assignment
        AstNode* dimselp = arrayselp;
        for (; VN_IS(dimselp, ArraySel); dimselp = VN_CAST(dimselp, ArraySel)->fromp()) {
            AstNode* valp = VN_CAST(dimselp, ArraySel)->bitp()->unlinkFrBack();
            dimvalp.push_front(valp);
        }
        AstVarRef* varrefp = VN_CAST(dimselp, VarRef);
        if (!varrefp) varrefp = VN_CAST(VN_CAST(lhsp, Sel)->fromp(), VarRef);
        UASSERT_OBJ(varrefp, nodep, "No var underneath arraysels");
        UASSERT_OBJ(varrefp->varScopep(), varrefp, "Var didn't get varscoped in V3Scope.cpp");
        varrefp->unlinkFrBack();
        AstVar* oldvarp = varrefp->varp();
        int modVecNum = m_scopeVecMap[varrefp->varScopep()]++;
        //
        AstNode* stmtsp = nullptr;
        std::deque<AstNode*> dimreadps;  // Read value for each dimension of assignment
        for (unsigned dimension = 0; dimension < dimvalp.size(); dimension++) {
            AstNode* dimp = dimvalp[dimension];
            if (VN_IS(dimp, Const)) {  // bit = const, can just use it
                dimreadps.push_front(dimp);
            } else {
                string bitvarname = (string("__Vdlyvdim") + cvtToStr(dimension) + "__"
                                     + oldvarp->shortName() + "__v" + cvtToStr(modVecNum));
                AstVarScope* bitvscp
                    = createVarSc(varrefp->varScopep(), bitvarname, dimp->width(), nullptr);
                AstAssign* bitassignp = new AstAssign(
                    nodep->fileline(), new AstVarRef(nodep->fileline(), bitvscp, VAccess::WRITE),
                    dimp);
                if (stmtsp) stmtsp->addNext(bitassignp);
                else stmtsp = bitassignp;
                dimreadps.push_front(new AstVarRef(nodep->fileline(), bitvscp, VAccess::READ));
            }
        }
        //
        //=== Bitselect: __Vdlyvlsb__
        AstNode* bitreadp = nullptr;  // Code to read Vdlyvlsb
        if (bitselp) {
            AstNode* lsbvaluep = bitselp->lsbp()->unlinkFrBack();
            if (VN_IS(bitselp->fromp(), Const)) {
                // vlsb = constant, can just push constant into where we use it
                bitreadp = lsbvaluep;
            } else {
                string bitvarname = (string("__Vdlyvlsb__") + oldvarp->shortName() + "__v"
                                     + cvtToStr(modVecNum));
                AstVarScope* bitvscp
                    = createVarSc(varrefp->varScopep(), bitvarname, lsbvaluep->width(), nullptr);
                AstAssign* bitassignp = new AstAssign(
                    nodep->fileline(), new AstVarRef(nodep->fileline(), bitvscp, VAccess::WRITE),
                    lsbvaluep);
                if (stmtsp) stmtsp->addNext(bitassignp);
                else stmtsp = bitassignp;
                bitreadp = new AstVarRef(nodep->fileline(), bitvscp, VAccess::READ);
            }
        }
        //=== Value: __Vdlyvval__
        AstNode* valreadp;  // Code to read Vdlyvval
        if (VN_IS(nodep->rhsp(), Const)) {
            // vval = constant, can just push constant into where we use it
            valreadp = nodep->rhsp()->unlinkFrBack();
        } else {
            string valvarname
                = (string("__Vdlyvval__") + oldvarp->shortName() + "__v" + cvtToStr(modVecNum));
            AstVarScope* valvscp
                = createVarSc(varrefp->varScopep(), valvarname, 0, nodep->rhsp()->dtypep());
            valreadp = new AstVarRef(nodep->fileline(), valvscp, VAccess::READ);
            auto* valassignp = new AstAssign(nodep->fileline(),
                                             new AstVarRef(nodep->fileline(), valvscp, VAccess::WRITE),
                                             nodep->rhsp()->unlinkFrBack());
            if (stmtsp) stmtsp->addNext(valassignp);
            else stmtsp = valassignp;
        }


        AstNode* selectsp = varrefp;
        for (int dimension = int(dimreadps.size()) - 1; dimension >= 0; --dimension) {
            selectsp = new AstArraySel(nodep->fileline(), selectsp, dimreadps[dimension]);
        }
        if (bitselp) {
            selectsp = new AstSel(nodep->fileline(), selectsp, bitreadp,
                                  bitselp->widthp()->cloneTree(false));
        }
        auto* assignDlyp = new AstAssignDly(nodep->fileline(), selectsp, valreadp);//nodep->rhsp()->unlinkFrBack());
        assignDlyp->user1SetOnce();
        if (stmtsp) stmtsp->addNext(assignDlyp);
        else stmtsp = assignDlyp;
        return stmtsp;
    }

    // VISITORS
    virtual void visit(AstCFunc* nodep) override {
        VL_RESTORER(m_cfuncp);
        {
            m_cfuncp = nodep;
            iterateChildren(nodep);
        }
    }
    virtual void visit(AstActive* nodep) override {
        iterateChildren(nodep);
    }
    virtual void visit(AstAssignDly* nodep) override {
        if (nodep->user1SetOnce()) return;
        if (m_cfuncp) {
            nodep->v3warn(E_UNSUPPORTED,
                          "Unsupported: Delayed assignment inside public function/task");
        }
        if (VN_IS(nodep->lhsp(), ArraySel)
            || (VN_IS(nodep->lhsp(), Sel)
                )) {

            nodep->replaceWith(createDlyArray(nodep));
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        } else {
            iterateChildren(nodep);
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerVisitor() override {}
};

//######################################################################
// Delayed class functions

void V3DynamicScheduler::dynSched(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { DynamicSchedulerVisitor visitor(nodep); }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("dynsched", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
