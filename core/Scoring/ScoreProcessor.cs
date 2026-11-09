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
        public bool Hidden;     // turns S / SS into the silver ranks

        public int Great, Ok, Meh, Miss, SliderBreaks;
        public int SliderTailHits, LargeTickMisses; // pp wants these

        readonly List<double> hitErrors = new List<double>();

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
                    maxComboPortion += HitResults.BaseScore(r) * Math.Pow(combo, combo_exponent);
                }
            }
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
                    comboPortion += HitResults.BaseScore(max) * Math.Pow(Combo, combo_exponent);
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

            TotalScore = (long)Math.Round((500000 * Accuracy * comboProgress
                                           + 500000 * Math.Pow(Accuracy, 5) * accProgress
                                           + bonusPortion) * Multiplier);
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
                if (r >= Rank.S && Miss > 0) r = Rank.A;
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
