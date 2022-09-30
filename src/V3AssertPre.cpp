// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Collect and print statistics
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2005-2022 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//  Pre steps:
//      Attach clocks to each assertion
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3AssertPre.h"

#include "V3Ast.h"
#include "V3Global.h"

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Assert class functions

class AssertPreVisitor final : public VNVisitor {
    // Removes clocks and other pre-optimizations
    // Eventually inlines calls to sequences, properties, etc.
    // We're not parsing the tree, or anything more complicated.
private:
    // NODE STATE/TYPES
    using RefVec = std::vector<AstFuncRef*>;
    using PropertyToRefsMap = std::unordered_map<AstProperty*, RefVec>;
    using PropertyToFuncMap = std::unordered_map<AstProperty*, AstFunc*>;
    PropertyToRefsMap m_propRefs;  // map properties to its references
    PropertyToFuncMap m_propFuncs;  // map old properties to function that replaced them
    // STATE
    // Reset each module:
    AstSenItem* m_seniDefaultp = nullptr;  // Default sensitivity (from AstDefClock)
    // Reset each assertion:
    AstSenItem* m_senip = nullptr;  // Last sensitivity
    // Reset each always:
    AstSenItem* m_seniAlwaysp = nullptr;  // Last sensitivity in always
    // Reset each assertion:
    AstNode* m_disablep = nullptr;  // Last disable

    // METHODS

    AstSenTree* newSenTree(AstNode* nodep) {
        // Create sentree based on clocked or default clock
        // Return nullptr for always
        AstSenTree* newp = nullptr;
        AstSenItem* senip = m_senip;
        if (!senip) senip = m_seniDefaultp;
        if (!senip) senip = m_seniAlwaysp;
        if (!senip) {
            nodep->v3warn(E_UNSUPPORTED, "Unsupported: Unclocked assertion");
            newp = new AstSenTree(nodep->fileline(), nullptr);
        } else {
            newp = new AstSenTree(nodep->fileline(), senip->cloneTree(true));
        }
        return newp;
    }
    void clearAssertInfo() {
        m_senip = nullptr;
        m_disablep = nullptr;
    }

    // VISITORS
    //========== Statements
    void visit(AstClocking* nodep) override {
        UINFO(8, "   CLOCKING" << nodep << endl);
        // Store the new default clock, reset on new module
        m_seniDefaultp = nodep->sensesp();
        // Trash it, keeping children
        if (nodep->bodysp()) {
            nodep->replaceWith(nodep->bodysp()->unlinkFrBack());
        } else {
            nodep->unlinkFrBack();
        }
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstAlways* nodep) override {
        iterateAndNextNull(nodep->sensesp());
        if (nodep->sensesp()) m_seniAlwaysp = nodep->sensesp()->sensesp();
        iterateAndNextNull(nodep->stmtsp());
        m_seniAlwaysp = nullptr;
    }
    void visit(AstNodeCoverOrAssert* nodep) override {
        if (nodep->sentreep()) return;  // Already processed
        clearAssertInfo();
        // Find Clocking's buried under nodep->exprsp
        iterateChildren(nodep);

        if (AstFuncRef* funcrefp = VN_CAST(nodep->propp(), FuncRef)) {
            if (AstProperty* propp = VN_CAST(funcrefp->taskp(), Property)) {
                // check if this property was already converted to a function
                const auto it = m_propFuncs.find(propp);
                if (it == m_propFuncs.end()) {
                    // it will be converted to AstFunc in visit(AstProperty*)
                    // there will be also sentreep set.
                    m_propRefs[propp].push_back(funcrefp);
                } else {
                    // change reference to a converted function
                    funcrefp->taskp(it->second);
                    // set sentreep to sentreep of expr of that property
                    if (!nodep->immediate())
                        nodep->sentreep(VN_CAST(it->second->user1p()->cloneTree(false), SenTree));
                }
            } else if (AstFunc* funcp = VN_CAST(funcrefp->taskp(), Func)) {
                if (!nodep->immediate() && funcp->user1p())
                    nodep->sentreep(VN_CAST(funcp->user1p()->cloneTree(false), SenTree));
            }
        }

        if (!nodep->immediate()) {
            if (!nodep->sentreep()) nodep->sentreep(newSenTree(nodep));
        }
        clearAssertInfo();
    }
    void visit(AstFell* nodep) override {
        if (nodep->sentreep()) return;  // Already processed
        iterateChildren(nodep);
        FileLine* const fl = nodep->fileline();
        AstNode* exprp = nodep->exprp()->unlinkFrBack();
        if (exprp->width() > 1) exprp = new AstSel(fl, exprp, 0, 1);
        AstNode* const past = new AstPast(fl, exprp, nullptr);
        past->dtypeFrom(exprp);
        exprp = new AstAnd(fl, past, new AstNot(fl, exprp->cloneTree(false)));
        exprp->dtypeSetBit();
        nodep->replaceWith(exprp);
        nodep->sentreep(newSenTree(nodep));
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstPast* nodep) override {
        if (nodep->sentreep()) return;  // Already processed
        iterateChildren(nodep);
        nodep->sentreep(newSenTree(nodep));
    }
    void visit(AstRose* nodep) override {
        if (nodep->sentreep()) return;  // Already processed
        iterateChildren(nodep);
        FileLine* const fl = nodep->fileline();
        AstNode* exprp = nodep->exprp()->unlinkFrBack();
        if (exprp->width() > 1) exprp = new AstSel(fl, exprp, 0, 1);
        AstNode* const past = new AstPast(fl, exprp, nullptr);
        past->dtypeFrom(exprp);
        exprp = new AstAnd(fl, new AstNot(fl, past), exprp->cloneTree(false));
        exprp->dtypeSetBit();
        nodep->replaceWith(exprp);
        nodep->sentreep(newSenTree(nodep));
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstStable* nodep) override {
        if (nodep->sentreep()) return;  // Already processed
        iterateChildren(nodep);
        FileLine* const fl = nodep->fileline();
        AstNode* exprp = nodep->exprp()->unlinkFrBack();
        AstNode* const past = new AstPast(fl, exprp, nullptr);
        past->dtypeFrom(exprp);
        exprp = new AstEq(fl, past, exprp->cloneTree(false));
        exprp->dtypeSetBit();
        nodep->replaceWith(exprp);
        nodep->sentreep(newSenTree(nodep));
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    void visit(AstImplication* nodep) override {
        if (nodep->sentreep()) return;  // Already processed

        FileLine* const fl = nodep->fileline();
        AstNode* const rhsp = nodep->rhsp()->unlinkFrBack();
        AstNode* lhsp = nodep->lhsp()->unlinkFrBack();

        if (m_disablep) lhsp = new AstAnd(fl, new AstNot(fl, m_disablep), lhsp);

        AstNode* const past = new AstPast(fl, lhsp, nullptr);
        past->dtypeFrom(lhsp);
        AstNode* const exprp = new AstOr(fl, new AstNot(fl, past), rhsp);
        exprp->dtypeSetBit();
        nodep->replaceWith(exprp);
        nodep->sentreep(newSenTree(nodep));
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    void visit(AstPropSpec* nodep) override {
        // No need to iterate the body, once replace will get iterated
        iterateAndNextNull(nodep->sensesp());
        if (m_senip)
            nodep->v3warn(E_UNSUPPORTED, "Unsupported: Only one PSL clock allowed per assertion");
        // Block is the new expression to evaluate
        AstNode* blockp = nodep->propp()->unlinkFrBack();
        if (AstNode* const disablep = nodep->disablep()) {
            m_disablep = disablep->cloneTree(false);
            if (VN_IS(nodep->backp(), Cover)) {
                blockp = new AstAnd(disablep->fileline(),
                                    new AstNot(disablep->fileline(), disablep->unlinkFrBack()),
                                    blockp);
            } else {
                blockp = new AstOr(disablep->fileline(), disablep->unlinkFrBack(), blockp);
            }
        }
        // Unlink and just keep a pointer to it, convert to sentree as needed
        m_senip = nodep->sensesp();
        nodep->replaceWith(blockp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    void visit(AstProperty* nodep) override {
        iterateChildren(nodep);
        // Convert property to a function
        // The only statements allowed in AstProperty are AstPropSpec (body) and AstVar
        // (arguments).
        AstNode* propBlockp = nodep->stmtsp()->unlinkFrBackWithNext();
        // while(!VN_IS(propBlockp, Var))
        //     propBlockp = propBlockp->nextp();
        // propBlockp->unlinkFrBack();
        AstFunc* funcp = new AstFunc(nodep->fileline(), nodep->name(), propBlockp,
                                     propBlockp->dtypep()->cloneTree(false));
        funcp->dtypeFrom(propBlockp);
        // Change references of old property to a new function
        // Add also sentreep nodes to asserts
        auto it = m_propRefs.find(nodep);
        if (it != m_propRefs.end()) {
            for (auto& refp : it->second) {
                refp->taskp(funcp);
                AstNodeCoverOrAssert* assertp = VN_CAST(refp->backp(), NodeCoverOrAssert);
                UASSERT_OBJ(assertp, refp, "Property reference not under assert object");
                if (assertp->immediate()) assertp->sentreep(newSenTree(nodep));
            }
        }
        // Add pair <old property, new function> to a map for future property references
        m_propFuncs[nodep] = funcp;
        nodep->replaceWith(funcp);
        funcp->user1p(newSenTree(funcp));
    }

    void visit(AstNodeModule* nodep) override {
        iterateChildren(nodep);
        // Reset defaults
        m_seniDefaultp = nullptr;
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit AssertPreVisitor(AstNetlist* nodep) {
        clearAssertInfo();
        // Process
        iterate(nodep);
    }
    ~AssertPreVisitor() override = default;
};

//######################################################################
// Top Assert class

void V3AssertPre::assertPreAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { AssertPreVisitor{nodep}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("assertpre", 0, dumpTree() >= 3);
}
