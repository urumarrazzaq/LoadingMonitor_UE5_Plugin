#include "../Source/LoadingMonitor/Private/LoadingMonitorProgress.h"
#include <cstdlib>
#include <iostream>

using LoadingMonitor::FProgressPresentation;

static void Check(bool Condition, const char* Message)
{
    if (!Condition) { std::cerr << "FAIL: " << Message << '\n'; std::exit(1); }
}

static float Advance(FProgressPresentation& Model, float Seconds, int Fps)
{
    for (int Frame = 0; Frame < static_cast<int>(Seconds * Fps); ++Frame)
    {
        const float Before = Model.GetPercent();
        Model.Tick(1.0f / Fps);
        Check(Model.GetPercent() >= Before, "Progress must not move backward");
        Check(Model.GetPercent() <= 100.0f, "Progress must not overshoot");
        if (!Model.IsComplete())
            Check(Model.GetPercent() - Before <= 35.0f / Fps + 0.0011f, "Burst exceeds travel speed limit");
    }
    return Model.GetPercent();
}

int main()
{
    FProgressPresentation Model;
    Check(!Model.HasProgress(), "New loads must start indeterminate");
    Model.Update(0);
    Advance(Model, 2.0f, 60);
    Check(!Model.HasProgress() && Model.GetPercent() == 0.0f, "No made-up progress without measurements");
    Model.Update(0, 100);
    Check(Model.GetPercent() == 0.0f, "First sample must not snap the display");
    const float BeforeRead = Model.GetPercent();
    for (int I = 0; I < 100; ++I) Check(Model.GetPercent() == BeforeRead, "Reads must not advance animation");
    Advance(Model, 1.0f, 60);
    Check(Model.GetPercent() > 0.0f && Model.GetPercent() < 95.0f, "Large batch must ease in");
    const float BeforeHitch = Model.GetPercent();
    Model.Tick(2.0f);
    Check(Model.GetPercent() - BeforeHitch <= 1.751f, "Long hitch must not cause a catch-up jump");
    Model.Update(1000, 0);
    Advance(Model, 8.0f, 60);
    Check(Model.GetPercent() == 95.0f && !Model.IsSettled(), "100% is reserved for map completion");
    Model.Complete();
    Advance(Model, 0.2f, 60);
    Check(Model.GetPercent() < 100.0f, "Finish must animate instead of snapping");
    Model.Complete(); // Repeated notifications must not restart the finish.
    Model.Update(10000, 0);
    Advance(Model, 0.5f, 60);
    Check(Model.IsSettled() && Model.GetPercent() == 100.0f, "Finish must reach exactly 100%");
    Model.Reset();
    Check(!Model.HasProgress() && !Model.IsComplete() && Model.GetPercent() == 0.0f, "Reset must clear the previous load");
    Model.Complete();
    Advance(Model, 1.0f, 60);
    Check(Model.IsSettled(), "A cached load with no compile samples must finish");

    float Results[3];
    const int Rates[] = {30, 60, 144};
    for (int I = 0; I < 3; ++I)
    {
        FProgressPresentation AtRate;
        AtRate.Update(50, 50);
        Results[I] = Advance(AtRate, 1.0f, Rates[I]);
    }
    Check(std::abs(Results[0] - Results[2]) < 0.15f, "Animation should agree across frame rates");

    FProgressPresentation Immediate;
    Immediate.Configure(4.0f, 35.0f, 0.0f);
    Immediate.Complete();
    Check(Immediate.IsSettled(), "Zero finish duration must complete immediately");

    FProgressPresentation Slow;
    Slow.Configure(0.5f, 35.0f, 0.45f);
    Slow.Update(0, 100);
    Advance(Slow, 0.5f, 60);
    Slow.Complete();
    Advance(Slow, 0.6f, 60);
    Check(Slow.IsSettled(), "Finish duration must not depend on smoothing speed");

    FProgressPresentation InvalidCounts;
    InvalidCounts.Update(-20, -10);
    Check(!InvalidCounts.HasProgress(), "Negative compile counts must be ignored");
    InvalidCounts.Tick(-1.0f);
    Check(InvalidCounts.GetPercent() == 0.0f, "Negative delta must not advance animation");
    std::cout << "PASS: unknown, bursts, hitches, late work, completion, reset, 30/60/144 FPS, and bounds\n";
}
