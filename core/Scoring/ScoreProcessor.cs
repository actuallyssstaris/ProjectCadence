// Standardised scoring as in osu!'s ScoreProcessor (https://github.com/ppy/osu), MIT licence.

using System;
using System.Collections.Generic;

namespace Osu5.Scoring
{
    public enum Rank : byte { D, C, B, A, S, SH, X, XH, F }

    public sealed class ScoreProcessor
    {
        const double combo_exponent = 0.5;

        double baseScore, maxBaseScoreSoFar;
        double comboPortion;
        double bonusPortion;
        int accuracyJudged;

        double maxBaseScore = 1, maxComboPortion = 1;
        int maxAccuracyJudgements = 1;

        public int Combo { get; private set; }
        public int MaxCombo { get; private set; }
        public double Accuracy { get; private set; } = 1;
        public long TotalScore { get; private set; }
        public bool Failed;
        public double Multiplier = 1;
        public int Ruleset;     // 0 osu, 1 taiko, 3 mania: the split between combo and accuracy differs
        public int Perfect, Good; // mania's extra judgements
        public int LargeTickHits, SmallTickHits, SmallTickMisses; // catch's droplets
        public bool Hidden;     // turns S / SS into the silver ranks

        public int Great, Ok, Meh, Miss, SliderBreaks;
        public int SliderTailHits, LargeTickMisses; // pp wants these

        readonly List<double> hitErrors = new List<double>();
        public IReadOnlyList<double> HitErrors => hitErrors;

        // feed every judgement's max result once, in time order, before play
        public void SetMaximums(IEnumerable<HitResult> maxResults)
        {
            int combo = 0;
            maxBaseScore = 0;
            maxComboPortion = 0;
            maxAccuracyJudgements = 0;

            foreach (var r in maxResults)
            {
                if (HitResults.AffectsAccuracy(r))
                    maxBaseScore += HitResults.BaseScore(r);
                if (HitResults.IsBasic(r))
                    maxAccuracyJudgements++;
                if (HitResults.AffectsCombo(r))
                {
                    combo++;
                    maxComboPortion += HitResults.BaseScore(r) * comboWeight(combo);
                }
            }
        }

        // osu! grows with the square root of the combo; taiko and mania level off at 400
        double comboWeight(int combo)
        {
            if (Ruleset == 0)
                return Math.Pow(combo, combo_exponent);
            return Math.Min(Math.Max(0.5, Math.Log(combo, 4)), Math.Log(400, 4));
        }

        public void Apply(HitResult result, HitResult max, double hitError = double.NaN)
        {
            if (HitResults.IsBonus(result))
            {
                bonusPortion += HitResults.BaseScore(result);
                update();
                return;
            }

            if (HitResults.AffectsCombo(max))
            {
                if (HitResults.BreaksCombo(result))
                {
                    if (Combo > 0 && result == HitResult.LargeTickMiss) SliderBreaks++;
                    Combo = 0;
                }
                else if (HitResults.AffectsCombo(result))
                {
                    Combo++;
                    comboPortion += HitResults.BaseScore(max) * comboWeight(Combo);
                }

                MaxCombo = Math.Max(MaxCombo, Combo);
            }

            if (HitResults.AffectsAccuracy(max))
            {
                maxBaseScoreSoFar += HitResults.BaseScore(max);
                baseScore += HitResults.BaseScore(result);
            }

            if (HitResults.IsBasic(max))
                accuracyJudged++;

            switch (result)
            {
                case HitResult.Great: Great++; break;
                case HitResult.Perfect: Perfect++; break;
                case HitResult.Good: Good++; break;
                case HitResult.LargeTickHit: LargeTickHits++; break;
                case HitResult.SmallTickHit: SmallTickHits++; break;
                case HitResult.SmallTickMiss: SmallTickMisses++; break;
                case HitResult.Ok: Ok++; break;
                case HitResult.Meh: Meh++; break;
                case HitResult.Miss: Miss++; break;
                case HitResult.SliderTailHit: SliderTailHits++; break;
                case HitResult.LargeTickMiss: LargeTickMisses++; break;
            }

            if (!double.IsNaN(hitError))
                hitErrors.Add(hitError);

            update();
        }

        void update()
        {
            Accuracy = maxBaseScoreSoFar > 0 ? baseScore / maxBaseScoreSoFar : 1;
            double comboProgress = maxComboPortion > 0 ? comboPortion / maxComboPortion : 1;
            double accProgress = maxAccuracyJudgements > 0 ? (double)accuracyJudged / maxAccuracyJudgements : 1;

            double total = Ruleset switch
            {
                1 => 250000 * comboProgress + 750000 * Math.Pow(Accuracy, 3.6) * accProgress,
                3 => 150000 * comboProgress + 850000 * Math.Pow(Accuracy, 2 + 2 * Accuracy) * accProgress,
                _ => 500000 * Accuracy * comboProgress + 500000 * Math.Pow(Accuracy, 5) * accProgress,
            };
            TotalScore = (long)Math.Round((total + bonusPortion) * Multiplier);
        }

        public Rank Rank
        {
            get
            {
                if (Failed) return Rank.F;

                Rank r;
                if (Accuracy >= 1) r = Rank.X;
                else if (Accuracy >= 0.95) r = Rank.S;
                else if (Accuracy >= 0.9) r = Rank.A;
                else if (Accuracy >= 0.8) r = Rank.B;
                else if (Accuracy >= 0.7) r = Rank.C;
                else r = Rank.D;

                // osu! ruleset: any miss caps you at A
                if (Ruleset == 0 && r >= Rank.S && Miss > 0) r = Rank.A;
                if (Hidden && r == Rank.S) r = Rank.SH;
                if (Hidden && r == Rank.X) r = Rank.XH;
                return r;
            }
        }

        public double UnstableRate
        {
            get
            {
                if (hitErrors.Count < 2) return 0;
                double mean = 0;
                foreach (double e in hitErrors) mean += e;
                mean /= hitErrors.Count;
                double v = 0;
                foreach (double e in hitErrors) v += (e - mean) * (e - mean);
                return Math.Sqrt(v / hitErrors.Count) * 10;
            }
        }
    }
}
