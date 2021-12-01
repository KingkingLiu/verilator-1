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

class DynamicSchedulerProcessVisitor final : public AstNVisitor {
private:
    // NODE STATE
    // AstNodeProcedure::user1()      -> bool.  Set true if shouldn't be split up
    AstUser1InUse m_inuser1;

    // STATE
    AstNodeProcedure* m_process = nullptr;

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstNodeProcedure* nodep) override {
        VL_RESTORER(m_process);
        {
            m_process = nodep;
            iterateChildren(nodep);
            if (nodep->user1()) {
                // Prevent splitting by wrapping body in an AstBegin
                auto* bodysp = nodep->bodysp()->unlinkFrBackWithNext();
                nodep->addStmtp(new AstBegin{nodep->fileline(), "", bodysp});
            }
        }
    }
    virtual void visit(AstDelay* nodep) override {
        if (m_process) m_process->user1u(true);
    }
    virtual void visit(AstTimingControl* nodep) override {
        if (m_process) m_process->user1u(true);
    }
    virtual void visit(AstWait* nodep) override {
        if (m_process) m_process->user1u(true);
    }
    virtual void visit(AstFork* nodep) override {
        if (m_process) m_process->user1u(!nodep->joinType().joinNone());
    }
    virtual void visit(AstTaskRef* nodep) override {
        // XXX detect only tasks with delays etc
        if (m_process) m_process->user1u(true);
    }
    virtual void visit(AstCMethodCall* nodep) override {
        // XXX detect only tasks with delays etc
        if (m_process) m_process->user1u(true);
    }
    virtual void visit(AstMethodCall* nodep) override {
        // XXX detect only tasks with delays etc
        if (m_process) m_process->user1u(true);
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerProcessVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerProcessVisitor() override {}
};

//######################################################################

class DynamicSchedulerAssignDlyVisitor final : public AstNVisitor {
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

    std::map<std::pair<int, int>, AstVarScope*> m_dimVars;
    std::unordered_map<int, AstVarScope*> m_lsbVars;
    std::unordered_map<AstNodeDType*, AstVarScope*> m_valVars;

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
                string bitvarname = "__Vdlyvdim" + cvtToStr(dimension) + "__"
                                    + cvtToStr(dimp->width()) + "bit__v" + cvtToStr(modVecNum);
                AstVarScope* bitvscp;
                auto it = m_dimVars.find(std::make_pair(dimension, dimp->width()));
                if (it != m_dimVars.end())
                    bitvscp = it->second;
                else {
                    bitvscp
                        = createVarSc(varrefp->varScopep(), bitvarname, dimp->width(), nullptr);
                    m_dimVars.insert(
                        std::make_pair(std::make_pair(dimension, dimp->width()), bitvscp));
                }
                AstAssign* bitassignp = new AstAssign(
                    nodep->fileline(), new AstVarRef(nodep->fileline(), bitvscp, VAccess::WRITE),
                    dimp);
                if (stmtsp)
                    stmtsp->addNext(bitassignp);
                else
                    stmtsp = bitassignp;
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
                string bitvarname = "__Vdlyvlsb__" + cvtToStr(lsbvaluep->width()) + "bit__v"
                                    + cvtToStr(modVecNum);
                AstVarScope* bitvscp;
                auto it = m_lsbVars.find(lsbvaluep->width());
                if (it != m_lsbVars.end())
                    bitvscp = it->second;
                else {
                    bitvscp = createVarSc(varrefp->varScopep(), bitvarname, lsbvaluep->width(),
                                          nullptr);
                    m_lsbVars.insert(std::make_pair(lsbvaluep->width(), bitvscp));
                }
                AstAssign* bitassignp = new AstAssign(
                    nodep->fileline(), new AstVarRef(nodep->fileline(), bitvscp, VAccess::WRITE),
                    lsbvaluep);
                if (stmtsp)
                    stmtsp->addNext(bitassignp);
                else
                    stmtsp = bitassignp;
                bitreadp = new AstVarRef(nodep->fileline(), bitvscp, VAccess::READ);
            }
        }
        //=== Value: __Vdlyvval__
        AstNode* valreadp;  // Code to read Vdlyvval
        if (VN_IS(nodep->rhsp(), Const)) {
            // vval = constant, can just push constant into where we use it
            valreadp = nodep->rhsp()->unlinkFrBack();
        } else {
            auto* dtypep = nodep->rhsp()->dtypep();
            string valvarname = dtypep->name();
            for (int i = 0; i < valvarname.length(); i++)
                if (valvarname[i] == '.') valvarname[i] = '_';
            valvarname = "__Vdlyvval__" + valvarname + cvtToStr(dtypep->width()) + "__v"
                         + cvtToStr(modVecNum);
            AstVarScope* valvscp;
            auto it = m_valVars.find(dtypep);
            if (it != m_valVars.end())
                valvscp = it->second;
            else {
                valvscp = createVarSc(varrefp->varScopep(), valvarname, 0, dtypep);
                m_valVars.insert(std::make_pair(dtypep, valvscp));
            }
            valreadp = new AstVarRef(nodep->fileline(), valvscp, VAccess::READ);
            auto* valassignp = new AstAssign(
                nodep->fileline(), new AstVarRef(nodep->fileline(), valvscp, VAccess::WRITE),
                nodep->rhsp()->unlinkFrBack());
            if (stmtsp)
                stmtsp->addNext(valassignp);
            else
                stmtsp = valassignp;
        }

        AstNode* selectsp = varrefp;
        for (int dimension = int(dimreadps.size()) - 1; dimension >= 0; --dimension) {
            selectsp = new AstArraySel(nodep->fileline(), selectsp, dimreadps[dimension]);
        }
        if (bitselp) {
            selectsp = new AstSel(nodep->fileline(), selectsp, bitreadp,
                                  bitselp->widthp()->cloneTree(false));
        }
        auto* assignDlyp = new AstAssignDly(nodep->fileline(), selectsp,
                                            valreadp);  // nodep->rhsp()->unlinkFrBack());
        assignDlyp->user1SetOnce();
        if (stmtsp)
            stmtsp->addNext(assignDlyp);
        else
            stmtsp = assignDlyp;
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
        m_dimVars.clear();
        m_lsbVars.clear();
        m_valVars.clear();
        iterateChildren(nodep);
    }
    virtual void visit(AstAssignDly* nodep) override {
        if (nodep->user1SetOnce()) return;
        if (m_cfuncp && m_cfuncp->funcPublic()) {
            nodep->v3warn(E_UNSUPPORTED,
                          "Unsupported: Delayed assignment inside public function/task");
        }
        if (VN_IS(nodep->lhsp(), ArraySel) || (VN_IS(nodep->lhsp(), Sel))) {

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
    explicit DynamicSchedulerAssignDlyVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerAssignDlyVisitor() override {}
};

//######################################################################

using VarEdge = std::pair<AstVarScope*, VEdgeType>;
using VarEdgeEventMap = std::map<VarEdge, AstVarScope*>;

class DynamicSchedulerCreateEventsVisitor final : public AstNVisitor {
private:
    // NODE STATE
    // AstVar::user1()      -> bool.  Set true if variable is waited on
    AstUser1InUse m_inuser1;

    // STATE
    using VarScopeSet = std::set<AstVarScope*>;
    VarScopeSet m_waitVars;

public:
    VarEdgeEventMap m_edgeEvents;

private:
    bool m_inTimingControlSens = false;
    bool m_inWait = false;

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()
    AstVarScope* getCreateEvent(AstVarScope* vscp, VEdgeType edgeType) {
        UASSERT_OBJ(vscp->scopep(), vscp, "Var unscoped");
        auto it = m_edgeEvents.find(std::make_pair(vscp, edgeType));
        if (it != m_edgeEvents.end()) return it->second;
        string newvarname = (string("__VedgeEvent__") + vscp->scopep()->nameDotless() + "__"
                             + edgeType.ascii() + "__" + vscp->varp()->name());
        auto* newvarp = new AstVar(vscp->fileline(), AstVarType::MODULETEMP, newvarname,
                                   vscp->findBasicDType(AstBasicDTypeKwd::EVENTVALUE));
        vscp->scopep()->modp()->addStmtp(newvarp);
        auto* newvscp = new AstVarScope(vscp->fileline(), vscp->scopep(), newvarp);
        vscp->user1p(newvscp);
        vscp->scopep()->addVarp(newvscp);
        m_edgeEvents.insert(std::make_pair(std::make_pair(vscp, edgeType), newvscp));
        return newvscp;
    }
    AstVarScope* getEvent(AstVarScope* vscp, VEdgeType edgeType) {
        auto it = m_edgeEvents.find(std::make_pair(vscp, edgeType));
        if (it != m_edgeEvents.end()) return it->second;
        return nullptr;
    }

    // VISITORS
    virtual void visit(AstTimingControl* nodep) override {
        VL_RESTORER(m_inTimingControlSens);
        m_inTimingControlSens = true;
        iterateAndNextNull(nodep->sensesp());
        m_inTimingControlSens = false;
        iterateAndNextNull(nodep->stmtsp());
    }
    virtual void visit(AstWait* nodep) override {
        VL_RESTORER(m_inWait);
        m_inWait = true;
        iterateAndNextNull(nodep->condp());
        if (m_waitVars.empty()) {
            if (nodep->bodysp())
                nodep->replaceWith(nodep->bodysp()->unlinkFrBack());
            else
                nodep->unlinkFrBack();
        } else {
            auto fl = nodep->fileline();
            AstNode* senitemsp = nullptr;
            for (auto* vscp : m_waitVars) {
                AstVarScope* eventp = vscp->varp()->dtypep()->basicp()->isEventValue()
                                          ? vscp
                                          : getCreateEvent(vscp, VEdgeType::ET_ANYEDGE);
                senitemsp = AstNode::addNext(
                    senitemsp, new AstSenItem{fl, VEdgeType::ET_ANYEDGE,
                                              new AstVarRef{fl, eventp, VAccess::READ}});
            }
            auto* condp = nodep->condp()->unlinkFrBack();
            auto* timingControlp = new AstTimingControl{
                fl, new AstSenTree{fl, VN_CAST(senitemsp, SenItem)}, nullptr};
            auto* whilep = new AstWhile{fl, new AstLogNot{fl, condp}, timingControlp};
            if (nodep->bodysp()) whilep->addNext(nodep->bodysp()->unlinkFrBack());
            nodep->replaceWith(whilep);
            m_waitVars.clear();
        }
        VL_DO_DANGLING(nodep->deleteTree(), nodep);
    }
    virtual void visit(AstSenItem* nodep) override {
        if (m_inTimingControlSens) {
            if (nodep->edgeType() == VEdgeType::ET_BOTHEDGE) {
                nodep->addNextHere(nodep->cloneTree(false));
                nodep->edgeType(VEdgeType::ET_POSEDGE);
                VN_CAST(nodep->nextp(), SenItem)->edgeType(VEdgeType::ET_NEGEDGE);
            }
        }
        iterateChildren(nodep);
    }
    virtual void visit(AstVarRef* nodep) override {
        if (m_inWait) {
            nodep->varp()->user1u(1);
            m_waitVars.insert(nodep->varScopep());
        } else if (m_inTimingControlSens) {
            if (!nodep->varp()->dtypep()->basicp()->isEventValue()) {
                nodep->varp()->user1u(1);
                auto edgeType = VN_CAST(nodep->backp(), SenItem)->edgeType();
                nodep->varScopep(getCreateEvent(nodep->varScopep(), edgeType));
                nodep->varp(nodep->varScopep()->varp());
            }
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerCreateEventsVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerCreateEventsVisitor() override {}
};

//######################################################################

class DynamicSchedulerAddTriggersVisitor final : public AstNVisitor {
private:
    // NODE STATE
    // AstVar::user1()      -> bool.  Set true if variable is waited on
    // AstUser1InUse    m_inuser1;      (Allocated for use in DynamicSchedulerCreateEventsVisitor)
    // AstNode::user2()      -> bool.  Set true if node has been processed
    AstUser2InUse m_inuser2;

    // STATE
    VarEdgeEventMap m_edgeEvents;
    using VarMap = std::map<const std::pair<AstNodeModule*, string>, AstVar*>;
    VarMap m_modVarMap;  // Table of new var names created under module
    size_t m_count = 0;

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    AstVarScope* getCreateVar(AstVarScope* oldvarscp, const string& name) {
        UASSERT_OBJ(oldvarscp->scopep(), oldvarscp, "Var unscoped");
        AstVar* varp;
        AstNodeModule* addmodp = oldvarscp->scopep()->modp();
        // We need a new AstVar, but only one for all scopes, to match the new AstVarScope
        const auto it = m_modVarMap.find(make_pair(addmodp, name));
        if (it != m_modVarMap.end()) {
            // Created module's AstVar earlier under some other scope
            varp = it->second;
        } else {
            varp = new AstVar{oldvarscp->fileline(), AstVarType::BLOCKTEMP, name,
                              oldvarscp->varp()};
            varp->dtypeFrom(oldvarscp);
            addmodp->addStmtp(varp);
            m_modVarMap.emplace(make_pair(addmodp, name), varp);
        }
        AstVarScope* varscp = new AstVarScope{oldvarscp->fileline(), oldvarscp->scopep(), varp};
        oldvarscp->scopep()->addVarp(varscp);
        return varscp;
    }

    AstVarScope* getEvent(AstVarScope* vscp, VEdgeType edgeType) {
        auto it = m_edgeEvents.find(std::make_pair(vscp, edgeType));
        if (it != m_edgeEvents.end()) return it->second;
        return nullptr;
    }

    // VISITORS
    virtual void visit(AstNodeAssign* nodep) override {
        if (nodep->user2SetOnce()) return;
        if (auto* varrefp = VN_CAST(nodep->lhsp(), VarRef)) {
            auto fl = nodep->fileline();
            if (varrefp->varp()->user1u().toInt() == 1) {
                auto* newvscp
                    = getCreateVar(varrefp->varScopep(), "__Vprevval" + std::to_string(m_count++)
                                                             + "__" + varrefp->name());
                nodep->addHereThisAsNext(
                    new AstAssign{fl, new AstVarRef{fl, newvscp, VAccess::WRITE},
                                  new AstVarRef{fl, varrefp->varScopep(), VAccess::READ}});

                if (auto* eventp = getEvent(varrefp->varScopep(), VEdgeType::ET_POSEDGE)) {
                    nodep->addNextHere(new AstIf{
                        fl,
                        new AstAnd{fl,
                                   new AstLogNot{fl, new AstVarRef{fl, newvscp, VAccess::READ}},
                                   new AstVarRef{fl, varrefp->varScopep(), VAccess::READ}},
                        new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
                }

                if (auto* eventp = getEvent(varrefp->varScopep(), VEdgeType::ET_NEGEDGE)) {
                    nodep->addNextHere(new AstIf{
                        fl,
                        new AstAnd{fl, new AstVarRef{fl, newvscp, VAccess::READ},
                                   new AstLogNot{fl, new AstVarRef{fl, varrefp->varScopep(),
                                                                   VAccess::READ}}},
                        new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
                }

                if (auto* eventp = getEvent(varrefp->varScopep(), VEdgeType::ET_ANYEDGE)) {
                    nodep->addNextHere(new AstIf{
                        fl,
                        new AstNeq{fl, new AstVarRef{fl, newvscp, VAccess::READ},
                                   new AstVarRef{fl, varrefp->varScopep(), VAccess::READ}},
                        new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
                }
            }
        }
    }
    virtual void visit(AstVarScope* nodep) override {
        AstVar* varp = nodep->varp();
        if (varp->user1u().toInt() == 1
            && (varp->isUsedClock() || (varp->isSigPublic() && varp->direction().isNonOutput()))) {
            auto fl = nodep->fileline();
            for (auto edgeType :
                 {VEdgeType::ET_ANYEDGE, VEdgeType::ET_POSEDGE, VEdgeType::ET_NEGEDGE}) {
                if (auto* eventp = getEvent(nodep, edgeType)) {
                    auto* activep = new AstActive{
                        fl, "",
                        new AstSenTree{fl,
                                       new AstSenItem{fl, edgeType,
                                                      new AstVarRef{fl, nodep, VAccess::READ}}}};
                    activep->sensesStorep(activep->sensesp());
                    auto* ifp = new AstIf{
                        fl, new AstLogNot{fl, new AstVarRef{fl, eventp, VAccess::READ}},
                        new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}};
                    activep->addStmtsp(ifp);
                    nodep->addNextHere(activep);
                }
            }
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerAddTriggersVisitor(
        DynamicSchedulerCreateEventsVisitor& createEventsVisitor, AstNetlist* nodep)
        : m_edgeEvents(std::move(createEventsVisitor.m_edgeEvents)) {
        iterate(nodep);
    }
    virtual ~DynamicSchedulerAddTriggersVisitor() override {}
};

//######################################################################

class DynamicSchedulerEventTriggeredVisitor final : public AstNVisitor {
private:
    // NODE STATE
    // AstEventTrigger::user1()      -> bool.  Set true if node has been processed
    // AstUser1InUse    m_inuser1;      (Allocated for use in DynamicSchedulerCreateEventsVisitor)

    // STATE

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstEventTrigger* nodep) override {
        if (nodep->user1SetOnce()) return;
        nodep->addHereThisAsNext(new AstAssign{
            nodep->fileline(),
            new AstVarRef{nodep->fileline(), VN_CAST(nodep->trigger(), VarRef)->varScopep(),
                          VAccess::WRITE},
            new AstConst{nodep->fileline(), AstConst::BitTrue()}});
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerEventTriggeredVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerEventTriggeredVisitor() override {}
};

//######################################################################
// DynamicScheduler class functions

void V3DynamicScheduler::process(AstNetlist* nodep) {
    { DynamicSchedulerProcessVisitor visitor(nodep); }
    V3Global::dumpCheckGlobalTree("dynsch_proc", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3DynamicScheduler::dynSched(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    UINFO(2, "  Add Edge Events...\n");
    DynamicSchedulerCreateEventsVisitor createEventsVisitor(nodep);
    V3Global::dumpCheckGlobalTree("dynsch_make_events", 0,
                                  v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Add Edge Event Triggers...\n");
    DynamicSchedulerAddTriggersVisitor addTriggersVisitor(createEventsVisitor, nodep);
    V3Global::dumpCheckGlobalTree("dynsch_add_triggers", 0,
                                  v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Add event.triggered Assignments...\n");
    DynamicSchedulerEventTriggeredVisitor eventTriggerVisitor(nodep);
    UINFO(2, "  Done.\n");
    V3Global::dumpCheckGlobalTree("dynsch", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
