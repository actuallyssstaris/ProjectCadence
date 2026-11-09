// osu!'s OsuDifficultyHitObject (https://github.com/ppy/osu), MIT licence,
// on top of this project's object model.

using System;
using System.Collections.Generic;
using System.Numerics;
using Osu5.Objects;

namespace Osu5.Stars
{
    sealed class DiffObject
    {
        public const int NormalisedRadius = 50;
        public const int NormalisedDiameter = NormalisedRadius * 2;
        public const int MinDeltaTime = 25;

        const float maximum_slider_radius = NormalisedRadius * 2.4f;
        const float assumed_slider_radius = NormalisedRadius * 1.8f;

        readonly List<DiffObject> all;

        public readonly int Index;
        public readonly OsuObject Base;
        public readonly OsuObject Last;
        public readonly double DeltaTime;
        public readonly double StartTime;
        public readonly double EndTime;
        public readonly double ClockRate;
        public readonly double HitWindowGreat;
        public readonly double AdjustedDeltaTime;

        public double LastObjectEndDeltaTime { get; private set; }
        public double JumpDistance { get; private set; }
        public double LazyJumpDistance { get; private set; }
        public double MinimumJumpDistance { get; private set; }
        public double MinimumJumpTime { get; private set; }
        public double TravelDistance { get; private set; }
        public double TravelTime { get; private set; }
        public Vector2? LazyEndPosition { get; private set; }
        public double LazyTravelDistance { get; private set; }
        public double LazyTravelTime { get; private set; }
        public double? Angle { get; private set; }
        public double? NormalisedVectorAngle { get; private set; }

        public double Preempt => Base.TimePreempt / ClockRate;
        public double SmallCircleBonus => Math.Max(1.0, 1.0 + (30 - Base.Radius) / 70);
        public double OverallDifficulty => (79.5 - HitWindowGreat / 2) / 6;

        // greatWindow: the one-sided 300 window in track time, 0 for spinners
        public DiffObject(OsuObject hitObject, OsuObject lastObject, double clockRate, double greatWindow, List<DiffObject> objects, int index)
        {
            all = objects;
            Index = index;
            Base = hitObject;
            Last = lastObject;
            DeltaTime = (hitObject.StartTime - lastObject.StartTime) / clockRate;
            StartTime = hitObject.StartTime / clockRate;
            EndTime = hitObject.EndTime / clockRate;
            ClockRate = clockRate;
            HitWindowGreat = 2 * greatWindow / clockRate;

            AdjustedDeltaTime = Math.Max(DeltaTime, MinDeltaTime);
            LastObjectEndDeltaTime = Previous() is DiffObject last ? Math.Max(StartTime - last.EndTime, MinDeltaTime) : AdjustedDeltaTime;

            computeSliderCursorPosition();
            setDistances(clockRate);
        }

        public DiffObject? Previous(int skip = 0)
        {
            int i = Index - (skip + 1);
            return i >= 0 && i < all.Count ? all[i] : null;
        }

        public DiffObject? Next(int skip = 0)
        {
            int i = Index + (skip + 1);
            return i >= 0 && i < all.Count ? all[i] : null;
        }

        public double OpacityAt(double time, bool hidden)
        {
            if (time > Base.StartTime)
                return 0.0;

            double fadeInStartTime = Base.StartTime - Base.TimePreempt;
            double fadeInDuration = 400 * Math.Min(1, Base.TimePreempt / OsuObject.PreemptMin);

            if (hidden)
            {
                // with Hidden the fade in is 40% of the approach, the fade out 30%
                double fadeOutStartTime = Base.StartTime - Base.TimePreempt + Base.TimePreempt * 0.4;
                double fadeOutDuration = Base.TimePreempt * 0.3;
                return Math.Min(
                    Math.Clamp((time - fadeInStartTime) / fadeInDuration, 0.0, 1.0),
                    1.0 - Math.Clamp((time - fadeOutStartTime) / fadeOutDuration, 0.0, 1.0));
            }

            return Math.Clamp((time - fadeInStartTime) / fadeInDuration, 0.0, 1.0);
        }

        public double CalculateDoubleTapFeasibility(DiffObject? nextObj)
        {
            if (nextObj == null) return 0;

            double currDeltaTime = Math.Max(1, DeltaTime);
            double nextDeltaTime = Math.Max(1, nextObj.DeltaTime);
            double deltaDifference = Math.Abs(nextDeltaTime - currDeltaTime);
            double speedRatio = currDeltaTime / Math.Max(currDeltaTime, deltaDifference);
            double windowRatio = DiffUtils.Pow(Math.Min(1, currDeltaTime / HitWindowGreat), 5);
            double distanceFactor = DiffUtils.Pow(DiffUtils.ReverseLerp(LazyJumpDistance, NormalisedDiameter, NormalisedRadius), 2);
            return 1.0 - DiffUtils.Pow(speedRatio, distanceFactor * (1 - windowRatio));
        }

        void setDistances(double clockRate)
        {
            if (Base is Slider currentSlider)
            {
                TravelDistance = LazyTravelDistance * Math.Max(1, DiffUtils.Pow(currentSlider.RepeatCount, 0.3));
                TravelTime = Math.Max(LazyTravelTime / clockRate, MinDeltaTime);
            }

            MinimumJumpTime = AdjustedDeltaTime;

            if (Base is Spinner || Last is Spinner)
                return;

            float scalingFactor = NormalisedRadius / Base.Radius;

            var lastDifficultyObject = Previous();
            var lastLastDifficultyObject = Previous(1);

            Vector2 lastCursorPosition = lastDifficultyObject != null ? endCursorPosition(lastDifficultyObject) : Last.StackedPosition;

            JumpDistance = (Last.StackedPosition - Base.StackedPosition).Length() * scalingFactor;
            LazyJumpDistance = (Base.StackedPosition - lastCursorPosition).Length() * scalingFactor;
            MinimumJumpDistance = LazyJumpDistance;

            if (Last is Slider lastSlider && lastDifficultyObject != null)
            {
                double lastTravelTime = Math.Max(lastDifficultyObject.LazyTravelTime / clockRate, MinDeltaTime);
                MinimumJumpTime = Math.Max(AdjustedDeltaTime - lastTravelTime, MinDeltaTime);

                float tailJumpDistance = (tailPosition(lastSlider) - Base.StackedPosition).Length() * scalingFactor;
                MinimumJumpDistance = Math.Max(0, Math.Min(LazyJumpDistance - (maximum_slider_radius - assumed_slider_radius), tailJumpDistance - maximum_slider_radius));
            }

            if (lastLastDifficultyObject != null && lastLastDifficultyObject.Base is not Spinner)
            {
                if (lastDifficultyObject!.Base is Slider prevSlider && lastDifficultyObject.TravelDistance > 0)
                    lastCursorPosition = prevSlider.StackedPosition;

                Vector2 lastLastCursorPosition = endCursorPosition(lastLastDifficultyObject);

                double angle = calculateAngle(Base.StackedPosition, lastCursorPosition, lastLastCursorPosition);
                double sliderAngle = calculateSliderAngle(lastDifficultyObject, lastLastCursorPosition);

                Vector2 v = Base.StackedPosition - lastCursorPosition;
                NormalisedVectorAngle = Math.Atan2(Math.Abs(v.Y), Math.Abs(v.X));
                Angle = Math.Min(angle, sliderAngle);
            }
        }

        static Vector2 tailPosition(Slider s) => s.Nested[^1].Position + s.StackOffset;

        void computeSliderCursorPosition()
        {
            if (Base is not Slider slider)
                return;

            double trackingEndTime = Math.Max(slider.StartTime + slider.Duration + Slider.TailLeniency, slider.StartTime + slider.Duration / 2);

            // stacked positions of head, ticks, repeats and tail, in time order
            var nested = new List<(Vector2 Pos, NestedKind Kind, double Time)>(slider.Nested.Count);
            foreach (var n in slider.Nested)
                nested.Add((n.Position + slider.StackOffset, n.Kind, n.Time));

            int lastTick = nested.FindLastIndex(n => n.Kind == NestedKind.Tick);
            if (lastTick >= 0 && nested[lastTick].Time > trackingEndTime)
            {
                trackingEndTime = nested[lastTick].Time;
                var t = nested[lastTick];
                nested.RemoveAt(lastTick);
                nested.Add(t);
            }

            LazyTravelTime = trackingEndTime - slider.StartTime;

            double endTimeMin = LazyTravelTime / slider.SpanDuration;
            if (endTimeMin % 2 >= 1)
                endTimeMin = 1 - endTimeMin % 1;
            else
                endTimeMin %= 1;

            LazyEndPosition = slider.StackedPosition + slider.Path.PositionAt(endTimeMin);

            Vector2 currCursorPosition = slider.StackedPosition;
            double scalingFactor = NormalisedRadius / slider.Radius;

            for (int i = 1; i < nested.Count; i++)
            {
                var currMovementObj = nested[i];
                Vector2 currMovement = currMovementObj.Pos - currCursorPosition;
                double currMovementLength = scalingFactor * currMovement.Length();

                double requiredMovement = assumed_slider_radius;

                if (i == nested.Count - 1)
                {
                    Vector2 lazyMovement = LazyEndPosition.Value - currCursorPosition;
                    if (lazyMovement.Length() < currMovement.Length())
                        currMovement = lazyMovement;
                    currMovementLength = scalingFactor * currMovement.Length();
                }
                else if (currMovementObj.Kind == NestedKind.Repeat)
                {
                    requiredMovement = NormalisedRadius;
                }

                if (currMovementLength > requiredMovement)
                {
                    currCursorPosition += currMovement * (float)((currMovementLength - requiredMovement) / currMovementLength);
                    currMovementLength *= (currMovementLength - requiredMovement) / currMovementLength;
                    LazyTravelDistance += currMovementLength;
                }

                if (i == nested.Count - 1)
                    LazyEndPosition = currCursorPosition;
            }
        }

        double calculateSliderAngle(DiffObject lastDifficultyObject, Vector2 lastLastCursorPosition)
        {
            Vector2 lastCursorPosition = endCursorPosition(lastDifficultyObject);

            if (lastDifficultyObject.Base is Slider prevSlider && lastDifficultyObject.TravelDistance > 0)
                lastLastCursorPosition = prevSlider.Nested[^2].Position + prevSlider.StackOffset;

            return calculateAngle(Base.StackedPosition, lastCursorPosition, lastLastCursorPosition);
        }

        static double calculateAngle(Vector2 currentPosition, Vector2 lastPosition, Vector2 lastLastPosition)
        {
            Vector2 v1 = lastLastPosition - lastPosition;
            Vector2 v2 = currentPosition - lastPosition;
            float dot = Vector2.Dot(v1, v2);
            float det = v1.X * v2.Y - v1.Y * v2.X;
            return Math.Abs(Math.Atan2(det, dot));
        }

        static Vector2 endCursorPosition(DiffObject o) => o.LazyEndPosition ?? o.Base.StackedPosition;
    }
}
