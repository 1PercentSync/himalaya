# Contact shadows in real-time rendering: a complete technical guide

**Contact shadows are a screen-space ray marching technique that fills the critical gap between shadow maps and the fine-detail shadows visible where objects meet surfaces.** By tracing short rays through the depth buffer toward the light source, they capture small-scale occlusion that shadow maps inherently miss due to resolution limits, bias offsets, and peter-panning. Every major engine — Unreal Engine, Unity HDRP, and custom engines like Bend Studio's Days Gone renderer — implements some variant of this technique, typically at a cost of **0.5–1.3 ms** on console hardware at 1080p. This report covers the full technical landscape: why contact shadows exist, how they work at the shader level, what goes wrong, and how to fix it.

---

## Why shadow maps alone aren't enough

Traditional shadow mapping projects the scene from the light's perspective into a depth texture, then compares each pixel's light-space depth to determine occlusion. This works for large-scale shadows but breaks down at fine scales for three interconnected reasons.

**Shadow map resolution is finite.** A directional light covering a 30 km² open world allocates perhaps 2048–4096 texels across cascades. Small geometric details — bolts, wrinkles, fingers touching a tabletop, grass blades — fall below the shadow map's Nyquist limit and produce no shadow at all. Increasing cascade resolution helps marginally but hits VRAM and bandwidth ceilings fast.

**Bias creates peter-panning.** To suppress shadow acne (self-shadowing caused by depth quantization), renderers apply a depth or normal bias that pushes the shadow slightly away from the caster. This creates a visible gap — the "peter-panning" artifact — precisely at the contact point where the shadow should be most visible. The tighter the contact, the more obvious the gap.

**Screen-space contact shadows fix both problems simultaneously.** Because they operate on the full-resolution depth buffer (which already captures every rasterized fragment), they detect occlusion at pixel-level granularity. And because the ray starts at the surface being shaded rather than at the light, the bias/peter-panning dilemma doesn't apply. The trade-off is that contact shadows are limited to screen-visible geometry and degrade over distance — which is precisely why they're called "contact" shadows: they're designed to work at close range where the occluder nearly touches the receiver.

---

## The core algorithm: screen-space ray marching step by step

The dominant implementation across all engines follows the same fundamental pattern. For each pixel on screen:

**1. Reconstruct the shading point's 3D position** from the depth buffer. Transform it into view space or clip space (engine-dependent). In UE4/5, the working space is clip space; in Karabelas's reference implementation, it's view space.

**2. Compute a ray direction toward the light source.** For a directional light, this is simply the negated light direction transformed into the working space. For point/spot lights, it's the normalized vector from the pixel to the light position.

**3. Determine the ray length and step size.** The maximum ray distance is kept deliberately short — Karabelas uses **0.05 in view space**, UE4/5 uses a normalized 0–1 value where 1.0 traverses the entire screen (typical values are 0.05–0.1). The step size is simply `max_distance / num_steps`, producing uniform spacing along the ray.

**4. Apply temporal jitter to the starting position.** Before entering the loop, offset the ray origin by a noise value scaled to one step length. This converts the discrete stepping pattern from banding into noise that TAA can resolve:

```hlsl
float offset = interleaved_gradient_noise(screen_position, frame_index);
ray_pos += ray_step * offset;
```

**5. March along the ray in discrete steps.** At each step:

- Advance the position: `ray_pos += ray_step`
- Project back to screen UV: `uv = project(ray_pos, projection_matrix)`
- Sample the depth buffer at that UV
- Compute `depth_delta = ray_depth - scene_depth`
- Test for occlusion: if `depth_delta > 0` (ray is behind the surface) AND `depth_delta < thickness` (ray hasn't passed entirely through a thick object), the pixel is shadowed

**6. Apply fade and output.** Fade the shadow near screen edges (to prevent hard cutoff at viewport boundaries) and at the maximum ray distance (to prevent a sharp shadow terminus). Multiply the result with the shadow map value — contact shadows can only darken, never lighten.

Here's a complete reference implementation (adapted from Panos Karabelas's Spartan Engine):

```hlsl
static const uint  MAX_STEPS     = 16;
static const float RAY_MAX_DIST  = 0.05;  // view-space units
static const float THICKNESS     = 0.02;  // thickness heuristic
static const float STEP_LENGTH   = RAY_MAX_DIST / (float)MAX_STEPS;

float ScreenSpaceContactShadow(float3 world_pos, float3 light_dir, float2 uv)
{
    // Work in view space
    float3 ray_pos = mul(float4(world_pos, 1.0), g_view).xyz;
    float3 ray_dir = mul(float4(-light_dir, 0.0), g_view).xyz;
    float3 ray_step = ray_dir * STEP_LENGTH;

    // Temporal jitter: convert banding to noise for TAA
    float noise = interleaved_gradient_noise(g_resolution * uv, g_frame);
    ray_pos += ray_step * (noise * 2.0 - 1.0);

    float occlusion = 0.0;
    for (uint i = 0; i < MAX_STEPS; i++)
    {
        ray_pos += ray_step;
        float2 sample_uv = project_to_uv(ray_pos, g_projection);

        if (any(sample_uv < 0.0) || any(sample_uv > 1.0))
            break;  // ray left screen

        float scene_depth = get_linear_depth(sample_uv);
        float depth_delta = ray_pos.z - scene_depth;

        if (depth_delta > 0.0 && depth_delta < THICKNESS)
        {
            occlusion = 1.0;
            occlusion *= screen_edge_fade(sample_uv);
            break;
        }
    }
    return 1.0 - occlusion;
}
```

The interleaved gradient noise function (from Jorge Jimenez's Call of Duty: Advanced Warfare GDC talk) is the de facto standard:

```hlsl
float interleaved_gradient_noise(float2 screen_pos, uint frame)
{
    screen_pos += float(frame) * 5.588238;  // temporal offset
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screen_pos, magic.xy)));
}
```

---

## How Unreal Engine implements ShadowRayCast

UE4/5's contact shadow lives inside the deferred lighting pass, computed per-pixel per-light in `DeferredLightingCommon.ush`. The `ShadowRayCast` function uses **8 hardcoded steps** — a notably aggressive choice that relies heavily on temporal jitter plus TAA for acceptable quality.

The algorithm works in **clip space** rather than view space. It transforms the ray start and end points from translated world space to clip space via `View.TranslatedWorldToClip`, then converts to screen-space UV for depth buffer sampling. Key implementation details that distinguish UE's approach:

**Self-intersection avoidance.** UE offsets the ray start by approximately **1.42 pixels** along the ray direction in screen space, plus a depth bias scaled by `(1 - abs(ray_dir.z) / length(ray_dir))`. This angle-dependent bias is larger for grazing rays (which are most prone to self-intersection) and smaller for rays perpendicular to the surface.

**Dynamic compare tolerance.** Rather than a fixed thickness, UE computes tolerance as the total depth span of the ray divided by the step count, multiplied by 2. This auto-scales with ray length and prevents moiré patterns on steep surfaces, though the documentation notes that "larger values make objects appear not grounded."

**The hit test uses an absolute-value formulation** rather than the two-sided comparison seen in other implementations:

```hlsl
float DepthDiff = SampleUVz.z - SampleDepth;
if (abs(DepthDiff + CompareTolerance) < CompareTolerance)
    FirstHitTime = SampleTime;
```

This elegantly encodes both the "ray behind surface" and "within thickness" conditions in a single comparison.

**Configuration exposed to artists.** The `Contact Shadow Length` parameter (0–1, default 0) controls ray travel distance. A value of 1.0 traverses the entire screen. Since step count is fixed at 8 regardless of length, **longer values produce proportionally worse artifacts** — UE's documentation explicitly warns about this. A `Contact Shadow Length In World Space` flag reinterprets the parameter as world-space meters instead of screen fraction. Per-primitive `bCastContactShadow` flags and per-light `ContactShadowCastingIntensity` / `ContactShadowNonCastingIntensity` floats provide fine-grained artistic control.

In **UE5**, the core `ShadowRayCast` algorithm remains essentially unchanged. The major UE5 shadow innovation is **Virtual Shadow Maps**, which provide pixel-accurate shadow maps paired with Nanite — contact shadows complement VSMs by catching the residual fine detail. Separately, Lumen's Screen Probe Gather includes its own **short-range AO pass** (`r.Lumen.ScreenProbeGather.ShortRangeAO`) that provides contact-level darkening for indirect lighting, operating independently of the per-light contact shadow system.

---

## Alternative techniques: capsules and distance fields

Screen-space ray marching isn't the only approach to contact-level shadowing. Two prominent alternatives solve different subsets of the problem.

**Capsule shadows** approximate skeletal mesh characters as a set of capsule primitives aligned to bones (thigh, forearm, torso). Shadow occlusion is computed analytically against these capsules — no depth buffer needed. First prominently used by Naughty Dog in *The Last of Us* (2013) and adopted as a built-in UE4 feature from version 4.11, capsule shadows are extremely cheap (a handful of primitives per character), naturally produce soft penumbras that widen with distance, and work even in baked-lighting environments where dynamic shadow maps don't exist. Their main limitations are that they only support skeletal meshes, cannot capture fine detail (fingers, accessories), and can cast shadows through walls since they don't test against scene geometry.

**Distance field soft shadows** use per-object signed distance fields (SDFs) stored as 3D volume textures. Rays traced through the SDF use sphere tracing (each step advances by the SDF's value, guaranteeing no missed intersections) to efficiently skip empty space. A key byproduct: tracking the minimum distance the ray passes by any surface approximates a cone trace, naturally producing **contact-hardening penumbras** — sharp near the caster, soft at distance — at no extra cost. Daniel Wright (Epic Games) presented this at SIGGRAPH 2015, noting 4× higher effective resolution compared to voxels because SDF values interpolate linearly at surfaces. The drawbacks are significant: SDFs must be generated offline, consume substantial GPU memory, don't support skeletal meshes or runtime deformation, and struggle with thin geometry.

**Bend Studio's compute-optimized approach** (Days Gone, 2019) deserves special mention. Their screen-space shadow pass uses a compute shader that aggressively shares depth buffer samples between neighboring threads in a workgroup via shared memory, amortizing texture reads across pixels marching in similar directions. Days Gone was reportedly the first title to apply screen-space shadows to sun lighting. The source code was released under Apache 2.0 at SIGGRAPH 2023.

---

## The five artifacts that will ruin your contact shadows

Every implementation encounters the same set of artifacts. Understanding their root causes is essential to fixing them.

**Banding** appears as staircase patterns at shadow boundaries when using fixed equidistant steps. Adjacent pixels test identical discrete positions, producing coherent step artifacts. The universal solution is temporal jitter — offsetting each pixel's ray start by a noise value scaled to one step length. This converts banding into per-pixel noise that TAA resolves across frames. Interleaved gradient noise and spatiotemporal blue noise (NVIDIA's STBN) are the preferred patterns; white noise converges too slowly.

**Self-shadowing** (shadow acne) occurs when the ray immediately hits the surface it originates from due to floating-point precision or depth buffer quantization. UE4 addresses this with a 1.42-pixel directional offset plus angle-dependent depth bias. Tomasz Stachowiak (h3r2tic) introduced a more robust solution: **dual depth sampling**, reading both bilinear-filtered and nearest-neighbor depth simultaneously. Point-sampled depth creates "duplo brick" artifacts from quantized values; bilinear depth creates false shadows at silhouettes via interpolation between foreground and background. Requiring the ray to pass below *both* surfaces eliminates both artifact classes — "the two approaches end up fixing each other's artifacts."

**Thickness ambiguity** is fundamental: the depth buffer is a 2.5D heightfield with no information about how thick objects are. When a ray passes behind a surface, is it inside a solid wall (genuine shadow) or past a thin leaf (false shadow)? The thickness parameter controls the maximum depth delta considered valid. Too small and thick objects cast incomplete shadows; too large and thin objects cast false shadows behind them. Keijiro Takahashi recommends setting thickness to "the average thickness of the objects in the view." h3r2tic's `march_behind_surfaces` mode allows rays to continue past failed thickness tests, finding valid occlusions beyond — critical for contact shadows but problematic for SSR where it causes light leaks.

**Temporal instability** manifests as flickering or shimmer, caused by noise patterns that aren't temporally coherent, marginal hits oscillating between shadow and lit states, and TAA jitter shifting ray sample positions. Solutions include using temporally-coherent noise (IGN with frame offset, or STBN), applying `smoothstep` at hit boundaries based on penetration depth rather than binary hit/miss, and keeping ray lengths short to reduce the probability of marginal intersections.

**Screen-space limitations** are inherent and unsolvable: objects behind the camera, occluded by other geometry, or outside the viewport simply don't exist in the depth buffer. This is mitigated by design — contact shadows supplement shadow maps rather than replacing them — and by screen-edge fading to prevent hard shadow cutoffs at viewport boundaries.

---

## Performance budget and optimization strategies

Contact shadow cost scales with **resolution × number of lights × step count**. At 1080p with 8–16 steps, expect **0.5–1.3 ms per light** on console-class hardware. Several optimizations can reduce this.

The most impactful optimization is **reducing step count and compensating with TAA**. h3r2tic's production renderer (Kajiya) uses just **4 linear steps** for contact shadows combined with IGN jitter and TAA accumulation. Over 4–8 frames, the effective sample count reaches 16–32 at no additional per-frame cost. This only works when TAA is active — without it, the result appears grainy.

**Early termination** provides free savings: break on first hit (shadow is binary anyway), and break when the ray leaves the screen. h3r2tic additionally clamps the step count to `max(2, floor(ray_length_pixels / MIN_PX_PER_STEP))` — never taking more steps than there are pixels to traverse.

**Half-resolution rendering** reduces ray count by 4× but requires edge-aware bilateral upsampling to prevent shadow bleeding across depth discontinuities. Checkerboard rendering (every other pixel) is an alternative that preserves more spatial detail while halving cost.

**Non-linear step distribution** (exponential march) compresses early steps and expands later ones, concentrating samples near the origin where contact detail matters most. h3r2tic exposes a `linear_march_exponent` parameter: a value of 1.0 gives equidistant steps, while higher values redistribute samples toward the surface.

**Hierarchical ray marching** (Hi-Z / depth mip chain) starts at coarse mip levels for large steps and refines on intersection. While powerful for SSR where rays traverse the full screen, h3r2tic notes it's "overkill for contact shadows" since rays are short. The Hi-Z buffer generation itself costs ~0.5 ms.

---

## Unity HDRP: the most configurable implementation

Unity's HDRP exposes contact shadows as a Volume framework override with the most artist-facing parameters of any major engine. The `sampleCount` is configurable (unlike UE's hardcoded 8), and a `distanceScaleFactor` scales ray length based on linear depth — shorter rays for nearby pixels, longer for distant ones — optimizing quality distribution automatically.

HDRP 16.0+ supports **up to 24 simultaneous lights** casting contact shadows (earlier versions allowed only one). The system includes `rayBias` (self-shadowing prevention), `thicknessScale` (thickness heuristic control), and separate `minDistance`/`maxDistance` with `fadeInDistance`/`fadeDistance` for smooth distance-based attenuation.

A critical Unity-specific caveat: contact shadows ignore the `Cast Shadows` property on Mesh Renderers. Any material that writes to the depth buffer will cast contact shadows regardless. To exclude objects, you must create a Shader Graph material that doesn't write depth — a counterintuitive workaround that catches many developers off guard.

URP (Universal Render Pipeline) does **not natively support contact shadows**. Developers must use third-party assets or custom Renderer Features implementing the same screen-space ray march.

---

## Conclusion

Contact shadows solve a narrow but visually critical problem: the fine-scale shadow detail that makes objects feel grounded and surfaces feel three-dimensional. The core technique — marching short rays through the depth buffer toward the light — has remained remarkably stable since its widespread adoption around 2018–2019. The key insight across all high-quality implementations is that **temporal jitter plus TAA is not optional but fundamental** — it transforms an 8-step march (which would be useless on its own) into an effective 32+ sample accumulation.

The most important parameter to get right is **ray length**: keep it short (0.05–0.1 screen fraction). Long rays amplify every artifact — noise, false occlusions, temporal instability — while providing diminishing shadow quality. The second most impactful choice is the **thickness heuristic**, which must balance thin-object false positives against thick-object incomplete shadows. h3r2tic's dual depth sampling (bilinear + nearest simultaneously) and march-behind-surfaces mode represent the current state of the art for robust handling of these edge cases.

For engine users rather than implementers, the practical advice is straightforward: in UE5, set Contact Shadow Length to **0.05–0.1**, ensure TAA or TSR is active, and rely on Virtual Shadow Maps plus Lumen short-range AO for the rest. In Unity HDRP, increase `sampleCount` from default if quality is insufficient, tune `thicknessScale` to match your scene's average object thickness, and use `maxDistance` to disable contact shadows where they're unreliable. In both engines, contact shadows are a supplement to — never a replacement for — proper shadow mapping.