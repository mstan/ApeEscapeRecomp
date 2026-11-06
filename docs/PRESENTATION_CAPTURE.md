# Explicit presentation source capture

`src/mods/ape_presentation_capture.cpp` is a developer-only, default-off
qualification producer. It does not generate or present intermediate frames.
The preloaded/release catalog excludes its package.

For SCUS-94423, guarded instruction hooks observe the original skeletal
projector at `80016A74`/`80016B44` and rigid projector at
`800161C4`/`80016260`. Each captured batch owns the original local SVECTORs,
matrix words, global camera words, native SXY/SZ and projection controls.
All three local inputs must equal the live GTE inputs. Source pointers and
indices must follow the original descriptor layout. Callbacks preserve guest
CPU, RAM and GTE state.

At the original DrawOTag argument read, `80028CD0`, the producer copies the
submitted linked-list command words in their original order. A bounded walk
rejects invalid addresses and cycles. Explicit source tokens reach polygon
corners only through intact CPU/GTE word copies; identical coordinates do not
establish lineage. Partial writes and arithmetic discard it. Unknown geometry
remains untracked. Any invalid list or overflow disqualifies the snapshot.

These are ordering-table submissions, not qualified distinct game updates.
Descriptor addresses establish provenance, not object lifetimes. Rigid
producers include UI with camera conventions that differ from the world.
Resource versions, remaining geometry producers, lifetime events, camera
domains and display deadlines still need qualification before this data can
enable interpolation. The debug reply therefore reports
`lifetime_qualified:false`.

`ape_presentation_capture_test` verifies read-only hooks, distinct instances,
explicit packet lineage, rigid layout, bounded list rejection and reset.
`ape_preloaded_mods_test` checks the development manifest and its exclusion
from the preloaded catalog. Muted original-disc software/headless runs have
exercised both projection paths and packet transport without tuple rejection,
invalid lists, overflow or failed token tagging. This does not establish
gameplay coverage, interpolation quality, cadence or latency.
