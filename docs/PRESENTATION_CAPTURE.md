# Explicit presentation source capture

`src/mods/ape_presentation_capture.cpp` is a developer-only, default-off
qualification producer. It does not generate or present intermediate frames.
The preloaded/release catalog excludes its package.

For SCUS-94423, guarded instruction hooks observe the original skeletal
projector at `80016A74`/`80016B44`, rigid projector at
`800161C4`/`80016260`, uncompressed world projector at
`800160F0`/`8001618C`, all four typed world quarter-turn loops, and the
terrain grid's original RTPT/RTPS sites. Each captured batch owns the original local SVECTORs,
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
tokens after its original half-word writers finish; child snapshots retain their
parent dependencies. Those recipes use native projective/depth-guard semantics,
not assumed world-space midpoints. No generic arithmetic starts preserving tokens.
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
The skeleton update at `80015504` overwrites its root matrix when composing
the camera at `800156C8`. Paired observations before that call and at its return
`800156D0` retain the original world root separately. A batch admits that world
root only when the lifetime, matrix pointer, resulting view root and camera all
still agree with the recorded operation. This uses original matrix provenance;
there is no fitted camera or decomposition of a camera-composed root as an
object pose. `root_domain` is 1 for that binding, 2 for authored typed-world
transforms, and 0 for an unqualified root. Bone hierarchy and other camera
domains still require their own scene-producer integration.
Leaf projection scopes belong to their original guest call frames and survive
nested draws, other guest stacks and ordering-table submissions. Stack/return
registers delimit the activation only; they never identify corresponding
vertices or objects. Return hooks retire each scope. Reset discards all scopes.
Resource versions, remaining geometry producers (including other subdivision
paths), lifetime events, camera
domains and display deadlines still need qualification before this data can
enable interpolation. The debug reply therefore reports
`lifetime_qualified:false`.

`ape_presentation_capture_test` verifies read-only hooks, distinct instances,
explicit packet lineage across submissions, multiple commands per node, rigid
and world layouts, all quarter-turns, terrain indices, subdivision parents,
bounded list rejection and reset.
`ape_preloaded_mods_test` checks the development manifest and its exclusion
from the preloaded catalog. Muted original-disc software/headless runs have
reached first-level gameplay and exercised these projection paths and packet
transport without tuple rejection, invalid lists, overflow or failed token
tagging. Partial and untracked polygons remain explicit. This does not establish
full-game coverage, interpolation quality, cadence or latency.
