// osu!'s difficulty evaluators (https://github.com/ppy/osu), MIT licence.
// Kept as close to upstream as the object model allows so the numbers match.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Objects;

namespace Osu5.Stars
{
    static class SnapAimEvaluator
    {
        public static double EvaluateDifficultyOf(DiffObject current, bool withSliderTravelDistance)
        {
            var osuCurrObj = current;
            var osuLastObj = current.Previous();

            if (current.Base is Spinner || current.Index <= 1 || osuLastObj!.Base is Spinner)
                return 0;

            const double wide_angle_multiplier = 9.67;
            const double acute_angle_multiplier = 2.41;
            const double slider_multiplier = 1.5;
            const double velocity_change_multiplier = 0.9;
            const double wiggle_multiplier = 1.02;

            var osuLast2Obj = current.Previous(2);

            const int radius = DiffObject.NormalisedRadius;
            const int diameter = DiffObject.NormalisedDiameter;

            double currDistance = withSliderTravelDistance ? osuCurrObj.LazyJumpDistance : osuCurrObj.JumpDistance;
            double currVelocity = currDistance / osuCurrObj.AdjustedDeltaTime;

            if (osuLastObj.Base is Slider && withSliderTravelDistance)
            {
                double sliderDistance = osuLastObj.LazyTravelDistance + osuCurrObj.LazyJumpDistance;
                currVelocity = Math.Max(currVelocity, sliderDistance / osuCurrObj.AdjustedDeltaTime);
            }

            double prevDistance = withSliderTravelDistance ? osuLastObj.LazyJumpDistance : osuLastObj.JumpDistance;
            double prevVelocity = prevDistance / osuLastObj.AdjustedDeltaTime;

            double snapDifficulty = currVelocity;
            snapDifficulty *= vectorAngleRepetition(osuCurrObj, osuLastObj);

            if (osuCurrObj.Angle != null && osuLastObj.Angle != null)
            {
                double currAngle = osuCurrObj.Angle.Value;
                double lastAngle = osuLastObj.Angle.Value;

                double velocityInfluence = Math.Min(currVelocity, prevVelocity);

                double acuteAngleBonus = 0;

                if (Math.Max(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime) < 1.25 * Math.Min(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime))
                {
                    acuteAngleBonus = CalcAngleAcuteness(currAngle);
                    acuteAngleBonus *= 0.08 + 0.92 * (1 - Math.Min(acuteAngleBonus, DiffUtils.Pow(CalcAngleAcuteness(lastAngle), 3)));
                    acuteAngleBonus *= velocityInfluence * DiffUtils.Smootherstep(DiffUtils.MillisecondsToBPM(osuCurrObj.AdjustedDeltaTime, 2), 300, 400) *
                                       DiffUtils.Smootherstep(currDistance, 0, diameter * 2);
                }

                double wideAngleBonus = calcAngleWideness(currAngle);
                wideAngleBonus *= 0.25 + 0.75 * (1 - Math.Min(wideAngleBonus, DiffUtils.Pow(calcAngleWideness(lastAngle), 3)));

                const double wide_angle_time_scale = 1.45;

                double wideAngleCurrVelocity = currDistance / DiffUtils.Pow(osuCurrObj.AdjustedDeltaTime, wide_angle_time_scale);
                double wideAnglePrevVelocity = prevDistance / DiffUtils.Pow(osuLastObj.AdjustedDeltaTime, wide_angle_time_scale);

                if (osuLastObj.Base is Slider && withSliderTravelDistance)
                {
                    double sliderDistance = osuLastObj.LazyTravelDistance + osuCurrObj.LazyJumpDistance;
                    wideAngleCurrVelocity = Math.Max(wideAngleCurrVelocity, sliderDistance / DiffUtils.Pow(osuCurrObj.AdjustedDeltaTime, wide_angle_time_scale));
                }

                wideAngleBonus *= Math.Min(wideAngleCurrVelocity, wideAnglePrevVelocity);

                if (osuLast2Obj != null)
                {
                    float distance = (osuLast2Obj.Base.StackedPosition - osuLastObj.Base.StackedPosition).Length();

                    if (distance < 1)
                        wideAngleBonus *= 1 - 0.55 * (1 - distance);
                }

                snapDifficulty += Math.Max(acuteAngleBonus * acute_angle_multiplier, wideAngleBonus * wide_angle_multiplier);

                double wiggleBonus = velocityInfluence
                                     * DiffUtils.Smootherstep(currDistance, radius, diameter)
                                     * DiffUtils.Pow(DiffUtils.ReverseLerp(currDistance, diameter * 3, diameter), 1.8)
                                     * DiffUtils.Smootherstep(currAngle, DiffUtils.DegreesToRadians(110), DiffUtils.DegreesToRadians(60))
                                     * DiffUtils.Smootherstep(prevDistance, radius, diameter)
                                     * DiffUtils.Pow(DiffUtils.ReverseLerp(prevDistance, diameter * 3, diameter), 1.8)
                                     * DiffUtils.Smootherstep(lastAngle, DiffUtils.DegreesToRadians(110), DiffUtils.DegreesToRadians(60));

                snapDifficulty += wiggleBonus * wiggle_multiplier;
            }

            if (Math.Max(prevVelocity, currVelocity) != 0)
            {
                if (withSliderTravelDistance)
                    currVelocity = currDistance / osuCurrObj.AdjustedDeltaTime;

                double distRatio = DiffUtils.Smoothstep(Math.Abs(prevVelocity - currVelocity) / Math.Max(prevVelocity, currVelocity), 0, 1);
                double overlapVelocityBuff = Math.Min(diameter * 1.25 / Math.Min(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime), Math.Abs(prevVelocity - currVelocity));
                double velocityChangeBonus = overlapVelocityBuff * distRatio;
                velocityChangeBonus *= DiffUtils.Pow(Math.Min(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime) / Math.Max(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime), 2);

                snapDifficulty += velocityChangeBonus * velocity_change_multiplier;
            }

            if (osuCurrObj.Base is Slider && withSliderTravelDistance)
            {
                double sliderBonus = osuCurrObj.TravelDistance / osuCurrObj.TravelTime;
                snapDifficulty += (sliderBonus < 1 ? sliderBonus : DiffUtils.Pow(sliderBonus, 0.75)) * slider_multiplier;
            }

            snapDifficulty *= osuCurrObj.SmallCircleBonus;
            snapDifficulty *= highBpmBonus(osuCurrObj.AdjustedDeltaTime);

            return snapDifficulty;
        }

        static double highBpmBonus(double ms) => 1 / (1 - DiffUtils.Pow(0.03, DiffUtils.Pow(ms / 1000, 0.65)));

        static double vectorAngleRepetition(DiffObject current, DiffObject previous)
        {
            if (current.Angle == null || previous.Angle == null)
                return 1;

            const double note_limit = 6;
            const double maximum_repetition_nerf = 0.15;
            const double maximum_vector_influence = 0.5;

            double constantAngleCount = 0;

            for (int index = 0; index < note_limit; index++)
            {
                var prevObj = current.Previous(index);
                if (prevObj == null)
                    break;

                if (Math.Max(current.AdjustedDeltaTime, prevObj.AdjustedDeltaTime) > 1.1 * Math.Min(current.AdjustedDeltaTime, prevObj.AdjustedDeltaTime))
                    break;

                if (prevObj.NormalisedVectorAngle != null && current.NormalisedVectorAngle != null)
                {
                    double angleDifference = Math.Abs(current.NormalisedVectorAngle.Value - prevObj.NormalisedVectorAngle.Value);
                    constantAngleCount += Math.Cos(8 * Math.Min(DiffUtils.DegreesToRadians(11.25), angleDifference));
                }
            }

            double vectorRepetition = DiffUtils.Pow(Math.Min(0.5 / constantAngleCount, 1), 2);
            double stackFactor = DiffUtils.Smootherstep(current.LazyJumpDistance, 0, DiffObject.NormalisedDiameter);

            double currAngle = current.Angle.Value;
            double lastAngle = previous.Angle.Value;

            double angleDifferenceAdjusted = Math.Cos(2 * Math.Min(DiffUtils.DegreesToRadians(45), Math.Abs(currAngle - lastAngle) * stackFactor));
            double baseNerf = 1 - maximum_repetition_nerf * CalcAngleAcuteness(lastAngle) * angleDifferenceAdjusted;

            return DiffUtils.Pow(baseNerf + (1 - baseNerf) * vectorRepetition * maximum_vector_influence * stackFactor, 2);
        }

        static double calcAngleWideness(double angle) => DiffUtils.Smoothstep(angle, DiffUtils.DegreesToRadians(40), DiffUtils.DegreesToRadians(140));

        public static double CalcAngleAcuteness(double angle) => DiffUtils.Smoothstep(angle, DiffUtils.DegreesToRadians(140), DiffUtils.DegreesToRadians(40));
    }

    static class AgilityEvaluator
    {
        public static double EvaluateDifficultyOf(DiffObject current)
        {
            if (current.Base is Spinner)
                return 0;

            const double distance_cap = DiffObject.NormalisedDiameter * 1.2;

            var osuPrevObj = current.Index > 0 ? current.Previous() : null;

            double travelDistance = osuPrevObj?.LazyTravelDistance ?? 0;
            double distance = travelDistance + current.LazyJumpDistance;
            double distanceScaled = Math.Min(distance, distance_cap) / distance_cap;

            double agilityDifficulty = distanceScaled * 1000 / current.AdjustedDeltaTime;
            agilityDifficulty *= DiffUtils.Pow(current.SmallCircleBonus, 1.5);
            agilityDifficulty *= highBpmBonus(current.AdjustedDeltaTime);
            return agilityDifficulty;
        }

        static double highBpmBonus(double ms) => 1 / (1 - DiffUtils.Pow(0.2, ms / 1000));
    }

    static class FlowAimEvaluator
    {
        public static double EvaluateDifficultyOf(DiffObject current, bool withSliderTravelDistance)
        {
            var osuCurrObj = current;
            var osuLastObj = current.Previous();

            if (current.Base is Spinner || current.Index <= 1 || osuLastObj!.Base is Spinner)
                return 0;

            const double velocity_change_multiplier = 0.52;

            var osuLastLastObj = current.Previous(1);

            double currDistance = withSliderTravelDistance ? osuCurrObj.LazyJumpDistance : osuCurrObj.JumpDistance;
            double prevDistance = withSliderTravelDistance ? osuLastObj.LazyJumpDistance : osuLastObj.JumpDistance;

            double currVelocity = currDistance / osuCurrObj.AdjustedDeltaTime;

            if (osuLastObj.Base is Slider && withSliderTravelDistance)
            {
                double sliderDistance = osuLastObj.LazyTravelDistance + osuCurrObj.LazyJumpDistance;
                currVelocity = Math.Max(currVelocity, sliderDistance / osuCurrObj.AdjustedDeltaTime);
            }

            double prevVelocity = prevDistance / osuLastObj.AdjustedDeltaTime;

            double flowDifficulty = currVelocity;
            flowDifficulty *= Math.Sqrt(osuCurrObj.SmallCircleBonus);
            flowDifficulty *= 1 + Math.Min(0.25,
                DiffUtils.Pow((Math.Max(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime) - Math.Min(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime)) / 50, 4));

            if (osuCurrObj.Angle != null && osuLastObj.Angle != null)
            {
                double angleDifference = Math.Abs(osuCurrObj.Angle.Value - osuLastObj.Angle.Value);
                double angleDifferenceAdjusted = Math.Sin(angleDifference / 2) * 180.0;
                double angularVelocity = angleDifferenceAdjusted / (osuCurrObj.AdjustedDeltaTime * 0.1);
                flowDifficulty *= 0.8 + Math.Sqrt(angularVelocity / 270.0);
            }

            double overlappedNotesWeight = 1;

            if (current.Index > 2)
            {
                double o1 = calculateOverlapFactor(osuCurrObj, osuLastObj);
                double o2 = calculateOverlapFactor(osuCurrObj, osuLastLastObj!);
                double o3 = calculateOverlapFactor(osuLastObj, osuLastLastObj!);
                overlappedNotesWeight = 1 - o1 * o2 * o3;
            }

            if (osuCurrObj.Angle != null)
                flowDifficulty += currVelocity * SnapAimEvaluator.CalcAngleAcuteness(osuCurrObj.Angle.Value) * overlappedNotesWeight;

            if (Math.Max(prevVelocity, currVelocity) != 0)
            {
                if (withSliderTravelDistance)
                    currVelocity = currDistance / osuCurrObj.AdjustedDeltaTime;

                double distRatio = DiffUtils.Smoothstep(Math.Abs(prevVelocity - currVelocity) / Math.Max(prevVelocity, currVelocity), 0, 1);
                double overlapVelocityBuff = Math.Min(DiffObject.NormalisedDiameter * 1.25 / Math.Min(osuCurrObj.AdjustedDeltaTime, osuLastObj.AdjustedDeltaTime),
                    Math.Abs(prevVelocity - currVelocity));

                flowDifficulty += overlapVelocityBuff * distRatio * overlappedNotesWeight * velocity_change_multiplier;
            }

            if (osuCurrObj.Base is Slider && withSliderTravelDistance)
                flowDifficulty += osuCurrObj.TravelDistance / osuCurrObj.TravelTime;

            flowDifficulty = DiffUtils.Pow(flowDifficulty, 1.45);
            return flowDifficulty * DiffUtils.Smootherstep(currDistance, 0, DiffObject.NormalisedRadius);
        }

        static double calculateOverlapFactor(DiffObject first, DiffObject second)
        {
            double objectRadius = first.Base.Radius;
            double distance = Vector2.Distance(first.Base.StackedPosition, second.Base.StackedPosition);
            return Math.Clamp(1 - DiffUtils.Pow(Math.Max(distance - objectRadius, 0) / objectRadius, 2), 0, 1);
        }
    }

    static class SpeedEvaluator
    {
        public static double EvaluateDifficultyOf(DiffObject current)
        {
            if (current.Base is Spinner)
                return 0;

            const double min_speed_bonus = 200;
            const double speed_balancing_factor = 40;

            double strainTime = current.AdjustedDeltaTime;
            double doubleTapFeasibility = 1.0 - current.CalculateDoubleTapFeasibility(current.Next());

            strainTime /= Math.Clamp((strainTime / current.HitWindowGreat) / 0.93, 0.92, 1);

            double speedBonus = 0.0;
            if (DiffUtils.MillisecondsToBPM(strainTime) > min_speed_bonus)
                speedBonus = 0.75 * DiffUtils.Pow((DiffUtils.BPMToMilliseconds(min_speed_bonus) - strainTime) / speed_balancing_factor, 2);

            double speedDifficulty = (1 + speedBonus) * 1000 / strainTime;
            speedDifficulty *= highBpmBonus(current.AdjustedDeltaTime);
            return speedDifficulty * doubleTapFeasibility;
        }

        static double highBpmBonus(double ms) => 1 / (1 - DiffUtils.Pow(0.3, ms / 1000));
    }

    static class RhythmEvaluator
    {
        public static double EvaluateDifficultyOf(DiffObject current)
        {
            if (current.Base is Spinner)
                return 0;

            const int history_time_max = 5 * 1000;
            const int history_objects_max = 32;
            const double rhythm_overall_multiplier = 0.95;

            double rhythmComplexitySum = 0;
            double deltaDifferenceEpsilon = current.HitWindowGreat * 0.3;

            var island = new Island(int.MaxValue);
            var previousIsland = new Island(int.MaxValue);
            var islands = new List<Island>();

            double startDifficulty = 0;
            bool firstDeltaSwitch = false;

            int historicalNoteCount = Math.Min(current.Index, history_objects_max);

            int rhythmStart = 0;
            while (rhythmStart < historicalNoteCount - 2 && current.StartTime - current.Previous(rhythmStart)!.StartTime < history_time_max)
                rhythmStart++;

            var prevObj = current.Previous(rhythmStart);
            var prevPrevObj = current.Previous(rhythmStart + 1);

            for (int i = rhythmStart; i > 0; i--)
            {
                var currObj = current.Previous(i - 1)!;

                if (currObj.Base is Spinner)
                    continue;

                double timeDecay = (history_time_max - (current.StartTime - currObj.StartTime)) / history_time_max;
                double noteDecay = (double)(historicalNoteCount - i) / historicalNoteCount;
                double currHistoricalDecay = Math.Min(noteDecay, timeDecay);

                const double delta_min_value = 1e-7;

                double currDelta = Math.Max(currObj.DeltaTime, delta_min_value);
                double prevDelta = Math.Max(prevObj!.DeltaTime, delta_min_value);
                double deltaDifference = Math.Abs(prevDelta - currDelta);

                if (island.Delta == int.MaxValue)
                    island = new Island((int)currDelta);

                double deltaDifferenceRatio = Math.Max(prevDelta, currDelta) / Math.Min(prevDelta, currDelta);
                double differenceMultiplier = Math.Clamp(2.0 - deltaDifferenceRatio / 8.0, 0.0, 1.0);
                double windowPenalty = Math.Clamp((deltaDifference - deltaDifferenceEpsilon) / deltaDifferenceEpsilon, 0, 1);

                double effectiveDifficulty = getEffectiveDifficulty(deltaDifferenceRatio) * windowPenalty * differenceMultiplier;

                if (prevObj.Base is Slider)
                {
                    double sliderLazyEndDelta = currObj.MinimumJumpTime;
                    double sliderLazyDeltaDifferenceRatio = Math.Max(sliderLazyEndDelta, currDelta) / Math.Min(sliderLazyEndDelta, currDelta);

                    double sliderRealEndDelta = currObj.LastObjectEndDeltaTime;
                    double sliderRealDeltaDifferenceRatio = Math.Max(sliderRealEndDelta, currDelta) / Math.Min(sliderRealEndDelta, currDelta);

                    double sliderEffectiveDifficulty = Math.Min(getEffectiveDifficulty(sliderLazyDeltaDifferenceRatio), getEffectiveDifficulty(sliderRealDeltaDifferenceRatio));
                    effectiveDifficulty = Math.Min(sliderEffectiveDifficulty, effectiveDifficulty);
                }

                if (deltaDifference < deltaDifferenceEpsilon)
                    island.AddDelta((int)currDelta);

                if (firstDeltaSwitch)
                {
                    if (deltaDifference > deltaDifferenceEpsilon)
                    {
                        if (currObj.Base is Slider)
                            effectiveDifficulty *= 0.5;

                        if (island.IsSimilarPolarity(previousIsland, deltaDifferenceEpsilon))
                            effectiveDifficulty *= 0.5;

                        if (Math.Max(prevPrevObj!.DeltaTime, delta_min_value) > prevDelta + deltaDifferenceEpsilon && prevDelta > currDelta + deltaDifferenceEpsilon)
                            effectiveDifficulty *= 0.125;

                        if (previousIsland.DeltaCount == island.DeltaCount)
                            effectiveDifficulty *= 0.5;

                        bool isSpeedingUp = prevDelta > currDelta + deltaDifferenceEpsilon;
                        if (isSpeedingUp)
                            effectiveDifficulty *= 0.65;

                        bool found = false;
                        foreach (var existingIsland in islands)
                        {
                            if (existingIsland.AlmostEquals(island, deltaDifferenceEpsilon))
                            {
                                if (previousIsland.AlmostEquals(island, deltaDifferenceEpsilon))
                                    existingIsland.Occurrences++;

                                double power = DiffUtils.Logistic(island.Delta, maxValue: 2.75, multiplier: 0.24, midpointOffset: 58.33);
                                effectiveDifficulty *= Math.Min(3.0 / existingIsland.Occurrences, DiffUtils.Pow(1.0 / existingIsland.Occurrences, power));
                                found = true;
                                break;
                            }
                        }

                        if (!found && island.DeltaCount > 0)
                            islands.Add(island);

                        effectiveDifficulty *= 1 - prevObj.CalculateDoubleTapFeasibility(currObj) * 0.75;

                        if (island.DeltaCount > 1)
                            rhythmComplexitySum += Math.Sqrt(effectiveDifficulty * startDifficulty) * currHistoricalDecay;
                        else
                            rhythmComplexitySum += 0.7 * currHistoricalDecay;

                        startDifficulty = effectiveDifficulty;

                        if (prevDelta + deltaDifferenceEpsilon < currDelta)
                            firstDeltaSwitch = false;

                        previousIsland = island;
                        island = new Island((int)currDelta);
                    }
                }
                else if (prevDelta > currDelta + deltaDifferenceEpsilon)
                {
                    firstDeltaSwitch = true;

                    if (currObj.Base is Slider)
                        effectiveDifficulty *= 0.6;
                    if (prevObj.Base is Slider)
                        effectiveDifficulty *= 0.6;

                    startDifficulty = effectiveDifficulty;
                    island = new Island((int)currDelta);
                }

                prevPrevObj = prevObj;
                prevObj = currObj;
            }

            rhythmComplexitySum *= DiffUtils.ReverseLerp(island.DeltaCount, 22, 3);

            return Math.Sqrt(4 + rhythmComplexitySum * rhythm_overall_multiplier) / 2.0;
        }

        static double getEffectiveDifficulty(double deltaDifferenceRatio)
        {
            const double rhythm_ratio_difficulty_multiplier = 26.0;
            double deltaDifferenceFraction = deltaDifferenceRatio - Math.Truncate(deltaDifferenceRatio);
            return 1.0 + rhythm_ratio_difficulty_multiplier * Math.Min(0.5, DiffUtils.SmoothstepBellCurve(deltaDifferenceFraction));
        }

        sealed class Island
        {
            public int Delta { get; private set; }
            public int DeltaCount { get; private set; } = 1;
            public int Occurrences { get; set; } = 1;

            public Island(int delta)
            {
                Delta = Math.Max(delta, DiffObject.MinDeltaTime);
            }

            public void AddDelta(int delta)
            {
                if (Delta == int.MaxValue)
                    Delta = Math.Max(delta, DiffObject.MinDeltaTime);
                DeltaCount++;
            }

            public bool IsSimilarPolarity(Island other, double epsilon)
            {
                if (DeltaCount <= 1 || other.DeltaCount <= 1)
                    return false;
                return Math.Abs(Delta - other.Delta) < epsilon && DeltaCount % 2 == other.DeltaCount % 2;
            }

            public bool AlmostEquals(Island other, double epsilon)
                => Math.Abs(Delta - other.Delta) < epsilon && DeltaCount == other.DeltaCount;
        }
    }

    static class ReadingEvaluator
    {
        const double reading_window_size = 3000;
        const double distance_influence_threshold = DiffObject.NormalisedDiameter * 1.5;

        public static double EvaluateDifficultyOf(DiffObject current, bool hidden)
        {
            if (current.Base is Spinner || current.Index == 0)
                return 0;

            var currObj = current;
            var nextObj = current.Next();

            double velocity = Math.Max(1, currObj.LazyJumpDistance / currObj.AdjustedDeltaTime);

            double currentVisibleObjectDensity = retrieveCurrentVisibleObjectDensity(currObj);
            double pastObjectDifficultyInfluence = getPastObjectDifficultyInfluence(currObj);
            double constantAngleNerfFactor = getConstantAngleNerfFactor(currObj);

            double noteDensityDifficulty = calculateDensityDifficulty(nextObj, velocity, constantAngleNerfFactor, pastObjectDifficultyInfluence, currentVisibleObjectDensity);
            double hiddenDifficulty = hidden
                ? calculateHiddenDifficulty(currObj, pastObjectDifficultyInfluence, currentVisibleObjectDensity, velocity, constantAngleNerfFactor)
                : 0;
            double preemptDifficulty = calculatePreemptDifficulty(velocity, constantAngleNerfFactor, currObj.Preempt);

            double readingDifficulty = DiffUtils.Norm(1.5, preemptDifficulty, hiddenDifficulty, noteDensityDifficulty);
            readingDifficulty *= highBpmBonus(currObj.AdjustedDeltaTime);
            return readingDifficulty;
        }

        static double calculateDensityDifficulty(DiffObject? nextObj, double velocity, double constantAngleNerfFactor,
                                                 double pastObjectDifficultyInfluence, double currentVisibleObjectDensity)
        {
            const double density_multiplier = 2.4;
            const double density_difficulty_base = 2.5;

            double futureObjectDifficultyInfluence = Math.Sqrt(currentVisibleObjectDensity);

            if (nextObj != null)
                futureObjectDifficultyInfluence *= DiffUtils.Smootherstep(nextObj.LazyJumpDistance, 15, distance_influence_threshold);

            double noteDensityDifficulty = DiffUtils.Pow(pastObjectDifficultyInfluence + futureObjectDifficultyInfluence, 1.7) * 0.4 * constantAngleNerfFactor * velocity;
            noteDensityDifficulty = Math.Max(0, noteDensityDifficulty - density_difficulty_base);
            return DiffUtils.Pow(noteDensityDifficulty, 0.45) * density_multiplier;
        }

        static double calculatePreemptDifficulty(double velocity, double constantAngleNerfFactor, double preempt)
        {
            const double preempt_balancing_factor = 140000;
            const double preempt_starting_point = 500;

            double preemptDifficulty = DiffUtils.Pow((preempt_starting_point - preempt + Math.Abs(preempt - preempt_starting_point)) / 2, 2.5) / preempt_balancing_factor;
            return preemptDifficulty * constantAngleNerfFactor * velocity;
        }

        static double calculateHiddenDifficulty(DiffObject currObj, double pastObjectDifficultyInfluence, double currentVisibleObjectDensity, double velocity,
                                                double constantAngleNerfFactor)
        {
            const double hidden_multiplier = 0.28;

            double preemptFactor = DiffUtils.Pow(currObj.Preempt, 2.2) * 0.01;
            double densityFactor = DiffUtils.Pow(currentVisibleObjectDensity + pastObjectDifficultyInfluence, 3.3) * 3;

            double hiddenDifficulty = (preemptFactor + densityFactor) * constantAngleNerfFactor * velocity * 0.01;
            hiddenDifficulty = DiffUtils.Pow(hiddenDifficulty, 0.4) * hidden_multiplier;

            var previousObj = currObj.Previous()!;

            // perfect stacks are harder the less time between notes
            if (currObj.LazyJumpDistance == 0 && currObj.OpacityAt(previousObj.Base.StartTime, true) == 0 && previousObj.StartTime > currObj.StartTime - currObj.Preempt)
                hiddenDifficulty += hidden_multiplier * 2500 / DiffUtils.Pow(currObj.AdjustedDeltaTime, 1.5);

            return hiddenDifficulty;
        }

        static double getPastObjectDifficultyInfluence(DiffObject currObj)
        {
            double influence = 0;

            for (int i = 0; i < currObj.Index; i++)
            {
                var loopObj = currObj.Previous(i);
                if (loopObj == null
                    || currObj.StartTime - loopObj.StartTime > reading_window_size
                    || loopObj.StartTime < currObj.StartTime - currObj.Preempt)
                    break;

                double loopDifficulty = currObj.OpacityAt(loopObj.Base.StartTime, false);
                loopDifficulty *= DiffUtils.Smootherstep(loopObj.LazyJumpDistance, 15, distance_influence_threshold);
                loopDifficulty *= getTimeNerfFactor(currObj.StartTime - loopObj.StartTime);
                influence += loopDifficulty;
            }

            return influence;
        }

        static double retrieveCurrentVisibleObjectDensity(DiffObject current)
        {
            double visibleObjectCount = 0;
            var hitObject = current.Next();

            while (hitObject != null)
            {
                if (hitObject.StartTime - current.StartTime > reading_window_size ||
                    current.StartTime < hitObject.StartTime - hitObject.Preempt)
                    break;

                double timeNerfFactor = getTimeNerfFactor(hitObject.StartTime - current.StartTime);
                visibleObjectCount += hitObject.OpacityAt(current.Base.StartTime, false) * timeNerfFactor;
                hitObject = hitObject.Next();
            }

            return visibleObjectCount;
        }

        static double getConstantAngleNerfFactor(DiffObject current)
        {
            const double minimum_angle_relevancy_time = 2000;
            const double maximum_angle_relevancy_time = 200;

            double constantAngleCount = 0;
            int index = 0;
            double currentTimeGap = 0;

            DiffObject loopObjPrev0 = current;
            DiffObject? loopObjPrev1 = null;
            DiffObject? loopObjPrev2 = null;

            while (currentTimeGap < minimum_angle_relevancy_time)
            {
                var loopObj = current.Previous(index);
                if (loopObj == null)
                    break;

                double longIntervalFactor = 1 - DiffUtils.ReverseLerp(loopObj.AdjustedDeltaTime, maximum_angle_relevancy_time, minimum_angle_relevancy_time);

                if (loopObj.Angle != null && current.Angle != null)
                {
                    double angleDifference = Math.Abs(current.Angle.Value - loopObj.Angle.Value);
                    double angleDifferenceAlternating = Math.PI;

                    if (loopObjPrev0.Angle != null && loopObjPrev1?.Angle != null && loopObjPrev2?.Angle != null)
                    {
                        angleDifferenceAlternating = Math.Abs(loopObjPrev1.Angle.Value - loopObj.Angle.Value);
                        angleDifferenceAlternating += Math.Abs(loopObjPrev2.Angle.Value - loopObjPrev0.Angle.Value);

                        double weight = 1.0;
                        weight *= DiffUtils.ReverseLerp(Math.Min(loopObj.Angle.Value, loopObjPrev0.Angle.Value) * 180 / Math.PI, 20, 5);
                        weight *= DiffUtils.ReverseLerp(Math.Max(loopObj.Angle.Value, loopObjPrev0.Angle.Value) * 180 / Math.PI, 60, 120);

                        angleDifferenceAlternating = double.Lerp(Math.PI, 0.1 * angleDifferenceAlternating, weight);
                    }

                    double stackFactor = DiffUtils.Smootherstep(loopObj.LazyJumpDistance, 0, DiffObject.NormalisedRadius);
                    constantAngleCount += Math.Cos(3 * Math.Min(DiffUtils.DegreesToRadians(30), Math.Min(angleDifference, angleDifferenceAlternating) * stackFactor)) * longIntervalFactor;
                }

                currentTimeGap = current.StartTime - loopObj.StartTime;
                index++;

                loopObjPrev2 = loopObjPrev1;
                loopObjPrev1 = loopObjPrev0;
                loopObjPrev0 = loopObj;
            }

            return Math.Clamp(2 / constantAngleCount, 0.2, 1);
        }

        static double getTimeNerfFactor(double deltaTime) => Math.Clamp(2 - deltaTime / (reading_window_size / 2), 0, 1);

        static double highBpmBonus(double ms) => 1 / (1 - DiffUtils.Pow(0.8, ms / 1000));
    }
}
