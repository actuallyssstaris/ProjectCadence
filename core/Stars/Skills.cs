// Skills from osu!'s difficulty calculation (https://github.com/ppy/osu), MIT
// licence: the generic strain/harmonic bases and the osu! Aim, Speed and Reading.

using System;
using System.Collections.Generic;
using System.Linq;

namespace Osu5.Stars
{
    abstract class Skill
    {
        protected readonly List<double> ObjectDifficulties = new List<double>();

        public void Process(DiffObject current) => ObjectDifficulties.Add(ProcessInternal(current));

        protected abstract double ProcessInternal(DiffObject current);

        public abstract double DifficultyValue();
    }

    abstract class HarmonicSkill : Skill
    {
        protected double ObjectWeightSum;

        protected virtual double HarmonicScale => 1.0;
        protected virtual double DecayExponent => 0.9;

        protected abstract double ObjectDifficultyOf(DiffObject current);

        protected sealed override double ProcessInternal(DiffObject current) => ObjectDifficultyOf(current);

        protected virtual List<double> GetTransformedDifficulties(List<double> difficulties) => difficulties;

        public override double DifficultyValue()
        {
            ObjectWeightSum = 0;
            if (ObjectDifficulties.Count == 0)
                return 0;

            List<double> difficulties = GetTransformedDifficulties(ObjectDifficulties);

            double difficulty = 0;
            int index = 0;
            foreach (double obj in difficulties.OrderDescending().Where(v => v > 0))
            {
                double weight = (1 + (HarmonicScale / (1 + index))) / (DiffUtils.Pow(index, DecayExponent) + 1 + (HarmonicScale / (1 + index)));
                ObjectWeightSum += weight;
                difficulty += obj * weight;
                index += 1;
            }

            return difficulty;
        }

        public virtual double CountTopWeightedObjectDifficulties(double difficultyValue)
        {
            if (ObjectDifficulties.Count == 0 || ObjectWeightSum == 0)
                return 0.0;

            double consistentTopObject = difficultyValue / ObjectWeightSum;
            if (consistentTopObject == 0)
                return 0;

            return ObjectDifficulties.Sum(d => DiffUtils.Logistic(d / consistentTopObject, 0.88, 10, 1.1));
        }

        public static double DifficultyToPerformance(double difficulty) => 4.0 * DiffUtils.Pow(difficulty, 3);
    }

    abstract class VariableLengthStrainSkill : Skill
    {
        protected readonly double DecayWeight;
        protected readonly int MaxSectionLength;

        double currentSectionPeak;
        double currentSectionBegin;
        double currentSectionEnd;
        readonly double maxStoredLength;
        readonly List<StrainPeak> strainPeaks = new List<StrainPeak>();
        double totalLength;
        readonly List<(double StrainValue, double StartTime)> queuedStrains = new List<(double, double)>();
        StrainPeak? finalPeak;

        protected VariableLengthStrainSkill(double decayWeight = 0.9, int maxSectionLength = 400)
        {
            DecayWeight = decayWeight;
            MaxSectionLength = maxSectionLength;
            maxStoredLength = 11 / (1 - DecayWeight);
        }

        protected abstract double StrainValueAt(DiffObject current);

        protected abstract double CalculateInitialStrain(double time, DiffObject current);

        protected sealed override double ProcessInternal(DiffObject current)
        {
            if (current.Index == 0)
            {
                currentSectionBegin = current.StartTime;
                currentSectionEnd = currentSectionBegin + MaxSectionLength;
                currentSectionPeak = StrainValueAt(current);
                return currentSectionPeak;
            }

            backfillPeaks(current);

            double currentStrain = StrainValueAt(current);

            if (currentStrain > currentSectionPeak)
            {
                queuedStrains.Clear();
                saveCurrentPeak(current.StartTime - currentSectionBegin);
                currentSectionBegin = current.StartTime;
                currentSectionEnd = currentSectionBegin + MaxSectionLength;
                currentSectionPeak = currentStrain;
            }
            else
            {
                while (queuedStrains.Count > 0 && queuedStrains[^1].StrainValue < currentStrain)
                    queuedStrains.RemoveAt(queuedStrains.Count - 1);
                queuedStrains.Add((currentStrain, current.StartTime));
            }

            return currentStrain;
        }

        void backfillPeaks(DiffObject current)
        {
            while (current.StartTime > currentSectionEnd)
            {
                saveCurrentPeak(currentSectionEnd - currentSectionBegin);
                currentSectionBegin = currentSectionEnd;

                if (queuedStrains.Count > 0)
                {
                    (double strain, double startTime) = queuedStrains[0];
                    queuedStrains.RemoveAt(0);
                    currentSectionEnd = startTime + MaxSectionLength;
                    currentSectionPeak = CalculateInitialStrain(currentSectionBegin, current);
                    currentSectionPeak = Math.Max(currentSectionPeak, strain);
                }
                else
                {
                    currentSectionEnd = currentSectionBegin + MaxSectionLength;
                    currentSectionPeak = CalculateInitialStrain(currentSectionBegin, current);
                }
            }
        }

        void saveCurrentPeak(double sectionLength)
        {
            if (finalPeak != null)
            {
                strainPeaks.Remove(finalPeak.Value);
                finalPeak = null;
            }

            strainPeaks.AddInPlace(new StrainPeak(currentSectionPeak, sectionLength));
            totalLength += sectionLength;

            while (totalLength > maxStoredLength * MaxSectionLength)
            {
                totalLength -= strainPeaks[^1].SectionLength;
                strainPeaks.RemoveAt(strainPeaks.Count - 1);
            }
        }

        public double CountTopWeightedStrains(double difficultyValue)
        {
            if (ObjectDifficulties.Count == 0)
                return 0.0;

            double consistentTopStrain = difficultyValue * (1 - DecayWeight);
            if (consistentTopStrain == 0)
                return ObjectDifficulties.Count;

            return ObjectDifficulties.Sum(s => DiffUtils.Logistic(s / consistentTopStrain, 0.88, 10, 1.1));
        }

        protected IEnumerable<StrainPeak> GetCurrentStrainPeaks()
        {
            if (finalPeak == null)
            {
                finalPeak = new StrainPeak(currentSectionPeak, currentSectionEnd - currentSectionBegin);
                strainPeaks.AddInPlace(finalPeak.Value);
            }
            return strainPeaks;
        }

        public readonly record struct StrainPeak : IComparable<StrainPeak>
        {
            public StrainPeak(double value, double sectionLength)
            {
                Value = value;
                SectionLength = Math.Round(sectionLength);
            }

            public double Value { get; }
            public double SectionLength { get; }

            public int CompareTo(StrainPeak other) => other.Value.CompareTo(Value);
        }
    }

    sealed class Aim : VariableLengthStrainSkill
    {
        readonly bool includeSliders;
        double currentStrain;
        readonly List<double> sliderStrains = new List<double>();

        public Aim(bool includeSliders)
        {
            this.includeSliders = includeSliders;
        }

        static double strainDecay(double ms) => DiffUtils.Pow(0.2, ms / 1000);

        protected override double CalculateInitialStrain(double time, DiffObject current) =>
            currentStrain * strainDecay(time - current.Previous()!.StartTime);

        protected override double StrainValueAt(DiffObject current)
        {
            double decay = strainDecay(current.AdjustedDeltaTime);
            currentStrain *= decay;
            currentStrain += calculateAdjustedDifficulty(current) * (1 - decay);

            if (current.Base is Osu5.Objects.Slider)
                sliderStrains.Add(currentStrain);

            return currentStrain;
        }

        public double GetDifficultSliders()
        {
            if (sliderStrains.Count == 0)
                return 0;
            double maxSliderStrain = sliderStrains.Max();
            if (maxSliderStrain == 0)
                return 0;
            return sliderStrains.Sum(strain => DiffUtils.Logistic(strain / maxSliderStrain, 0.5, 12.0));
        }

        double calculateAdjustedDifficulty(DiffObject current)
        {
            const double skill_multiplier_snap = 70.9;
            const double skill_multiplier_agility = 2.35;
            const double skill_multiplier_flow = 242.0;

            double snapDifficulty = SnapAimEvaluator.EvaluateDifficultyOf(current, includeSliders) * skill_multiplier_snap;
            double agilityDifficulty = AgilityEvaluator.EvaluateDifficultyOf(current) * skill_multiplier_agility;
            double flowDifficulty = FlowAimEvaluator.EvaluateDifficultyOf(current, includeSliders) * skill_multiplier_flow;

            double totalDifficulty = calculateTotalValue(snapDifficulty, agilityDifficulty, flowDifficulty);
            totalDifficulty *= 0.985 + DiffUtils.Pow(Math.Max(0, current.OverallDifficulty), 2) / 4000;
            return totalDifficulty;
        }

        static double calculateTotalValue(double snapDifficulty, double agilityDifficulty, double flowDifficulty)
        {
            const double skill_multiplier_total = 1.12;
            const double combined_snap_norm_exponent = 1.2;

            double combinedSnapDifficulty = DiffUtils.Norm(combined_snap_norm_exponent, snapDifficulty, agilityDifficulty);
            double pSnap = calculateSnapFlowProbability(flowDifficulty / combinedSnapDifficulty);
            double pFlow = 1 - pSnap;

            double totalDifficulty = combinedSnapDifficulty * pSnap + flowDifficulty * pFlow;
            return totalDifficulty * skill_multiplier_total;
        }

        static double calculateSnapFlowProbability(double ratio)
        {
            const double k = 7.27;
            if (ratio == 0)
                return 0;
            if (double.IsNaN(ratio))
                return 1;
            return DiffUtils.Logistic(-k * Math.Log(ratio));
        }

        public override double DifficultyValue()
        {
            double difficulty = 0;
            double time = 0;

            foreach (StrainPeak strain in getReducedStrainPeaks())
            {
                double startTime = time;
                double endTime = time + strain.SectionLength / MaxSectionLength;
                double weight = DiffUtils.Pow(DecayWeight, startTime) - DiffUtils.Pow(DecayWeight, endTime);
                difficulty += strain.Value * weight;
                time = endTime;
            }

            return difficulty / (1 - DecayWeight);
        }

        // the first seconds count for less, people tend to warm up
        IEnumerable<StrainPeak> getReducedStrainPeaks()
        {
            const int reduced_section_time = 4000;
            const double reduced_strain_baseline = 0.727;
            const int chunk_size = 20;

            List<StrainPeak> strains = GetCurrentStrainPeaks().Where(p => p.Value > 0).ToList();

            double time = 0;
            int skipCount = 0;

            while (strains.Count > skipCount && time < reduced_section_time)
            {
                StrainPeak strain = strains[skipCount];

                for (double addedTime = 0; addedTime < strain.SectionLength; addedTime += chunk_size)
                {
                    double scale = Math.Log10(DiffUtils.Lerp(1, 10, Math.Clamp((time + addedTime) / reduced_section_time, 0, 1)));
                    strains.Add(new StrainPeak(
                        strain.Value * DiffUtils.Lerp(reduced_strain_baseline, 1.0, scale),
                        Math.Min(chunk_size, strain.SectionLength - addedTime)));
                }

                time += strain.SectionLength;
                skipCount++;
            }

            return strains.Skip(skipCount).OrderByDescending(p => p.Value);
        }
    }

    sealed class Speed : HarmonicSkill
    {
        double currentStrain;

        protected override double HarmonicScale => 20;
        protected override double DecayExponent => 0.9;

        static double strainDecay(double ms) => DiffUtils.Pow(0.3, ms / 1000);

        protected override double ObjectDifficultyOf(DiffObject current)
        {
            const double skill_multiplier = 1.16;

            double decay = strainDecay(current.AdjustedDeltaTime);
            currentStrain *= decay;
            currentStrain += SpeedEvaluator.EvaluateDifficultyOf(current) * (1 - decay) * skill_multiplier;

            return currentStrain * RhythmEvaluator.EvaluateDifficultyOf(current);
        }

        public double RelevantObjectCount()
        {
            if (ObjectDifficulties.Count == 0)
                return 0;
            double maxStrain = ObjectDifficulties.Max();
            if (maxStrain == 0)
                return 0;
            return ObjectDifficulties.Sum(strain => DiffUtils.Logistic(strain / maxStrain, 0.5, 12.0));
        }
    }

    sealed class Reading : HarmonicSkill
    {
        readonly bool hidden;
        double currentStrain;
        double reducedNoteCount;
        double? reducedDuration;

        public Reading(bool hidden)
        {
            this.hidden = hidden;
        }

        static double strainDecay(double ms) => DiffUtils.Pow(0.8, ms / 1000);

        protected override double ObjectDifficultyOf(DiffObject current)
        {
            const double skill_multiplier = 2.5;
            const double reduced_difficulty_duration = 60 * 1000;

            double decay = strainDecay(current.DeltaTime);
            currentStrain *= decay;
            currentStrain += calculateAdjustedDifficulty(current) * (1 - decay) * skill_multiplier;

            reducedDuration ??= current.StartTime + reduced_difficulty_duration;
            if (current.StartTime <= reducedDuration)
                reducedNoteCount++;

            return currentStrain;
        }

        double calculateAdjustedDifficulty(DiffObject current)
        {
            double difficulty = ReadingEvaluator.EvaluateDifficultyOf(current, hidden);
            difficulty *= 0.825 + DiffUtils.Pow(Math.Max(0, current.OverallDifficulty), 2.2) / 1125.0;
            return difficulty;
        }

        // the first minute is assumed memorised
        protected override List<double> GetTransformedDifficulties(List<double> difficulties)
        {
            difficulties = difficulties.Where(v => v > 0).ToList();

            const double reduced_difficulty_base_line = 0.0;

            for (int i = 0; i < Math.Min(difficulties.Count, reducedNoteCount); i++)
            {
                double scale = Math.Log10(DiffUtils.Lerp(1, 10, Math.Clamp(i / reducedNoteCount, 0, 1)));
                difficulties[i] *= DiffUtils.Lerp(reduced_difficulty_base_line, 1.0, scale);
            }

            return difficulties;
        }

        public override double CountTopWeightedObjectDifficulties(double difficultyValue)
        {
            if (ObjectDifficulties.Count == 0 || ObjectWeightSum == 0)
                return 0.0;

            double consistentTopNote = difficultyValue / ObjectWeightSum;
            if (consistentTopNote == 0)
                return 0;

            return ObjectDifficulties.Sum(d => DiffUtils.Logistic(d / consistentTopNote, 1.15, 5, 1.1));
        }
    }
}
