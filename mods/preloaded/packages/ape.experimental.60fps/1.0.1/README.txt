Ape Escape Temporal Frame Blending

This mod leaves Ape Escape's executable, VSync waits, simulation, timers, and
audio untouched. It combines the two most recent completed game frames in
PSXrecomp's OpenGL presentation path at the display refresh or a fixed 60,
120, 144, or 165 presentation rate.

The motion-adaptive clarity blend avoids crossfading large pixel changes to
reduce double-image trails. It is presentation-only temporal blending, not
motion-vector generation, so it cannot reconstruct true in-between positions.
