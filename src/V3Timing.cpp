// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Prepare AST for timing features
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
//
// A suspendable process/task is one that can be suspended using a delay, wait
// statement, or event control, and will be emitted as a coroutine.
//
// A dynamically scheduled process/task is a suspendable process that can be
// resumed at any point in a given time slot.
//
// V3Timing's Transformations:
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
//         (similarly with intra event controls)
//
//      Each Delay:
//          Scale delay according to the timescale
//
//      Each Delay, EventControl, Wait:
//          Mark containing task/process as suspendable
//          Each EventControl, Wait:
//              Mark containing task/process as dynamically scheduled
//      Each CFunc:
//          If calling/overriding/overriden by a suspendable CFunc, mark it suspendable
//          If calling/overriding/overriden by a dynamically scheduled CFunc, mark it dynamically
//          scheduled
//          Suspendable CFunc return type is VerilatedCoroutine
//      Each process:
//          If calling a suspendable CFunc, mark it suspendable
//          If calling a dynamically scheduled CFunc, mark it dynamically scheduled
//      Each variable:
//          If written to by a dynamically scheduled process/task:
//              Mark it as such
//          If written to by a suspendable process/task:
//              Mark it as such (used in other Verilator passes)
//      Each always process:
//          If suspendable and has no sentree:
//              Transform process into an initial process with a body like this:
//                  forever
//                      process_body;
//          If waiting on a variable written to b:
//              Transform process into an initial process with a body like this:
//                  forever
//                      @(sensp) begin
//                          process_body;
//                      end
//              Mark it as dynamically scheduled
//      Each AssignDly:
//          If in a suspendable process:
//              Transform into:
//                  fork @__VdlyEvent__ lhsp = rhsp; join_none
//
//      Each Fork:
//          Move each statement to a separate new function
//          Add call to new function in place of moved statement
//          If fork is not join_none:
//              Create a join struct used for syncing processes
//
//      Each EventControl, Wait:
//          Create event variables for triggering those.
//          For Wait:
//              Transform into:
//                  while (!wait.condp)
//                      @(vars from wait.condp);
//                  wait.bodysp;
//              (process the new EventControl as specified below)
//          For EventControl:
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
//      Each continuous assignment:
//          If there is an edge event variable associated with the LHS:
//              Transform it into an Always with a normal assign (needed for next step)
//
//      Each assignment:
//          If there is an edge event variable associated with the LHS:
//              Create an EventTrigger for this event variable under an if that checks if the edge
//              occurs
//      Each clocked var:
//          If there is an edge event variable associated with it:
//              Create a new Active for this edge with an EventTrigger for this event variable
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3Timing.h"
#include "V3Ast.h"
#include "V3EmitCBase.h"
#include "V3UniqueNames.h"

//######################################################################
// Transform intra assignment delays

class TimingIntraAssignControlVisitor final : public VNVisitor {
private:
    // STATE
    AstScope* m_scopep = nullptr;  // Current scope
    V3UniqueNames m_intraVarNames;  // Temp var name generator
    bool m_underFork = false;  // Are we under a fork?

    // METHODS
    AstVarScope* getCreateIntraVar(AstNode* nodep) {
        auto name = "__Vintraval__" + m_intraVarNames.get(nodep);
        AstNodeModule* modp = m_scopep->modp();
        auto* const varp
            = new AstVar{nodep->fileline(), VVarType::BLOCKTEMP, name, nodep->dtypep()};
        modp->addStmtp(varp);
        AstVarScope* varscp = new AstVarScope{nodep->fileline(), m_scopep, varp};
        m_scopep->addVarp(varscp);
        return varscp;
    }
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstScope* nodep) override {
        m_scopep = nodep;
        iterateChildren(nodep);
        m_scopep = nullptr;
    }
    virtual void visit(AstFork* nodep) override {
        VL_RESTORER(m_underFork);
        m_underFork = true;
        iterateChildren(nodep);
    }
    virtual void visit(AstBegin* nodep) override {
        VL_RESTORER(m_underFork);
        m_underFork = false;
        iterateChildren(nodep);
    }
    virtual void visit(AstAssign* nodep) override {
        if (auto* const controlp = nodep->timingControlp()) {
            controlp->unlinkFrBack();
            auto* const newvscp = getCreateIntraVar(nodep->lhsp());
            auto* const assignp = new AstAssign{
                nodep->fileline(), new AstVarRef{nodep->fileline(), newvscp, VAccess::WRITE},
                nodep->rhsp()->unlinkFrBack()};
            if (m_underFork)
                nodep->replaceWith(new AstBegin{nodep->fileline(), "", assignp});
            else
                nodep->replaceWith(assignp);
            nodep->rhsp(new AstVarRef{nodep->fileline(), newvscp, VAccess::READ});
            if (auto* sensesp = VN_CAST(controlp, SenTree))
                assignp->addNextHere(new AstEventControl{controlp->fileline(), sensesp, nodep});
            else
                assignp->addNextHere(new AstDelay{controlp->fileline(), controlp, nodep});
        }
    }
    virtual void visit(AstAssignW* nodep) override {
        if (auto* const controlp = nodep->timingControlp()) {
            controlp->unlinkFrBack();
            auto* const newvscp = getCreateIntraVar(nodep);
            auto* const alwaysp = new AstAlways{
                nodep->fileline(), VAlwaysKwd::ALWAYS,
                new AstSenTree{nodep->fileline(),
                               new AstSenItem{nodep->fileline(), AstSenItem::Combo()}},
                new AstAssign{nodep->fileline(),
                              new AstVarRef{nodep->fileline(), newvscp, VAccess::WRITE},
                              nodep->rhsp()->unlinkFrBack()}};
            nodep->replaceWith(alwaysp);
            if (auto* sensesp = VN_CAST(controlp, SenTree))
                alwaysp->addStmtp(new AstEventControl{controlp->fileline(), sensesp, nodep});
            else
                alwaysp->addStmtp(new AstDelay{controlp->fileline(), controlp, nullptr});
            alwaysp->addStmtp(
                new AstAssign{nodep->fileline(), nodep->lhsp()->unlinkFrBack(),
                              new AstVarRef{nodep->fileline(), newvscp, VAccess::READ}});
            nodep->deleteTree();
        }
    }
    virtual void visit(AstAssignDly* nodep) override {
        if (auto* controlp = nodep->timingControlp()) {
            controlp->unlinkFrBack();
            auto* const newvscp = getCreateIntraVar(nodep);
            nodep->addHereThisAsNext(new AstAssign{
                nodep->fileline(), new AstVarRef{nodep->fileline(), newvscp, VAccess::WRITE},
                nodep->rhsp()->unlinkFrBack()});
            nodep->rhsp(new AstVarRef{nodep->fileline(), newvscp, VAccess::READ});
            if (auto* sensesp = VN_CAST(controlp, SenTree))
                controlp = new AstEventControl{controlp->fileline(), sensesp, nullptr};
            else
                controlp = new AstDelay{controlp->fileline(), controlp, nullptr};
            if (m_underFork) {
                nodep->replaceWith(controlp);  // No need to create another fork
            } else {
                auto* const forkp = new AstFork{nodep->fileline(), "", controlp};
                forkp->joinType(VJoinType::JOIN_NONE);
                nodep->replaceWith(forkp);
            }
            if (auto* eventControlp = VN_CAST(controlp, EventControl)) {
                eventControlp->stmtsp(nodep);
            } else {
                auto* delayp = VN_AS(controlp, Delay);
                delayp->stmtsp(nodep);
            }
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit TimingIntraAssignControlVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~TimingIntraAssignControlVisitor() override {}
};

//######################################################################
// Scale delays according to the timescale

class TimingDelayTimescaleVisitor final : public VNVisitor {
private:
    // STATE
    double m_scaleFactor;  // Factor to scale delays by

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstDelay* nodep) override {
        auto* const timep = nodep->lhsp()->unlinkFrBack();
        if (timep->dtypep()->isDouble())
            nodep->lhsp(new AstMulD{
                nodep->fileline(), timep,
                new AstConst{nodep->fileline(), AstConst::RealDouble(), m_scaleFactor}});
        else
            nodep->lhsp(new AstMul{nodep->fileline(), timep,
                                   new AstConst{nodep->fileline(), AstConst::Unsized64(),
                                                static_cast<uint64_t>(m_scaleFactor)}});
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    // CONSTRUCTORS
    explicit TimingDelayTimescaleVisitor(AstNetlist* nodep) {
        int scalePowerOfTen = nodep->timeunit().powerOfTen() - nodep->timeprecision().powerOfTen();
        m_scaleFactor = std::pow(10.0, scalePowerOfTen);
        iterate(nodep);
    }
    virtual ~TimingDelayTimescaleVisitor() override {}
};

//######################################################################
// Base class for visitors that deal with var edge events

class TimingEdgeEventVisitor VL_NOT_FINAL : public VNVisitor {
private:
    // NODE STATE
    //  AstVar::user1()      -> AstVarScope*.  Event to trigger on var's posedge
    //  AstVar::user2()      -> AstVarScope*.  Event to trigger on var's negedge
    //  AstVar::user3()      -> AstVarScope*.  Event to trigger on var's anyedge
    // Derived classes should allocate the user data

protected:
    static AstVarScope* getEdgeEvent(const AstNode* const nodep, VEdgeType edgeType) {
        switch (edgeType) {
        case VEdgeType::ET_POSEDGE:
            if (nodep->user1()) return VN_AS(nodep->user1u().toNodep(), VarScope);
            break;
        case VEdgeType::ET_NEGEDGE:
            if (nodep->user2()) return VN_AS(nodep->user2u().toNodep(), VarScope);
            break;
        case VEdgeType::ET_ANYEDGE:
            if (nodep->user3()) return VN_AS(nodep->user3u().toNodep(), VarScope);
            break;
        default: nodep->v3fatalSrc("Unhandled edge type: " << edgeType);
        }
        return nullptr;
    }

    static bool hasEdgeEvents(const AstNode* const nodep) {
        return nodep->user1() || nodep->user2() || nodep->user3();
    }

    static AstVarScope* getCreateEdgeEvent(AstVarScope* const varScopep, VEdgeType edgeType) {
        auto* const varp = varScopep->varp();
        auto* const scopep = varScopep->scopep();
        if (auto* const eventp = getEdgeEvent(varp, edgeType)) return eventp;
        string newvarname = (string("__VedgeEvent__") + scopep->nameDotless() + "__"
                             + edgeType.ascii() + "__" + varp->name());
        auto* const newvarp = new AstVar{varp->fileline(), VVarType::VAR, newvarname,
                                         varp->findBasicDType(VBasicDTypeKwd::EVENTVALUE)};
        scopep->modp()->addStmtp(newvarp);
        auto* const newvscp = new AstVarScope{varp->fileline(), scopep, newvarp};
        scopep->addVarp(newvscp);
        switch (edgeType) {
        case VEdgeType::ET_POSEDGE: varp->user1p(newvscp); break;
        case VEdgeType::ET_NEGEDGE: varp->user2p(newvscp); break;
        case VEdgeType::ET_ANYEDGE: varp->user3p(newvscp); break;
        default: varp->v3fatalSrc("Unhandled edge type: " << edgeType);
        }
        return newvscp;
    }
};

//######################################################################
// Mark and transform nodes affected by timing

class TimingTransformVisitor final : public TimingEdgeEventVisitor {
private:
    // TYPES
    struct Overrides {
        using CFuncSet = std::unordered_set<AstCFunc*>;
        CFuncSet nodes;

        CFuncSet::const_iterator begin() const { return nodes.begin(); }
        CFuncSet::const_iterator end() const { return nodes.end(); }
        void insert(AstCFunc* const nodep) { nodes.insert(nodep); }
    };

    // NODE STATE
    //  AstNode::user1()       -> bool.  Set true if process/function/task is suspendable
    //  AstNode::user2()       -> bool.  Set true if process/function/task is dynamically scheduled
    //                                   (should vars written to in process/function/task spread
    //                                   the 'dynamically scheduled' status?)
    //  AstCFunc::user3()      -> bool.  Set true if node has been processed
    //  AstVar::user1()      -> AstVarScope*.  Event to trigger on var's posedge
    //  AstVar::user2()      -> AstVarScope*.  Event to trigger on var's negedge
    //  AstVar::user3()      -> AstVarScope*.  Event to trigger on var's anyedge
    //  AstVar::user4()      -> bool.  Is written to by a dynamically scheduled process?
    VNUser1InUse m_inuser1;
    VNUser2InUse m_inuser2;
    VNUser3InUse m_inuser3;
    VNUser4InUse m_inuser4;

    // STATE
    std::unordered_map<AstCFunc*, Overrides>
        m_overrides;  // Maps CFuncs to CFuncs overriding them/overridden by them
    AstClass* m_classp = nullptr;  // Current class
    AstScope* m_scopep = nullptr;  // Current scope
    AstVarScope* m_dlyEvent = nullptr;  // Event used for triggering delayed assignments
    AstNode* m_proc = nullptr;  // NodeProcedure/CFunc/Fork we're under
    bool m_repeat = false;  // Re-run this visitor?
    bool m_underFork = false;  // Are we under a begin?

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    AstVarScope* getCreateDlyEvent() {
        if (m_dlyEvent) return m_dlyEvent;
        string newvarname = "__VdlyEvent__";
        auto fl = new FileLine{m_scopep->fileline()};
        auto* const newvarp = new AstVar{fl, VVarType::MODULETEMP, newvarname,
                                         m_scopep->findBasicDType(VBasicDTypeKwd::EVENTVALUE)};
        m_scopep->modp()->addStmtp(newvarp);
        auto* const newvscp = new AstVarScope{fl, m_scopep, newvarp};
        m_scopep->addVarp(newvscp);
        return m_dlyEvent = newvscp;
    }

    // Mark the current process/function as suspendable. We will need to repeat the whole process
    void setSuspendableProcess() {
        v3Global.timing(true);
        if (!isSuspendable(m_proc)) {
            setSuspendable(m_proc);
            m_repeat = true;
        }
    }
    bool isSuspendableProcess() const { return isSuspendable(m_proc); }

    static void setSuspendable(AstNode* const proc) { proc->user1(true); }
    static bool isSuspendable(const AstNode* const proc) { return proc->user1(); }

    // Mark the current process/function as suspendable AND dynamic
    void setDynamicProcess() {
        setSuspendableProcess();
        setDynamic(m_proc);  // dynamically scheduled
    }
    bool isDynamicProcess() const { return isDynamic(m_proc); }

    static void setDynamic(AstNode* const proc) { proc->user2(true); }
    static bool isDynamic(const AstNode* const proc) { return proc->user2(); }

    static void setWrittenByDynamic(AstVar* const proc) { proc->user4(true); }
    static bool isWrittenByDynamic(const AstVar* const proc) { return proc->user4(); }

    // Should the always process be transformed into an initial?
    static bool shouldTransformToInitial(AstAlways* const nodep) {
        auto* sensesp = nodep->sensesp();
        if (sensesp) {
            // Transform if the process is waiting on a dynamic var or an event
            for (auto* senitemp = sensesp->sensesp(); senitemp;
                 senitemp = VN_AS(senitemp->nextp(), SenItem)) {
                auto* varp = AstNode::findVarp(senitemp->sensp());
                if (varp && (isWrittenByDynamic(varp) || varp->isEventValue())) return true;
            }
            return false;
        } else {
            // Transform if process is suspendable and has no sentree
            // e.g. always #1 clk = ~clk;
            return nodep->isSuspendable();
        }
    }

    // VISITORS
    virtual void visit(AstScope* nodep) override {
        m_scopep = nodep;
        iterateChildren(nodep);
        m_scopep = nullptr;
    }
    virtual void visit(AstClass* nodep) override {
        m_classp = nodep;
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeProcedure* nodep) override {
        VL_RESTORER(m_proc);
        m_proc = nodep;
        iterateChildren(nodep);
        nodep->isSuspendable(isSuspendable(nodep));
    }
    virtual void visit(AstAlways* nodep) override {
        // Transform if the process is suspendable and has no sentree
        if (shouldTransformToInitial(nodep)) {
            auto* const fl = nodep->fileline();
            auto* sensesp = nodep->sensesp();
            auto* bodysp = nodep->bodysp();
            if (bodysp) bodysp->unlinkFrBackWithNext();
            if (sensesp) {
                sensesp = sensesp->cloneTree(false);
                for (auto* senitemp = sensesp->sensesp(); senitemp;
                     senitemp = VN_AS(senitemp->nextp(), SenItem)) {
                    auto* varScopep = AstNode::findVarScopep(sensesp->sensesp()->sensp());
                    if (varScopep->varp()->isEventValue()) continue;
                    auto* const eventp = getCreateEdgeEvent(varScopep, senitemp->edgeType());
                    auto* const newSenitemp = new AstSenItem{
                        senitemp->fileline(), VEdgeType::ET_ANYEDGE,
                        new AstVarRef{senitemp->fileline(), eventp, VAccess::READ}};
                    senitemp->replaceWith(newSenitemp);
                    VL_DO_DANGLING(senitemp->deleteTree(), senitemp);
                    senitemp = newSenitemp;
                }
                bodysp = new AstEventControl{fl, sensesp, bodysp};
            }
            auto* const whilep = new AstWhile{fl, new AstConst{fl, AstConst::BitTrue()}, bodysp};
            auto* const initialp = new AstInitial{fl, whilep};
            nodep->replaceWith(initialp);
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
            visit(initialp);
        } else {
            visit(static_cast<AstNodeProcedure*>(nodep));
        }
    }
    virtual void visit(AstCFunc* nodep) override {
        VL_RESTORER(m_proc);
        m_proc = nodep;
        iterateChildren(nodep);
        if (nodep->isVirtual() && !nodep->user3SetOnce()) {
            for (auto* cextp = m_classp->extendsp(); cextp;
                 cextp = VN_AS(cextp->nextp(), ClassExtends)) {
                cextp->classp()->repairCache();
                auto* cfuncp = VN_CAST(cextp->classp()->findMember(nodep->name()), CFunc);
                if (!cfuncp) continue;
                m_overrides[nodep].insert(cfuncp);
                m_overrides[cfuncp].insert(nodep);
            }
        }
        if (!isSuspendable(nodep)) return;
        nodep->rtnType("VerilatedCoroutine");
        for (auto cfuncp : m_overrides[nodep]) {
            if (cfuncp->isCoroutine()) continue;
            setSuspendable(cfuncp);
            m_repeat = true;
        }
    }
    virtual void visit(AstDelay* nodep) override {
        setSuspendableProcess();
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeAssign* nodep) override {
        if (nodep->timingControlp()) setSuspendableProcess();
        iterateChildren(nodep);
    }
    virtual void visit(AstEventControl* nodep) override {
        setDynamicProcess();
        iterateChildren(nodep);
    }
    virtual void visit(AstWait* nodep) override {
        setDynamicProcess();
        iterateChildren(nodep);
    }
    virtual void visit(AstFork* nodep) override {
        if (!nodep->joinType().joinNone()) setDynamicProcess();
        VL_RESTORER(m_proc);
        VL_RESTORER(m_underFork);
        m_proc = nodep;
        m_underFork = true;
        setDynamicProcess();
        iterateChildren(nodep);
    }
    virtual void visit(AstBegin* nodep) override {
        VL_RESTORER(m_underFork);
        m_underFork = false;
        iterateChildren(nodep);
    }
    virtual void visit(AstNodeCCall* nodep) override {
        if (nodep->funcp()->isCoroutine()) {
            if (isDynamic(nodep))
                setDynamicProcess();
            else
                setSuspendableProcess();
        }
        iterateChildren(nodep);
    }
    virtual void visit(AstVarRef* nodep) override {
        if (m_proc && nodep->access().isWriteOrRW()) {
            auto* const varp = nodep->varp();
            varp->isWrittenBySuspendable(varp->isWrittenBySuspendable() || isSuspendable(m_proc));
            if (!isWrittenByDynamic(varp) && isDynamicProcess()) {
                m_repeat = true;
                setWrittenByDynamic(varp);
            }
        }
    }
    virtual void visit(AstAssignDly* nodep) override {
        if (!isSuspendable(m_proc)) return;
        auto* const fl = nodep->fileline();
        auto* const eventp = getCreateDlyEvent();
        auto* const assignp
            = new AstAssign{fl, nodep->lhsp()->unlinkFrBack(), nodep->rhsp()->unlinkFrBack()};
        auto* const eventControlp = new AstEventControl{
            fl,
            new AstSenTree{fl, new AstSenItem{fl, VEdgeType::ET_ANYEDGE,
                                              new AstVarRef{fl, eventp, VAccess::READ}}},
            assignp};
        if (m_underFork) {
            nodep->replaceWith(eventControlp);  // No need to create another fork
        } else {
            auto* const forkp = new AstFork{nodep->fileline(), "", eventControlp};
            forkp->joinType(VJoinType::JOIN_NONE);
            nodep->replaceWith(forkp);
        }
        VL_DO_DANGLING(delete nodep, nodep);
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    AstVarScope* getDlyEvent() { return m_dlyEvent; }

    // CONSTRUCTORS
    explicit TimingTransformVisitor(AstNetlist* nodep) {
        do {
            m_repeat = false;
            iterate(nodep);
        } while (m_repeat);
    }
    virtual ~TimingTransformVisitor() override {}
};

//######################################################################
// Transform forks

class TimingForkVisitor final : public VNVisitor {
private:
    // NODE STATE
    //  AstFork::user1()       -> bool.  Set true if any forked process is suspendable
    //  AstFork::user2()       -> bool.  Set true if any forked process is dynamically scheduled
    //  AstFork::user3()      -> bool.  Set true if node has been processed
    // VNUser1InUse    m_inuser1;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser2;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser3;      (Allocated for use in TimingTransformVisitor)

    // STATE
    AstScope* m_scopep = nullptr;  // Current scope
    std::map<AstVarScope*, AstVarScope*>
        m_locals;  // Map from var accessed by process to func-local var
    AstVar* m_joinEventp;  // Join struct sync event member
    AstVar* m_joinCounterp;  // Join struct thread counter member
    AstClassRefDType* m_joinDTypep;  // Join struct type
    AstCFunc* m_joinNewp;  // Join struct constructor
    V3UniqueNames m_forkNames;  // Fork name generator

    enum { FORK, GATHER, REPLACE } m_mode = FORK;  // Stages for this visitor

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstScope* nodep) override {
        VL_RESTORER(m_scopep);
        m_scopep = nodep;
        iterateChildren(nodep);
        m_scopep = nullptr;
    }
    virtual void visit(AstVarRef* nodep) override {
        if (m_mode == GATHER) {
            if (nodep->varp()->varType() == VVarType::BLOCKTEMP)
                m_locals.insert(std::make_pair(nodep->varScopep(), nullptr));
        } else if (m_mode == REPLACE) {
            if (auto* const newvscp = m_locals[nodep->varScopep()]) {
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
        if (nodep->user3SetOnce()) return;

        AstVarScope* joinVscp = nullptr;
        auto forkName = m_forkNames.get(nodep);
        if (!nodep->unnamed()) forkName = nodep->name() + "__" + forkName;
        forkName = "__Vfork__" + forkName;
        if (nodep->user1() && !nodep->joinType().joinNone()) {
            auto* const joinVarp = new AstVar{nodep->fileline(), VVarType::BLOCKTEMP,
                                              forkName + "__join", m_joinDTypep};
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

            AstCFunc* const cfuncp
                = new AstCFunc{stmtp->fileline(), forkName + "__" + std::to_string(joinCount++),
                               m_scopep, "VerilatedCoroutine"};
            m_scopep->addActivep(cfuncp);

            // Create list of arguments and move to function
            AstNode* argsp = nullptr;
            for (auto& p : m_locals) {
                auto* const varscp = p.first;
                auto* const varp = varscp->varp()->cloneTree(false);
                varp->funcLocal(true);
                varp->direction(VDirection::INPUT);
                cfuncp->addArgsp(varp);
                AstVarScope* const newvscp = new AstVarScope{varp->fileline(), m_scopep, varp};
                m_scopep->addVarp(newvscp);
                p.second = newvscp;
                argsp = AstNode::addNext(argsp,
                                         new AstVarRef{stmtp->fileline(), varscp, VAccess::READ});
            }
            auto* const ccallp = new AstCCall{stmtp->fileline(), cfuncp, argsp};
            stmtp->replaceWith(ccallp);

            if (auto* const beginp = VN_CAST(stmtp, Begin)) {
                cfuncp->addStmtsp(beginp->stmtsp()->unlinkFrBackWithNext());
                VL_DO_DANGLING(beginp->deleteTree(), beginp);
            } else {
                cfuncp->addStmtsp(stmtp);
            }

            if (joinVscp) {
                auto* const counterSelp = new AstMemberSel{
                    nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::WRITE},
                    m_joinCounterp->dtypep()};
                counterSelp->varp(m_joinCounterp);
                cfuncp->addStmtsp(
                    new AstAssign{nodep->fileline(), counterSelp,
                                  new AstSub{nodep->fileline(), counterSelp->cloneTree(false),
                                             new AstConst{nodep->fileline(), 1}}});
                auto* const eventSelp = new AstMemberSel{
                    nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::WRITE},
                    m_joinEventp->dtypep()};
                eventSelp->varp(m_joinEventp);
                cfuncp->addStmtsp(new AstEventTrigger{nodep->fileline(), eventSelp});
            }

            m_mode = REPLACE;
            iterateChildren(cfuncp);
            stmtp = ccallp->nextp();
        }

        if (joinVscp) {
            auto* const cnewp = new AstCNew{nodep->fileline(), m_joinNewp, nullptr};
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
            auto* const eventSelp = new AstMemberSel{
                nodep->fileline(), new AstVarRef{nodep->fileline(), joinVscp, VAccess::READ},
                m_joinEventp->dtypep()};
            eventSelp->varp(m_joinEventp);
            nodep->addNextHere(new AstWhile{
                nodep->fileline(),
                new AstGt{nodep->fileline(), counterSelp, new AstConst{nodep->fileline(), 0}},
                new AstEventControl{
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
    explicit TimingForkVisitor(AstNetlist* nodep) {
        auto* const joinClassp = new AstClass{nodep->fileline(), "__Vjoin"};
        auto* const joinClassPackagep = new AstClassPackage{nodep->fileline(), "__Vjoin__Vclpkg"};
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
        nodep->topModulep()->addStmtp(cellp);
        auto* const joinScopep
            = new AstScope{nodep->fileline(), joinClassp, "__Vjoin", nullptr, nullptr};
        joinClassp->addMembersp(joinScopep);
        m_joinEventp = new AstVar{nodep->fileline(), VVarType::MEMBER, "wakeEvent",
                                  nodep->findBasicDType(VBasicDTypeKwd::EVENTVALUE)};
        joinClassp->addMembersp(m_joinEventp);
        joinClassp->addMembersp(new AstVarScope{nodep->fileline(), joinScopep, m_joinEventp});
        m_joinCounterp = new AstVar{nodep->fileline(), VVarType::MEMBER, "counter",
                                    nodep->findSigned32DType()};
        joinClassp->addMembersp(m_joinCounterp);
        joinClassp->addMembersp(new AstVarScope{nodep->fileline(), joinScopep, m_joinCounterp});
        m_joinDTypep = new AstClassRefDType{nodep->fileline(), joinClassp, nullptr};
        m_joinDTypep->dtypep(m_joinDTypep);
        nodep->typeTablep()->addTypesp(m_joinDTypep);
        m_joinNewp = new AstCFunc{nodep->fileline(), "new", joinScopep, ""};
        m_joinNewp->argTypes(EmitCBaseVisitor::symClassVar());
        m_joinNewp->isConstructor(true);
        const string resetStmt = VIdProtect::protect("_ctor_var_reset") + "(vlSymsp);\n";
        m_joinNewp->addInitsp(new AstCStmt{nodep->fileline(), resetStmt});
        joinScopep->addActivep(m_joinNewp);
        iterate(nodep);
    }
    virtual ~TimingForkVisitor() override {}
};

//######################################################################
// Create edge events

class TimingCreateEdgeEventsVisitor final : public TimingEdgeEventVisitor {
private:
    // NODE STATE
    //  AstVar::user1()      -> AstNode*.  Event to trigger on var's posedge
    //  AstVar::user2()      -> AstNode*.  Event to trigger on var's negedge
    //  AstVar::user3()      -> AstNode*.  Event to trigger on var's anyedge
    // VNUser1InUse    m_inuser1;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser2;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser3;      (Allocated for use in TimingTransformVisitor)

    // STATE
    using VarScopeSet = std::set<AstVarScope*>;
    VarScopeSet m_waitVars;  // Set of vars in wait expression

    bool m_inEventControlSens = false;  // Are we under a event control sens list?
    bool m_inWait = false;  // Are we under a wait statement?
    AstSenItem* m_senItemp = nullptr;  // The senitem we're under

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstEventControl* nodep) override {
        VL_RESTORER(m_inEventControlSens);
        m_inEventControlSens = true;
        iterateAndNextNull(nodep->sensesp());
        m_inEventControlSens = false;
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
            for (auto* const vscp : m_waitVars) {
                AstVarScope* const eventp = vscp->varp()->isEventValue()
                                                ? vscp
                                                : getCreateEdgeEvent(vscp, VEdgeType::ET_ANYEDGE);
                senitemsp = AstNode::addNext(
                    senitemsp, new AstSenItem{fl, VEdgeType::ET_ANYEDGE,
                                              new AstVarRef{fl, eventp, VAccess::READ}});
            }
            auto* const condp = nodep->condp()->unlinkFrBack();
            auto* const eventControlp
                = new AstEventControl{fl, new AstSenTree{fl, VN_AS(senitemsp, SenItem)}, nullptr};
            // Put the event control in a while loop with the wait statement as condition
            auto* const whilep = new AstWhile{fl, new AstLogNot{fl, condp}, eventControlp};
            if (nodep->bodysp()) whilep->addNext(nodep->bodysp()->unlinkFrBackWithNext());
            nodep->replaceWith(whilep);
            m_waitVars.clear();
        }
        VL_DO_DANGLING(nodep->deleteTree(), nodep);
    }
    virtual void visit(AstSenItem* nodep) override {
        VL_RESTORER(m_senItemp);
        m_senItemp = nodep;
        if (m_inEventControlSens) {
            // Split bothedge into posedge and negedge, to react to those triggers
            if (nodep->edgeType() == VEdgeType::ET_BOTHEDGE) {
                nodep->addNextHere(nodep->cloneTree(false));
                nodep->edgeType(VEdgeType::ET_POSEDGE);
                VN_AS(nodep->nextp(), SenItem)->edgeType(VEdgeType::ET_NEGEDGE);
            }
        }
        iterateChildren(nodep);
    }
    virtual void visit(AstVarRef* nodep) override {
        if (m_inWait) {
            m_waitVars.insert(nodep->varScopep());
        } else if (m_inEventControlSens) {
            if (!nodep->varp()->isEventValue()) {
                auto edgeType = m_senItemp->edgeType();
                nodep->varScopep(getCreateEdgeEvent(nodep->varScopep(), edgeType));
                nodep->varp(nodep->varScopep()->varp());
            }
        }
    }
    virtual void visit(AstNodeSel* nodep) override { iterate(nodep->fromp()); }
    virtual void visit(AstMemberSel* nodep) override {
        if (m_inWait) {
            iterateChildren(nodep);
        } else if (m_inEventControlSens) {
            if (!nodep->varp()->isEventValue()) {
                auto edgeType = m_senItemp->edgeType();
                nodep->replaceWith(new AstVarRef(
                    nodep->fileline(), getCreateEdgeEvent(AstNode::findVarScopep(nodep), edgeType),
                    VAccess::READ));
                VL_DO_DANGLING(nodep->deleteTree(), nodep);
            }
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit TimingCreateEdgeEventsVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~TimingCreateEdgeEventsVisitor() override {}
};

//######################################################################
// Transform continuous assignments into processes if LHS has edge events, to allow adding triggers
// in TimingAddTriggersVisitor

class TimingContinuousAssignVisitor final : public TimingEdgeEventVisitor {
private:
    // NODE STATE
    //  AstVar::user1()      -> AstNode*.  Event to trigger on var's posedge
    //  AstVar::user2()      -> AstNode*.  Event to trigger on var's negedge
    //  AstVar::user3()      -> AstNode*.  Event to trigger on var's anyedge
    // VNUser1InUse    m_inuser1;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser2;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser3;      (Allocated for use in TimingTransformVisitor)

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstNodeAssign* nodep) override {
        if (!VN_IS(nodep, AssignW) && !VN_IS(nodep, AssignAlias)) return;
        if (auto* const lvarp = AstNode::findVarp(nodep->lhsp())) {
            if (!hasEdgeEvents(lvarp)) return;
            auto* const lhsp = nodep->lhsp()->unlinkFrBack();
            auto* const rhsp = nodep->rhsp()->unlinkFrBack();
            lvarp->fileline()->warnOff(V3ErrorCode::UNOPTFLAT, true);
            auto* const alwaysp = new AstAlways{nodep->fileline(), VAlwaysKwd::ALWAYS, nullptr,
                                                new AstAssign{nodep->fileline(), lhsp, rhsp}};
            nodep->replaceWith(alwaysp);
            nodep->deleteTree();
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit TimingContinuousAssignVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~TimingContinuousAssignVisitor() override {}
};

//######################################################################
// Add triggers for edge events

class TimingAddTriggersVisitor final : public TimingEdgeEventVisitor {
private:
    // NODE STATE
    //  AstNodeAssign::user1()      -> bool.  Set true if node has been processed
    //  AstVar::user1()      -> AstNode*.  Event to trigger on var's posedge
    //  AstVar::user2()      -> AstNode*.  Event to trigger on var's negedge
    //  AstVar::user3()      -> AstNode*.  Event to trigger on var's anyedge
    // VNUser1InUse    m_inuser1;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser2;      (Allocated for use in TimingTransformVisitor)
    // VNUser2InUse    m_inuser3;      (Allocated for use in TimingTransformVisitor)

    // STATE
    V3UniqueNames m_forkNames;  // Prev val temp var name generator

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    AstVarScope* getCreateVar(AstVarScope* const oldvarscp) {
        const auto name = "__Vprevval__" + m_forkNames.get(oldvarscp->varp());
        AstNodeModule* const modp = oldvarscp->scopep()->modp();
        auto* const varp
            = new AstVar{oldvarscp->fileline(), VVarType::BLOCKTEMP, name, oldvarscp->varp()};
        varp->dtypeFrom(oldvarscp);
        modp->addStmtp(varp);
        AstVarScope* const varscp
            = new AstVarScope{oldvarscp->fileline(), oldvarscp->scopep(), varp};
        oldvarscp->scopep()->addVarp(varscp);
        return varscp;
    }

    // VISITORS
    virtual void visit(AstNodeAssign* nodep) override {
        if (nodep->user1SetOnce()) return;
        if (auto* const varScopep = AstNode::findVarScopep(nodep->lhsp())) {
            auto* const fl = nodep->fileline();
            auto* const varp = varScopep->varp();
            if (!hasEdgeEvents(varp)) return;
            auto* const newvscp = getCreateVar(varScopep);
            AstNode* stmtspAfter = nullptr;

            // Trigger posedge event if (~prevval && currentval)
            if (auto* const eventp = getEdgeEvent(varp, VEdgeType::ET_POSEDGE)) {
                stmtspAfter = AstNode::addNext(
                    stmtspAfter,
                    new AstIf{fl,
                              new AstAnd{fl,
                                         new AstNot{fl, new AstVarRef{fl, newvscp, VAccess::READ}},
                                         new AstVarRef{fl, varScopep, VAccess::READ}},
                              new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
            }

            // Trigger negedge event if (prevval && ~currentval)
            if (auto* const eventp = getEdgeEvent(varp, VEdgeType::ET_NEGEDGE)) {
                stmtspAfter = AstNode::addNext(
                    stmtspAfter,
                    new AstIf{
                        fl,
                        new AstAnd{fl, new AstVarRef{fl, newvscp, VAccess::READ},
                                   new AstNot{fl, new AstVarRef{fl, varScopep, VAccess::READ}}},
                        new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
            }

            // Trigger anyedge event if (prevval != currentval)
            if (auto* const eventp = getEdgeEvent(varp, VEdgeType::ET_ANYEDGE)) {
                stmtspAfter = AstNode::addNext(
                    stmtspAfter,
                    new AstIf{fl,
                              new AstNeq{fl, new AstVarRef{fl, newvscp, VAccess::READ},
                                         new AstVarRef{fl, varScopep, VAccess::READ}},
                              new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}}});
            }

            UASSERT(stmtspAfter, "Unhandled edge event!");
            nodep->addHereThisAsNext(new AstAssign{fl, new AstVarRef{fl, newvscp, VAccess::WRITE},
                                                   new AstVarRef{fl, varScopep, VAccess::READ}});
            nodep->addNextHere(stmtspAfter);
        }
    }
    virtual void visit(AstVarScope* nodep) override {
        auto* const varp = nodep->varp();
        // If a var has edge events and could've been written to from the outside (e.g. the main
        // function), create a clocked Active that triggers the edge events
        if (hasEdgeEvents(varp) && (varp->isUsedClock() || varp->isSigPublic())) {
            auto fl = nodep->fileline();
            for (auto edgeType :
                 {VEdgeType::ET_POSEDGE, VEdgeType::ET_NEGEDGE, VEdgeType::ET_ANYEDGE}) {
                if (auto* const eventp = getEdgeEvent(varp, edgeType)) {
                    auto* const activep = new AstActive{
                        fl, "",
                        new AstSenTree{
                            fl, new AstSenItem{
                                    fl,
                                    edgeType == VEdgeType::ET_ANYEDGE
                                        ? VEdgeType::ET_BOTHEDGE
                                        : edgeType,  // Use bothedge as anyedge is not clocked
                                    new AstVarRef{fl, nodep, VAccess::READ}}}};
                    activep->sensesStorep(activep->sensesp());
                    auto* const ifp
                        = new AstEventTrigger{fl, new AstVarRef{fl, eventp, VAccess::WRITE}};
                    auto* const alwaysp
                        = new AstAlways{nodep->fileline(), VAlwaysKwd::ALWAYS, nullptr, ifp};
                    activep->addStmtsp(alwaysp);
                    nodep->scopep()->addActivep(activep);
                }
            }
        }
    }
    virtual void visit(AstInitialStatic* nodep) override {}

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit TimingAddTriggersVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~TimingAddTriggersVisitor() override {}
};

//######################################################################
// Manage the lifetime of class member events

class TimingClassEventVisitor final : public VNVisitor {
private:
    // STATE
    AstClass* m_classp = nullptr;  // Current class
    AstScope* m_classScopep = nullptr;  // Current class scope
    AstNode* m_resetStmtsp = nullptr;  // Statements that reset member events

    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstClass* nodep) override {
        VL_RESTORER(m_resetStmtsp);
        VL_RESTORER(m_classScopep);
        m_classp = nodep;
        iterateChildren(nodep);
        if (m_resetStmtsp) {
            auto* const cancelEventsFuncp
                = new AstCFunc{nodep->fileline(), "_cancel_events", m_classScopep, ""};
            cancelEventsFuncp->argTypes(EmitCBaseVisitor::symClassVar());
            cancelEventsFuncp->addStmtsp(m_resetStmtsp);
            m_classScopep->addActivep(cancelEventsFuncp);
            nodep->repairCache();
        }
    }
    virtual void visit(AstScope* nodep) override {
        if (m_classp && !m_classScopep) m_classScopep = nodep;
        iterateChildren(nodep);
    }
    virtual void visit(AstVarScope* nodep) override {
        if (!m_classp) return;
        if (nodep->dtypep()->basicp() && nodep->dtypep()->basicp()->isEventValue()) {
            AstNode* bodysp
                = new AstText{nodep->fileline(), "vlSymsp->__Vm_eventDispatcher.cancel("};
            bodysp->addNext(new AstVarRef{nodep->fileline(), nodep, VAccess::READ});
            bodysp->addNext(new AstText{nodep->fileline(), ");\n"});
            m_resetStmtsp = AstNode::addNext(m_resetStmtsp, bodysp);
        }
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }
    virtual void visit(AstNodeModule*) override {}  // Accelarate

public:
    // CONSTRUCTORS
    explicit TimingClassEventVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~TimingClassEventVisitor() override {}
};

//######################################################################
// Remove event triggers if not needed

class TimingCleanTriggersVisitor final : public VNVisitor {
private:
    // METHODS
    VL_DEBUG_FUNC;  // Declare debug()

    // VISITORS
    virtual void visit(AstEventTrigger* nodep) override {
        nodep->replaceWith(new AstAssign{nodep->fileline(), nodep->trigp()->unlinkFrBack(),
                                         new AstConst{nodep->fileline(), AstConst::BitTrue()}});
        VL_DO_DANGLING(nodep->deleteTree(), nodep);
    }

    //--------------------
    virtual void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    // CONSTRUCTORS
    explicit TimingCleanTriggersVisitor(AstNetlist* nodep) { iterate(nodep); }
    virtual ~TimingCleanTriggersVisitor() override {}
};

//######################################################################
// Timing class functions

void V3Timing::timingAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    UINFO(2, "  Transform Intra Assign Delays...\n");
    { TimingIntraAssignControlVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("timing_intra", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Apply Timescale To Delays...\n");
    { TimingDelayTimescaleVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("timing_scale", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    UINFO(2, "  Mark/Transform for Timing...\n");
    TimingTransformVisitor visitor(nodep);  // Keep it around to keep user data
    V3Global::dumpCheckGlobalTree("timing_transform", 0,
                                  v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
    if (v3Global.timing()) {  // Do we have any suspendable processes/tasks?
        UINFO(2, "  Add AstResumeTriggered...\n");
        auto fl = nodep->fileline();
        auto* const activep = new AstActive{
            fl, "resumeTriggered", new AstSenTree{fl, new AstSenItem{fl, AstSenItem::Combo()}}};
        activep->sensesStorep(activep->sensesp());
        activep->addStmtsp(new AstResumeTriggered{
            fl, visitor.getDlyEvent() ? new AstVarRef{fl, visitor.getDlyEvent(), VAccess::WRITE}
                                      : nullptr});
        nodep->topScopep()->scopep()->addActivep(activep);
        UINFO(2, "  Move Forked Processes to New Functions...\n");
        { TimingForkVisitor{nodep}; }
        V3Global::dumpCheckGlobalTree("timing_forks", 0,
                                      v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
        UINFO(2, "  Add Edge Events...\n");
        { TimingCreateEdgeEventsVisitor{nodep}; }
        V3Global::dumpCheckGlobalTree("timing_events", 0,
                                      v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
        UINFO(2, "  Add Edge Event Triggers...\n");
        { TimingContinuousAssignVisitor{nodep}; }
        { TimingAddTriggersVisitor{nodep}; }
        V3Global::dumpCheckGlobalTree("timing_triggers", 0,
                                      v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
        UINFO(2, "  Add Class Event Cleanup...\n");
        { TimingClassEventVisitor{nodep}; }
    } else {
        UINFO(2, "  Remove Event Triggers...\n");
        { TimingCleanTriggersVisitor{nodep}; }
    }
    V3Global::dumpCheckGlobalTree("timing", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
