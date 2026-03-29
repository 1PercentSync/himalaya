# Real-time path tracing in games has arrived — here's how it works

**Full path tracing — where a single unified algorithm simulates all light transport — has crossed the threshold from tech demo to shipping AAA games.** As of early 2026, roughly 15 titles run fully path-traced renderers, powered by a convergence of dedicated ray tracing hardware, AI-driven denoising and upscaling, and breakthrough sampling algorithms like ReSTIR. The technology still demands high-end GPUs and relies heavily on neural reconstruction to fill in what brute-force computation cannot yet deliver, but the trajectory is unmistakable: path tracing is replacing hand-tuned rasterization pipelines one lighting effect at a time. NVIDIA leads the ecosystem with its integrated RTX stack, though AMD and Intel are closing hardware gaps, and cross-vendor API standardization through DXR 1.2 and Vulkan extensions is broadening the foundation.

---

## The hardware arms race: RT cores, BVH engines, and cluster acceleration

Real-time path tracing is made possible by **fixed-function ray tracing units** built into modern GPUs that offload the two most expensive operations — BVH traversal and ray-triangle intersection — from general-purpose shader cores. Without this dedicated silicon, path tracing at even one sample per pixel would consume all available compute, leaving nothing for shading.

**NVIDIA's Blackwell architecture (RTX 50 series, January 2025)** represents the most aggressive push. Its 4th-generation RT cores replace the per-triangle intersection engine with a **Triangle Cluster Intersection Engine** that processes groups of ~128–256 triangles as atomic units, paired with on-chip cluster decompression hardware. This design directly enables RTX Mega Geometry — a system that treats clusters of triangles as first-class BVH primitives through Cluster-Level Acceleration Structures (CLAS) and Partitioned Top-Level Acceleration Structures (PTLAS). The result is up to **100× more ray-traced triangles** per scene and **2× raw ray-triangle intersection throughput** over Ada Lovelace. The RTX 5090 delivers **380 RT TFLOPS** with 170 RT cores, 32 GB GDDR7, and hardware-accelerated Linear Swept Spheres for hair and fur rendering at 2× the speed of prior methods.

**AMD's RDNA 4 (Radeon RX 9070 series, March 2025)** doubles ray tracing performance over RDNA 3 through a **widened 8-wide BVH traversal structure** (up from 4-wide), doubled intersection engines, and new hardware instance transforms that were previously shader-based. Dynamic register allocation — releasing registers during traversal and reclaiming them during shading — improves wave occupancy for divergent path tracing workloads. RDNA 4 also introduces 2nd-generation AI accelerators with FP8 matrix operations that power FSR 4, AMD's first ML-based upscaler. However, AMD still lacks hardware equivalents to NVIDIA's Opacity Micromaps, Shader Execution Reordering, and Mega Geometry, and offers no high-end GPU competing with the RTX 5090.

**Intel's Battlemage (Arc B580, December 2024)** occupies the budget segment with 2nd-generation Ray Tracing Units featuring 3 traversal pipelines and doubled triangle intersection units per RTU, plus 16 KB dedicated BVH caches. The B580 processed **467.9 million rays per second** in Cyberpunk 2077's path tracing mode and showed a remarkable **90% performance uplift from SER** in DXR 1.2 demos — the largest gain of any architecture, reflecting Battlemage's sensitivity to thread divergence.

On consoles, the **PS5 Pro (November 2024)** delivers roughly **2× the ray tracing performance** of the base PS5 with RDNA 3-adjacent hardware, but full path tracing remains impractical. Mark Cerny acknowledged it would be "a little tricky" but didn't rule it out. Microsoft's next-generation Xbox ("Project Helix") promises "an order of magnitude increase in ray tracing performance, up to and including path tracing," targeting approximately 2028.

---

## Denoising: the critical bridge between one sample and a clean image

A path tracer at one sample per pixel produces extreme noise. The entire viability of real-time path tracing hinges on reconstruction algorithms that turn this statistical noise into coherent images within a millisecond budget.

**NVIDIA's DLSS Ray Reconstruction** is the current gold standard. Introduced with DLSS 3.5 in 2023 and upgraded to a **vision transformer model in DLSS 4 (January 2025)**, it replaces the traditional multi-stage denoising pipeline — separate passes for reflections, GI, AO, and shadows — with a single neural network inference pass. The model ingests noisy ray-traced color, motion vectors, depth, normals, roughness, and specular hit distances, then outputs a clean, temporally stable image. By seeing full lighting context rather than isolated effects, Ray Reconstruction preserves high-frequency detail that traditional denoisers destroy. DLSS 4.5 (CES 2026) introduced a second-generation transformer that Digital Foundry found could reconstruct ray-traced reflections nearly perfectly even without Ray Reconstruction enabled, hinting at an eventual convergence of denoising and upscaling into a unified model. Over **175 games** support DLSS 4, with Ray Reconstruction integrated into every major path-traced title.

**NVIDIA's Real-Time Denoisers (NRD)** library provides the hardware-agnostic alternative, deployed in 15+ AAA titles. Its three denoisers — **ReBLUR** (blur-based, handles AO/SO alongside radiance), **ReLAX** (optimized for RTXDI signals), and **SIGMA** (shadow-specific) — operate as spatiotemporal bilateral filters guided by per-pixel G-buffer data. NRD's Spherical Harmonics mode achieves quality reportedly comparable to DLSS Ray Reconstruction without requiring Tensor Cores, making it the primary option for cross-platform titles.

**AMD's FSR Ray Regeneration**, launched in December 2025 as part of the Redstone suite, is AMD's neural denoiser equivalent. It processes separate diffuse, specular, normals/depth, and radiance channels through a neural network, but as of early 2026 supports only one game (Call of Duty: Black Ops 7) and is considered slightly less polished than NVIDIA's more mature solution.

The foundational algorithm underpinning most denoisers is **SVGF (Spatiotemporal Variance-Guided Filtering)**, published in 2017. SVGF combines temporal reprojection (reusing samples from prior frames via motion vectors), variance estimation (tracking per-pixel noise levels over time), and à-trous wavelet spatial filtering with edge-stopping functions based on depth, normal, and luminance similarity. Its successor **A-SVGF** improved history invalidation through moment buffers. NRD evolved directly from SVGF with approximately 50% better performance.

---

## Upscaling and frame generation turn 15 FPS into 120

Even with denoising, path tracing at native 4K is prohibitively expensive. The practical solution is a three-stage pipeline: render at low resolution, denoise, then AI-upscale to the target resolution. Frame generation then multiplies the apparent framerate.

**NVIDIA's DLSS 4 stack** is the most complete implementation. Super Resolution renders at roughly 25% of target pixel count (e.g., 960×540 for 4K) and reconstructs detail using a transformer model. Multi Frame Generation (RTX 50 exclusive) then creates **up to 3 additional frames** per rendered frame — a 4× framerate multiplier on top of the SR boost. DLSS 4.5 extends this to **6× frame generation** and introduces Dynamic Multi Frame Generation, which automatically adjusts the multiplier to maintain a target framerate. In practice, Cyberpunk 2077's Overdrive Mode achieves roughly **5× total performance multiplication** (SR + FG + RR combined) versus native 4K without DLSS. Resident Evil Requiem at 4K with path tracing and full DLSS 4 MFG runs at **280+ FPS on an RTX 5090**.

**AMD's FSR Redstone (December 2025)** responds with four ML-powered components: FSR Upscaling (neural SR, RDNA 4 exclusive), FSR Frame Generation (neural, 1:1 ratio only — no multi-frame generation yet), FSR Ray Regeneration (neural denoiser), and FSR Radiance Caching (neural radiance prediction from sparse samples). The critical limitation is that all ML features require RDNA 4 hardware; older AMD GPUs fall back to analytical algorithms.

**Intel's XeSS 3** matches NVIDIA's multi-frame generation capability with up to **3 generated frames per rendered frame** (3:1 ratio), though it lacks a dedicated ray tracing denoiser. XeSS 2.1 opened all features to non-Intel GPUs.

Frame generation introduces latency since interpolated frames don't reflect the latest input state. **NVIDIA Reflex 2 Frame Warp** addresses this by sampling the latest mouse input after rendering completes and warping the output frame to the new camera position, reducing latency by up to **75%** (e.g., 56 ms → 14 ms in THE FINALS). AMD Anti-Lag 2 and Intel XeLL provide simpler driver-level solutions.

---

## ReSTIR and the algorithmic revolution in light sampling

The mathematical backbone of modern real-time path tracing is the **ReSTIR (Reservoir-based Spatio-Temporal Importance Resampling)** family of algorithms, which dramatically reduce the number of rays needed for low-noise results.

**ReSTIR DI** (Bitterli et al., 2020) maintains compact "reservoir" data structures that store weighted candidate light samples per pixel, then shares samples spatially (between neighboring pixels) and temporally (from prior frames) through weighted reservoir sampling. This approximates having traced orders of magnitude more shadow rays than actually computed, enabling scenes with **millions of light sources** — like Cyberpunk 2077's Night City — to be lit with just a few rays per pixel. NVIDIA's RTXDI SDK provides the production implementation.

**ReSTIR GI** (2021) extends this principle to multi-bounce indirect lighting, achieving **9.3× to 166× MSE improvements** over standard 1-SPP path tracing at a cost of only ~0.9 ms at 1080p. **ReSTIR PT** (2022) generalizes further to entire light transport paths at any bounce depth using the GRIS (Generalized Resampled Importance Sampling) mathematical framework. ReSTIR PT is the current state of the art, deployed in NVIDIA's Zorah tech demo within the NvRTX branch of Unreal Engine 5.

The algorithm family continues expanding rapidly through academic research: **ReSTIR BDPT** (2024–2025) adds bidirectional path tracing for caustics, **Area ReSTIR** (SIGGRAPH 2024) improves antialiasing sample counts by up to 25×, and **ReSTIR PG** (SIGGRAPH Asia 2025) integrates path guiding for hard-to-reach lights.

Beyond ReSTIR, key techniques include **Next Event Estimation** (tracing shadow rays directly toward sampled lights at each path vertex), **Light BVH** hierarchies for importance-sampling among thousands of emissive triangles, **Multiple Importance Sampling** combining BRDF and light sampling, and **Russian roulette** for statistically unbiased early path termination. **Radiance caching** — through NVIDIA's **SHaRC** (Spatial Hash Radiance Cache) and **NRC** (Neural Radiance Cache) — allows paths to terminate after 2–3 bounces by reading cached radiance. NRC uses a small neural network trained at runtime that can terminate **96% of paths early**, with overhead of only ~2.6 ms at 1080p.

---

## APIs are standardizing around ray tracing as a first-class feature

**DirectX Raytracing 1.2** (announced GDC 2025) represents the most significant API advance, standardizing two features previously available only on NVIDIA hardware. **Shader Execution Reordering (SER)**, now mandatory in Shader Model 6.9, uses HitObject constructs to separate ray tracing from shading, enabling driver-level thread reordering that delivers **24–370% performance improvements** depending on the title and GPU. **Opacity Micromaps (OMM)** accelerate alpha-tested geometry tracing by up to 2.3×. DXR 1.2 also introduces **Cooperative Vectors** for hardware-accelerated neural network inference within shaders, enabling neural materials and textures. Microsoft announced further DXR extensions for summer 2026 including clustered geometry, partitioned TLAS, and indirect acceleration structure operations.

**Vulkan Ray Tracing** provides equivalent functionality through KHR extensions (acceleration_structure, ray_tracing_pipeline, ray_query), with recent additions including **VK_EXT_ray_tracing_invocation_reorder** (cross-vendor SER, announced GDC 2025) showing a 47.78% improvement in path tracing benchmarks, and **VK_EXT_opacity_micromap**. Vulkan remains critical for id Tech games (Indiana Jones, DOOM: The Dark Ages use Vulkan exclusively) and Linux/SteamOS.

**Metal Ray Tracing** gained hardware acceleration with Apple's M3 chip (2023) and doubled RT performance with M4 (2024), but Apple Silicon targets hybrid RT effects in ported AAA titles rather than full path tracing.

---

## Fifteen games and counting now ship with full path tracing

The roster of fully path-traced games has grown from tech demos to mainstream AAA releases:

- **Quake II RTX (2019)** and **Minecraft with RTX (2020)** proved the concept with simple geometry
- **Portal with RTX (2022)** showcased NVIDIA's RTX Remix remastering platform
- **Cyberpunk 2077 RT Overdrive (2023)** remains the benchmark — a full open-world path tracer using ReSTIR DI/GI, SHaRC, and DLSS Ray Reconstruction to handle Night City's vast light complexity
- **Alan Wake 2 (2023)** on Remedy's Northlight engine combined path-traced indirect lighting with the first deployment of RTX Mega Geometry
- **Black Myth: Wukong (2024)** achieved a world-first with two-level ray tracing for order-independent transparencies, rendering particles in reflections
- **Indiana Jones and the Great Circle (2024)** on id Tech 7 became the first AAA title to **require** RT hardware as a minimum spec
- **DOOM: The Dark Ages (2025)** on id Tech 8 mandated RT with no fallback, adding path tracing via a post-launch update using SHaRC radiance caching
- **Resident Evil Requiem (February 2026)** is 2026's showcase, hitting **280+ FPS on RTX 5090** with DLSS 4 MFG and path tracing

Upcoming titles include PRAGMATA (April 2026), 007 First Light (May 2026), CONTROL Resonant, and The Witcher IV — the latter using RTX Mega Geometry's new foliage system for dense path-traced environments. The RTX Remix community is also remastering classics including Half-Life 2, Quake III Arena, and Mirror's Edge with full path tracing.

---

## Game engines are building path tracing into their core pipelines

**Unreal Engine 5** offers the most layered approach. Lumen, UE5's default GI system, uses signed distance field ray marching with a surface cache for approximate indirect lighting — not true path tracing, but fast enough for broad hardware. Enabling Lumen's hardware RT mode replaces SDF tracing with BVH-based intersection against actual geometry while still reading from the surface cache. UE5 also includes a separate dedicated path tracer for reference-quality offline rendering. The most advanced implementation lives in NVIDIA's **NvRTX branch**, which integrates ReSTIR PT, RTXDI, RTX Mega Geometry, NRC, and SHaRC into UE5 — enabling real-time path tracing of **500-million-triangle scenes** as demonstrated in the Zorah tech demo. Epic's own **Megalights** system (UE5.5) adds stochastic direct lighting for vastly more dynamic shadow-casting area lights.

**Unity's HDRP** supports hybrid RT effects and a progressive path tracer, but targets architectural visualization rather than real-time gameplay. Its path tracer resets accumulation on camera movement and lacks the temporal game-specific infrastructure of NRD or DLSS Ray Reconstruction.

**id Tech 7/8** has emerged as the most production-proven path tracing engine after REDengine, with Indiana Jones and DOOM: The Dark Ages both shipping mandatory RT. id Software's Billy Khan stated that "path tracing is now part of id Tech," noting the team saved **years of development** by using RT lighting instead of baked lightmaps.

**Remedy's Northlight** (Alan Wake 2, FBC: Firebreak) combines a GPU-driven mesh shader pipeline with path-traced indirect lighting and was the first engine to ship with RTX Mega Geometry support. **CD Projekt RED's REDengine** powered Cyberpunk's path tracing implementation but the studio has transitioned to UE5 for The Witcher IV.

Open-source frameworks including **NVIDIA Falcor** (research-oriented DXR path tracer), **RTXPT** (production-ready C++/HLSL path tracer with full DLSS 4 and ReSTIR integration), and **RTX Remix** (remastering platform for classic D3D8/D3D9 games, v1.0 released March 2025 with the first neural shader deployment via NRC) provide accessible entry points for developers.

---

## Where path tracing goes from here

The most consequential near-term trend is **neural rendering** — replacing expensive traditional computations with small neural networks running on Tensor Cores. NVIDIA's RTX Kit bundles Neural Texture Compression (7× VRAM savings), Neural Materials (5× faster material processing), and Neural Radiance Cache (terminates 96% of paths early) into a unified suite. **Cooperative Vectors** in DXR 1.2 / SM 6.9 standardize neural inference within shaders across the industry, making these techniques available beyond NVIDIA's ecosystem. Intel demonstrated a **10× speedup** for neural block texture compression using Cooperative Vectors.

**RTX Mega Geometry** solves the fundamental incompatibility between modern LOD systems like Nanite and ray tracing. By processing triangle clusters rather than individual primitives, it reduces BVH overhead by ~100× and enables path tracing of film-quality geometry. A new foliage system announced at GDC 2026 in partnership with CD Projekt RED will extend this to dense vegetated environments for The Witcher IV.

The ReSTIR algorithm family continues its rapid evolution toward a complete, unified path resampling framework. **ReSTIR BDPT** adds caustics support, **Reservoir Splatting** (SIGGRAPH 2025) enables motion blur through forward-projected path reuse, and **ReSTIR PG** integrates path guiding for scenes with hard-to-reach light paths. Each advance reduces the samples needed per pixel while maintaining physical accuracy.

**Full rasterization replacement remains distant but accelerating.** id Tech 8 mandating RT hardware signals the beginning of the end for non-RT fallback paths, but most studios still see 50%+ performance loss with full RT enabled. The consensus trajectory is clear: fewer traced rays combined with better AI reconstruction will close the gap faster than raw hardware improvements alone. NVIDIA claims a **10,000× path tracing performance improvement** from Pascal to Blackwell and projects 1,000,000× improvement with Rubin (2027–2028). Console path tracing will likely arrive with the next generation around 2028, when AMD's RT hardware and ML upscaling mature sufficiently. Until then, real-time path tracing remains overwhelmingly a high-end PC phenomenon — but one that is rapidly becoming the definitive way to light a virtual world.

## Conclusion

Real-time path tracing in 2025–2026 is no longer a question of "if" but "when it becomes standard." The technology rests on four pillars working in concert: **dedicated RT hardware** (NVIDIA's 4th-gen RT cores lead with cluster-based acceleration, while AMD's RDNA 4 doubles previous-gen throughput), **ReSTIR-family sampling algorithms** (enabling physically accurate lighting from just one ray per pixel), **AI-driven reconstruction** (DLSS Ray Reconstruction's transformer model collapses multi-stage denoising into a single inference pass), and **neural frame multiplication** (DLSS 4's multi-frame generation turns 30 FPS base rates into 120+ FPS output). The most underappreciated development is the standardization of previously vendor-specific features — SER and OMMs in DXR 1.2, Cooperative Vectors for neural shading — which will democratize path tracing beyond NVIDIA's ecosystem. The next frontier is neural rendering: replacing not just denoising but materials, textures, and radiance computation itself with learned models, collapsing the traditional graphics pipeline into an increasingly AI-driven reconstruction from sparse physical simulation.