# Implementing GTAO: algorithm, pitfalls, and production tricks

**Ground Truth Ambient Occlusion (GTAO) achieves near-ray-traced AO quality at a fraction of the cost by exploiting a key insight: the depth buffer is a heightfield, so visibility within any hemisphere slice reduces to two horizon angles whose cosine-weighted integral has a closed-form solution.** Introduced by Jimenez, Wu, Pesce, and Jarabo at SIGGRAPH 2016 ("Practical Realtime Strategies for Accurate Indirect Occlusion," Activision technical memo ATVI-TR-16-01), GTAO replaced ad-hoc attenuation functions with radiometrically correct integration and shipped in *Call of Duty* at **0.5 ms on PS4** at 1080p. It has since become the default AO method in Unreal Engine (since 4.24), Unity HDRP, and numerous open-source engines. Intel's open-source **XeGTAO** implementation (MIT license) is the most thorough publicly available reference, while the 2023 Visibility Bitmask extension addresses GTAO's chief remaining limitation — thin-object halos.

---

## How GTAO differs from SSAO, HBAO, and HBAO+

All screen-space AO techniques estimate a visibility integral over the hemisphere above each surface point, but they differ fundamentally in how they sample, weight, and solve that integral.

**Crytek's SSAO (2007)** scatters random sample points inside a sphere (or hemisphere) around each pixel and counts how many fall behind geometry in the depth buffer. It has no physical basis — the weighting is uniform, samples are unstructured, and flat surfaces self-occlude because roughly half the sphere penetrates the surface. It requires many samples to converge and produces characteristic noise.

**HBAO (Bavoil et al., 2008)** introduced the horizon-based approach: for each azimuthal direction φ, ray-march the depth buffer to find the maximum elevation angle (horizon angle), then integrate the unoccluded solid angle. This was a major step toward physical correctness, but HBAO uses **uniform weighting** — it computes the unoccluded fraction of the hemisphere without foreshortening — and applies a continuous **attenuation function** (ambient obscurance) rather than binary visibility. Both choices are ad hoc and don't match the rendering equation. **HBAO+** by NVIDIA optimized HBAO with interleaved rendering and cache-friendly memory access patterns, achieving roughly 3× the speed without changing the mathematical formulation.

GTAO makes two decisive changes. First, it replaces uniform weighting with a **cosine-weighted integral** that matches the actual AO definition derived from the rendering equation. Second, it solves the inner integral (over elevation angle within each slice) **analytically** using a closed-form expression, rather than numerically. The outer integral over azimuthal directions remains Monte Carlo but converges fast because each slice captures maximum information. The result matches ray-traced ground truth under the heightfield assumption — hence the name.

| Feature | SSAO | HBAO/HBAO+ | GTAO |
|---|---|---|---|
| Sampling method | Random hemisphere points | Directional ray-march | Directional horizon search |
| Inner integral | Numerical (all Monte Carlo) | Numerical or LUT | **Analytical closed-form** |
| Cosine weighting | No | No (uniform) | **Yes** |
| Visibility model | Point occlusion test | Continuous attenuation | **Binary (physically correct)** |
| Reference frame | Surface normal | Surface tangent | **View vector** |
| Multi-bounce handling | None | Ad-hoc attenuation | **Data-driven polynomial** |

---

## The mathematical core: from rendering equation to two horizon angles

Starting from the rendering equation under the assumptions of uniform infinite environment lighting, single-bounce illumination, and Lambertian surfaces, the ambient occlusion term reduces to:

**A(x) = (1/π) · ∫ V(x, ωᵢ) · ⟨nₓ, ωᵢ⟩⁺ dωᵢ**

integrated over the hemisphere H² above point x. V is binary visibility (1 = unoccluded within radius r, 0 = occluded), and the cosine term ⟨nₓ, ωᵢ⟩⁺ provides physically correct foreshortening.

GTAO reformulates this in polar coordinates. The hemisphere is sliced by azimuthal angle φ ∈ [0, π), and within each slice, the elevation θ is measured **from the view vector** (following Timonen 2013), not the surface normal. Because the depth buffer is a heightfield, occlusion within a slice cannot interleave — once geometry rises above the horizon line, everything behind it is occluded. This means **all visibility information in a slice is captured by just two maximum horizon angles**, θ₁(φ) and θ₂(φ), found by searching opposite directions along the slice.

The surface normal nₓ is projected onto each slice's 2D plane (spanned by the slice direction and view vector). The projected normal's angle γ relative to the view vector and its length (which compensates for foreshortening of the projection) define the integration bounds. The key formula — the inner integral for one slice — has an exact closed-form solution:

**â(h, γ) = ¼ · (−cos(2h − γ) + cos(γ) + 2h · sin(γ))**

The total visibility for a slice is `projectedNormalLength × (â(h₀, γ) + â(h₁, γ))`, requiring only **2 cos, 1 sin, and 3 acos** operations after optimization. This makes the shader almost entirely **memory-bound** — ALU is essentially free, a crucial property for GPU efficiency. The outer integral is approximated by averaging over m slice directions (typically 1–3 per pixel), with spatial and temporal filtering providing the equivalent of dozens more.

---

## Step-by-step implementation pipeline

A production GTAO implementation, exemplified by Intel's XeGTAO, consists of three to four compute shader passes.

**Pass 0 — Depth prefiltering.** Convert hardware depth to linear view-space depth and build a **5-level MIP hierarchy**. The MIP filter uses a weighted average biased toward the farthest sample (not a simple min or average), which preserves thin-occluder information better than rotated-grid subsampling. Distant samples along a ray-march direction will read coarser MIPs, dramatically improving texture cache efficiency for large radii. Store as R16F (or R32F if 16-bit precision is insufficient for the scene's depth range).

**Pass 1 — Normal generation (optional).** If a G-buffer with screen-space normals is available, use it directly — this always produces better results than depth-reconstructed normals. When normals must be reconstructed from depth, XeGTAO computes cross products from four neighboring depth-derived positions (left, right, top, bottom), weighting by edge quality to avoid crossing depth discontinuities. This edge-aware reconstruction requires 32-bit depth; **16-bit depth visibly degrades reconstructed normal quality**.

**Pass 2 — Main GTAO computation.** For each pixel, the shader:

1. Reads view-space depth and normal, computes view-space position and view vector.
2. Computes screen-space radius from the world-space effect radius (`screenRadius = effectRadius / viewspacePixelSize`).
3. For each slice direction (rotated by per-pixel noise from the Hilbert-R2 sequence or interleaved gradient noise):
   - Projects the normal onto the slice plane to get angle γ and weighting factor `projectedNormalLength`.
   - Initializes horizon cosines to the tangent plane limits: `cos(γ ± π/2)`.
   - For each step along the direction (distributed by a power curve, typically quadratic, pushing more samples near the center):
     - Computes sample offset with sub-pixel jitter from the R1 sequence.
     - Selects MIP level: `mipLevel = clamp(log2(offsetLength) − mipSamplingOffset, 0, 5)`.
     - Samples depth on **both sides** of the pixel (positive and negative directions).
     - Computes horizon cosine: `shc = dot(normalize(samplePos − centerPos), viewVec)`.
     - Applies distance falloff weight: `weight = saturate(dist × falloffMul + falloffAdd)` and lerps the horizon cosine toward the low-horizon default when outside the effect radius.
     - Updates running maximum: `horizonCos = max(horizonCos, shc)`.
   - Converts final horizon cosines to angles via `acos`, computes the closed-form integral for both sides, accumulates weighted visibility.
4. Averages visibility over all slices, applies an optional power curve for artistic control, outputs AO (and optionally bent normals packed into RGBA8).

**Pass 3 — Spatial denoising.** A **5×5 edge-aware bilateral blur** uses pre-computed edge weights (depth differences between neighboring pixels, packed into 2 bits per direction in an R8 texture). Edge weights are enforced symmetrically — a pixel's edge weight toward its neighbor is multiplied by the neighbor's reciprocal edge — producing stable, sharp blur boundaries. A small amount of intentional leaking at high-edge-count pixels (3–4 boundaries) reduces aliasing. The denoiser can be applied 1–3 times (sharp/medium/soft presets).

**Temporal integration** is typically handled by the engine's TAA rather than a dedicated pass. XeGTAO varies the noise offset each frame (`NoiseIndex = frameIndex % 64`), and TAA implicitly averages across frames. The original Activision implementation used aggressive dedicated temporal reprojection to accumulate **96 effective directions** (6 frames × 16 post-blur directions) from just 1 direction per pixel per frame.

---

## Sampling configurations and performance characteristics

The number of slices and steps per slice directly controls quality and cost. XeGTAO provides three presets:

| Preset | Slices | Steps/slice | Total samples | Cost (1080p, RTX 2060) |
|---|---|---|---|---|
| Low | 2 | 4 | 8 | ~0.25 ms |
| Medium | 2 | 6 | 12 | ~0.37 ms |
| High | 3 | 6 | 18 | ~0.56 ms |

At 4K on an RTX 3070, the High preset costs approximately **1.4 ms**. Adding bent normals output increases cost by roughly **25%**. The original Activision implementation achieved 0.5 ms on PS4 at 1080p by running at half resolution with 1 slice and 12 steps, relying heavily on temporal accumulation.

The sample distribution along each direction uses a **quadratic power curve** (`s = (linearStep)²`), clustering more samples near the center pixel where thin crevice detail matters most. XeGTAO's auto-tune system — a built-in ray tracer generating 512-spp ground truth — searched parameter space and found the optimal distribution power to be approximately **2.0**, with the Hilbert-R2 noise pattern providing the best spatial-temporal discrepancy properties.

---

## Common pitfalls and how to solve them

**Banding artifacts** arise from insufficient step count along each slice direction, causing quantized horizon angles. Increasing steps per slice reduces banding but costs performance. The primary mitigation is **noise-based dithering** — per-pixel jitter of both slice angle and step offset breaks coherent bands into high-frequency noise that spatial and temporal filters remove efficiently. XeGTAO's final noise approach uses a Hilbert curve index driving the R2 quasi-random sequence (after evaluating and rejecting 2D blue noise, 3D spatiotemporal blue noise, and simple hash functions). Interleaved gradient noise (`frac(52.9829189 × frac(dot(pixCoord, float2(0.06711056, 0.00583715))))`) remains popular in TAA-heavy pipelines because it is optimized for 3×3 neighborhood rejection.

**Thin-object halos** are arguably the hardest problem. The depth buffer treats every visible surface as infinitely thick behind it, so thin objects (poles, fences, foliage) cast unrealistically strong occlusion, producing dark halos. The original GTAO paper's thickness heuristic assumes object thickness is proportional to screen-space width, using an exponential moving average to attenuate horizon angles when subsequent samples recede. XeGTAO found that **increasing slice count while reducing steps per slice** achieves a similar effect more cheaply, and exposes a "thin occluder compensation" parameter (default 0 for speed, 0.7 for quality). The most robust modern solution is the **Visibility Bitmask** approach (Therrien et al., 2023): replacing two horizon angles with a 32-bit mask of occluded/unoccluded sectors, allowing light to pass behind surfaces of constant thickness. Bevy engine switched from GTAO to this approach in version 0.15.

**Temporal instability** — flickering and swimming when the camera moves — stems from low per-frame sample counts and the screen-space nature of sampling. Solutions include dedicated temporal reprojection with depth/normal-based rejection (Frictional Games' SOMA approach: blend factor weighted by both geometry distance and AO difference), or relying on TAA with carefully controlled per-frame noise variance. XeGTAO deliberately keeps temporal variance low enough that TAA doesn't mischaracterize noise as geometric features. Half-resolution rendering amplifies instability and should be paired with robust temporal filtering.

**Normal reconstruction errors** cause incorrect hemisphere orientation, leading to false self-occlusion on flat surfaces and wrong AO intensity on curved surfaces. Depth-derived normals are faceted on smooth geometry and unreliable at depth discontinuities. XeGTAO's edge-aware cross-product reconstruction helps, but **G-buffer normals are always preferable**. When using depth-reconstructed normals, 32-bit depth is essential — 16-bit "visibly degrades quality."

**Depth buffer precision** affects every stage. Non-linear hardware depth requires careful linearization (extract constants directly from the projection matrix). Using 8-bit or low-precision intermediate formats causes visible stepping in horizon angles. The depth MIP hierarchy's filter must bias toward far depth (not average) to avoid thin occluders disappearing at coarser levels. XeGTAO's falloff function interpolates toward the hemisphere horizon `cos(γ ± π/2)` rather than toward −1, making attenuation independent of the projected normal and preventing view-angle-dependent haloing.

**Over-occlusion in corners** is physically correct but often too dark due to the single-bounce assumption. The **multi-bounce approximation** from the original paper corrects this cheaply: a cubic polynomial `G(A, ρ) = a(ρ)·A³ − b(ρ)·A² + c(ρ)·A` maps single-bounce AO to multi-bounce AO as a function of surface albedo ρ, with linear coefficient functions (e.g., `a(ρ) = 2.0404ρ − 0.3324`). This recovers energy lost to missing interreflections and is essentially free to compute.

---

## Reference implementations across engines and open-source projects

**Intel XeGTAO** (github.com/GameTechDev/XeGTAO) is the gold-standard open-source reference. Three compute passes (PrefilterDepths → MainPass → Denoise) in self-contained HLSL files, MIT-licensed, with an auto-tune system for parameter optimization against ray-traced ground truth. It supports optional bent normals, FP16 math, and integrates easily into any DX12 codebase.

**Unreal Engine** exposes GTAO via `r.AmbientOcclusion.Method=1` (since UE 4.24). Key CVars include `r.GTAO.NumAngles` (default 2), `r.GTAO.Downsample`, `r.GTAO.TemporalFilter`, and `r.GTAO.ThicknessBlend`. The spatial filter was broken in UE 4.26–4.27 (fixed by setting `r.GTAO.SpatialFilter=0`); this was resolved in UE5. Performance is reported at roughly 3 ms at 1080p in default configuration — heavier than XeGTAO's approach but with higher default sample counts.

**Unity HDRP** includes a built-in GTAO implementation as its primary AO method. The HDRP code served as the basis for the Visibility Bitmask paper's reference implementation. Third-party options include **Amplify Occlusion** (open source, upgraded from HBAO to GTAO in v2), **MaxwellGengYF's Unity-Ground-Truth-Ambient-Occlusion** (340 GitHub stars, includes specular occlusion via bent-normal cone intersection), and **bladesero's GTAO_URP** for the Universal Render Pipeline.

**Bevy engine** (Rust/WGSL) implemented GTAO in version 0.11 (2023), citing XeGTAO as a primary reference, then upgraded to **Visibility Bitmask AO (VBAO)** in version 0.15 for better thin-geometry handling. **Godot** has an active proposal (issue #3223) to replace its ASSAO implementation with GTAO. **O3DE** (Open 3D Engine) developed a GTAO implementation for its Atom renderer with 5 quality levels, though without temporal denoising or thickness heuristics.

---

## Conclusion

GTAO's enduring value comes from finding the right mathematical abstraction: the heightfield assumption converts an intractable hemisphere integral into a small number of 1D horizon searches with an exact closed-form inner solution, hitting the sweet spot between physical accuracy and real-time budget. The most impactful implementation decisions are not in the core algorithm but in the supporting infrastructure — **noise pattern selection** (Hilbert-R2 or IGN over plain blue noise), **temporal integration strategy** (dedicated reprojection vs. TAA reliance), and **thin-object handling** (thickness heuristics or the newer visibility bitmask). For new implementations, starting from XeGTAO's codebase and adapting its 3-pass compute pipeline is the most efficient path. The Visibility Bitmask extension (2023) represents the clearest evolutionary step forward, replacing GTAO's two-angle representation with a bitmask that elegantly resolves the thin-object problem while enabling screen-space indirect lighting in the same framework.