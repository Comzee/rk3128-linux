# Crypto v1 and RNG: accepted subset and experimental paths

RK3128's crypto-v1 block is at **0x100fc000**, size0x4000, SPI40 (GIC ID72), reset53. The measured clock pair is148.5/74.25 MHz. It has AES/DES/EDE and MD5/SHA1/SHA256 datapaths plus an oscillator RNG. This collection preserves both working guarded code and diagnostic paths because the failures matter to anyone developing the hardware support.

## Conservative reference profile

The reference profile is **hash DMA + hwrng + named low-priority PIO ciphers** (`CONFIG_CRYPTO_DEV_RK3128_HASH_PIO`). The compile-time guard refuses native cipher DMA, including diagnostic attempts through the guarded driver. MD5/SHA1/SHA256 DMA algorithms use priority400; the seven AES/DES/EDE PIO wrappers remain priority1 below CPU implementations. This is an engineering admission boundary, not a recommendation to use obsolete MD5/SHA1/DES for new protocols.

Small messages use synchronous software below measured cutoffs:4 KiB for AES/DES/SHA256,16 KiB for MD5/SHA1,1 KiB for EDE. Streaming hashes also have software fallback. Both clocks remain on for the binding lifetime in the conservative profile. The standalone hwrng-only configuration remains a separate alternative; do not bind two drivers to the same block concurrently.

The guarded path completed **1,024 ordered transition pairs**, **256 concurrent cases**, and fresh full kernel self-tests for all ten registered algorithms after RNG traffic. A finite plain HEVC check recorded **1,044 frames**, zero drops/unshown frames/VOP errors. Artificial multi-source stress had nonzero drops in both guarded and CPU-baseline runs of unequal lengths; those results do not prove causal performance improvement.

## Native cipher DMA remains experimental

Earlier built-in tests passed native AES ECB/CBC/CTR, DES/EDE ECB/CBC and the hashes, including fragmented/in-place/tail cases. Those results did **not** establish transition safety: a later-loaded AES19216 KiB case still stalled with RNG disabled. Early versus late initialization was investigated and did not supply a complete explanation. Full/module profiles therefore expose experiment code, not accepted default acceleration.

The working DMA recipe uses distinct private coherent buffers in validated upper DDR, with no per-request BLOCK_FLUSH or ECB-IV write. Lower-DDR/aliased cipher buffers are refused. Native PIO CBC/CTR mode selection produced ECB; wrappers must implement their documented construction instead of claiming that the native mode bit works.

The shared RNG/cipher/hash block requires one owner and ordered clearing of retained state. A cipher mode that passes once after boot can fail after another engine has run. Use independent software references and ordered-pair/concurrent matrices; throughput-only testing misses that class of failure.

## RNG quality and shared power state

The oscillator profile uses period1000 and quality128/1024, with startup/continuous repetition-count, adaptive-proportion and repeated-output tests. A failure latches it off. Finite1 MiB captures gave approximate raw estimates0.744–0.782 bits/bit; the engineering design floor was0.5 with conservative credited quality reduced further. These are finite engineering estimates, **not entropy certification**.

The drivers check the always-on PMU before accessing potentially gated crypto MMIO, recover only the retained idle-request bit and refuse boundedly if the port stays idle. Random register reads into a clock-gated block can hang the SoC. PKA remains unsupported/refused; incorrect results are not registered as a working API. The legacy hash PRNG is not registered either.

## Runtime power-down is diagnostic, default off

The `zz-761` follow-up comes after764 and retains the diagnostic-only runtime-PM work. Hash/RNG/PIO cycles and at least **32,768 real API resume callbacks** passed in bounded tests, but a reboot from the suspended trial failed to recover over Wi-Fi. Therefore the conservative profile keeps clocks enabled and does not claim accepted runtime power-down. Successful resume traffic and a working system shutdown/reboot are separate requirements.

## Tools

`crypto-transition-test.c`, `crypto-transition-matrix.c` and `crypto-kernel-test.c` are explicit kernel-API comparison modules. `crypto-test.c` uses AF_ALG; upstream AF_ALG excludes some asynchronous algorithms, so AF_ALG absence alone does not show driver absence. `crypto-vectors.py` builds independent expected values; the RNG health/entropy tools analyze caller-supplied finite samples. No captured random data, keys or machine state is included.

Build module probes against the exact kernel configuration; they are not ordinary userspace C programs. The MMIO-oriented `crypto-probe.c` is an experimental diagnostic and must not be used while another driver owns the block. No source in this collection makes raw cipher DMA or power-down a safe general-purpose default.
