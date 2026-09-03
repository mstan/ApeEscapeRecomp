#!/usr/bin/env python3
"""Release overlay-staging test: the shard cache and the overlay toolchain.

usage: test_release_overlay_staging.py <repo-root> <psxrecomp-root>

WHY THIS EXISTS
---------------
Two things have to reach a release package for a title's overlays to run as
native code on a player's machine:

  cache/<game-id>/<compiler>/<arch-abi>/<cg-tag>/*.dll
      prebuilt shards. The loader scans that EXACT path and ignores every other
      tag namespace, so shards under a stale tag are worth precisely as much as
      no shards at all -- and a shard count cannot tell the two apart.

  overlay_toolchain/python/python.exe
      the runtime gates autocompile on this one file (main.cpp: `tk_present`).
      Without it the capture -> compile fail-safe can NEVER fire, so any overlay
      the bundled cache misses is interpreted forever.

Nothing fails when a packager omits either one. The runtime falls back to the
dirty-RAM interpreter and reports itself perfectly healthy, which is how Ape
Escape shipped its entire release history with neither (measured: disp_native=0,
disp_interp=4,480,307 in one session) while its own game.toml claimed to ship an
AOT cache. Four separate titles then reimplemented the staging by hand and the
copies drifted -- stale tag format strings that matched zero shards, warnings
where there should have been failures, unpinned toolchain downloads.

So this test asserts two different things at two different costs:

  CONTRACT (always runs, needs no build products)
      the packager delegates to the framework's shared staging module and does
      not carry a private copy of anything the module owns. Every drift defect
      listed above is a contract violation, so this half is the half that would
      have caught them -- before a 40-minute Release build, not after.

  ARTIFACT (runs when a staged release or release zip is present)
      the real staged tree has the toolchain, and -- when the STAGED game.toml
      declares [runtime] overlay_cache -- a non-empty shard cache under exactly
      ONE tag. Point it at one explicitly with PSX_RELEASE_STAGE / PSX_RELEASE_ZIP,
      or let it discover release-stage/ and *-windows-x64.zip under the repo.

An artifact check cannot run in a bare checkout: producing one needs the game's
disc image and a full Release build. That is stated rather than hidden -- the
run prints exactly which halves executed.
"""

import os
import re
import sys
import zipfile

FAILURES = []
NOTES = []


def fail(check, message):
    FAILURES.append("[%s] %s" % (check, message))


def note(message):
    NOTES.append(message)


# ---------------------------------------------------------------------------
# game.toml reading. Deliberately not a TOML parser: this must work on a bare
# python3 with no third-party modules, and it only ever needs one scalar out of
# one table.
# ---------------------------------------------------------------------------

def toml_scalar(path, table, key):
    """Value of <table>.<key> as a raw string, or None. Comment lines are not
    values: a `# overlay_cache = true` note in the config must never be read as
    a declaration."""
    section = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^\[\[?([^\]]+)\]\]?$", line)
            if m:
                section = m.group(1).strip()
                continue
            if section != table:
                continue
            m = re.match(r"^" + re.escape(key) + r"\s*=\s*(.+?)\s*(?:#.*)?$", line)
            if m:
                return m.group(1).strip()
    return None


def declares_overlay_cache(path):
    v = toml_scalar(path, "runtime", "overlay_cache")
    return v is not None and v.lower() == "true"


def game_id(path):
    v = toml_scalar(path, "game", "id")
    return v.strip('"').strip("'") if v else None


# ---------------------------------------------------------------------------
# CONTRACT: what tools/package_release.ps1 must and must not contain.
# ---------------------------------------------------------------------------

# The one tag shape the loader scans. Nothing but compile_overlays.cache_tag()
# may produce it, so any packager that formats a tag itself is a stale parallel
# implementation waiting to happen (it already happened twice: the _f<flavor>
# suffix and the _gc<config-hash> field were both added without the copies
# following, and each time a valid cache staged ZERO shards).
TAG_RE = re.compile(r"^cg\d+_[0-9a-f]{8}_gc[0-9a-f]{8}_f\d+$")

LOCAL_TAG_FORMATS = [
    "'cg%d_",           # PowerShell here-string building the tag in python
    '"cg%d_',
    "cg{0}_",           # PowerShell -f format
    "'cg%s_",
]


def strip_noncode(text):
    """Blank out PowerShell comments and here-string bodies, keeping line numbers.

    Every "must NOT contain" check below has to run against CODE, not prose. A
    packager is expected to explain, in comments, exactly which duplicated
    implementation it replaced -- naming Invoke-WebRequest, AllowNoCache and the
    rest -- and a checker that cannot tell a warning about a defect from the
    defect would forbid documenting the thing it exists to prevent. Here-string
    bodies go too: START_HERE.txt and RELEASE.txt text is player-facing prose
    that stages nothing.

    Lines are replaced with empty strings rather than removed so reported line
    numbers still match the real file.
    """
    out = []
    in_here = None
    for raw in text.splitlines():
        if in_here is not None:
            # A here-string terminator must be at the very start of the line.
            if raw.strip() == in_here:
                in_here = None
            out.append("")
            continue
        m = re.search(r'@("|\')\s*$', raw)
        if m:
            in_here = m.group(1) + "@"
            out.append(raw[:m.start()])
            continue
        # Cut a trailing comment only when the '#' is outside quotes. Counting
        # quotes before it is enough here: PowerShell has no multi-line ordinary
        # strings, and here-strings are already handled above.
        cut = None
        for i, ch in enumerate(raw):
            if ch == "#":
                before = raw[:i]
                if before.count('"') % 2 == 0 and before.count("'") % 2 == 0:
                    cut = i
                    break
        out.append(raw if cut is None else raw[:cut])
    return "\n".join(out)


def check_contract(repo, fw):
    ps1 = os.path.join(repo, "tools", "package_release.ps1")
    if not os.path.isfile(ps1):
        fail("contract", "no packager at %s" % ps1)
        return
    with open(ps1, "r", encoding="utf-8", errors="replace") as fh:
        full = fh.read()
    # Everything below reads CODE. See strip_noncode(): a packager must be able
    # to name, in comments, the duplicated implementation it replaced.
    text = strip_noncode(full)
    lines = text.splitlines()

    # -- the packager must delegate, not duplicate --------------------------
    if not re.search(r"release_overlay_stage\.ps1", text):
        fail("shared-module",
             "packager does not dot-source the framework's "
             "tools/release_overlay_stage.ps1; overlay staging must be a CALL, "
             "never a copy")

    # Add-OverlayToolchain must be called, and called UNCONDITIONALLY. A
    # top-level call (column 0) is the only form that cannot be skipped by a
    # branch, and the runtime's autocompile fail-safe depends on it existing in
    # every package, cache or no cache.
    tk_calls = [i + 1 for i, l in enumerate(lines)
                if re.match(r"^Add-OverlayToolchain\b", l)]
    tk_any = [i + 1 for i, l in enumerate(lines)
              if re.search(r"\bAdd-OverlayToolchain\b", l)]
    if not tk_any:
        fail("toolchain-staged",
             "packager never calls Add-OverlayToolchain, so the release ships "
             "no overlay_toolchain/ and the runtime's autocompile gate "
             "(main.cpp tk_present) is false forever -- every overlay the "
             "bundled cache misses stays interpreted")
    elif not tk_calls:
        fail("toolchain-unconditional",
             "Add-OverlayToolchain is only reachable from an indented/"
             "conditional context (lines %s); it must be staged "
             "unconditionally at top level" % tk_any)

    # Nothing the module owns may be reimplemented here. Get-PinnedArchive
    # verifies the SHA256 of every archive on every use, including cache hits;
    # a private Invoke-WebRequest + bare Test-Path trusts whatever the mirror
    # served the day the cache was first filled, forever.
    for needle, why in (
        ("Invoke-WebRequest",
         "downloads a toolchain archive itself instead of via the module's "
         "Get-PinnedArchive, which SHA256-verifies every archive on every use"),
        ("Expand-Archive",
         "unpacks a toolchain archive itself; Add-OverlayToolchain owns that"),
        ("embed-amd64.zip",
         "names the embedded-python archive itself; the pinned version and hash "
         "live in the module"),
        ("win64-bin.zip",
         "names the tcc archive itself; the pinned version and hash live in the "
         "module"),
    ):
        if needle in text:
            fail("no-duplicate-toolchain", "packager %s (%r)" % (why, needle))

    # -- the tag must come from the tool that owns the cache layout ---------
    if "Get-OverlayCgTag" not in text:
        fail("tag-from-module",
             "packager does not call Get-OverlayCgTag; the cache tag must come "
             "from compile_overlays.cache_tag(), never from a local format string")
    for bad in LOCAL_TAG_FORMATS:
        if bad in text:
            fail("tag-from-module",
                 "packager formats the cache tag itself (%r). Every field added "
                 "to the tag since -- _gc<config-hash>, _f<flavor> -- was added "
                 "without the copies following, and each time the filter matched "
                 "nothing and a valid cache staged ZERO shards" % bad)

    # The tag folds in a hash of the game.toml, so it must be derived from the
    # STAGED config. Derived from the dev config it names a namespace the
    # shipped runtime never reads, and nothing complains.
    # Join PowerShell backtick line continuations first: the call spans several
    # lines, and a regex that tries to walk them inline silently matched only
    # the first one and then reported "cannot find -GameToml" on a call that
    # plainly had it.
    joined = re.sub(r"`[ \t]*\r?\n[ \t]*", " ", text)
    m = re.search(r"Get-OverlayCgTag[^\n]*", joined)
    if m:
        call = m.group(0)
        gm = re.search(r"-GameToml\s+(\$[A-Za-z_][A-Za-z0-9_]*|\([^)]*\))", call)
        if not gm:
            fail("tag-from-staged-config",
                 "cannot find -GameToml on the Get-OverlayCgTag call")
        else:
            arg = gm.group(1)
            expr = arg
            if arg.startswith("$"):
                am = re.search(re.escape(arg) + r"\s*=\s*([^\n]+)", text)
                expr = am.group(1) if am else ""
            if "$Stage" not in expr:
                fail("tag-from-staged-config",
                     "Get-OverlayCgTag -GameToml resolves to %r, which does not "
                     "reference $Stage. The tag hashes the game.toml, so a tag "
                     "derived from the DEV config names a cache namespace the "
                     "shipped runtime never scans." % expr.strip())

    # -- a declared cache is mandatory, with no way out --------------------
    dev_toml = os.path.join(repo, "game.toml")
    declared = os.path.isfile(dev_toml) and declares_overlay_cache(dev_toml)
    if declared:
        if not re.search(r"\bAdd-OverlayCache\b", text):
            fail("cache-staged",
                 "game.toml declares [runtime] overlay_cache = true but the "
                 "packager never calls Add-OverlayCache, so the shipped runtime "
                 "scans cache/ on every launch and finds nothing")
        # No escape hatch. This project has shipped four separate incomplete
        # packages by warning instead of throwing; a switch that turns the throw
        # off is the same defect with a flag on it.
        if "AllowNoCache" in text:
            fail("no-escape-hatch",
                 "packager exposes/forwards AllowNoCache. A title that declares "
                 "overlay_cache must ship a cache or fail; there is no "
                 "deliberate-downgrade path")
        for i, l in enumerate(lines):
            if re.search(r"Write-Warning", l) and re.search(r"cache", l, re.I):
                fail("cache-required",
                     "line %d warns about the overlay cache instead of failing: "
                     "%s" % (i + 1, l.strip()))

    # -- quarantined caches are never an input -----------------------------
    # A quarantined cross-version cache can carry the SAME cg tag as a good one
    # (measured: cg10_a4319b6f_gcc31ae4a9_f0 on both, 6 of 50 shard filenames in
    # common) so the tag filter cannot reject it. Only the path can.
    if "QUARANTINE" not in text:
        fail("refuse-quarantine",
             "packager does not refuse a cache source path containing "
             "QUARANTINE. A matching cg tag does NOT prove compatibility, so "
             "the path is the only signal left")

    # -- never bare `python` -----------------------------------------------
    # Bare `python` here resolves to the cygwin interpreter, which SIGSEGVs
    # under job spawn and then falls back silently.
    for i, l in enumerate(lines):
        if re.search(r"(?<![-\w.])&\s+python\b(?!\s*-3)", l) or "(& python " in l:
            fail("no-bare-python",
                 "line %d invokes bare `python` (cygwin interpreter; SIGSEGVs "
                 "under job spawn): %s" % (i + 1, l.strip()))

    note("contract: checked %s (%d lines)" % (ps1, len(lines)))
    return ps1


def check_module(fw):
    """Pin the framework behaviour the titles depend on.

    Add-OverlayCache decides what a release cache may contain by its -Include
    list. Three things must stay out of it:

      .abi_<tag>.ok  a completed-sweep memo. Shipping it SUPPRESSES the
                     runtime's ABI preflight, which is the check that rejects
                     stale shards on first launch (a quarantined Ape cache
                     carried .abi_00000015.ok while overlay_api.h was at 22).
      .c             the patched intermediates; megabytes of dead weight.
      .pair-lock     producer-side bookkeeping.
    """
    mod = os.path.join(fw, "tools", "release_overlay_stage.ps1")
    if not os.path.isfile(mod):
        fail("module-present",
             "no shared staging module at %s -- is psxrecomp-v4 checked out and "
             "is the framework pin new enough to carry it?" % mod)
        return
    with open(mod, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for fn in ("Get-OverlayCgTag", "Add-OverlayCache", "Add-OverlayToolchain",
               "Get-PinnedArchive", "Add-ModCatalog"):
        if ("function %s" % fn) not in text:
            fail("module-present",
                 "shared module does not define %s; the framework pin is too "
                 "old for this packager" % fn)
    m = re.search(r"Add-OverlayCache.*?-Include\s+([^\n|]+)", text, re.S)
    if not m:
        fail("module-include-list",
             "cannot find Add-OverlayCache's -Include list in the shared module")
    else:
        inc = m.group(1)
        for banned in (".c", ".ok", ".pair-lock"):
            if ("*%s" % banned) in inc:
                fail("module-include-list",
                     "shared module's cache -Include list admits *%s: %s"
                     % (banned, inc.strip()))
    note("module: checked %s" % mod)


# ---------------------------------------------------------------------------
# ARTIFACT: the real staged tree / zip.
# ---------------------------------------------------------------------------

class Tree(object):
    """A staged release, from a directory or a zip, as forward-slash paths.

    A zip may or may not carry a single top-level folder (Tomba 2's does,
    Ape Escape's does not), so strip one common root if every entry shares it.
    Reading the shipped zip and reading the stage must agree, and they only do
    if that difference is normalised away here rather than in each check.
    """

    def __init__(self, label, names, sizes):
        self.label = label
        roots = set(n.split("/", 1)[0] for n in names if "/" in n)
        if len(roots) == 1 and not any("/" not in n for n in names):
            root = roots.pop() + "/"
            names = [n[len(root):] for n in names]
            sizes = dict((k[len(root):], v) for k, v in sizes.items())
        self.names = [n for n in names if n]
        self.sizes = sizes

    @classmethod
    def from_dir(cls, path):
        names, sizes = [], {}
        for dirpath, _dirnames, filenames in os.walk(path):
            for fn in filenames:
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, path).replace(os.sep, "/")
                names.append(rel)
                sizes[rel] = os.path.getsize(full)
        return cls(path, names, sizes)

    @classmethod
    def from_zip(cls, path):
        with zipfile.ZipFile(path) as zf:
            infos = [i for i in zf.infolist() if not i.is_dir()]
        names = [i.filename.replace("\\", "/") for i in infos]
        sizes = dict((i.filename.replace("\\", "/"), i.file_size) for i in infos)
        return cls(path, names, sizes)

    def under(self, prefix):
        return [n for n in self.names if n.startswith(prefix)]

    def read(self, name):
        raise NotImplementedError


class DirTree(Tree):
    def __init__(self, path):
        t = Tree.from_dir(path)
        Tree.__init__(self, path, t.names, t.sizes)
        self._path = path

    def read(self, name):
        with open(os.path.join(self._path, name.replace("/", os.sep)),
                  "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()


class ZipTree(Tree):
    def __init__(self, path):
        t = Tree.from_zip(path)
        Tree.__init__(self, path, t.names, t.sizes)
        self._path = path
        self._prefix = ""
        with zipfile.ZipFile(path) as zf:
            raw = [i.filename.replace("\\", "/")
                   for i in zf.infolist() if not i.is_dir()]
        if raw and self.names and raw[0] != self.names[0]:
            self._prefix = raw[0][:len(raw[0]) - len(self.names[0])]

    def read(self, name):
        with zipfile.ZipFile(self._path) as zf:
            return zf.read(self._prefix + name).decode("utf-8", "replace")


def discover(repo):
    trees = []
    env_stage = os.environ.get("PSX_RELEASE_STAGE")
    env_zip = os.environ.get("PSX_RELEASE_ZIP")
    if env_stage:
        if os.path.isdir(env_stage):
            trees.append(DirTree(env_stage))
        else:
            fail("artifact", "PSX_RELEASE_STAGE=%s is not a directory" % env_stage)
    if env_zip:
        if os.path.isfile(env_zip):
            trees.append(ZipTree(env_zip))
        else:
            fail("artifact", "PSX_RELEASE_ZIP=%s is not a file" % env_zip)
    if trees:
        return trees
    stage_root = os.path.join(repo, "release-stage")
    if os.path.isdir(stage_root):
        for entry in sorted(os.listdir(stage_root)):
            full = os.path.join(stage_root, entry)
            if os.path.isdir(full):
                trees.append(DirTree(full))
    for entry in sorted(os.listdir(repo)):
        if entry.endswith("-windows-x64.zip"):
            trees.append(ZipTree(os.path.join(repo, entry)))
    return trees


def check_artifact(tree, dev_declared, dev_id):
    label = tree.label
    n = len(tree.names)
    print("  artifact %s: %d entries" % (label, n))

    # -- the shipped config is the contract --------------------------------
    staged_tomls = [x for x in tree.names
                    if x.endswith(".toml") and "/" not in x]
    staged_declared = None
    staged_id = None
    for t in staged_tomls:
        text = tree.read(t)
        section, decl, gid = None, None, None
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^\[\[?([^\]]+)\]\]?$", line)
            if m:
                section = m.group(1).strip()
                continue
            if section == "runtime":
                m = re.match(r"^overlay_cache\s*=\s*(\S+)", line)
                if m:
                    decl = m.group(1).lower() == "true"
            if section == "game":
                m = re.match(r'^id\s*=\s*"([^"]+)"', line)
                if m:
                    gid = m.group(1)
        if decl is not None or gid is not None:
            staged_declared = decl if decl is not None else staged_declared
            staged_id = gid or staged_id
    if staged_declared is None:
        fail("staged-config",
             "%s: no top-level game config declares [runtime] overlay_cache "
             "either way (found %s)" % (label, staged_tomls))
        staged_declared = dev_declared
    elif staged_declared != dev_declared:
        fail("staged-config",
             "%s: the SHIPPED config declares overlay_cache=%s but the "
             "validated game.toml declares %s. The cache tag hashes the config, "
             "so the two configs name different namespaces and the shipped one "
             "is not what was validated." % (label, staged_declared, dev_declared))
    if staged_id and dev_id and staged_id != dev_id:
        fail("staged-config", "%s: shipped [game] id %s != game.toml %s"
             % (label, staged_id, dev_id))

    # -- the toolchain, unconditionally ------------------------------------
    tk = tree.under("overlay_toolchain/")
    gate = "overlay_toolchain/python/python.exe"
    if not tk:
        fail("toolchain-present",
             "%s: no overlay_toolchain/ entries. The runtime gates autocompile "
             "on %s, so with it absent the capture -> compile fail-safe can "
             "never fire and every overlay the cache misses is interpreted "
             "forever." % (label, gate))
    elif gate not in tree.names:
        fail("toolchain-present",
             "%s: overlay_toolchain/ has %d entries but not %s, the exact file "
             "the runtime's tk_present gate probes" % (label, len(tk), gate))
    else:
        for want in ("overlay_toolchain/compile_overlays.py",
                     "overlay_toolchain/psxrecomp-game.exe"):
            if want not in tree.names:
                fail("toolchain-present", "%s: missing %s" % (label, want))
        print("  overlay_toolchain: %d entries (gate %s present)" % (len(tk), gate))

    # -- the cache -----------------------------------------------------------
    cache = tree.under("cache/")
    dlls = [x for x in cache if x.endswith(".dll")]
    if not staged_declared:
        if cache:
            fail("no-undeclared-cache",
                 "%s: shipped config does not declare overlay_cache yet %d "
                 "cache/ entries were staged; the runtime never scans them"
                 % (label, len(cache)))
        print("  cache: none staged (shipped config does not declare "
              "overlay_cache)")
        return

    if not dlls:
        fail("cache-non-empty",
             "%s: shipped config declares [runtime] overlay_cache = true but "
             "the package contains no cache/**/*.dll, so every player's first "
             "visit to every area runs on the dirty-RAM interpreter"
             % label)
    tags = set()
    for d in dlls:
        parts = d.split("/")
        if len(parts) < 2:
            continue
        tags.add(parts[-2])
    if len(tags) > 1:
        fail("cache-one-tag",
             "%s: cache shards span %d tag namespaces %s. The loader scans one "
             "tag; the rest are download weight it never reads."
             % (label, len(tags), sorted(tags)))
    for t in tags:
        if not TAG_RE.match(t):
            fail("cache-one-tag",
                 "%s: cache shard directory %r is not a cache_tag() namespace "
                 "(cg<ver>_<hash>_gc<confighash>_f<flavor>); the loader will "
                 "not scan it" % (label, t))
    if staged_id:
        wrong = [x for x in cache if not x.startswith("cache/%s/" % staged_id)]
        if wrong:
            fail("cache-one-tag",
                 "%s: %d cache entries are not under cache/%s/: %s"
                 % (label, len(wrong), staged_id, wrong[:3]))
    if dlls:
        print("  cache: %d DLL(s) under exactly %d tag %s"
              % (len(dlls), len(tags), sorted(tags)))

    # Producer-side and memo files must never ship. .abi_<tag>.ok is the worst
    # of them: it records "this tag was swept" and shipping it SUPPRESSES the
    # runtime's ABI preflight, the check that rejects stale shards on first
    # launch.
    for pat, why in (
        (r"\.abi_[0-9a-fA-F]+\.ok$",
         "a completed-sweep memo; shipping it suppresses the runtime's ABI "
         "preflight, which is what rejects stale shards on first launch"),
        (r"\.c$", "a patched-C intermediate, not a shard"),
        (r"\.pair-lock$", "producer-side bookkeeping"),
    ):
        bad = [x for x in cache if re.search(pat, x)]
        if bad:
            fail("cache-no-producer-files",
                 "%s: %d cache entr%s match %s (%s): %s"
                 % (label, len(bad), "y" if len(bad) == 1 else "ies", pat, why,
                    bad[:3]))

    # Zero-length shards load as nothing.
    empty = [x for x in dlls if tree.sizes.get(x, 0) == 0]
    if empty:
        fail("cache-non-empty", "%s: %d zero-byte shard(s): %s"
             % (label, len(empty), empty[:3]))


def check_artifact_catalog(tree, repo):
    """The mod catalog, counted the way the AppImage layout test counts it:
    manifest.toml files, not .json. An earlier check looked for *.json and
    found zero, which is indistinguishable from a catalog that did not ship."""
    manifests = [x for x in tree.names
                 if x.startswith("mods/") and x.endswith("manifest.toml")]
    print("  mods: %d manifest.toml" % len(manifests))
    if not manifests:
        fail("mod-catalog", "%s: no mods/**/manifest.toml staged" % tree.label)
    expected = None
    conf = os.path.join(repo, "packaging", "release", "app.conf")
    if os.path.isfile(conf):
        with open(conf, "r", encoding="utf-8", errors="replace") as fh:
            m = re.search(r"^EXPECTED_MODS=(\d+)", fh.read(), re.M)
        if m:
            expected = int(m.group(1))
    if expected is not None and len(manifests) != expected:
        fail("mod-catalog",
             "%s: packaging/release/app.conf declares EXPECTED_MODS=%d but the "
             "package carries %d manifest.toml. The Linux layout test asserts "
             "the same number, so the two platforms would ship different "
             "catalogs." % (tree.label, expected, len(manifests)))
    # Machine-local state must never ride along.
    for bad in ("mods/state.toml", "mods/state.toml.tmp"):
        if bad in tree.names:
            fail("mod-catalog", "%s: %s is this machine's own mod selection and "
                                "must not ship" % (tree.label, bad))
    if [x for x in tree.names if x.startswith("mods/installed/")]:
        fail("mod-catalog", "%s: mods/installed/ holds locally installed "
                            ".psxmod archives and must not ship" % tree.label)


def check_artifact_hygiene(tree):
    """Files that must never reach a player."""
    banned = [
        (r"(?i)^overlay_captures.*\.json$",
         "capture stores contain snapshots of game code read from a disc"),
        (r"(?i)SCPH\d+\.BIN$", "a retail BIOS dump"),
        (r"(?i)^settings\.toml$", "the builder's own persisted settings"),
        (r"(?i)^keybinds\.ini$", "the builder's own key bindings"),
        (r"(?i)\.(cue|iso|mcd|mcr|pst)$", "disc images and save data"),
    ]
    for pat, why in banned:
        bad = [x for x in tree.names if re.search(pat, x.split("/")[-1])
               or re.search(pat, x)]
        if bad:
            fail("hygiene", "%s: %d entr%s must never ship (%s): %s"
                 % (tree.label, len(bad), "y" if len(bad) == 1 else "ies", why,
                    bad[:4]))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    repo = os.path.abspath(argv[1])
    fw = os.path.abspath(argv[2])
    print("repo=%s" % repo)
    print("psxrecomp=%s" % fw)

    check_module(fw)
    check_contract(repo, fw)

    dev_toml = os.path.join(repo, "game.toml")
    dev_declared = os.path.isfile(dev_toml) and declares_overlay_cache(dev_toml)
    dev_id = game_id(dev_toml) if os.path.isfile(dev_toml) else None
    print("game.toml: id=%s overlay_cache=%s" % (dev_id, dev_declared))

    trees = discover(repo)
    if not trees:
        print("ARTIFACT half SKIPPED: no staged release or *-windows-x64.zip "
              "found. Producing one needs the game's disc image and a full "
              "Release build, so it cannot run in a bare checkout. Set "
              "PSX_RELEASE_STAGE or PSX_RELEASE_ZIP to check a real package.")
    for tree in trees:
        check_artifact(tree, dev_declared, dev_id)
        check_artifact_catalog(tree, repo)
        check_artifact_hygiene(tree)

    for n in NOTES:
        print("note: %s" % n)
    if FAILURES:
        print("")
        print("FAILED %d check(s):" % len(FAILURES))
        for f in FAILURES:
            print("  - %s" % f)
        return 1
    print("")
    print("OK: overlay staging contract%s satisfied"
          % ("" if not trees else " + %d artifact(s)" % len(trees)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
