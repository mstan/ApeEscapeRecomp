# Explicit presentation source capture

`src/mods/ape_presentation_capture.cpp` supplies the default-off Geometry
Interpolation package. Normal package activation starts 60 FPS presentation
during the qualified gameplay phase. Retained-phase export and source
diagnostics remain separate qualification commands.

For SCUS-94423, guarded instruction hooks observe the original skeletal
projector at `80016A74`/`80016B44`, rigid projector at
`800161C4`/`80016260`, uncompressed world projector at
`800160F0`/`8001618C`, all four typed world quarter-turn loops, and the
terrain grid's original RTPT/RTPS sites, and both flat-textured and Gouraud
subdivision routines. Each captured batch owns the original local SVECTORs,
matrix words, global camera words, native SXY/SZ and projection controls.
Captured inputs must equal the live GTE inputs. Typed world batches retain
both authored vertices and the separate quarter-turn/origin transform; terrain
vertices use the original height-array indices, including coarse cell centres.
Source pointers and indices must follow the original descriptor layout. Callbacks preserve guest
CPU, RAM and GTE state.

At the original DrawOTag argument read, `80028CD0`, the producer copies the
submitted linked-list command words in their original order, parsing every
command in a node using the runtime's GP0 decoder. A bounded walk rejects
invalid addresses and cycles. A bounded immutable source ring retains references
across multiple ordering-table submissions in one update. Explicit source tokens reach polygon
corners only through intact CPU/GTE word copies; identical coordinates do not
establish lineage. Partial writes and arithmetic discard it. Unknown geometry
remains untracked. The recursive GT4 subdivision producer binds explicit parent
tokens after its original half-word writers finish and before recursive children
consume them. The first edges are observed at the common branch successors
`8001DF70`/`8001E0D0`; recursive-entry observation at `8001DB74` handles coordinates
completed in JAL delay slots. Child snapshots retain their parent dependencies.
Each recipe validates both original parent XY/Z and the original result using
the depth guard 164 and the authored raster-edge bias. Sampling recomputes those
explicit edges from the sampled parents. No generic arithmetic preserves tokens.
Any invalid list or overflow disqualifies the snapshot.

These are ordering-table submissions, not qualified distinct game updates.
Descriptor addresses establish provenance, not object lifetimes. Rigid
producers include UI with camera conventions that differ from the world.
Five original descriptor constructors now start distinct lifetime witnesses
after their header stores. Witnesses bind to the exact descriptor header word
through write-invalidated transport; an overwrite or restore makes it unknown,
even if the replacement value agrees. Each copied source batch owns the observed
lifetime. This does not yet qualify all allocation/reuse paths or terrain
lifetimes, and restore currently cuts capture history rather than reconstructing
live identities. The report includes constructor and witnessed-batch counts.
The original matrix composer at `8002FF50` receives camera, world and output
matrix operands. When the camera operand is the original global camera, entry
observation retains the world matrix before in-place composition. Observation
at `800300A4` retains the result and puts a write-invalidated witness on its
first word. Skeletal and rigid world sources admit that binding only when the
witness, matrix pointer, full result and camera still agree. Object lifetime
qualification remains independent. This replaces the earlier skeleton-only
call-site binding and keeps rigid UI domains separate. No fitted camera or
camera-composed object pose is used. `root_domain` is 1 for this binding, 2 for
authored typed-world transforms, and 0 for an unqualified root. Bone hierarchy
and other camera domains still require their own scene-producer integration.
Leaf projection scopes belong to their original guest call frames and survive
nested draws, other guest stacks and ordering-table submissions. Stack/return
registers delimit the activation only; they never identify corresponding
vertices or objects. Return hooks retire each scope. Reset discards all scopes.
Full-game source coverage, lifetime events, camera domains and display-rate
scheduling still require qualification. The debug reply therefore reports
`lifetime_qualified:false`.

The developer command `gpu on` retains an immutable GPU texture version with
each submission. `gpu render <path>` draws captured polygons and rectangles into
a separate target and exports a PNG at the effective internal resolution. It performs same-command
CPU, RAM, CPU-VRAM and native GPU image checks; the guest cannot advance between
those observations. The old debug `pause` command is unsupported and must not
be used to compare separate live-frame observations as though they were frozen.
`gpu freeze` owns a complete copied submission and retained texture reference;
`gpu render-held <path>` renders it after subsequent native frames. The separate
`gpu resource-test` temporarily changes one native texel, renders independently
retained old/new versions through cache turnover, and restores the texel. These
qualification controls are development-only; use isolated test sessions.

`gpu pair` retains the frozen submission and the latest submission.
`gpu pair-adjacent` retains the two latest submissions, using a bounded third
owned frame so the older data is not overwritten. The command
`gpu interpolate <alpha> <path>` samples explicit camera/world-root/local-vertex
geometry at a fixed phase and exports it. The camera's authored lens matrix at
`800AF0A0` is copied separately: the original camera construction composes it
through `8002FF50`, and its 3040/4096 Y scale is projection rather than rotation.
The adapter removes that observed scale from view coordinates and retains it
in the projection. Unsupported non-diagonal lenses are rejected. This uses
original matrix operands, without fitting a camera to screen coordinates.
The original look-at constructor also supplies its explicit eye, look vector
and roll. A proper rigid orientation is retained separately from the exact
native linear view adjustment. All eight matrix words and the constructor
lens must retain their write-invalidated provenance before this path is used.

This is a retained-pair qualification prototype. It currently renders the newer
submission's visible command topology, and leaves unsupported/unknown geometry
at native positions. Skipped commands and sampled triangles are reported
explicitly. Full topology/visibility, effective widescreen/FOV, remaining geometry
domains and discontinuities still require qualification. `gpu live on` prepares
a midpoint on publication and selects it nonblocking on the repeated native
present. Native simulation and presentation deadlines continue normally.
Display-rate scheduling remains unimplemented.

`ape_presentation_capture_test` verifies read-only hooks, distinct instances,
explicit packet lineage across submissions, multiple commands per node, rigid
and world layouts, all quarter-turns, terrain indices, subdivision parents,
bounded list rejection and reset.
`ape_preloaded_mods_test` checks visible, default-off package selection and
its normal presentation activation. Muted original-disc software/headless runs have
reached first-level gameplay and exercised these projection paths and packet
transport without tuple rejection, invalid lists, overflow or failed token
tagging. Partial and untracked polygons remain explicit. This does not establish
full-game coverage, interpolation quality, cadence or latency.
