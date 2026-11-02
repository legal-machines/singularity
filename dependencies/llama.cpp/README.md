# llama.cpp for local inference, and its two patches

## Summary

Local inference runs a local model in `llama-server`, a process Singularity
starts and talks to over loopback HTTP; Singularity's own code never links
llama.cpp. The build (`deploy/backend/cmake/LocalInferenceRuntime.cmake`)
downloads the official MIT-licensed llama.cpp `b10516` source archive (upstream
tag commit `b95502b`), checks it against its pinned SHA-256, applies the two
patches in this folder with GNU patch, and builds `llama-server` and the
libraries it loads as a separate Release build.

## The patches

- `b10516-moe-expert-cache.patch`: expert residency for mixture-of-experts models.
  Routed experts are read from the model file into a bounded set of slots,
  kept by least recent use, with cache, load and eviction telemetry, prefill
  and decode accounting, safe-point contraction of the cache, and a CPU/Metal
  provider that runs resident experts on Metal and the rest on ggml's CPU
  thread pool, merging exact rows before the graph continues. It is supported
  in production on Apple silicon with Metal; other platforms keep llama.cpp's
  stock behaviour and report dynamic expert residency as unsupported.
- `b10516-moe-route-trace.patch`: the offloader's measured record of
  what it read: requested and read bytes, the peak of transient reads in
  flight, and a per-decision route trace that local inference reads back for
  its telemetry.

The initial Metal routed-expert interception and compact shared-buffer pool
derive from the MIT-licensed experimental llama.cpp fork at
`https://github.com/kisasexypantera94/llama.cpp.git`, commits
`d1607595e47381d889c28cace0386b31a1c3cb89` through
`5e9ddf13f59e3a98e34536df071fde6c5971d431`. Legal Machines rebased that work
onto `b10516`, fixed its correctness and load-boundary defects, and wrote the
rest independently. The licence and notice that ship with the application are
`deploy/backend/licenses/llama.cpp/LICENSE` and `NOTICE`.

To check a patch by hand, extract the `b10516` source archive and run
`patch -p1 --forward --input <absolute path to the patch>` in it.
