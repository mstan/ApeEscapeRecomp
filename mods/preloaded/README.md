# Built-in Ape Escape mods

These packages are copied beside the runtime at build time and appear on the
launcher's Mods page. They contain metadata and guarded patches only; trusted
native callbacks are compiled into ApeEscapeRecomp itself.

Fresh profiles enable Skip FMVs, Fit to screen, PGXP, and the hidden prop
visibility correction. Quick Gadget Select and 60 FPS Geometry Interpolation
default off. Geometry Interpolation uses OpenGL and keeps native fallback in
unsupported scenes; display-rate output is not included. Saved enhancement
choices override their defaults. The target hash is the SHA-256 of the
SCUS-94423 MODE2/2352 data-track BIN used by the mod loader for disc identity.
