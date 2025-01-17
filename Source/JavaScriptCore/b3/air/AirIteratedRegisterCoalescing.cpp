/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AirIteratedRegisterCoalescing.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLiveness.h"
#include "AirPhaseScope.h"
#include "AirRegisterPriority.h"
#include <wtf/ListHashSet.h>

namespace JSC { namespace B3 { namespace Air {

static bool debug = false;
static bool traceDebug = false;

template<Arg::Type type>
struct MoveInstHelper;

template<>
struct MoveInstHelper<Arg::GP> {
    static bool mayBeCoalescable(const Inst& inst)
    {
        bool isMove = inst.opcode == Move;
        if (!isMove)
            return false;

        ASSERT_WITH_MESSAGE(inst.args.size() == 2, "We assume coalecable moves only have two arguments in a few places.");
        ASSERT(inst.args[0].isType(Arg::GP));
        ASSERT(inst.args[1].isType(Arg::GP));

        return inst.args[0].isTmp() && inst.args[1].isTmp();
    }
};

template<>
struct MoveInstHelper<Arg::FP> {
    static bool mayBeCoalescable(const Inst& inst)
    {
        if (inst.opcode != MoveDouble)
            return false;

        ASSERT_WITH_MESSAGE(inst.args.size() == 2, "We assume coalecable moves only have two arguments in a few places.");
        ASSERT(inst.args[0].isType(Arg::FP));
        ASSERT(inst.args[1].isType(Arg::FP));

        return inst.args[0].isTmp() && inst.args[1].isTmp();
    }
};


// The speed of the alocator depends directly on how fast we can query information associated with a Tmp
// and/or its ownership to a set.
//
// HashSet/HashMap operations are overly expensive for that.
//
// Instead of a Hash structure, Tmp are indexed directly by value in Arrays. The internal integer is used as the index
// to reference them quickly. In some sets, we do not care about the colored regs, we still allocate the memory for them
// and just do not use it.
template<Arg::Type type>
struct AbsoluteTmpHelper;

template<>
struct AbsoluteTmpHelper<Arg::GP> {
    static unsigned absoluteIndex(const Tmp& tmp)
    {
        ASSERT(tmp.isGP());
        ASSERT(static_cast<int>(tmp.internalValue()) > 0);
        return tmp.internalValue();
    }

    static unsigned absoluteIndex(unsigned tmpIndex)
    {
        return absoluteIndex(Tmp::gpTmpForIndex(tmpIndex));
    }

    static Tmp tmpFromAbsoluteIndex(unsigned tmpIndex)
    {
        return Tmp::tmpForInternalValue(tmpIndex);
    }
};

template<>
struct AbsoluteTmpHelper<Arg::FP> {
    static unsigned absoluteIndex(const Tmp& tmp)
    {
        ASSERT(tmp.isFP());
        ASSERT(static_cast<int>(tmp.internalValue()) < 0);
        return -tmp.internalValue();
    }

    static unsigned absoluteIndex(unsigned tmpIndex)
    {
        return absoluteIndex(Tmp::fpTmpForIndex(tmpIndex));
    }

    static Tmp tmpFromAbsoluteIndex(unsigned tmpIndex)
    {
        return Tmp::tmpForInternalValue(-tmpIndex);
    }
};

template<Arg::Type type>
class IteratedRegisterCoalescingAllocator {
public:
    IteratedRegisterCoalescingAllocator(Code& code)
        : m_numberOfRegisters(regsInPriorityOrder(type).size())
    {
        initializeDegrees(code);

        unsigned tmpArraySize = this->tmpArraySize(code);
        m_adjacencyList.resize(tmpArraySize);
        m_moveList.resize(tmpArraySize);
        m_coalescedTmps.resize(tmpArraySize);
        m_isOnSelectStack.ensureSize(tmpArraySize);
    }

    void build(Inst& inst, const Liveness<Tmp>::LocalCalc& localCalc)
    {
        // All the Def()s interfere with eachother.
        inst.forEachTmp([&] (Tmp& arg, Arg::Role role, Arg::Type argType) {
            if (argType != type)
                return;

            if (Arg::isDef(role)) {
                inst.forEachTmp([&] (Tmp& otherArg, Arg::Role role, Arg::Type) {
                    if (argType != type)
                        return;

                    if (Arg::isDef(role))
                        addEdge(arg, otherArg);
                });
            }
        });

        if (MoveInstHelper<type>::mayBeCoalescable(inst)) {
            for (const Arg& arg : inst.args) {
                HashSet<Inst*>& list = m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(arg.tmp())];
                list.add(&inst);
            }
            m_worklistMoves.add(&inst);

            // We do not want the Use() of this move to interfere with the Def(), even if it is live
            // after the Move. If we were to add the interference edge, it would be impossible to
            // coalesce the Move even if the two Tmp never interfere anywhere.
            Tmp defTmp;
            Tmp useTmp;
            inst.forEachTmp([&defTmp, &useTmp] (Tmp& argTmp, Arg::Role role, Arg::Type) {
                if (Arg::isDef(role))
                    defTmp = argTmp;
                else {
                    ASSERT(Arg::isUse(role));
                    useTmp = argTmp;
                }
            });
            ASSERT(defTmp);
            ASSERT(useTmp);

            for (const Tmp& liveTmp : localCalc.live()) {
                if (liveTmp != useTmp && liveTmp.isGP() == (type == Arg::GP))
                    addEdge(defTmp, liveTmp);
            }
        } else
            addEdges(inst, localCalc.live());
    }

    void allocate()
    {
        makeWorkList();

        if (debug) {
            dumpInterferenceGraphInDot(WTF::dataFile());
            dataLog("Initial work list\n");
            dumpWorkLists(WTF::dataFile());
        }

        do {
            if (traceDebug) {
                dataLog("Before Graph simplification iteration\n");
                dumpWorkLists(WTF::dataFile());
            }

            if (!m_simplifyWorklist.isEmpty())
                simplify();
            else if (!m_worklistMoves.isEmpty())
                coalesce();
            else if (!m_freezeWorklist.isEmpty())
                freeze();
            else if (!m_spillWorklist.isEmpty())
                selectSpill();

            if (traceDebug) {
                dataLog("After Graph simplification iteration\n");
                dumpWorkLists(WTF::dataFile());
            }
        } while (!m_simplifyWorklist.isEmpty() || !m_worklistMoves.isEmpty() || !m_freezeWorklist.isEmpty() || !m_spillWorklist.isEmpty());

        assignColors();
    }

    Tmp getAlias(Tmp tmp) const
    {
        Tmp alias = tmp;
        while (Tmp nextAlias = m_coalescedTmps[AbsoluteTmpHelper<type>::absoluteIndex(alias)])
            alias = nextAlias;
        return alias;
    }

    const HashSet<Tmp>& spilledTmp() const { return m_spilledTmp; }
    Reg allocatedReg(Tmp tmp) const
    {
        ASSERT(!tmp.isReg());
        ASSERT(m_coloredTmp.size());
        ASSERT(tmp.isGP() == (type == Arg::GP));

        Reg reg = m_coloredTmp[AbsoluteTmpHelper<type>::absoluteIndex(tmp)];
        if (!reg) {
            // We only care about Tmps that interfere. A Tmp that never interfere with anything
            // can take any register.
            reg = regsInPriorityOrder(type).first();
        }
        return reg;
    }

private:
    static unsigned tmpArraySize(Code& code)
    {
        unsigned numTmps = code.numTmps(type);
        return AbsoluteTmpHelper<type>::absoluteIndex(numTmps);
    }

    void initializeDegrees(Code& code)
    {
        unsigned tmpArraySize = this->tmpArraySize(code);
        m_degrees.resize(tmpArraySize);

        // All precolored registers have  an "infinite" degree.
        unsigned firstNonRegIndex = AbsoluteTmpHelper<type>::absoluteIndex(0);
        for (unsigned i = 0; i < firstNonRegIndex; ++i)
            m_degrees[i] = std::numeric_limits<unsigned>::max();

        bzero(m_degrees.data() + firstNonRegIndex, (tmpArraySize - firstNonRegIndex) * sizeof(unsigned));
    }

    void addEdges(Inst& inst, const HashSet<Tmp>& liveTmp)
    {
        // All the Def()s interfere with everthing live.
        inst.forEachTmp([&] (Tmp& arg, Arg::Role role, Arg::Type argType) {
            if (argType == type && Arg::isDef(role)) {
                for (const Tmp& liveTmp : liveTmp) {
                    if (liveTmp.isGP() == (type == Arg::GP))
                        addEdge(arg, liveTmp);
                }
            }
        });
    }

    void addEdge(const Tmp& a, const Tmp& b)
    {
        if (a == b)
            return;

        if (m_interferenceEdges.add(InterferenceEdge(a, b)).isNewEntry) {
            if (!a.isReg()) {
                ASSERT(!m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(a)].contains(b));
                m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(a)].append(b);
                m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(a)]++;
            }

            if (!b.isReg()) {
                ASSERT(!m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(b)].contains(a));
                m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(b)].append(a);
                m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(b)]++;
            }
        }
    }

    void makeWorkList()
    {
        unsigned firstNonRegIndex = AbsoluteTmpHelper<type>::absoluteIndex(0);
        for (unsigned i = firstNonRegIndex; i < m_degrees.size(); ++i) {
            unsigned degree = m_degrees[i];
            if (!degree)
                continue;

            Tmp tmp = AbsoluteTmpHelper<type>::tmpFromAbsoluteIndex(i);

            if (degree >= m_numberOfRegisters)
                m_spillWorklist.add(tmp);
            else if (!m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(tmp)].isEmpty())
                m_freezeWorklist.add(tmp);
            else
                m_simplifyWorklist.append(tmp);
        }
    }

    void simplify()
    {
        Tmp last = m_simplifyWorklist.takeLast();

        ASSERT(!m_selectStack.contains(last));
        ASSERT(!m_isOnSelectStack.get(AbsoluteTmpHelper<type>::absoluteIndex(last)));
        m_selectStack.append(last);
        m_isOnSelectStack.quickSet(AbsoluteTmpHelper<type>::absoluteIndex(last));

        forEachAdjacent(last, [this](Tmp adjacentTmp) {
            decrementDegree(adjacentTmp);
        });
    }

    template<typename Function>
    void forEachAdjacent(Tmp tmp, Function function)
    {
        for (Tmp adjacentTmp : m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]) {
            if (!hasBeenSimplified(adjacentTmp))
                function(adjacentTmp);
        }
    }

    bool hasBeenSimplified(Tmp tmp)
    {
        return m_isOnSelectStack.quickGet(AbsoluteTmpHelper<type>::absoluteIndex(tmp)) || !!m_coalescedTmps[AbsoluteTmpHelper<type>::absoluteIndex(tmp)];
    }

    void decrementDegree(Tmp tmp)
    {
        ASSERT(m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]);

        unsigned oldDegree = m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]--;
        if (oldDegree == m_numberOfRegisters) {
            enableMovesOnValueAndAdjacents(tmp);
            m_spillWorklist.remove(tmp);
            if (isMoveRelated(tmp))
                m_freezeWorklist.add(tmp);
            else
                m_simplifyWorklist.append(tmp);
        }
    }

    template<typename Function>
    void forEachNodeMoves(Tmp tmp, Function function)
    {
        for (Inst* inst : m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]) {
            if (m_activeMoves.contains(inst) || m_worklistMoves.contains(inst))
                function(*inst);
        }
    }

    bool isMoveRelated(Tmp tmp)
    {
        for (Inst* inst : m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]) {
            if (m_activeMoves.contains(inst) || m_worklistMoves.contains(inst))
                return true;
        }
        return false;
    }

    void enableMovesOnValue(Tmp tmp)
    {
        for (Inst* inst : m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]) {
            if (m_activeMoves.remove(inst))
                m_worklistMoves.add(inst);
        }
    }

    void enableMovesOnValueAndAdjacents(Tmp tmp)
    {
        enableMovesOnValue(tmp);

        forEachAdjacent(tmp, [this] (Tmp adjacentTmp) {
            enableMovesOnValue(adjacentTmp);
        });
    }

    void coalesce()
    {
        Inst* moveInst = m_worklistMoves.takeLast();
        ASSERT(moveInst->args.size() == 2);

        Tmp u = moveInst->args[0].tmp();
        u = getAlias(u);
        Tmp v = moveInst->args[1].tmp();
        v = getAlias(v);

        if (v.isReg())
            std::swap(u, v);

        if (traceDebug)
            dataLog("Coalescing ", *moveInst, " u = ", u, " v = ", v, "\n");

        if (u == v) {
            addWorkList(u);

            if (traceDebug)
                dataLog("    Coalesced\n");
        } else if (v.isReg() || m_interferenceEdges.contains(InterferenceEdge(u, v))) {
            addWorkList(u);
            addWorkList(v);

            if (traceDebug)
                dataLog("    Constrained\n");
        } else if (canBeSafelyCoalesced(u, v)) {
            combine(u, v);
            addWorkList(u);

            if (traceDebug)
                dataLog("    Safe Coalescing\n");
        } else {
            m_activeMoves.add(moveInst);

            if (traceDebug)
                dataLog("    Failed coalescing, added to active moves.\n");
        }
    }

    bool canBeSafelyCoalesced(Tmp u, Tmp v)
    {
        ASSERT(!v.isReg());
        if (u.isReg())
            return precoloredCoalescingHeuristic(u, v);
        return conservativeHeuristic(u, v);
    }

    bool precoloredCoalescingHeuristic(Tmp u, Tmp v)
    {
        ASSERT(u.isReg());
        ASSERT(!v.isReg());

        // If any adjacent of the non-colored node is not an adjacent of the colored node AND has a degree >= K
        // there is a risk that this node needs to have the same color as our precolored node. If we coalesce such
        // move, we may create an uncolorable graph.
        auto adjacentsOfV = m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(v)];
        for (Tmp adjacentTmp : adjacentsOfV) {
            if (!adjacentTmp.isReg()
                && !hasBeenSimplified(adjacentTmp)
                && m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(adjacentTmp)] >= m_numberOfRegisters
                && !m_interferenceEdges.contains(InterferenceEdge(u, adjacentTmp)))
                return false;
        }
        return true;
    }

    bool conservativeHeuristic(Tmp u, Tmp v)
    {
        // This is using the Briggs' conservative coalescing rule:
        // If the number of combined adjacent node with a degree >= K is less than K,
        // it is safe to combine the two nodes. The reason is that we know that if the graph
        // is colorable, we have fewer than K adjacents with high order and there is a color
        // for the current node.
        ASSERT(u != v);
        ASSERT(!u.isReg());
        ASSERT(!v.isReg());

        auto adjacentsOfU = m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(u)];
        auto adjacentsOfV = m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(v)];

        if (adjacentsOfU.size() + adjacentsOfV.size() < m_numberOfRegisters) {
            // Shortcut: if the total number of adjacents is less than the number of register, the condition is always met.
            return true;
        }

        HashSet<Tmp> highOrderAdjacents;

        for (Tmp adjacentTmp : adjacentsOfU) {
            ASSERT(adjacentTmp != v);
            ASSERT(adjacentTmp != u);
            if (!hasBeenSimplified(adjacentTmp) && m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(adjacentTmp)] >= m_numberOfRegisters) {
                auto addResult = highOrderAdjacents.add(adjacentTmp);
                if (addResult.isNewEntry && highOrderAdjacents.size() >= m_numberOfRegisters)
                    return false;
            }
        }
        for (Tmp adjacentTmp : adjacentsOfV) {
            ASSERT(adjacentTmp != u);
            ASSERT(adjacentTmp != v);
            if (!hasBeenSimplified(adjacentTmp) && m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(adjacentTmp)] >= m_numberOfRegisters) {
                auto addResult = highOrderAdjacents.add(adjacentTmp);
                if (addResult.isNewEntry && highOrderAdjacents.size() >= m_numberOfRegisters)
                    return false;
            }
        }

        ASSERT(highOrderAdjacents.size() < m_numberOfRegisters);
        return true;
    }

    void addWorkList(Tmp tmp)
    {
        if (!tmp.isReg() && m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(tmp)] < m_numberOfRegisters && !isMoveRelated(tmp)) {
            m_freezeWorklist.remove(tmp);
            m_simplifyWorklist.append(tmp);
        }
    }

    void combine(Tmp u, Tmp v)
    {
        if (!m_freezeWorklist.remove(v))
            m_spillWorklist.remove(v);

        ASSERT(!m_coalescedTmps[AbsoluteTmpHelper<type>::absoluteIndex(v)]);
        m_coalescedTmps[AbsoluteTmpHelper<type>::absoluteIndex(v)] = u;

        HashSet<Inst*>& vMoves = m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(v)];
        m_moveList[AbsoluteTmpHelper<type>::absoluteIndex(u)].add(vMoves.begin(), vMoves.end());

        forEachAdjacent(v, [this, u] (Tmp adjacentTmp) {
            addEdge(adjacentTmp, u);
            decrementDegree(adjacentTmp);
        });

        if (m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(u)] >= m_numberOfRegisters && m_freezeWorklist.remove(u))
            m_spillWorklist.add(u);
    }

    void freeze()
    {
        Tmp victim = m_freezeWorklist.takeAny();
        m_simplifyWorklist.append(victim);
        freezeMoves(victim);
    }

    void freezeMoves(Tmp tmp)
    {
        forEachNodeMoves(tmp, [this, tmp] (Inst& inst) {
            if (!m_activeMoves.remove(&inst))
                m_worklistMoves.remove(&inst);

            Tmp otherTmp = inst.args[0].tmp() != tmp ? inst.args[0].tmp() : inst.args[1].tmp();
            if (m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(otherTmp)] < m_numberOfRegisters && !isMoveRelated(otherTmp)) {
                m_freezeWorklist.remove(otherTmp);
                m_simplifyWorklist.append(otherTmp);
            }
        });
    }

    void selectSpill()
    {
        // FIXME: we should select a good candidate based on all the information we have.
        // FIXME: we should never select a spilled tmp as we would never converge.

        auto iterator = m_spillWorklist.begin();

        auto victimIterator = iterator;
        unsigned maxDegree = m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(*iterator)];

        ++iterator;
        for (;iterator != m_spillWorklist.end(); ++iterator) {
            unsigned tmpDegree = m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(*iterator)];
            if (tmpDegree > maxDegree) {
                victimIterator = iterator;
                maxDegree = tmpDegree;
            }
        }

        Tmp victimTmp = *victimIterator;
        m_spillWorklist.remove(victimIterator);
        m_simplifyWorklist.append(victimTmp);
        freezeMoves(victimTmp);
    }

    void assignColors()
    {
        ASSERT(m_simplifyWorklist.isEmpty());
        ASSERT(m_worklistMoves.isEmpty());
        ASSERT(m_freezeWorklist.isEmpty());
        ASSERT(m_spillWorklist.isEmpty());

        // Reclaim as much memory as possible.
        m_interferenceEdges.clear();
        m_degrees.clear();
        m_moveList.clear();
        m_worklistMoves.clear();
        m_activeMoves.clear();
        m_simplifyWorklist.clear();
        m_spillWorklist.clear();
        m_freezeWorklist.clear();

        // Try to color the Tmp on the stack.
        m_coloredTmp.resize(m_adjacencyList.size());
        const auto& registersInPriorityOrder = regsInPriorityOrder(type);

        while (!m_selectStack.isEmpty()) {
            Tmp tmp = m_selectStack.takeLast();
            ASSERT(!tmp.isReg());
            ASSERT(!m_coloredTmp[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]);

            RegisterSet coloredRegisters;
            for (Tmp adjacentTmp : m_adjacencyList[AbsoluteTmpHelper<type>::absoluteIndex(tmp)]) {
                Tmp aliasTmp = getAlias(adjacentTmp);
                if (aliasTmp.isReg()) {
                    coloredRegisters.set(aliasTmp.reg());
                    continue;
                }

                Reg reg = m_coloredTmp[AbsoluteTmpHelper<type>::absoluteIndex(aliasTmp)];
                if (reg)
                    coloredRegisters.set(reg);
            }

            bool colorAssigned = false;
            for (Reg reg : registersInPriorityOrder) {
                if (!coloredRegisters.get(reg)) {
                    m_coloredTmp[AbsoluteTmpHelper<type>::absoluteIndex(tmp)] = reg;
                    colorAssigned = true;
                    break;
                }
            }

            if (!colorAssigned)
                m_spilledTmp.add(tmp);
        }
        m_selectStack.clear();

        if (!m_spilledTmp.isEmpty())
            m_coloredTmp.clear();
    }

#pragma mark - Debugging helpers.

    void dumpInterferenceGraphInDot(PrintStream& out)
    {
        out.print("graph InterferenceGraph { \n");

        HashSet<Tmp> tmpsWithInterferences;
        for (const auto& edge : m_interferenceEdges) {
            tmpsWithInterferences.add(edge.first());
            tmpsWithInterferences.add(edge.second());
        }

        for (const auto& tmp : tmpsWithInterferences)
            out.print("    ", tmp.internalValue(), " [label=\"", tmp, " (", m_degrees[AbsoluteTmpHelper<type>::absoluteIndex(tmp)], ")\"];\n");

        for (const auto& edge : m_interferenceEdges)
            out.print("    ", edge.first().internalValue(), " -- ", edge.second().internalValue(), ";\n");
        out.print("}\n");
    }

    void dumpWorkLists(PrintStream& out)
    {
        out.print("Simplify work list:\n");
        for (Tmp tmp : m_simplifyWorklist)
            out.print("    ", tmp, "\n");
        out.print("Moves work list:\n");
        for (Inst* inst : m_worklistMoves)
            out.print("    ", *inst, "\n");
        out.print("Freeze work list:\n");
        for (Tmp tmp : m_freezeWorklist)
            out.print("    ", tmp, "\n");
        out.print("Spill work list:\n");
        for (Tmp tmp : m_spillWorklist)
            out.print("    ", tmp, "\n");
    }

#pragma mark -

    // Interference edges are not directed. An edge between any two Tmps is represented
    // by the concatenated values of the smallest Tmp followed by the bigger Tmp.
    class InterferenceEdge {
    public:
        InterferenceEdge()
        {
        }

        InterferenceEdge(Tmp a, Tmp b)
        {
            ASSERT(a.internalValue());
            ASSERT(b.internalValue());
            ASSERT_WITH_MESSAGE(a != b, "A Tmp can never interfere with itself. Doing so would force it to be the superposition of two registers.");

            unsigned aInternal = a.internalValue();
            unsigned bInternal = b.internalValue();
            if (bInternal < aInternal)
                std::swap(aInternal, bInternal);
            m_value = static_cast<uint64_t>(aInternal) << 32 | bInternal;
        }

        InterferenceEdge(WTF::HashTableDeletedValueType)
            : m_value(std::numeric_limits<uint64_t>::max())
        {
        }

        Tmp first() const
        {
            return Tmp::tmpForInternalValue(m_value >> 32);
        }

        Tmp second() const
        {
            return Tmp::tmpForInternalValue(m_value & 0xffffffff);
        }

        bool operator==(const InterferenceEdge other) const
        {
            return m_value == other.m_value;
        }

        bool isHashTableDeletedValue() const
        {
            return *this == InterferenceEdge(WTF::HashTableDeletedValue);
        }

        unsigned hash() const
        {
            return WTF::IntHash<uint64_t>::hash(m_value);
        }

    private:
        uint64_t m_value { 0 };
    };

    struct InterferenceEdgeHash {
        static unsigned hash(const InterferenceEdge& key) { return key.hash(); }
        static bool equal(const InterferenceEdge& a, const InterferenceEdge& b) { return a == b; }
        static const bool safeToCompareToEmptyOrDeleted = true;
    };
    typedef SimpleClassHashTraits<InterferenceEdge> InterferenceEdgeHashTraits;

    unsigned m_numberOfRegisters { 0 };

    // The interference graph.
    HashSet<InterferenceEdge, InterferenceEdgeHash, InterferenceEdgeHashTraits> m_interferenceEdges;
    Vector<Vector<Tmp, 0, UnsafeVectorOverflow, 4>, 0, UnsafeVectorOverflow> m_adjacencyList;
    Vector<unsigned, 0, UnsafeVectorOverflow> m_degrees;

    // List of every move instruction associated with a Tmp.
    Vector<HashSet<Inst*>> m_moveList;

    // Colors.
    Vector<Reg, 0, UnsafeVectorOverflow> m_coloredTmp;
    HashSet<Tmp> m_spilledTmp;

    // Values that have been coalesced with an other value.
    Vector<Tmp> m_coalescedTmps;

    // The stack of Tmp removed from the graph and ready for coloring.
    BitVector m_isOnSelectStack;
    Vector<Tmp> m_selectStack;

    // Work lists.
    // Set of "move" enabled for possible coalescing.
    ListHashSet<Inst*> m_worklistMoves;
    // Set of "move" not yet ready for coalescing.
    HashSet<Inst*> m_activeMoves;
    // Low-degree, non-Move related.
    Vector<Tmp> m_simplifyWorklist;
    // High-degree Tmp.
    HashSet<Tmp> m_spillWorklist;
    // Low-degree, Move related.
    HashSet<Tmp> m_freezeWorklist;
};

template<Arg::Type type>
static bool isUselessMoveInst(const Inst& inst)
{
    return MoveInstHelper<type>::mayBeCoalescable(inst) && inst.args[0].tmp() == inst.args[1].tmp();
}

template<Arg::Type type>
static void assignRegisterToTmpInProgram(Code& code, const IteratedRegisterCoalescingAllocator<type>& allocator)
{
    for (BasicBlock* block : code) {
        // Give Tmp a valid register.
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);
            inst.forEachTmpFast([&] (Tmp& tmp) {
                if (tmp.isReg() || tmp.isGP() == (type != Arg::GP))
                    return;

                Tmp aliasTmp = allocator.getAlias(tmp);
                Tmp assignedTmp;
                if (aliasTmp.isReg())
                    assignedTmp = Tmp(aliasTmp.reg());
                else {
                    auto reg = allocator.allocatedReg(aliasTmp);
                    ASSERT(reg);
                    assignedTmp = Tmp(reg);
                }
                ASSERT(assignedTmp.isReg());
                tmp = assignedTmp;
            });
        }

        // Remove all the useless moves we created in this block.
        block->insts().removeAllMatching(isUselessMoveInst<type>);
    }
}

template<Arg::Type type>
static void addSpillAndFillToProgram(Code& code, const HashSet<Tmp>& spilledTmp)
{
    // Allocate stack slot for each spilled value.
    HashMap<Tmp, StackSlot*> stackSlots;
    for (Tmp tmp : spilledTmp) {
        bool isNewTmp = stackSlots.add(tmp, code.addStackSlot(8, StackSlotKind::Anonymous)).isNewEntry;
        ASSERT_UNUSED(isNewTmp, isNewTmp);
    }

    // Rewrite the program to get rid of the spilled Tmp.
    InsertionSet insertionSet(code);
    for (BasicBlock* block : code) {
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);

            // Try to replace the register use by memory use when possible.
            for (unsigned i = 0; i < inst.args.size(); ++i) {
                Arg& arg = inst.args[i];
                if (arg.isTmp() && arg.type() == type && !arg.isReg()) {
                    auto stackSlotEntry = stackSlots.find(arg.tmp());
                    if (stackSlotEntry == stackSlots.end())
                        continue;

                    if (inst.admitsStack(i)) {
                        arg = Arg::stack(stackSlotEntry->value);
                        continue;
                    }
                }
            }

            // For every other case, add Load/Store as needed.
            inst.forEachTmp([&] (Tmp& tmp, Arg::Role role, Arg::Type argType) {
                if (tmp.isReg() || argType != type)
                    return;

                auto stackSlotEntry = stackSlots.find(tmp);
                if (stackSlotEntry == stackSlots.end())
                    return;

                Arg arg = Arg::stack(stackSlotEntry->value);
                Opcode move = type == Arg::GP ? Move : MoveDouble;

                if (Arg::isUse(role)) {
                    Tmp newTmp = code.newTmp(type);
                    insertionSet.insert(instIndex, move, inst.origin, arg, newTmp);
                    tmp = newTmp;
                }
                if (Arg::isDef(role))
                    insertionSet.insert(instIndex + 1, move, inst.origin, tmp, arg);
            });
        }
        insertionSet.execute(block);
    }
}

template<Arg::Type type>
static void iteratedRegisterCoalescingOnType(Code& code)
{
    while (true) {
        IteratedRegisterCoalescingAllocator<type> allocator(code);
        Liveness<Tmp> liveness(code);
        for (BasicBlock* block : code) {
            Liveness<Tmp>::LocalCalc localCalc(liveness, block);
            for (unsigned instIndex = block->size(); instIndex--;) {
                Inst& inst = block->at(instIndex);
                allocator.build(inst, localCalc);
                localCalc.execute(inst);
            }
        }

        allocator.allocate();
        if (allocator.spilledTmp().isEmpty()) {
            assignRegisterToTmpInProgram(code, allocator);
            return;
        }
        addSpillAndFillToProgram<type>(code, allocator.spilledTmp());
    }
}

void iteratedRegisterCoalescing(Code& code)
{
    PhaseScope phaseScope(code, "iteratedRegisterCoalescing");

    bool gpIsColored = false;
    bool fpIsColored = false;

    // First we run both allocator together as long as they both spill.
    while (!gpIsColored && !fpIsColored) {
        IteratedRegisterCoalescingAllocator<Arg::GP> gpAllocator(code);
        IteratedRegisterCoalescingAllocator<Arg::FP> fpAllocator(code);

        // Liveness Analysis can be prohibitively expensive. It is shared
        // between the two allocators to avoid doing it twice.
        Liveness<Tmp> liveness(code);
        for (BasicBlock* block : code) {
            Liveness<Tmp>::LocalCalc localCalc(liveness, block);
            for (unsigned instIndex = block->size(); instIndex--;) {
                Inst& inst = block->at(instIndex);

                gpAllocator.build(inst, localCalc);
                fpAllocator.build(inst, localCalc);

                localCalc.execute(inst);
            }
        }

        gpAllocator.allocate();
        if (gpAllocator.spilledTmp().isEmpty()) {
            assignRegisterToTmpInProgram(code, gpAllocator);
            gpIsColored = true;
        } else
            addSpillAndFillToProgram<Arg::GP>(code, gpAllocator.spilledTmp());

        fpAllocator.allocate();
        if (fpAllocator.spilledTmp().isEmpty()) {
            assignRegisterToTmpInProgram(code, fpAllocator);
            fpIsColored = true;
        } else
            addSpillAndFillToProgram<Arg::FP>(code, fpAllocator.spilledTmp());
    };

    if (!gpIsColored)
        iteratedRegisterCoalescingOnType<Arg::GP>(code);
    if (!fpIsColored)
        iteratedRegisterCoalescingOnType<Arg::FP>(code);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
