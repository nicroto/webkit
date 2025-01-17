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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FontVariantBuilder_h
#define FontVariantBuilder_h

namespace WebCore {

template <typename T>
inline void applyValueFontVariantLigatures(T& receiver, CSSValue& value)
{
    FontVariantLigatures common = FontVariantLigatures::Normal;
    FontVariantLigatures discretionary = FontVariantLigatures::Normal;
    FontVariantLigatures historical = FontVariantLigatures::Normal;
    FontVariantLigatures contextualAlternates = FontVariantLigatures::Normal;

    if (is<CSSValueList>(value)) {
        for (auto& item : downcast<CSSValueList>(value)) {
            switch (downcast<CSSPrimitiveValue>(item.get()).getValueID()) {
            case CSSValueNoCommonLigatures:
                common = FontVariantLigatures::No;
                break;
            case CSSValueCommonLigatures:
                common = FontVariantLigatures::Yes;
                break;
            case CSSValueNoDiscretionaryLigatures:
                discretionary = FontVariantLigatures::No;
                break;
            case CSSValueDiscretionaryLigatures:
                discretionary = FontVariantLigatures::Yes;
                break;
            case CSSValueNoHistoricalLigatures:
                historical = FontVariantLigatures::No;
                break;
            case CSSValueHistoricalLigatures:
                historical = FontVariantLigatures::Yes;
                break;
            case CSSValueContextual:
                contextualAlternates = FontVariantLigatures::Yes;
                break;
            case CSSValueNoContextual:
                contextualAlternates = FontVariantLigatures::No;
                break;
            default:
                ASSERT_NOT_REACHED();
                break;
            }
        }
    } else {
        switch (downcast<CSSPrimitiveValue>(value).getValueID()) {
        case CSSValueNormal:
            break;
        case CSSValueNone:
            common = FontVariantLigatures::No;
            discretionary = FontVariantLigatures::No;
            historical = FontVariantLigatures::No;
            contextualAlternates = FontVariantLigatures::No;
            break;
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    receiver.setVariantCommonLigatures(common);
    receiver.setVariantDiscretionaryLigatures(discretionary);
    receiver.setVariantHistoricalLigatures(historical);
    receiver.setVariantContextualAlternates(contextualAlternates);
}

template <typename T>
inline void applyValueFontVariantNumeric(T& receiver, CSSValue& value)
{
    FontVariantNumericFigure figure = FontVariantNumericFigure::Normal;
    FontVariantNumericSpacing spacing = FontVariantNumericSpacing::Normal;
    FontVariantNumericFraction fraction = FontVariantNumericFraction::Normal;
    FontVariantNumericOrdinal ordinal = FontVariantNumericOrdinal::Normal;
    FontVariantNumericSlashedZero slashedZero = FontVariantNumericSlashedZero::Normal;

    if (is<CSSValueList>(value)) {
        for (auto& item : downcast<CSSValueList>(value)) {
            switch (downcast<CSSPrimitiveValue>(item.get()).getValueID()) {
            case CSSValueLiningNums:
                figure = FontVariantNumericFigure::LiningNumbers;
                break;
            case CSSValueOldstyleNums:
                figure = FontVariantNumericFigure::OldStyleNumbers;
                break;
            case CSSValueProportionalNums:
                spacing = FontVariantNumericSpacing::ProportionalNumbers;
                break;
            case CSSValueTabularNums:
                spacing = FontVariantNumericSpacing::TabularNumbers;
                break;
            case CSSValueDiagonalFractions:
                fraction = FontVariantNumericFraction::DiagonalFractions;
                break;
            case CSSValueStackedFractions:
                fraction = FontVariantNumericFraction::StackedFractions;
                break;
            case CSSValueOrdinal:
                ordinal = FontVariantNumericOrdinal::Yes;
                break;
            case CSSValueSlashedZero:
                slashedZero = FontVariantNumericSlashedZero::Yes;
                break;
            default:
                ASSERT_NOT_REACHED();
                break;
            }
        }
    } else
        ASSERT(downcast<CSSPrimitiveValue>(value).getValueID() == CSSValueNormal);

    receiver.setVariantNumericFigure(figure);
    receiver.setVariantNumericSpacing(spacing);
    receiver.setVariantNumericFraction(fraction);
    receiver.setVariantNumericOrdinal(ordinal);
    receiver.setVariantNumericSlashedZero(slashedZero);
}

template <typename T>
inline void applyValueFontVariantEastAsian(T& receiver, CSSValue& value)
{
    FontVariantEastAsianVariant variant = FontVariantEastAsianVariant::Normal;
    FontVariantEastAsianWidth width = FontVariantEastAsianWidth::Normal;
    FontVariantEastAsianRuby ruby = FontVariantEastAsianRuby::Normal;

    if (is<CSSValueList>(value)) {
        for (auto& item : downcast<CSSValueList>(value)) {
            switch (downcast<CSSPrimitiveValue>(item.get()).getValueID()) {
            case CSSValueJis78:
                variant = FontVariantEastAsianVariant::Jis78;
                break;
            case CSSValueJis83:
                variant = FontVariantEastAsianVariant::Jis83;
                break;
            case CSSValueJis90:
                variant = FontVariantEastAsianVariant::Jis90;
                break;
            case CSSValueJis04:
                variant = FontVariantEastAsianVariant::Jis04;
                break;
            case CSSValueSimplified:
                variant = FontVariantEastAsianVariant::Simplified;
                break;
            case CSSValueTraditional:
                variant = FontVariantEastAsianVariant::Traditional;
                break;
            case CSSValueFullWidth:
                width = FontVariantEastAsianWidth::FullWidth;
                break;
            case CSSValueProportionalWidth:
                width = FontVariantEastAsianWidth::ProportionalWidth;
                break;
            case CSSValueRuby:
                ruby = FontVariantEastAsianRuby::Yes;
                break;
            default:
                ASSERT_NOT_REACHED();
                break;
            }
        }
    } else
        ASSERT(downcast<CSSPrimitiveValue>(value).getValueID() == CSSValueNormal);

    receiver.setVariantEastAsianVariant(variant);
    receiver.setVariantEastAsianWidth(width);
    receiver.setVariantEastAsianRuby(ruby);
}

}

#endif
