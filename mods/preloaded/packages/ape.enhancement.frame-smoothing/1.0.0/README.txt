Ape Escape Frame Smoothing

This optional presentation effect keeps Ape Escape's executable, simulation,
timers, VBlank cadence, and audio unchanged. The OpenGL renderer crossfades the
two most recently completed game frames and presents those blends on its
original thread and context.

This is temporal blending. It does not calculate motion vectors or reconstruct
true in-between object positions, so moving objects can show double images.
The change-adaptive clarity blend reduces trails on large pixel changes.
