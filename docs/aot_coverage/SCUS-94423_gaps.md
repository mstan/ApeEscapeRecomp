# AOT static-coverage recall — SCUS-94423

_How much of the played reference set did the play-free static extractor reproduce, and how much lies in compiled static code?_

- Static shard cache: `F:\Projects\psxrecomp\ApeEscapeRecomp\build-aot\cache\SCUS-94423\gcc\win-x64\cg5_84eaacd4`
- Static manifest entries: **2859**

- Base BIOS native dispatch entries: **1314**; relocated kernel body ranges: **37**
- Combined metrics below count both the play-free overlay cache and the separately generated, live-byte-guarded base BIOS.

## vs live capture history

- Sources: **1 capture file + verified append-only history**
- Verified immutable snapshots: **21**; invalid records: **0**
- Dispatch entries exercised: **107**
- Discovered by static: **11** (**10.3%** entry-level recall)
- Covered by compiled static code ranges: **72** (**67.3%** code-range recall)
- Overlay-only true code-range gaps: **35**
- Including base BIOS native code ranges: **107** (**100.0%**)
- Combined true code-range gaps: **0**
- Exact-entry misses (diagnostic; may be interior fragments): **96**
