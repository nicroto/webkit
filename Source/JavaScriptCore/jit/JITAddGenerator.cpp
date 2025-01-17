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

#if ENABLE(JIT)
#include "JITAddGenerator.h"

namespace JSC {

void JITAddGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_scratchGPR != InvalidGPRReg);
    ASSERT(m_scratchGPR != m_left.payloadGPR());
    ASSERT(m_scratchGPR != m_right.payloadGPR());
#if ENABLE(JSVALUE32_64)
    ASSERT(m_scratchGPR != m_left.tagGPR());x
    ASSERT(m_scratchGPR != m_right.tagGPR());
    ASSERT(m_scratchFPR != InvalidFPRReg);
#endif

    if (!m_leftType.mightBeNumber() || !m_rightType.mightBeNumber()) {
        m_slowPathJumpList.append(jit.jump());
        return;
    }

    if (m_operandsConstness == RightIsConstInt32) {
        // Try to do intVar + intConstant.
        CCallHelpers::Jump notInt32 = jit.branchIfNotInt32(m_left);

        m_slowPathJumpList.append(jit.branchAdd32(CCallHelpers::Overflow, m_left.payloadGPR(), CCallHelpers::Imm32(m_rightConstInt32), m_scratchGPR));

        jit.boxInt32(m_scratchGPR, m_result);
        m_endJumpList.append(jit.jump());

        if (!jit.supportsFloatingPoint()) {
            m_slowPathJumpList.append(notInt32);
            return;
        }

        // Try to do doubleVar + double(intConstant).
        notInt32.link(&jit);
        if (!m_leftType.definitelyIsNumber())
            m_slowPathJumpList.append(jit.branchIfNotNumber(m_left, m_scratchGPR));

        jit.unboxDoubleNonDestructive(m_left, m_leftFPR, m_scratchGPR, m_scratchFPR);

        jit.move(CCallHelpers::Imm32(m_rightConstInt32), m_scratchGPR);
        jit.convertInt32ToDouble(m_scratchGPR, m_rightFPR);

        // Fall thru to doubleVar + doubleVar.

    } else {
        ASSERT(m_operandsConstness == NeitherAreConstInt32);
        CCallHelpers::Jump leftNotInt;
        CCallHelpers::Jump rightNotInt;

        // Try to do intVar + intVar.
        leftNotInt = jit.branchIfNotInt32(m_left);
        rightNotInt = jit.branchIfNotInt32(m_right);

        m_slowPathJumpList.append(jit.branchAdd32(CCallHelpers::Overflow, m_right.payloadGPR(), m_left.payloadGPR(), m_scratchGPR));

        jit.boxInt32(m_scratchGPR, m_result);
        m_endJumpList.append(jit.jump());

        if (!jit.supportsFloatingPoint()) {
            m_slowPathJumpList.append(leftNotInt);
            m_slowPathJumpList.append(rightNotInt);
            return;
        }

        leftNotInt.link(&jit);
        if (!m_leftType.definitelyIsNumber())
            m_slowPathJumpList.append(jit.branchIfNotNumber(m_left, m_scratchGPR));
        if (!m_rightType.definitelyIsNumber())
            m_slowPathJumpList.append(jit.branchIfNotNumber(m_right, m_scratchGPR));

        jit.unboxDoubleNonDestructive(m_left, m_leftFPR, m_scratchGPR, m_scratchFPR);
        CCallHelpers::Jump rightIsDouble = jit.branchIfNotInt32(m_right);

        jit.convertInt32ToDouble(m_right.payloadGPR(), m_rightFPR);
        CCallHelpers::Jump rightWasInteger = jit.jump();

        rightNotInt.link(&jit);
        if (!m_rightType.definitelyIsNumber())
            m_slowPathJumpList.append(jit.branchIfNotNumber(m_right, m_scratchGPR));

        jit.convertInt32ToDouble(m_left.payloadGPR(), m_leftFPR);

        rightIsDouble.link(&jit);
        jit.unboxDoubleNonDestructive(m_right, m_rightFPR, m_scratchGPR, m_scratchFPR);

        rightWasInteger.link(&jit);

        // Fall thru to doubleVar + doubleVar.
    }

    // Do doubleVar + doubleVar.
    jit.addDouble(m_rightFPR, m_leftFPR);
    jit.boxDouble(m_leftFPR, m_result);

    m_endJumpList.append(jit.jump());
}

} // namespace JSC

#endif // ENABLE(JIT)
