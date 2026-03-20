# Implementing PCSS for directional lights

**Percentage Closer Soft Shadows (PCSS) produces physically-plausible contact-hardening shadows — hard at the base of a caster, softening with distance — but adapting the algorithm from point/spot lights to directional lights requires rethinking every formula.** The core challenge: directional lights have no finite position, so the classic similar-triangles derivation breaks down. The fix is surprisingly clean — remove the perspective division term and replace "light size" with angular diameter — but cascaded shadow maps, depth bias, and performance realities introduce a host of practical pitfalls. This guide covers the complete implementation path with shader pseudocode, every major pitfall, and production-proven solutions.

PCSS was introduced by Randima Fernando at NVIDIA in 2005 and remains the most widely deployed contact-hardening soft shadow technique in real-time rendering. Unity HDRP, Godot, Bevy, and numerous AAA titles (GTA V, Far Cry 4, Dying Light) ship it. The algorithm's elegance lies in its three-step structure that drops into any existing shadow mapping pipeline as a replacement for the shadow lookup function.

---

## The three-pass algorithm adapted for parallel light

PCSS replaces a standard shadow map lookup with three sequential steps executed per shaded fragment: a **blocker search**, **penumbra estimation**, and **variable-width PCF filtering**. For a directional (orthographic) light, each step simplifies relative to the perspective case.

### Step 1 — Blocker search

Sample the shadow map in a region around the current fragment's projected UV position. For each sample, compare the stored depth against the receiver's depth. Texels with stored depth less than the receiver depth are "blockers." Accumulate their depths and count.

**Critical difference for directional lights:** the search radius is **constant** in shadow map UV space. For a perspective light, the search radius shrinks with distance (`searchWidth = LIGHT_SIZE_UV * (zReceiver - NEAR) / zReceiver`) because of perspective foreshortening. An orthographic projection has no foreshortening, so the search radius is simply `LIGHT_SIZE_UV` — the light's projected size in UV coordinates.

### Step 2 — Penumbra estimation

Compute the average blocker depth from accumulated results. If no blockers were found, the fragment is fully lit (early-out, return 1.0). Otherwise, estimate penumbra width.

**The classic perspective formula** derives from similar triangles sharing a vertex at the light's position:

```
wPenumbra = (dReceiver − dBlocker) / dBlocker × wLight
```

**For directional lights**, the light is at infinity. Taking the limit as light distance approaches infinity, the `1/dBlocker` factor vanishes. The simplified formula becomes:

```
wPenumbra = (dReceiver − dBlocker) × LIGHT_SIZE_UV
```

This is the single most important adaptation. The division by `dBlocker` is removed because orthographic projection introduces no perspective scaling — a given world-space distance maps to the same UV-space distance regardless of depth.

### Step 3 — Variable-width PCF filtering

Apply standard percentage-closer filtering using the estimated `wPenumbra` as the kernel radius. This step is identical for all light types — only the radius input changes.

### Complete HLSL pseudocode

```hlsl
#define BLOCKER_SEARCH_SAMPLES 16
#define PCF_SAMPLES            25
// Light angular diameter → UV-space size
// LIGHT_SIZE_UV = tan(angularDiameter/2) * 2.0 / frustumWidth
uniform float  LIGHT_SIZE_UV;      // per cascade!
uniform float2 poissonDisk[25];    // pre-generated Poisson samples

float PCSS_DirectionalLight(
    Texture2D shadowMap, SamplerState pointSamp,
    SamplerComparisonState pcfSamp,
    float2 uv, float zReceiver)
{
    // ---- STEP 1: BLOCKER SEARCH ----
    float searchRadius = LIGHT_SIZE_UV;  // constant for ortho
    float blockerSum   = 0.0;
    float numBlockers  = 0.0;

    for (int i = 0; i < BLOCKER_SEARCH_SAMPLES; ++i)
    {
        float2 sampleUV = uv + poissonDisk[i] * searchRadius;
        float  smDepth  = shadowMap.SampleLevel(pointSamp, sampleUV, 0);
        if (smDepth < zReceiver)       // this texel is a blocker
        {
            blockerSum += smDepth;
            numBlockers += 1.0;
        }
    }
    if (numBlockers < 1.0)
        return 1.0;                    // fully lit — skip filtering

    float avgBlockerDepth = blockerSum / numBlockers;

    // ---- STEP 2: PENUMBRA ESTIMATION ----
    // NO division by avgBlockerDepth for directional lights
    float penumbraUV = (zReceiver - avgBlockerDepth) * LIGHT_SIZE_UV;

    // ---- STEP 3: VARIABLE PCF ----
    float shadow = 0.0;
    for (int j = 0; j < PCF_SAMPLES; ++j)
    {
        float2 offset = poissonDisk[j] * penumbraUV;
        shadow += shadowMap.SampleCmpLevelZero(
                      pcfSamp, uv + offset, zReceiver);
    }
    return shadow / PCF_SAMPLES;
}
```

Key implementation details: the blocker search **must** use point sampling to read raw depth values; the PCF pass **should** use hardware comparison sampling (`SampleCmpLevelZero`) which provides free bilinear interpolation of comparison results, effectively quadrupling sample density. This means **16 bilinear PCF taps evaluate 64 depth comparisons**.

---

## Defining "light size" when there is no light position

For point and spot lights, light size is a physical radius in world units. For directional lights, the parameter is the **angular diameter** of the light source — the angle subtended by the light disc as seen from any point in the scene. The sun, for instance, subtends approximately **0.53°** (0.00927 radians).

Production engines expose this differently. Unity HDRP uses an **"Angular Diameter"** property on the directional light. Godot exposes `light_angular_distance` in degrees. Bevy migrated from an abstract `soft_shadow_size` to a physically-based `angular_size` in radians. The NVIDIA integration whitepaper uses a pair of constants — `LIGHT_WORLD_SIZE` and `LIGHT_FRUSTUM_WIDTH` — whose ratio yields the UV-space light size:

```
LIGHT_SIZE_UV = LIGHT_WORLD_SIZE / LIGHT_FRUSTUM_WIDTH
```

Or equivalently, from angular diameter θ:

```
LIGHT_SIZE_UV = 2 × tan(θ/2) / FRUSTUM_WIDTH
```

**This value must be recomputed per cascade** when using cascaded shadow maps, because each cascade covers a different world-space frustum width while the light's angular diameter stays constant.

---

## Pitfalls that will break your implementation

### Shadow acne amplified by variable kernels

Standard shadow mapping already suffers from self-shadowing artifacts (shadow acne). PCSS makes this substantially worse. Larger penumbra estimates produce larger PCF kernels, which sample texels further from the center point — increasing the depth discrepancy between the stored depth and the comparison depth. The blocker search phase introduces its own bias problem, since it also performs depth comparisons to classify texels as blockers.

**The solution is a multi-pronged bias strategy.** Combine constant depth bias, slope-scaled bias (`tan(acos(dot(N,L)))`, clamped to a maximum of ~2.0), and normal offset bias (shift the shadow lookup position along the surface normal). The NVIDIA integration whitepaper recommends **receiver plane depth bias**: compute the shadow-space depth gradient `(dz/du, dz/dv)` from screen-space derivatives, then offset the comparison depth for each PCF sample proportionally to its UV distance from the kernel center. This elegantly scales bias with kernel size. Ignacio Castaño's shadow work on The Witness demonstrated that the combination of normal + slope-scale bias with per-cascade scaling provides the most robust results in practice.

### Cascade boundaries cause penumbra discontinuities

Cascaded shadow maps are the standard approach for directional light shadows, but they create the single most frustrating PCSS problem: **penumbra size jumps at cascade transitions**. Each cascade has a different frustum width, meaning `LIGHT_SIZE_UV` differs between cascades. The same world-space blocker-receiver gap maps to different UV distances in adjacent cascades, producing visibly different penumbra sizes at the boundary.

Three solutions work well together. First, **keep the PCF filter kernel at a fixed world-space size** across all cascades by scaling the kernel's texel width inversely with the cascade's texel-to-world ratio — this alone hides most seams. Second, **convert blocker depths to world space** before computing penumbra, rather than operating in per-cascade normalized depth space. This ensures the penumbra formula produces consistent results regardless of which cascade a fragment falls in. Third, **blend shadow results between cascades** at boundaries (Unity HDRP provides `UNITY_USE_CASCADE_BLENDING` with configurable blend distance). As Matt Pettineo (MJP) noted: "Matching the filter kernel for each cascade helps hide the seams at cascade transitions, which means you don't have to try to filter across adjacent cascades."

**Depth clamping ("pancaking")** introduces an additional cascade problem. Occluders beyond a cascade's near plane get their depth clamped, distorting blocker depth estimates. Bevy's engine review documents this clearly: "Close cascades and pancaking really screws this up; any caster which is beyond the cascade will be at the light's near plane, which means we have a massive penumbra." The fix: extend each cascade's light-space frustum to encompass all potential occluders, or disable depth clamping entirely when PCSS is active.

### Light bleeding from the single-layer depth limitation

PCSS uses a single shadow map depth value per texel — only the nearest occluder is recorded. When a receiver sits behind **multiple occluders at different depths**, the algorithm averages their depths into a nonsensical estimate. The classic example: a person walking between a building and the sun creates a person-shaped bright patch moving across the inside of a dark room, because the person's depth, averaged with the wall's depth, produces an incorrect penumbra estimate.

This is a **fundamental limitation** of single-layer shadow mapping, not a bug to fix. Mitigation strategies include clamping maximum penumbra width to prevent absurdly large soft regions, scaling the PCF kernel **1.2–1.6× larger** than the blocker search region (Sterna 2018) to ensure adequate coverage, and accepting that deeply layered scenes will have some inaccuracy. Multi-view soft shadows (Bavoil 2011) address this by jittering the light position across multiple shadow maps, at correspondingly higher cost.

### Thin objects vanish from shadows

Thin geometry — leaves, fences, hair, weapon blades — can project to sub-texel size in the shadow map, falling between depth samples entirely. The blocker search never finds them, so they cast no shadow at all. Even when resolved, thin objects interact badly with bias: any bias larger than the object's shadow-map thickness causes Peter Panning (shadow detaches from caster). Back-face-only shadow rendering, a common anti-acne trick, fails here because thin objects' back faces sit nearly atop their front faces.

**Screen-space contact shadows** (ray marching in the depth buffer) are the standard complement to PCSS for this case. They capture fine detail and hard contact shadows that shadow mapping inherently struggles with. Additionally, higher shadow map resolution targeted at critical geometry (via tighter cascade fitting or separate shadow maps for important small objects) mitigates the sub-texel problem.

---

## Performance: from naive to production-ready

Naive PCSS is expensive. The original paper used 32 blocker search + 64 PCF samples per pixel — **96 texture fetches per shadowed fragment**, many of them dependent on preceding results. This creates GPU wavefront divergence: fully-lit pixels early-out cheaply while penumbra pixels burn through dozens of samples with data-dependent branching.

### Effective optimization strategies

**Temporal accumulation** is the highest-impact optimization available today. Instead of 25+ samples per frame, use **1–4 jittered samples per pixel** with a different per-pixel rotation each frame, then rely on TAA to accumulate results over multiple frames. Bevy's engine documentation explicitly recommends this approach. Interleaved gradient noise (Jimenez, Call of Duty: Advanced Warfare) provides temporally stable per-pixel rotation that works well with TAA denoising.

**Decoupled penumbra estimation** (Sterna 2018, "Contact-hardening Soft Shadows Made Fast") computes the blocker search and penumbra estimation at **quarter resolution** in a separate pass, applies a 7×7 Gaussian blur to smooth discontinuities, then uses the upsampled penumbra width to drive a standard PCF kernel in the full-resolution shadow pass. This achieves **~2.5× speedup** over naive PCSS with near-identical visual quality.

**Hardware bilinear PCF** is critical. Using `SampleCmpLevelZero` for the filtering pass means each tap evaluates a 2×2 comparison with bilinear interpolation of results. A 16-sample Poisson disk with bilinear PCF produces 64 effective comparisons. Matt Pettineo reports only **~0.4ms overhead** going from 2×2 to 7×7 PCF at 1080p using `GatherCmp` on DX11 hardware. For the blocker search, point sampling is mandatory (you need raw depths), so optimized sampling matters more here.

**The right sample distribution** matters enormously. Poisson disk distributions outperform regular grids at equal sample counts by avoiding moiré patterns. Vogel disk (golden angle spiral) distributions are computationally trivial to generate and have good coverage properties. Per-pixel rotation of any distribution — using screen-space hash or interleaved gradient noise — converts structured banding into high-frequency noise that is far less perceptible and more amenable to temporal filtering.

A practical sample budget for shipping: **16 blocker search samples + 16–25 PCF samples** with hardware bilinear, per-pixel Poisson rotation, and temporal accumulation. This is the configuration used across multiple shipped titles.

---

## Reference implementations and authoritative sources

The foundational references form a clear lineage. Fernando's 2005 SIGGRAPH sketch introduced the algorithm. The 2008 NVIDIA integration whitepaper by Myers, Fernando, and Bavoil ("Integrating Realistic Soft Shadows into Your Game Engine") provides complete HLSL code with Poisson disk sampling and gradient-based depth bias — this remains the canonical implementation reference. Bavoil's GDC 2008 presentation ("Advanced Soft Shadow Mapping Techniques") contextualizes PCSS among competing approaches.

**Unity HDRP** implements PCSS for both punctual and directional lights, exposed when Shadow Filtering Quality is set to "High." The shader lives in `HDShadowAlgorithms.hlsl`, with the directional light using Angular Diameter as the light size parameter. Separate blocker and filter sample counts are tunable per light. TheMasonX's `UnityPCSS` GitHub repository provides an accessible built-in-pipeline implementation for directional lights specifically.

**Unreal Engine 5** largely moved beyond traditional PCSS with **Virtual Shadow Maps** (VSMs), which achieve contact-hardening through Shadow Map Ray Tracing (SMRT) across ultra-high-resolution virtualized shadow maps paired with Nanite. VSMs use clipmaps for directional lights — expanding rings around the camera analogous to cascades but at much higher virtual resolution (up to 16K). The "Source Angle" parameter on directional lights controls penumbra softness. This represents the current state-of-the-art for production engines, though it requires Nanite's infrastructure. UE4 did support traditional PCSS via engine source modifications.

**Key papers beyond the original** include Variance Soft Shadow Mapping (Yang et al., Pacific Graphics 2010), which replaces the expensive per-sample blocker search with VSM moment-based estimation for **10×+ speedup** on large penumbras; the Screen-Space PCSS work (MohammadBagher et al., SIGGRAPH 2010) that moves all operations to screen space with separable bilateral filtering; and Schwärzler et al.'s temporal coherence approach that caches shadow values in a history buffer for **2–2.5× speedup** in typical game scenes.

For hands-on study, the NVIDIA GameWorks Soft Shadows sample (available for GL4, GLES, and Vulkan) provides a complete, tunable implementation with preset sample counts from 25/25 to 100/100. The VulkanSceneGraph PCSS discussion documents a modern Vulkan implementation with cascade-aware depth conversion using `tan(angleSubtended/2)`. Several GitHub repositories (proskur1n/vwa-code, Oitron/Variance-Soft-Shadow-Mapping) provide educational OpenGL implementations with side-by-side PCF/PCSS/VSSM comparison.

---

## Conclusion

Implementing PCSS for directional lights reduces to three key adaptations from the perspective case: **use a constant blocker search radius** (no perspective foreshortening in orthographic projection), **remove the `1/dBlocker` division** in the penumbra formula, and **recompute `LIGHT_SIZE_UV` per cascade** since each covers a different world-space extent. The algorithm is straightforward in isolation — the difficulty lies entirely in the interaction with cascaded shadow maps, bias strategies, and performance budgets.

The most impactful techniques for a production implementation are temporal accumulation with TAA (turning 25+ samples into 1–4 jittered samples per frame), hardware bilinear PCF (quadrupling effective sample density for free), and world-space-consistent penumbra estimation across cascades (eliminating the most visible artifact). Screen-space contact shadows should be treated as a mandatory complement, not an optional extra — they fill the exact gaps (thin geometry, contact points) where PCSS inherently fails. The trend in cutting-edge engines (UE5's Virtual Shadow Maps) points toward replacing the shadow map sampling approach entirely with ray tracing through high-resolution virtualized depth, but PCSS remains the practical choice for any engine that cannot afford that infrastructure.