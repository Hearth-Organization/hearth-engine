// Atom in Rust, the main pass: physically based lighting (GGX specular, Smith shadowing, Schlick Fresnel, Lambert
// diffuse), the way Atom's standard PBR material lights things, then ACES tone mapping.

struct Light { pos_range: vec4<f32>, color: vec4<f32> };
struct Frame {
    view_proj: mat4x4<f32>,
    camera: vec4<f32>,
    sun_dir: vec4<f32>,
    sun_color: vec4<f32>,
    ambient: vec4<f32>,
    counts: vec4<f32>,
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
@group(1) @binding(0) var<uniform> object: Object;

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

fn aces(x: vec3<f32>) -> vec3<f32> {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));
}

@fragment
fn fs_main(i: VOut) -> @location(0) vec4<f32> {
    let n = normalize(i.normal);
    let v = normalize(frame.camera.xyz - i.world);
    let albedo = object.color.rgb;
    let metallic = clamp(object.params.x, 0.0, 1.0);
    let rough = clamp(object.params.y, 0.04, 1.0);
    var c = frame.ambient.rgb * albedo + object.emissive.rgb;
    if (frame.sun_color.a > 0.0) {
        c += shade(n, v, normalize(-frame.sun_dir.xyz), frame.sun_color.rgb, albedo, metallic, rough);
    }
    let count = u32(frame.counts.x);
    for (var k = 0u; k < count; k++) {
        let lt = frame.lights[k];
        let d = lt.pos_range.xyz - i.world;
        let dist = length(d);
        let range = lt.pos_range.w;
        // Inverse square, smoothly reaching nothing at the light's range.
        let fall = pow(clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0), 2.0) / max(dist * dist, 0.01);
        c += shade(n, v, d / max(dist, 0.0001), lt.color.rgb * fall, albedo, metallic, rough);
    }
    return vec4<f32>(aces(c * frame.counts.y), object.color.a);
}
