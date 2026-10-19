#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LoadingMonitor
{
    // Plain value model: FLoadingState owns synchronization; only Slate advances presentation.
    // Kept independent of engine objects so timing regressions can be tested without launching UE.
    class FProgressPresentation
    {
    public:
        void Configure(float InSmoothingSpeed, float InMaxSpeed, float InCompletionDuration)
        {
            SmoothingSpeed = std::clamp(InSmoothingSpeed, 0.5f, 20.0f);
            MaxSpeed = std::clamp(InMaxSpeed, 5.0f, 100.0f);
            CompletionDuration = std::clamp(InCompletionDuration, 0.0f, 3.0f);
        }

        void Reset()
        {
            TotalCompleted = 0;
            Target = Displayed = Velocity = CompletionStart = CompletionElapsed = 0.0f;
            bMeasured = bCompleted = false;
        }

        void Update(int32_t Remaining, int32_t JustCompleted = 0)
        {
            if (bCompleted) return;
            TotalCompleted += std::max(0, JustCompleted);
            const int64_t Total = TotalCompleted + std::max(0, Remaining);
            if (Total > 0)
            {
                // Compilation is only part of map loading. Reserve the final 5% for map readiness.
                Target = std::max(Target, 95.0f * static_cast<float>(TotalCompleted) / static_cast<float>(Total));
                bMeasured = true;
            }
        }

        void Complete()
        {
            if (bCompleted) return;
            bCompleted = bMeasured = true;
            CompletionStart = Displayed;
            CompletionElapsed = 0.0f;
            Target = 100.0f;
            Velocity = 0.0f;
            if (CompletionDuration <= 0.0f) Displayed = 100.0f;
        }

        void Tick(float DeltaSeconds)
        {
            // Do not catch up an entire stalled render interval in one visible frame.
            const float Dt = std::clamp(DeltaSeconds, 0.0f, 0.05f);
            if (Dt <= 0.0f) return;
            if (bCompleted)
            {
                CompletionElapsed += Dt;
                const float T = CompletionDuration > 0.0f
                    ? std::clamp(CompletionElapsed / CompletionDuration, 0.0f, 1.0f) : 1.0f;
                const float Ease = T * T * (3.0f - 2.0f * T);
                Displayed = CompletionStart + (100.0f - CompletionStart) * Ease;
                return;
            }

            // Exact critically damped spring: batch arrivals change the target without snapping
            // position or velocity. Limit travel speed as well, so large batches remain readable.
            const float Offset = Displayed - Target;
            const float C = Velocity + SmoothingSpeed * Offset;
            const float Decay = std::exp(-SmoothingSpeed * Dt);
            const float Next = Target + (Offset + C * Dt) * Decay;
            Velocity = std::clamp((Velocity - SmoothingSpeed * C * Dt) * Decay, 0.0f, MaxSpeed);
            Displayed = std::clamp(Next, Displayed, std::min(Target, Displayed + MaxSpeed * Dt));
            if (Target - Displayed < 0.001f)
            {
                Displayed = Target;
                Velocity = 0.0f;
            }
        }

        float GetPercent() const { return Displayed; }
        bool HasProgress() const { return bMeasured; }
        bool IsComplete() const { return bCompleted; }
        bool IsSettled() const { return bCompleted && Displayed >= 100.0f; }

    private:
        int64_t TotalCompleted = 0;
        float Target = 0.0f;
        float Displayed = 0.0f;
        float Velocity = 0.0f;
        float SmoothingSpeed = 4.0f;
        float MaxSpeed = 35.0f;
        float CompletionDuration = 0.45f;
        float CompletionStart = 0.0f;
        float CompletionElapsed = 0.0f;
        bool bMeasured = false;
        bool bCompleted = false;
    };
}
