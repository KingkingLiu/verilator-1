// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Deals with signal strength
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

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3SignalStrength.h"
#include "V3Ast.h"
#include "V3Stats.h"
#include "V3Inst.h"
#include "V3Graph.h"

#include <algorithm>
#include <map>

//######################################################################

class SignalStrengthVisitor final : public VNVisitor {

    // TYPES
    using Assigns = std::vector<AstAssignW*>;
    using VarToAssignsMap = std::unordered_map<AstVar*, Assigns>;

    // MEMBERS
    VarToAssignsMap m_assigns;  // Assigns in current module

    // VISITORS
    virtual void visit(AstAssignW* nodep) override {
        if (AstVarRef* varRefp = VN_CAST(nodep->lhsp(), VarRef)) {
            if (varRefp->varp()->varType() == VVarType::WIRE
                && !varRefp->varp()->basicp()->isRanged())
                m_assigns[varRefp->varp()].push_back(nodep);
        }
    }

    AstAssign* getStrengthAssignmentp(FileLine* fl, AstVar* strengthVarp, int strengthLevel,
                                      AstNode* assignedValuep, AstConst* compareConstp) {
        return new AstAssign(
            fl, new AstVarRef(fl, strengthVarp, VAccess::WRITE),
            new AstCond(fl,
                        new AstLogAnd(fl,
                                      new AstLt(fl, new AstVarRef(fl, strengthVarp, VAccess::READ),
                                                new AstConst(fl, strengthLevel)),
                                      new AstEqCase(fl, assignedValuep, compareConstp)),
                        new AstConst(fl, strengthLevel),
                        new AstVarRef(fl, strengthVarp, VAccess::READ)));
    }

    virtual void visit(AstNodeModule* nodep) override {
        UINFO(8, nodep << endl);
        m_assigns = VarToAssignsMap();
        iterateChildren(nodep);
        for (auto& varpAssigns : m_assigns) {
            AstVar* varp = varpAssigns.first;
            FileLine* varFilelinep = varp->fileline();
            Assigns assigns = varpAssigns.second;
            if (assigns.size() > 1
                || (assigns.size() == 1 && assigns[0]->strengthSpecp()
                    && (assigns[0]->strengthSpecp()->strength0p()->strengthLevel == 0
                        || assigns[0]->strengthSpecp()->strength1p()->strengthLevel == 0))) {
                AstVar* strength0Varp = new AstVar(
                    varFilelinep, VVarType::MODULETEMP, varp->name() + "__s0", VFlagChildDType(),
                    new AstBasicDType(varFilelinep, VBasicDTypeKwd::INTEGER));
                AstVar* strength1Varp = new AstVar(
                    varFilelinep, VVarType::MODULETEMP, varp->name() + "__s1", VFlagChildDType(),
                    new AstBasicDType(varFilelinep, VBasicDTypeKwd::INTEGER));
                nodep->addStmtp(strength0Varp);
                nodep->addStmtp(strength1Varp);
                AstBegin* strengthBlockp = new AstBegin(
                    varFilelinep, varp->name() + "_strength_computing_block", nullptr);
                strengthBlockp->addStmtsp(new AstAssign(
                    varFilelinep, new AstVarRef(varFilelinep, strength0Varp, VAccess::WRITE),
                    new AstConst(varFilelinep, 0)));
                strengthBlockp->addStmtsp(new AstAssign(
                    varFilelinep, new AstVarRef(varFilelinep, strength1Varp, VAccess::WRITE),
                    new AstConst(varFilelinep, 0)));

                for (size_t i = 0; i < assigns.size(); i++) {
                    int strength0Level, strength1Level;
                    if (AstStrengthSpec* strengthSpec = assigns[i]->strengthSpecp()) {
                        strength0Level = strengthSpec->strength0p()->strengthLevel;
                        strength1Level = strengthSpec->strength1p()->strengthLevel;
                    } else {
                        strength0Level = 6;  // default strength in strong (6)
                        strength1Level = 6;
                    }

                    FileLine* filelinep = assigns[i]->fileline();
                    if (strength0Level != 0) {
                        strengthBlockp->addStmtsp(getStrengthAssignmentp(
                            filelinep, strength0Varp, strength0Level,
                            assigns[i]->rhsp()->cloneTree(false),
                            new AstConst(filelinep, AstConst::WidthedValue(), 1, 0)));
                        strengthBlockp->addStmtsp(getStrengthAssignmentp(
                            filelinep, strength0Varp, strength0Level,
                            assigns[i]->rhsp()->cloneTree(false),
                            new AstConst(filelinep, AstConst::StringToParse(), "1'x")));
                    }
                    if (strength1Level != 0) {
                        strengthBlockp->addStmtsp(getStrengthAssignmentp(
                            filelinep, strength1Varp, strength1Level,
                            assigns[i]->rhsp()->cloneTree(false),
                            new AstConst(filelinep, AstConst::WidthedValue(), 1, 1)));
                        strengthBlockp->addStmtsp(getStrengthAssignmentp(
                            filelinep, strength1Varp, strength1Level,
                            assigns[i]->rhsp()->cloneTree(false),
                            new AstConst(filelinep, AstConst::StringToParse(), "1'x")));
                    }

                    assigns[i]->unlinkFrBack();
                }
                nodep->addStmtp(
                    new AstAlways(varFilelinep, VAlwaysKwd::ALWAYS, nullptr, strengthBlockp));

                AstVarRef* varRefp = new AstVarRef(varFilelinep, varp, VAccess::WRITE);
                nodep->addStmtp(new AstAssignW(
                    varFilelinep, varRefp,
                    new AstCond(
                        varFilelinep,
                        new AstGt(varFilelinep,
                                  new AstVarRef(varFilelinep, strength0Varp, VAccess::READ),
                                  new AstVarRef(varFilelinep, strength1Varp, VAccess::READ)),
                        new AstConst(varFilelinep, AstConst::StringToParse(), "'0"),
                        new AstCond(
                            varFilelinep,
                            new AstEq(varFilelinep,
                                      new AstVarRef(varFilelinep, strength0Varp, VAccess::READ),
                                      new AstVarRef(varFilelinep, strength1Varp, VAccess::READ)),
                            new AstCond(
                                varFilelinep,
                                new AstEq(
                                    varFilelinep,
                                    new AstVarRef(varFilelinep, strength0Varp, VAccess::READ),
                                    new AstConst(varFilelinep, AstConst::StringToParse(), "'0")),
                                new AstConst(varFilelinep, AstConst::StringToParse(), "'z"),
                                new AstConst(varFilelinep, AstConst::StringToParse(), "'x")),
                            new AstConst(varFilelinep, AstConst::StringToParse(), "'1")))));
            }
        }
    }

    virtual void visit(AstNetlist* nodep) override { iterateChildrenBackwards(nodep); }

    virtual void visit(AstNode* nodep) override {}

public:
    // CONSTRUCTORS
    explicit SignalStrengthVisitor(AstNode* nodep) { iterate(nodep); }
    virtual ~SignalStrengthVisitor() override {}
    VL_DEBUG_FUNC;  // Declare debug()
};

//######################################################################
// SignalStrength class functions

void V3SignalStrength::handleStrength(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { SignalStrengthVisitor{nodep}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("signalStrength", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
