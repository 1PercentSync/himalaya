#version 460

/**
 * @file shadow_miss.rmiss
 * @brief Shadow ray miss shader — marks the light as visible (not occluded).
 *
 * SBT miss index 1. When a shadow ray reaches tMax without hitting geometry,
 * sets ShadowPayload.visible = 1.  The default (0 = occluded) is set by the
 * caller before traceRayEXT.
 */

#define HIMALAYA_RT
#include "common/bindings.glsl"
#include "rt/pt_common.glsl"

layout(location = 1) rayPayloadInEXT ShadowPayload shadow_payload;

void main() {
    shadow_payload.visible = 1;
}
