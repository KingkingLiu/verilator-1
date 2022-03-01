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
// V3DynamicScheduler's Transformations:
//
//      Each assignment with an intra assignment delay:
//         Transform 'a = #delay b' into:
//             temp = b;
//             #delay a = temp;
//         Transform 'a <= #delay b' into:
//             temp = b;
//             fork
//                 #delay a = temp;
//             join_none
//
//      Each Delay, TimingControl, Wait:
//          Mark containing task for dynamic scheduling
//          Mark containing process for dynamic scheduling
//      Each CFunc:
//          If it's virtual and any overriding/overridden func is marked, mark it
//      Each CFunc calling a marked CFunc:
//          Mark it for dynamic scheduling
//      Each process calling a marked CFunc:
//          Mark it for dynamic scheduling
//      Each variable written to in a marked process/task:
//          Mark it for dynamic scheduling
//      Each always process:
//          If marked and has no sentree:
//              Transform process into an initial process with a body like this:
//                  forever
//                      process_body;
//          If waiting on a marked variable:
//              Transform process into an initial process with a body like this:
//                  forever
//                      @(sensp) begin
//                          process_body;
//                      end
//              Mark it
//
//      Each AssignDly:
//          If in marked process:
//              Transform into:
//                  fork @__VdlyEvent__ lhsp = rhsp; join_none
//
//      Each Fork:
//          Move each statement to a new function
//          Add call to new function in place of moved statement
//
//      Each TimingControl, Wait:
//          Create event variables for triggering those.
//          For Wait:
//              Transform into:
//                  while (!wait.condp)
//                      @(vars from wait.condp);
//                  wait.bodysp;
//              (process the new TimingControl as specified below)
//          For TimingControl:
//              If waiting on event, leave it as is
//              If waiting on posedge:
//                  Create a posedge event variable for the awaited signal
//              If waiting on negedge:
//                  Create a negedge event variable for the awaited signal
//              If waiting on bothedge:
//                  Split it into posedge and negedge
//                  Create a posedge event variable for the awaited signal
//                  Create a negedge event variable for the awaited signal
//              If waiting on anyedge:
//                  Create a anyedge event variable for the awaited signal
//              For each variable in the condition being waited on:
//                  Create an anyedge event variable for the awaited variable
//
//      Each assignment:
//          If there is an edge event variable associated with the LHS:
//              Create an EventTrigger for this event variable under an if that checks if the edge
//              occurs
//      Each clocked var:
//          If there is an edge event variable associated with it:
//              Create a new Active for this edge with an EventTrigger for this event variable
//
//      Each class with an event member:
//          In the destructor, notify the dispatcher that the event var doesn't exist anymore.
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3DynamicScheduler.h"
#include "V3Ast.h"
#include "V3EmitCBase.h"

static AstVarScope* getCreateEvent(AstVar* varp, AstScope* scopep, VEdgeType edgeType) {
    if (auto* eventp = varp->edgeEvent(edgeType)) return eventp;
    string newvarname = (string("__VedgeEvent__") + scopep->nameDotless() + "__" + edgeType.ascii()
                         + "__" + varp->name());
    auto* newvarp = new AstVar{varp->fileline(), AstVarType::VAR, newvarname,
                               varp->findBasicDType(AstBasicDTypeKwd::EVENTVALUE)};
    newvarp->sigPublic(true);
    scopep->modp()->addStmtp(newvarp);
    auto* newvscp = new AstVarScope{varp->fileline(), scopep, newvarp};
    scopep->addVarp(newvscp);
    varp->edgeEvent(edgeType, newvscp);
    return newvscp;
}

static AstVarScope* getCreateEvent(AstVarScope* vscp, VEdgeType edgeType) {
    UASSERT_OBJ(vscp->scopep(), vscp, "Var unscoped");
    return getCreateEvent(vscp->varp(), vscp->scopep(), edgeType);
}

//######################################################################
// Mark nodes affected by dynamic scheduling

class DynamicSchedulerMarkDynamicVisitor final : public AstNVisitor {
private:
    // TYPES
    struct Overrides {
        std::unordered_set<AstCFunc*> nodes;

        auto begin() { return nodes.begin(); }
        auto end() { return nodes.end(); }
        bool operator[](AstCFunc* nodep) const { return nodes.find(nodep) != nodes.end(); }
        void insert(AstCFunc* nodep) { nodes.insert(nodep); }
    };

    // NODE STATE
    //  AstCFunc::user1()      -> bool.  Set true if node has been processed
    AstUser1InUse m_inuser1;

    // STATE
    std::unordered_map<AstCFunc*, Overrides> m_overrides;  // Maps CFuncs to CFuncs overriding them/overridden by them
    AstClass* m_classp = nullptr;  // Current class
    bool m_dynamic = false;  // Are we in a dynamically scheduled process/function?
    bool m_repeat = false;  // Re-run this visitor?

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // If we mark the current process/function as dynamically scheduled, we will need to repeat the whole process
    void setDynamic() {
        if (!m_dynamic) m_repeat = true;
        m_dynamic = true;
    }
    // Extract class member of a given name (it's needed as this is after V3Class)
    static AstNode* findMember(AstClass* classp, const std::string& name) {
        for (AstNode* itemp = VN_CAST(classp->membersp(), Scope)->blocksp(); itemp; // TODO: CHANGE THIS! FIRST MEMBER IS NOT NECESSARILY SCOPE
             itemp = itemp->nextp()) {
            if (itemp->name() == name) return itemp;
        }
        return nullptr;
    }

    // VISITORS
    virtual void visit(AstClass* nodep) override {
        VL_RESTORER(m_classp);
        m_classp = nodep;
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeProcedure* nodep) override {
        VL_RESTORER(m_dynamic);
        m_dynamic = nodep->isDynamic();
        iterateChildren(nodep);
        nodep->isDynamic(m_dynamic);
    }
    virtual void visit(AstAlways* nodep) override {
        auto* sensesp = nodep->sensesp();
        // Transform if the process is marked and has no sentree
        bool transform = !sensesp && nodep->bodysp() && nodep->isDynamic();
        // Transform if the process is waiting on a dynamic var
        if (!transform)
            transform = sensesp && sensesp->sensesp() && sensesp->sensesp()->varp()
                        && sensesp->sensesp()->varp()->isDynamic();
        if (transform) {
            auto fl = nodep->fileline();
            auto* bodysp = nodep->bodysp();
            if (bodysp) bodysp->unlinkFrBackWithNext();
            if (sensesp) {
                sensesp = sensesp->cloneTree(false);
                for (auto* senitemp = sensesp->sensesp(); senitemp;
                     senitemp = VN_CAST(senitemp->nextp(), SenItem)) {
                    if (senitemp->varp()->isEventValue()) continue;
                    auto* eventp = getCreateEvent(
                        senitemp->varp(), senitemp->varScopep()->scopep(), senitemp->edgeType());
                    auto* newSenitemp = new AstSenItem{
                        senitemp->fileline(), VEdgeType::ET_ANYEDGE,
                        new AstVarRef{senitemp->fileline(), eventp, VAccess::READ}};
                    senitemp->replaceWith(newSenitemp);
                    VL_DO_DANGLING(senitemp->deleteTree(), senitemp);
                    senitemp = newSenitemp;
                }
                bodysp = new AstBegin{fl, "", bodysp};
                bodysp = new AstTimingControl{fl, sensesp, bodysp};
            }
            auto* whilep = new AstWhile{fl, new AstConst{fl, AstConst::BitTrue()}, bodysp};
            auto* initialp = new AstInitial{fl, whilep};
            nodep->replaceWith(initialp);
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
            visit(initialp);
        } else {
            visit(static_cast<AstNodeProcedure*>(nodep));
        }
    }
    virtual void visit(AstCFunc* nodep) override {
        VL_RESTORER(m_dynamic);
        m_dynamic = nodep->isCoroutine();
        iterateChildren(nodep);
        if (nodep->isVirtual() && !nodep->user1SetOnce()) {
            for (auto* cextp = m_classp->extendsp(); cextp;
                 cextp = VN_CAST(cextp->nextp(), ClassExtends)) {
                auto* cfuncp = VN_CAST(findMember(cextp->classp(), nodep->name()), CFunc);
                if (!cfuncp) continue;
                m_overrides[nodep].insert(cfuncp);
                m_overrides[cfuncp].insert(nodep);
            }
        }
        if (!m_dynamic) return;
        nodep->rtnType("VerilatedCoroutine");
        for (auto cfuncp : m_overrides[nodep]) {
            if (cfuncp->isCoroutine()) continue;
            cfuncp->rtnType("VerilatedCoroutine");
            m_repeat = true;
        }
    }
    virtual void visit(AstDelay* nodep) override {
        setDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeAssign* nodep) override {
        if (nodep->delayp()) setDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstTimingControl* nodep) override {
        setDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstWait* nodep) override {
        setDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstFork* nodep) override {
        // If we're waiting for child processes, we're dynamic
        if (!nodep->joinType().joinNone()) setDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeCCall* nodep) override {
        if (nodep->funcp()->isCoroutine()) setDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstVarRef* nodep) override {
        if (m_dynamic && nodep->access().isWriteOrRW()) { nodep->varp()->isDynamic(true); }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerMarkDynamicVisitor(AstNetlist* nodep) {
        do {
            m_repeat = false;
            iterate(nodep);
        } while (m_repeat);
    }
    virtual ~DynamicSchedulerMarkDynamicVisitor() override {}
};

//######################################################################

class DynamicSchedulerIntraAssignDelayVisitor final : public AstNVisitor {
private:
    // STATE
    using VarMap = std::map<const std::pair<AstNodeModule*, string>, AstVar*>;
    VarMap m_modVarMap;  // Table of new var names created under module
    size_t m_count = 0;

    // METHODS
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
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstAssign* nodep) override {
        if (auto* delayp = nodep->delayp()) {
            delayp->unlinkFrBack();
            auto* lhsp = VN_CAST(nodep->lhsp(), VarRef);
            auto* newvscp
                = getCreateVar(lhsp->varScopep(),
                               "__Vintraval" + std::to_string(m_count++) + "__" + lhsp->name());
            nodep->addHereThisAsNext(new AstAssign{
                nodep->fileline(), new AstVarRef{nodep->fileline(), newvscp, VAccess::WRITE},
                nodep->rhsp()->unlinkFrBack()});
            nodep->rhsp(new AstVarRef{nodep->fileline(), newvscp, VAccess::READ});
            nodep->addHereThisAsNext(new AstDelay{delayp->fileline(), delayp, nullptr});
        }
    }
    virtual void visit(AstAssignDly* nodep) override {
        if (auto* delayp = nodep->delayp()) {
            auto* lhsp = VN_CAST(nodep->lhsp(), VarRef);
            auto* newvscp
                = getCreateVar(lhsp->varScopep(),
                               "__Vintraval" + std::to_string(m_count++) + "__" + lhsp->name());
            nodep->addHereThisAsNext(new AstAssign{
                nodep->fileline(), new AstVarRef{nodep->fileline(), newvscp, VAccess::WRITE},
                nodep->rhsp()->unlinkFrBack()});
            nodep->rhsp(new AstVarRef{nodep->fileline(), newvscp, VAccess::READ});
            delayp->unlinkFrBack();
            auto* forkp = new AstFork{nodep->fileline(), "", nullptr};
            forkp->joinType(VJoinType::JOIN_NONE);
            nodep->replaceWith(forkp);
            forkp->addStmtsp(new AstDelay{delayp->fileline(), delayp, nodep});
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerIntraAssignDelayVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerIntraAssignDelayVisitor() override {}
};

//######################################################################
// Transform forks

class DynamicSchedulerForkVisitor final : public AstNVisitor {
private:
    // NODE STATE
    //  AstFork::user1()      -> bool.  Set true if node has been processed
    AstUser1InUse m_inuser1;

    // STATE
    AstScope* m_scopep = nullptr;
    std::map<AstVarScope*, AstVarScope*> m_locals;
    size_t m_count = 0;
    AstVar* m_joinEventp;
    AstVar* m_joinCounterp;
    AstClassRefDType* m_joinDTypep;
    AstCFunc* m_joinNewp;

    enum { FORK, GATHER, REPLACE } m_mode = FORK;

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstScope* nodep) override {
        m_scopep = nodep;
        iterateChildren(nodep);
        m_scopep = nullptr;
    }
    virtual void visit(AstVarRef* nodep) override {
        if (m_mode == GATHER) {
            if (nodep->varp()->varType() == AstVarType::BLOCKTEMP)
                m_locals.insert(std::make_pair(nodep->varScopep(), nullptr));
        } else if (m_mode == REPLACE) {
            if (auto* newvscp = m_locals[nodep->varScopep()]) {
                nodep->varScopep(newvscp);
                nodep->varp(newvscp->varp());
            }
        }
    }
    virtual void visit(AstFork* nodep) override {
        if (m_mode != FORK) {
            iterateChildren(nodep);
            return;
        }
        if (nodep->user1SetOnce()) return;

        AstVarScope* joinVscp = nullptr;
        if (!nodep->joinType().joinNone()) {
            auto* joinVarp
                = new AstVar{nodep->fileline(), AstVarType::BLOCKTEMP,
                             "__Vfork__" + std::to_string(m_count++) + "__join", m_joinDTypep};
            joinVarp->funcLocal(true);
            joinVscp = new AstVarScope{joinVarp->fileline(), m_scopep, joinVarp};
            m_scopep->addVarp(joinVscp);
            nodep->addHereThisAsNext(joinVarp);
        }

        VL_RESTORER(m_mode);
        auto stmtp = nodep->stmtsp();
        vluint32_t joinCount = 0;
        while (stmtp) {
            m_locals.clear();
            m_mode = GATHER;
            iterateChildren(stmtp);
            if (joinVscp) m_locals.insert(std::make_pair(joinVscp, nullptr));

            AstCFunc* const cfuncp = new AstCFunc{stmtp->fileline(),
                                                  "__Vfork__" + std::to_string(m_count++) + "__"
                                                      + std::to_string(joinCount++),
                                                  m_scopep, "VerilatedCoroutine"};
            m_scopep->addActivep(cfuncp);

            // Create list of arguments and move to function
            AstNode* argsp = nullptr;
            for (auto& p : m_locals) {
                auto* varscp = p.first;
                auto* varp = varscp->varp()->cloneTree(false);
                varp->funcLocal(true);
                varp->direction(VDirection::INPUT);
                cfuncp->addArgsp(varp);
                AstVarScope* const newvscp = new AstVarScope{varp->fileline(), m_scopep, varp};
                m_scopep->addVarp(newvscp);
                p.second = newvscp;
                argsp = AstNode::addNext(argsp,
                                         new AstVarRef{stmtp->fileline(), varscp, VAccess::READ});
            }
            auto* ccallp = new AstCCall{stmtp->fileline(), cfuncp, argsp};
            stmtp->replaceWith(ccallp);

            if (joinVscp) {
                auto* counterSelp = new AstMemberSel{
                    nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::WRITE},
                    m_joinCounterp->dtypep()};
                counterSelp->varp(m_joinCounterp);
                stmtp->addNext(
                    new AstAssign{nodep->fileline(), counterSelp,
                                  new AstSub{nodep->fileline(), counterSelp->cloneTree(false),
                                             new AstConst{nodep->fileline(), 1}}});
                auto* eventSelp = new AstMemberSel{
                    nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::WRITE},
                    m_joinEventp->dtypep()};
                eventSelp->varp(m_joinEventp);
                stmtp->addNext(new AstEventTrigger{nodep->fileline(), eventSelp});
            }

            cfuncp->addStmtsp(stmtp);
            m_mode = REPLACE;
            iterateChildren(cfuncp);
            stmtp = ccallp->nextp();
        }

        if (joinVscp) {
            auto* cnewp = new AstCNew{nodep->fileline(), m_joinNewp, nullptr};
            cnewp->dtypep(m_joinDTypep);
            auto* assignp
                = new AstAssign{nodep->fileline(),
                                new AstVarRef{nodep->fileline(), joinVscp, VAccess::WRITE}, cnewp};
            nodep->addHereThisAsNext(assignp);

            auto* counterSelp = new AstMemberSel{
                nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::WRITE},
                m_joinCounterp->dtypep()};
            counterSelp->varp(m_joinCounterp);
            if (joinCount > 0 && nodep->joinType().joinAny()) joinCount = 1;
            assignp = new AstAssign{nodep->fileline(), counterSelp,
                                    new AstConst{nodep->fileline(), joinCount}};
            nodep->addHereThisAsNext(assignp);

            counterSelp = new AstMemberSel{
                nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::READ},
                m_joinCounterp->dtypep()};
            counterSelp->varp(m_joinCounterp);
            auto* eventSelp = new AstMemberSel{
                nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::READ},
                m_joinEventp->dtypep()};
            eventSelp->varp(m_joinEventp);
            nodep->addNextHere(new AstWhile{
                nodep->fileline(),
                new AstGt{nodep->fileline(), counterSelp, new AstConst{nodep->fileline(), 0}},
                new AstTimingControl{
                    nodep->fileline(),
                    new AstSenTree{
                        nodep->fileline(),
                        new AstSenItem{nodep->fileline(), VEdgeType::ET_ANYEDGE, eventSelp}},
                    nullptr}});
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerForkVisitor(AstNetlist* nodep) {
        auto* joinClassp = new AstClass{nodep->fileline(), "Join"};
        auto* joinClassPackagep = new AstClassPackage{nodep->fileline(), "Join__Vclpkg"};
        joinClassp->classOrPackagep(joinClassPackagep);
        joinClassPackagep->classp(joinClassp);
        nodep->addModulep(joinClassPackagep);
        nodep->addModulep(joinClassp);
        AstCell* const cellp = new AstCell{joinClassPackagep->fileline(),
                                           joinClassPackagep->fileline(),
                                           joinClassPackagep->name(),
                                           joinClassPackagep->name(),
                                           nullptr,
                                           nullptr,
                                           nullptr};
        cellp->modp(joinClassPackagep);
        v3Global.rootp()->topModulep()->addStmtp(cellp);
        auto* joinScopep = new AstScope{nodep->fileline(), joinClassp, "Join", nullptr, nullptr};
        joinClassp->addMembersp(joinScopep);
        m_joinEventp = new AstVar{nodep->fileline(), AstVarType::MEMBER, "wakeEvent",
                                  nodep->findBasicDType(AstBasicDTypeKwd::EVENTVALUE)};
        joinClassp->addMembersp(m_joinEventp);
        m_joinCounterp = new AstVar{nodep->fileline(), AstVarType::MEMBER, "counter",
                                    nodep->findSigned32DType()};
        joinClassp->addMembersp(m_joinCounterp);
        m_joinDTypep = new AstClassRefDType{nodep->fileline(), joinClassp, nullptr};
        m_joinDTypep->dtypep(m_joinDTypep);
        nodep->typeTablep()->addTypesp(m_joinDTypep);
        m_joinNewp = new AstCFunc{nodep->fileline(), "new", joinScopep, ""};
        m_joinNewp->argTypes(EmitCBaseVisitor::symClassVar());
        m_joinNewp->isConstructor(true);
        joinScopep->addActivep(m_joinNewp);
        iterate(nodep);
    }
    virtual ~DynamicSchedulerForkVisitor() override {}
};

//######################################################################
// Transform delayed/non-blocking assignments

class DynamicSchedulerAssignDlyVisitor final : public AstNVisitor {
private:
    // STATE
    AstVarScope* m_dlyEvent = nullptr;
    AstScope* m_scopep = nullptr;
    bool m_dynamic = false;
    bool m_inFork = false;

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    AstVarScope* getCreateDlyEvent() {
        if (m_dlyEvent) return m_dlyEvent;
        string newvarname = "__VdlyEvent__";
        auto fl = new FileLine{m_scopep->fileline()};
        fl->warnOff(V3ErrorCode::UNOPTFLAT, true);
        auto* newvarp = new AstVar{fl, AstVarType::MODULETEMP, newvarname,
                                   m_scopep->findBasicDType(AstBasicDTypeKwd::EVENTVALUE)};
        m_scopep->modp()->addStmtp(newvarp);
        auto* newvscp = new AstVarScope{fl, m_scopep, newvarp};
        m_scopep->addVarp(newvscp);
        return m_dlyEvent = newvscp;
    }

    // VISITORS
    virtual void visit(AstScope* nodep) override {
        m_scopep = nodep;
        iterateChildren(nodep);
        m_scopep = nullptr;
    }
    virtual void visit(AstNodeProcedure* nodep) override {
        VL_RESTORER(m_dynamic);
        m_dynamic = nodep->isDynamic();
        iterateChildren(nodep);
    }
    virtual void visit(AstCFunc* nodep) override {
        VL_RESTORER(m_dynamic);
        m_dynamic = nodep->isCoroutine();
        iterateChildren(nodep);
    }
    virtual void visit(AstFork* nodep) override {
        VL_RESTORER(m_dynamic);
        m_dynamic = true;
        VL_RESTORER(m_inFork);
        m_inFork = true;
        iterateChildren(nodep);
    }
    virtual void visit(AstBegin* nodep) override {
        VL_RESTORER(m_inFork);
        m_inFork = false;
        iterateChildren(nodep);
    }
    virtual void visit(AstAssignDly* nodep) override {
        if (!m_dynamic) return;
        auto fl = nodep->fileline();
        auto* eventp = getCreateDlyEvent();
        auto* assignp
            = new AstAssign{fl, nodep->lhsp()->unlinkFrBack(), nodep->rhsp()->unlinkFrBack()};
        auto* timingControlp = new AstTimingControl{
            fl,
            new AstSenTree{fl, new AstSenItem{fl, VEdgeType::ET_ANYEDGE,
                                              new AstVarRef{fl, eventp, VAccess::READ}}},
            assignp};
        if (m_inFork) {
            nodep->replaceWith(timingControlp);
        } else {
            auto* forkp = new AstFork{nodep->fileline(), "", timingControlp};
            forkp->joinType(VJoinType::JOIN_NONE);
            nodep->replaceWith(forkp);
        }
        VL_DO_DANGLING(delete nodep, nodep);
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // METHODS
    AstVarScope* getDlyEvent() { return m_dlyEvent; }

    // CONSTRUCTORS
    explicit DynamicSchedulerAssignDlyVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerAssignDlyVisitor() override {}
};

//######################################################################
// Create edge events

class DynamicSchedulerCreateEventsVisitor final : public AstNVisitor {
private:
    // STATE
    using VarScopeSet = std::set<AstVarScope*>;
    VarScopeSet m_waitVars;

    bool m_inTimingControlSens = false;  // Are we under a timing control sens list?
    bool m_inWait = false;  // Are we under a wait statement?
    AstSenItem* m_senItemp = nullptr;  // The senitem we're under

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

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
        if (m_waitVars.empty()) {  // There are no vars to wait on
            if (nodep->bodysp())
                nodep->replaceWith(nodep->bodysp()->unlinkFrBackWithNext());
            else
                nodep->unlinkFrBack();
        } else {
            auto fl = nodep->fileline();
            AstNode* senitemsp = nullptr;
            // Wait on anyedge events related to the vars in the wait statement
            for (auto* vscp : m_waitVars) {
                AstVarScope* eventp = vscp->varp()->isEventValue()
                                          ? vscp
                                          : getCreateEvent(vscp, VEdgeType::ET_ANYEDGE);
                senitemsp = AstNode::addNext(
                    senitemsp, new AstSenItem{fl, VEdgeType::ET_ANYEDGE,
                                              new AstVarRef{fl, eventp, VAccess::READ}});
            }
            auto* condp = nodep->condp()->unlinkFrBack();
            auto* timingControlp = new AstTimingControl{
                fl, new AstSenTree{fl, VN_CAST(senitemsp, SenItem)}, nullptr};
            // Put the timing control in a while loop with the wait statement as condition
            auto* whilep = new AstWhile{fl, new AstLogNot{fl, condp}, timingControlp};
            if (nodep->bodysp()) whilep->addNext(nodep->bodysp()->unlinkFrBackWithNext());
            nodep->replaceWith(whilep);
            m_waitVars.clear();
        }
        VL_DO_DANGLING(nodep->deleteTree(), nodep);
    }
    virtual void visit(AstSenItem* nodep) override {
        VL_RESTORER(m_senItemp);
        m_senItemp = nodep;
        if (m_inTimingControlSens) {
            // Split bothedge into posedge and negedge, to react to those triggers
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
            m_waitVars.insert(nodep->varScopep());
        } else if (m_inTimingControlSens) {
            if (!nodep->varp()->isEventValue()) {
                auto edgeType = m_senItemp->edgeType();
                nodep->varScopep(getCreateEvent(nodep->varScopep(), edgeType));
                nodep->varp(nodep->varScopep()->varp());
            }
        }
    }
    virtual void visit(AstNodeSel* nodep) override {
        iterate(nodep->fromp());
    }
    virtual void visit(AstMemberSel* nodep) override {
        if (m_inWait) {
            m_waitVars.insert(VN_CAST(nodep->fromp(), VarRef)->varScopep());
        } else if (m_inTimingControlSens) {
            if (!nodep->varp()->isEventValue()) {
                auto edgeType = m_senItemp->edgeType();
                nodep->replaceWith(new AstVarRef(
                    nodep->fileline(),
                    getCreateEvent(VN_CAST(nodep->fromp(), VarRef)->varScopep(), edgeType),
                    VAccess::READ));
                VL_DO_DANGLING(nodep->deleteTree(), nodep);
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
// Add triggers for edge events

class DynamicSchedulerAddTriggersVisitor final : public AstNVisitor {
private:
    // NODE STATE
    //  AstNodeAssign::user1()      -> bool.  Set true if node has been processed
    AstUser1InUse m_inuser1;

    // STATE
    using VarMap = std::map<const std::pair<AstNodeModule*, string>, AstVar*>;
    VarMap m_modVarMap;  // Table of new var names created under module
    size_t m_count = 0;
    AstTopScope* m_topScopep = nullptr;  // Current top scope

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

    // VISITORS
    virtual void visit(AstTopScope* nodep) override {
        m_topScopep = nodep;
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeAssign* nodep) override {
        if (nodep->user1SetOnce()) return;
        if (auto* varrefp = VN_CAST(nodep->lhsp(), VarRef)) {
            auto fl = nodep->fileline();
            if (!varrefp->varp()->hasEdgeEvents()) return;
            auto* newvscp
                = getCreateVar(varrefp->varScopep(),
                               "__Vprevval" + std::to_string(m_count++) + "__" + varrefp->name());
            AstNode* stmtspAfter = nullptr;

            // Trigger posedge event if (~prevval && currentval)
            if (auto* eventp = varrefp->varp()->edgeEvent(VEdgeType::ET_POSEDGE)) {
                stmtspAfter = AstNode::addNext(
                    stmtspAfter,
                    new AstIf{fl,
                              new AstAnd{
                                  fl, new AstNot{fl, new AstVarRef{fl, newvscp, VAccess::READ}},
                                  new AstVarRef{fl, varrefp->varScopep(), VAccess::READ}},
                              new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
            }

            // Trigger negedge event if (prevval && ~currentval)
            if (auto* eventp = varrefp->varp()->edgeEvent(VEdgeType::ET_NEGEDGE)) {
                stmtspAfter = AstNode::addNext(
                    stmtspAfter,
                    new AstIf{fl,
                              new AstAnd{fl, new AstVarRef{fl, newvscp, VAccess::READ},
                                         new AstNot{fl, new AstVarRef{fl, varrefp->varScopep(),
                                                                         VAccess::READ}}},
                              new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
            }

            // Trigger anyedge event if (prevval != currentval)
            if (auto* eventp = varrefp->varp()->edgeEvent(VEdgeType::ET_ANYEDGE)) {
                stmtspAfter = AstNode::addNext(
                    stmtspAfter,
                    new AstIf{fl,
                              new AstNeq{fl, new AstVarRef{fl, newvscp, VAccess::READ},
                                         new AstVarRef{fl, varrefp->varScopep(), VAccess::READ}},
                              new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
            }

            UASSERT(stmtspAfter, "Unhandled edge event!");
            auto* stmtspBefore
                = new AstAssign{fl, new AstVarRef{fl, newvscp, VAccess::WRITE},
                                new AstVarRef{fl, varrefp->varScopep(), VAccess::READ}};
            nodep->addHereThisAsNext(stmtspBefore);
            nodep->addNextHere(stmtspAfter);
        }
    }
    virtual void visit(AstVarScope* nodep) override {
        AstVar* varp = nodep->varp();
        // If a var has edge events and could've been written to from the outside (e.g. the main function), create a clocked Active that triggers the edge events
        if (varp->hasEdgeEvents() && (varp->isUsedClock() || varp->isSigPublic())) {
            auto fl = nodep->fileline();
            for (auto edgeType :
                 {VEdgeType::ET_POSEDGE, VEdgeType::ET_NEGEDGE, VEdgeType::ET_ANYEDGE}) {
                if (auto* eventp = varp->edgeEvent(edgeType)) {
                    auto* activep = new AstActive{
                        fl, "",
                        new AstSenTree{fl,
                                       new AstSenItem{fl,
                                                      edgeType == VEdgeType::ET_ANYEDGE
                                                          ? VEdgeType::ET_BOTHEDGE
                                                          : edgeType, // Use bothedge as anyedge is not clocked
                                                      new AstVarRef{fl, nodep, VAccess::READ}}}};
                    m_topScopep->addSenTreep(activep->sensesp());
                    auto* ifp = new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}};
                    auto* alwaysp
                        = new AstAlways{nodep->fileline(), VAlwaysKwd::ALWAYS, nullptr, ifp};
                    activep->addStmtsp(alwaysp);
                    nodep->scopep()->addActivep(activep);
                }
            }
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerAddTriggersVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerAddTriggersVisitor() override {}
};

//######################################################################
// Manage the lifetime of class member events

class DynamicSchedulerClassEventVisitor final : public AstNVisitor {
private:
    // STATE
    AstClass* m_classp = nullptr;  // Current class
    AstNode* m_resetStmtsp = nullptr;
    AstCFunc* m_constructor = nullptr;
    AstCFunc* m_destructor = nullptr;

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstClass* nodep) override {
        VL_RESTORER(m_classp);
        VL_RESTORER(m_resetStmtsp);
        VL_RESTORER(m_destructor);
        m_classp = nodep;
        iterateChildren(nodep);
        if (m_resetStmtsp) {
            UASSERT_OBJ(m_constructor, nodep, "Class has no constructor");
            UASSERT_OBJ(m_destructor, nodep, "Class has no destructor");
            nodep->addMembersp(new AstVar{nodep->fileline(), AstVarType::MEMBER, "vlSymsp",
                                          nodep->findBasicDType(AstBasicDTypeKwd::SYMSPTR)});
            m_constructor->addStmtsp(
                new AstCStmt{nodep->fileline(), "this->vlSymsp = vlSymsp;\n"});
            m_destructor->addStmtsp(m_resetStmtsp);
        }
    }
    virtual void visit(AstVar* nodep) override {
        if (!m_classp) return;
        if (nodep->dtypep()->basicp() && nodep->dtypep()->basicp()->isEventValue()) {
            m_resetStmtsp = AstNode::addNext(
                m_resetStmtsp,
                new AstCStmt{nodep->fileline(), "vlSymsp->__Vm_eventDispatcher.cancel("
                                                    + nodep->nameProtect() + ");\n"});
        }
    }
    virtual void visit(AstCFunc* nodep) override {
        if (nodep->isDestructor())
            m_destructor = nodep;
        else if (nodep->isConstructor())
            m_constructor = nodep;
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit DynamicSchedulerClassEventVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~DynamicSchedulerClassEventVisitor() override {}
};

//######################################################################
// DynamicScheduler class functions

void V3DynamicScheduler::processes(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    UINFO(2, "  Transform Intra Assign Delays...\n");
    { DynamicSchedulerIntraAssignDelayVisitor visitor(nodep); }
    V3Global::dumpCheckGlobalTree("dsch_transf_intra", 0,
                                  v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Mark Dynamic...\n");
    { DynamicSchedulerMarkDynamicVisitor visitor(nodep); }
    V3Global::dumpCheckGlobalTree("dsch_mark_dyn", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Transform AssignDlys in Suspendable Processes...\n");
    {
        DynamicSchedulerAssignDlyVisitor visitor(nodep);
        UINFO(2, "  Add AstResumeTriggered...\n");
        auto fl = nodep->fileline();
        auto* activep = new AstActive{fl, "resumeTriggered",
                                      new AstSenTree{fl, new AstSenItem{fl, AstSenItem::Combo()}}};
        activep->sensesStorep(activep->sensesp());
        activep->addStmtsp(new AstResumeTriggered{
            fl, visitor.getDlyEvent() ? new AstVarRef{fl, visitor.getDlyEvent(), VAccess::WRITE}
                                      : nullptr});
        nodep->topScopep()->scopep()->addActivep(activep);
    }
    V3Global::dumpCheckGlobalTree("dsch_transf_dly", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Move Forked Processes to New Functions...\n");
    { DynamicSchedulerForkVisitor visitor(nodep); }
    V3Global::dumpCheckGlobalTree("dsch_procs", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3DynamicScheduler::events(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    UINFO(2, "  Add Edge Events...\n");
    DynamicSchedulerCreateEventsVisitor createEventsVisitor(nodep);
    V3Global::dumpCheckGlobalTree("dsch_make_events", 0,
                                  v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Add Edge Event Triggers...\n");
    DynamicSchedulerAddTriggersVisitor addTriggersVisitor(nodep);
    V3Global::dumpCheckGlobalTree("dsch_add_triggers", 0,
                                  v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Done.\n");
    V3Global::dumpCheckGlobalTree("dsch_events", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3DynamicScheduler::classes(AstNetlist* nodep) {
    { DynamicSchedulerClassEventVisitor visitor(nodep); }
    V3Global::dumpCheckGlobalTree("dsch_classes", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
