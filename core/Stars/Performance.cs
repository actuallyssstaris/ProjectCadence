// pp as osu!'s OsuPerformanceCalculator works it out (https://github.com/ppy/osu),
// MIT licence. Only the path for lazer-style scores: slider heads and tails count
// for accuracy here, so the classic / legacy-score branches never apply.

using System;
using Osu5.Play;
using Osu5.Scoring;

namespace Osu5.Stars
{
    public struct PlayStats
    {
        public double Accuracy;
        public int MaxCombo;
        public int Great, Ok, Meh, Miss;
        public int SliderTailHits;
        public int LargeTickMisses;
    }

    public static class Performance
    {
        public static double Calculate(StarResult a, Mods mods, PlayStats s)
        {
            var p = new calc(a, mods, s);
            return p.Total();
        }

        sealed class calc
        {
            readonly StarResult attributes;
            readonly Mods mods;
            readonly double accuracy;
            readonly int scoreMaxCombo, countGreat, countOk, countMeh, countMiss, countSliderTickMiss, countSliderEndsDropped;
            readonly double greatHitWindow, okHitWindow, mehHitWindow, overallDifficulty;
            double effectiveMissCount;
            double? speedDeviation;

            int totalHits => countGreat + countOk + countMeh + countMiss;
            int totalSuccessfulHits => countGreat + countOk + countMeh;
            int totalImperfectHits => countOk + countMeh + countMiss;

            public calc(StarResult a, Mods m, PlayStats s)
            {
                attributes = a;
                mods = m;
                accuracy = Math.Clamp(s.Accuracy, 0, 1);
                scoreMaxCombo = Math.Clamp(s.MaxCombo, 0, a.MaxCombo);
                countGreat = s.Great;
                countOk = s.Ok;
                countMeh = s.Meh;
                countMiss = s.Miss;
                countSliderEndsDropped = a.SliderCount - s.SliderTailHits;
                countSliderTickMiss = s.LargeTickMisses;

                double clockRate = ModRules.Rate(m);
                var w = new HitWindows(a.OverallDifficulty);
                greatHitWindow = w.Great / clockRate;
                okHitWindow = w.Ok / clockRate;
                mehHitWindow = w.Meh / clockRate;
                overallDifficulty = (79.5 - greatHitWindow) / 6;

                effectiveMissCount = comboBasedEstimatedMissCount();
                effectiveMissCount = Math.Max(countMiss, effectiveMissCount);
                effectiveMissCount = Math.Min(totalHits, effectiveMissCount);
                effectiveMissCount = Math.Max(0, effectiveMissCount);
            }

            public double Total()
            {
                double multiplier = StarRating.PerformanceBaseMultiplier;
                if ((mods & Mods.NoFail) != 0)
                    multiplier *= Math.Max(0.90, 1.0 - 0.02 * effectiveMissCount);

                speedDeviation = calculateSpeedDeviation();

                double aimValue = computeAimValue();
                double speedValue = computeSpeedValue();
                double accuracyValue = computeAccuracyValue();
                double readingValue = computeReadingValue();

                return DiffUtils.Norm(StarRating.PerformanceNormExponent, aimValue, speedValue, accuracyValue, readingValue) * multiplier;
            }

            double computeAimValue()
            {
                double aimDifficulty = attributes.Aim;

                if (attributes.SliderCount > 0 && attributes.AimDifficultSliderCount > 0)
                {
                    double estimateImproperlyFollowedDifficultSliders = Math.Clamp(countSliderEndsDropped + countSliderTickMiss, 0, attributes.AimDifficultSliderCount);
                    double sliderNerfFactor = (1 - attributes.SliderFactor) * DiffUtils.Pow(1 - estimateImproperlyFollowedDifficultSliders / attributes.AimDifficultSliderCount, 3) + attributes.SliderFactor;
                    aimDifficulty *= sliderNerfFactor;
                }

                double aimValue = 4.0 * DiffUtils.Pow(aimDifficulty, 3);

                double lengthBonus = 0.95 + 0.35 * Math.Min(1.0, totalHits / 2000.0) +
                                     (totalHits > 2000 ? Math.Log10(totalHits / 2000.0) * 0.5 : 0.0);
                aimValue *= lengthBonus;

                if (effectiveMissCount > 0)
                {
                    double relevantMissCount = Math.Min(effectiveMissCount, totalImperfectHits + countSliderTickMiss);
                    aimValue *= calculateMissPenalty(relevantMissCount, attributes.AimDifficultStrainCount);
                }

                aimValue *= accuracy;
                return aimValue;
            }

            double computeSpeedValue()
            {
                if (speedDeviation == null)
                    return 0.0;

                double speedValue = HarmonicSkill.DifficultyToPerformance(attributes.Speed);

                if (effectiveMissCount > 0)
                {
                    double relevantMissCount = Math.Min(effectiveMissCount, totalImperfectHits + countSliderTickMiss);
                    speedValue *= calculateMissPenalty(relevantMissCount, attributes.SpeedDifficultStrainCount);
                }

                speedValue *= calculateSpeedHighDeviationNerf();

                double effectiveHitWindow = 20 * DiffUtils.Pow(4 / attributes.Speed, 0.35);
                double effectiveAccuracy = DiffUtils.Erf(effectiveHitWindow / (double)speedDeviation);
                speedValue *= DiffUtils.Pow(effectiveAccuracy, 2);
                return speedValue;
            }

            double computeAccuracyValue()
            {
                int amountHitObjectsWithAccuracy = attributes.HitCircleCount + attributes.SliderCount;

                double betterAccuracyPercentage = amountHitObjectsWithAccuracy > 0
                    ? ((countGreat - Math.Max(totalHits - amountHitObjectsWithAccuracy, 0)) * 6 + countOk * 2 + countMeh) / (double)(amountHitObjectsWithAccuracy * 6)
                    : 0;
                if (betterAccuracyPercentage < 0)
                    betterAccuracyPercentage = 0;

                double accuracyValue = DiffUtils.Pow(1.52163, overallDifficulty) * DiffUtils.Pow(betterAccuracyPercentage, 24) * 2.83;
                accuracyValue *= amountHitObjectsWithAccuracy < 1000
                    ? DiffUtils.Pow(amountHitObjectsWithAccuracy / 1000.0, 0.3)
                    : DiffUtils.Pow(amountHitObjectsWithAccuracy / 1000.0, 0.1);
                return accuracyValue;
            }

            double computeReadingValue()
            {
                double readingValue = HarmonicSkill.DifficultyToPerformance(attributes.Reading);
                if (effectiveMissCount > 0)
                    readingValue *= calculateMissPenalty(effectiveMissCount, attributes.ReadingDifficultNoteCount);
                readingValue *= DiffUtils.Pow(accuracy, 3);
                return readingValue;
            }

            double comboBasedEstimatedMissCount()
            {
                if (attributes.SliderCount <= 0)
                    return countMiss;

                double missCount = countMiss;
                double fullComboThreshold = attributes.MaxCombo - countSliderEndsDropped;
                if (scoreMaxCombo < fullComboThreshold)
                    missCount = fullComboThreshold / Math.Max(1.0, scoreMaxCombo);
                return Math.Min(missCount, countSliderTickMiss + countMiss);
            }

            double? calculateSpeedDeviation()
            {
                if (totalSuccessfulHits == 0)
                    return null;

                double speedNoteCount = attributes.SpeedNoteCount;
                speedNoteCount += (totalHits - attributes.SpeedNoteCount) * 0.1;

                double relevantCountMiss = Math.Min(countMiss, speedNoteCount);
                double relevantCountMeh = Math.Min(countMeh, speedNoteCount - relevantCountMiss);
                double relevantCountOk = Math.Min(countOk, speedNoteCount - relevantCountMiss - relevantCountMeh);
                double relevantCountGreat = Math.Max(0, speedNoteCount - relevantCountMiss - relevantCountMeh - relevantCountOk);

                return calculateDeviation(relevantCountGreat, relevantCountOk, relevantCountMeh);
            }

            double? calculateDeviation(double relevantCountGreat, double relevantCountOk, double relevantCountMeh)
            {
                if (relevantCountGreat + relevantCountOk + relevantCountMeh <= 0)
                    return null;

                double n = Math.Max(1, relevantCountGreat + relevantCountOk);
                double p = relevantCountGreat / n;
                const double z = 2.32634787404;

                double pLowerBound = Math.Min(p, (n * p + z * z / 2) / (n + z * z) - z / (n + z * z) * Math.Sqrt(n * p * (1 - p) + z * z / 4));

                double deviation;
                if (pLowerBound > 0.01)
                {
                    deviation = greatHitWindow / (DiffUtils.SQRT2 * DiffUtils.ErfInv(pLowerBound));
                    double okHitWindowTailAmount = Math.Sqrt(2 / Math.PI) * okHitWindow * Math.Exp(-0.5 * DiffUtils.Pow(okHitWindow / deviation, 2))
                                                   / (deviation * DiffUtils.Erf(okHitWindow / (DiffUtils.SQRT2 * deviation)));
                    deviation *= Math.Sqrt(1 - okHitWindowTailAmount);
                }
                else
                {
                    deviation = okHitWindow / Math.Sqrt(3);
                }

                double mehVariance = (mehHitWindow * mehHitWindow + okHitWindow * mehHitWindow + okHitWindow * okHitWindow) / 3;
                return Math.Sqrt(((relevantCountGreat + relevantCountOk) * DiffUtils.Pow(deviation, 2) + relevantCountMeh * mehVariance) / (relevantCountGreat + relevantCountOk + relevantCountMeh));
            }

            double calculateSpeedHighDeviationNerf()
            {
                if (speedDeviation == null)
                    return 0;

                double speedValue = HarmonicSkill.DifficultyToPerformance(attributes.Speed);
                double excessSpeedDifficultyCutoff = 100 + 220 * DiffUtils.Pow(22 / speedDeviation.Value, 6.5);
                if (speedValue <= excessSpeedDifficultyCutoff)
                    return 1.0;

                const double scale = 50;
                double adjustedSpeedValue = scale * (Math.Log((speedValue - excessSpeedDifficultyCutoff) / scale + 1) + excessSpeedDifficultyCutoff / scale);
                double lerp = 1 - DiffUtils.ReverseLerp(speedDeviation.Value, 22.0, 27.0);
                adjustedSpeedValue = double.Lerp(adjustedSpeedValue, speedValue, lerp);
                return adjustedSpeedValue / speedValue;
            }

            static double calculateMissPenalty(double missCount, double difficultStrainCount)
                => 0.93 / (missCount / (4 * Math.Log(Math.Max(1, difficultStrainCount))) + 1);
        }
    }
}
