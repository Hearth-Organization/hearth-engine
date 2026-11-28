// Atom in Rust, the main pass: physically based lighting (GGX specular, Smith shadowing, Schlick Fresnel, Lambert
// diffuse) like Atom's standard PBR, a shadow-mapped sun with soft edges, a sky that lights and is reflected, base
// colour textures, then ACES tone mapping.

struct Light { pos_range: vec4<f32>, color: vec4<f32> };
struct Frame {
    view_proj: mat4x4<f32>,
    inv_view_proj: mat4x4<f32>,
    light_view_proj: mat4x4<f32>,
    camera: vec4<f32>,
    sun_dir: vec4<f32>,
    sun_color: vec4<f32>,
    ambient: vec4<f32>,
    counts: vec4<f32>,      // lights, exposure
    sky_zenith: vec4<f32>,  // w: 1 when there's a sky
    sky_horizon: vec4<f32>,
    sky_ground: vec4<f32>,
    shadow: vec4<f32>,      // on, bias, texel size
    lights: array<Light, 16>,
};
struct Object {
    model: mat4x4<f32>,
    normal: mat4x4<f32>,
    color: vec4<f32>,
    params: vec4<f32>,   // metallic, roughness
    emissive: vec4<f32>,
};

@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var shadow_map: texture_depth_2d;
@group(0) @binding(2) var shadow_sampler: sampler_comparison;
@group(1) @binding(0) var<uniform> object: Object;
@group(1) @binding(1) var base_tex: texture_2d<f32>;
@group(1) @binding(2) var base_sampler: sampler;

struct VIn { @location(0) pos: vec3<f32>, @location(1) normal: vec3<f32>, @location(2) uv: vec2<f32> };
struct VOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
};

@vertex
fn vs_main(v: VIn) -> VOut {
    var o: VOut;
    let w = object.model * vec4<f32>(v.pos, 1.0);
    o.world = w.xyz;
    o.clip = frame.view_proj * w;
    o.normal = (object.normal * vec4<f32>(v.normal, 0.0)).xyz;
    o.uv = v.uv;
    return o;
}

// The sun's view, for the shadow map.
@vertex
fn vs_shadow(v: VIn) -> @builtin(position) vec4<f32> {
    return frame.light_view_proj * object.model * vec4<f32>(v.pos, 1.0);
}

const PI: f32 = 3.14159265;

fn d_ggx(nh: f32, a: f32) -> f32 {
    let a2 = a * a;
    let d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

fn g_smith(nv: f32, nl: f32, r: f32) -> f32 {
    let k = (r + 1.0) * (r + 1.0) / 8.0;
    return (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
}

fn shade(n: vec3<f32>, v: vec3<f32>, l: vec3<f32>, radiance: vec3<f32>, albedo: vec3<f32>, metallic: f32, rough: f32) -> vec3<f32> {
    let h = normalize(v + l);
    let nl = max(dot(n, l), 0.0);
    let nv = max(dot(n, v), 0.0001);
    let nh = max(dot(n, h), 0.0);
    let f0 = mix(vec3<f32>(0.04), albedo, metallic);
    let f = f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);
    let spec = d_ggx(nh, rough * rough) * g_smith(nv, nl, rough) * f / (4.0 * nv * max(nl, 0.0001));
    let kd = (vec3<f32>(1.0) - f) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * radiance * nl;
}

// The sky in a direction (Z up): ground below, horizon to zenith above.
fn sky(d: vec3<f32>) -> vec3<f32> {
    let up = d.z;
    if (up < 0.0) {
        return mix(frame.sky_horizon.rgb, frame.sky_ground.rgb, clamp(-up * 4.0, 0.0, 1.0));
    }
    return mix(frame.sky_horizon.rgb, frame.sky_zenith.rgb, pow(clamp(up, 0.0, 1.0), 0.6));
}

fn sun_visible(world: vec3<f32>, n: vec3<f32>) -> f32 {
    if (frame.shadow.x < 0.5) {
        return 1.0;
    }
    let p = frame.light_view_proj * vec4<f32>(world + n * 0.02, 1.0);
    let ndc = p.xyz / p.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;
    }
    // 3 x 3 samples for soft edges.
    var lit = 0.0;
    let t = frame.shadow.z;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            lit += textureSampleCompareLevel(shadow_map, shadow_sampler, uv + vec2<f32>(f32(x), f32(y)) * t, ndc.z - frame.shadow.y);
        }
    }
    return lit / 9.0;
}

fn aces(x: vec3<f32>) -> vec3<f32> {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));
}

@fragment
fn fs_main(i: VOut) -> @location(0) vec4<f32> {
    let tex = textureSample(base_tex, base_sampler, i.uv);
    let n = normalize(i.normal);
    let v = normalize(frame.camera.xyz - i.world);
    let albedo = object.color.rgb * tex.rgb;
    let metallic = clamp(object.params.x, 0.0, 1.0);
    let rough = clamp(object.params.y, 0.04, 1.0);
    var c = object.emissive.rgb;
    if (frame.sky_zenith.w > 0.5) {
        // Light from the sky, and its reflection (blurrier the rougher the surface).
        let f0 = mix(vec3<f32>(0.04), albedo, metallic);
        let nv = max(dot(n, v), 0.0);
        let fres = f0 + (max(vec3<f32>(1.0 - rough), f0) - f0) * pow(1.0 - nv, 5.0);
        let irradiance = sky(n) * 0.5 + sky(vec3<f32>(0.0, 0.0, 1.0)) * 0.25 + sky(vec3<f32>(0.0, 0.0, -1.0)) * 0.25;
        let reflected = mix(sky(reflect(-v, n)), irradiance, rough * rough);
        c += (vec3<f32>(1.0) - fres) * (1.0 - metallic) * albedo * irradiance + fres * reflected;
    } else {
        c += frame.ambient.rgb * albedo;
    }
    if (frame.sun_color.a > 0.0) {
        c += shade(n, v, normalize(-frame.sun_dir.xyz), frame.sun_color.rgb, albedo, metallic, rough) * sun_visible(i.world, n);
    }
    let count = u32(frame.counts.x);
    for (var k = 0u; k < count; k++) {
        let lt = frame.lights[k];
        let d = lt.pos_range.xyz - i.world;
        let dist = length(d);
        let range = lt.pos_range.w;
        let fall = pow(clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0), 2.0) / max(dist * dist, 0.01);
        c += shade(n, v, d / max(dist, 0.0001), lt.color.rgb * fall, albedo, metallic, rough);
    }
    return vec4<f32>(aces(c * frame.counts.y), object.color.a * tex.a);
}

// The sky behind everything: one triangle over the whole screen.
struct SkyOut { @builtin(position) clip: vec4<f32>, @location(0) ndc: vec2<f32> };

@vertex
fn vs_sky(@builtin(vertex_index) i: u32) -> SkyOut {
    var o: SkyOut;
    let xy = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;
    o.clip = vec4<f32>(xy, 1.0, 1.0);
    o.ndc = xy;
    return o;
}

@fragment
fn fs_sky(i: SkyOut) -> @location(0) vec4<f32> {
    let far = frame.inv_view_proj * vec4<f32>(i.ndc, 1.0, 1.0);
    let near = frame.inv_view_proj * vec4<f32>(i.ndc, 0.0, 1.0);
    let d = normalize(far.xyz / far.w - near.xyz / near.w);
    return vec4<f32>(aces(sky(d) * frame.counts.y), 1.0);
}
