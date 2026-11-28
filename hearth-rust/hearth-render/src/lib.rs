//! Atom in Rust: Hearth Engine's renderer, on wgpu (Vulkan on Linux, any graphics card, or Mesa's software
//! driver when there's none, which is how GitHub renders the test pictures).
//!
//! What it draws today: meshes with physically based materials (colour, metal, roughness, glow, a colour texture
//! with mipmaps), a sun with soft shadows, up to 16 point lights, a sky that lights the scene and shows in
//! reflections, depth and ACES tone mapping, into an image. Next: normal maps, the Nanite-style detail from
//! hearth-detail, a window, then the rest of what Atom does.

pub mod shapes;

use std::collections::HashMap;

use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Vec3};
use wgpu::util::DeviceExt;

pub const MAX_LIGHTS: usize = 16;
const COLOR: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;
const DEPTH: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Pod, Zeroable)]
pub struct Vertex {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
}

#[derive(Clone, Debug, Default)]
pub struct MeshData {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Material {
    /// Linear colour and alpha.
    pub color: [f32; 4],
    pub metallic: f32,
    pub roughness: f32,
    pub emissive: [f32; 3],
    /// A colour texture (multiplied by `color`).
    pub texture: Option<TextureId>,
}

impl Default for Material {
    fn default() -> Material {
        Material { color: [0.8, 0.8, 0.8, 1.0], metallic: 0.0, roughness: 0.5, emissive: [0.0; 3], texture: None }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TextureId(u32);

/// A simple sky: colours straight up, at the horizon and below. It lights the scene and shows in reflections.
#[derive(Clone, Copy, Debug)]
pub struct Sky {
    pub zenith: [f32; 3],
    pub horizon: [f32; 3],
    pub ground: [f32; 3],
}

impl Default for Sky {
    fn default() -> Sky {
        Sky { zenith: [0.15, 0.3, 0.7], horizon: [0.7, 0.75, 0.8], ground: [0.15, 0.13, 0.12] }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Camera {
    pub eye: Vec3,
    pub target: Vec3,
    pub up: Vec3,
    /// Vertical field of view, in degrees.
    pub fov: f32,
    pub near: f32,
    pub far: f32,
}

impl Default for Camera {
    fn default() -> Camera {
        Camera { eye: Vec3::new(0.0, -4.0, 1.5), target: Vec3::ZERO, up: Vec3::Z, fov: 60.0, near: 0.05, far: 500.0 }
    }
}

impl Camera {
    pub fn view_proj(&self, aspect: f32) -> Mat4 {
        // wgpu's clip space is DirectX-style (depth 0 to 1, Y up).
        glam::camera::rh::proj::directx::perspective(self.fov.to_radians(), aspect, self.near, self.far) * glam::camera::rh::view::look_at_mat4(self.eye, self.target, self.up)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Sun {
    /// The way the light travels (down and across).
    pub direction: Vec3,
    pub color: [f32; 3],
    pub intensity: f32,
}

/// God rays: beams of sunlight in the air, through windows, gaps and trees. Real light shafts: every pixel steps
/// through the air toward the camera and checks the sun's shadow at each step, so they work with the sun
/// off-screen too. Needs `shadows` on (without them the air is evenly hazy).
#[derive(Clone, Copy, Debug)]
pub struct GodRays {
    /// How thick the air is (dust, mist): 0.02 is clear, 0.1 hazy, 0.3 foggy.
    pub density: f32,
    /// How bright the beams are.
    pub strength: f32,
    /// Steps through the air for each pixel: more is smoother and slower (16 for handhelds, 32 normally, 64 best).
    pub steps: u32,
    /// How much brighter the beams are when looking toward the sun (0 = the same all round, 0.8 = much brighter).
    pub toward_sun: f32,
    /// The farthest the beams reach (metres).
    pub distance: f32,
}

impl Default for GodRays {
    fn default() -> GodRays {
        GodRays { density: 0.06, strength: 1.0, steps: 32, toward_sun: 0.5, distance: 120.0 }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct PointLight {
    pub position: Vec3,
    pub color: [f32; 3],
    pub intensity: f32,
    pub range: f32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MeshId(u32);

#[derive(Clone, Debug)]
pub struct Object {
    pub mesh: MeshId,
    pub transform: Mat4,
    pub material: Material,
}

/// A model with Nanite-style detail (hearth-detail), placed in the scene.
#[derive(Clone, Debug)]
pub struct DetailObject {
    pub detail: DetailId,
    pub transform: Mat4,
    pub material: Material,
    /// How many pixels a simplification may move the shape on screen (1 is hard to see).
    pub max_pixels: f32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DetailId(u32);

#[derive(Clone, Debug)]
pub struct Scene {
    pub camera: Camera,
    pub sun: Option<Sun>,
    pub ambient: [f32; 3],
    pub lights: Vec<PointLight>,
    pub objects: Vec<Object>,
    pub details: Vec<DetailObject>,
    /// Linear background colour.
    pub clear: [f32; 4],
    pub exposure: f32,
    /// Drawn behind everything and used for light and reflections (instead of `ambient`).
    pub sky: Option<Sky>,
    /// The sun casts shadows.
    pub shadows: bool,
    /// Beams of sunlight in the air (off when `None`).
    pub god_rays: Option<GodRays>,
}

impl Default for Scene {
    fn default() -> Scene {
        Scene { camera: Camera::default(), sun: None, ambient: [0.03; 3], lights: Vec::new(), objects: Vec::new(), details: Vec::new(), clear: [0.0, 0.0, 0.0, 1.0], exposure: 1.0, sky: None, shadows: true, god_rays: None }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuLight {
    pos_range: [f32; 4],
    color: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuFrame {
    view_proj: [[f32; 4]; 4],
    inv_view_proj: [[f32; 4]; 4],
    light_view_proj: [[f32; 4]; 4],
    camera: [f32; 4],
    sun_dir: [f32; 4],
    sun_color: [f32; 4],
    ambient: [f32; 4],
    counts: [f32; 4],
    sky_zenith: [f32; 4],
    sky_horizon: [f32; 4],
    sky_ground: [f32; 4],
    shadow: [f32; 4],
    rays: [f32; 4],
    rays_more: [f32; 4],
    lights: [GpuLight; MAX_LIGHTS],
}

pub const SHADOW_SIZE: u32 = 2048;

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuObject {
    model: [[f32; 4]; 4],
    normal: [[f32; 4]; 4],
    color: [f32; 4],
    params: [f32; 4],
    emissive: [f32; 4],
}

struct GpuDetail {
    detail: hearth_detail::Detail,
    vertices: wgpu::Buffer,
    bounds: (Vec3, Vec3),
}

struct GpuMesh {
    vertices: wgpu::Buffer,
    indices: wgpu::Buffer,
    count: u32,
    /// The mesh's box (min, max), for fitting the sun's shadow to the scene.
    bounds: (Vec3, Vec3),
}

pub struct Renderer {
    device: wgpu::Device,
    queue: wgpu::Queue,
    pipeline: wgpu::RenderPipeline,
    shadow_pipeline: wgpu::RenderPipeline,
    sky_pipeline: wgpu::RenderPipeline,
    object_layout: wgpu::BindGroupLayout,
    frame_buffer: wgpu::Buffer,
    frame_group: wgpu::BindGroup,
    shadow_frame_group: wgpu::BindGroup,
    shadow_map: wgpu::Texture,
    meshes: HashMap<MeshId, GpuMesh>,
    textures: HashMap<TextureId, wgpu::TextureView>,
    details: HashMap<DetailId, GpuDetail>,
    /// Triangles drawn in the last frame (all objects and the detail models' chosen pieces).
    pub last_triangles: u64,
    white: wgpu::TextureView,
    sampler: wgpu::Sampler,
    next_mesh: u32,
    pub width: u32,
    pub height: u32,
    color: wgpu::Texture,
    depth: wgpu::Texture,
    /// What graphics card (or software driver) it's drawing with.
    pub adapter_name: String,
}

fn uniform_layout(device: &wgpu::Device, label: &str, visibility: wgpu::ShaderStages) -> wgpu::BindGroupLayout {
    device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some(label),
        entries: &[wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility,
            ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
            count: None,
        }],
    })
}

fn texture_entry(binding: u32, sample_type: wgpu::TextureSampleType) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Texture { sample_type, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
        count: None,
    }
}

fn sampler_entry(binding: u32, kind: wgpu::SamplerBindingType) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry { binding, visibility: wgpu::ShaderStages::FRAGMENT, ty: wgpu::BindingType::Sampler(kind), count: None }
}

fn uniform_entry() -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding: 0,
        visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
        ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None },
        count: None,
    }
}

fn targets(device: &wgpu::Device, width: u32, height: u32) -> (wgpu::Texture, wgpu::Texture) {
    let size = wgpu::Extent3d { width, height, depth_or_array_layers: 1 };
    let color = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("hearth colour"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: COLOR,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let depth = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("hearth depth"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: DEPTH,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    });
    (color, depth)
}

impl Renderer {
    /// A renderer that draws into an image (no window): for tools, tests and thumbnails.
    pub fn headless(width: u32, height: u32) -> Result<Renderer, String> {
        if width == 0 || height == 0 {
            return Err("the picture needs a size".into());
        }
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default()))
            .map_err(|e| format!("no graphics card was found ({e}); on a computer without one, install Mesa's Vulkan driver (lavapipe)"))?;
        let adapter_name = adapter.get_info().name;
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("hearth"),
            required_features: wgpu::Features::empty(),
            required_limits: wgpu::Limits::downlevel_defaults(),
            experimental_features: wgpu::ExperimentalFeatures::disabled(),
            memory_hints: wgpu::MemoryHints::Performance,
            trace: wgpu::Trace::Off,
        }))
        .map_err(|e| format!("the graphics card wouldn't start: {e}"))?;

        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("hearth pbr"), source: wgpu::ShaderSource::Wgsl(include_str!("shader.wgsl").into()) });
        let frame_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("frame"),
            entries: &[uniform_entry(), texture_entry(1, wgpu::TextureSampleType::Depth), sampler_entry(2, wgpu::SamplerBindingType::Comparison)],
        });
        let shadow_frame_layout = uniform_layout(&device, "shadow frame", wgpu::ShaderStages::VERTEX_FRAGMENT);
        let object_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("object"),
            entries: &[uniform_entry(), texture_entry(1, wgpu::TextureSampleType::Float { filterable: true }), sampler_entry(2, wgpu::SamplerBindingType::Filtering)],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("hearth pbr"), bind_group_layouts: &[Some(&frame_layout), Some(&object_layout)], immediate_size: 0 });
        let shadow_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("hearth shadow"), bind_group_layouts: &[Some(&shadow_frame_layout), Some(&object_layout)], immediate_size: 0 });
        let sky_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("hearth sky"), bind_group_layouts: &[Some(&frame_layout)], immediate_size: 0 });
        let attributes = [
            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 12, shader_location: 1 },
            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 24, shader_location: 2 },
        ];
        let buffers = [Some(wgpu::VertexBufferLayout { array_stride: std::mem::size_of::<Vertex>() as wgpu::BufferAddress, step_mode: wgpu::VertexStepMode::Vertex, attributes: &attributes })];
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("hearth pbr"),
            layout: Some(&layout),
            vertex: wgpu::VertexState { module: &shader, entry_point: Some("vs_main"), compilation_options: Default::default(), buffers: &buffers },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format: COLOR, blend: Some(wgpu::BlendState::ALPHA_BLENDING), write_mask: wgpu::ColorWrites::ALL })],
            }),
            primitive: wgpu::PrimitiveState { cull_mode: Some(wgpu::Face::Back), ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::Less),
                stencil: Default::default(),
                bias: Default::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview_mask: None,
            cache: None,
        });
        let shadow_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("hearth shadow"),
            layout: Some(&shadow_layout),
            vertex: wgpu::VertexState { module: &shader, entry_point: Some("vs_shadow"), compilation_options: Default::default(), buffers: &buffers },
            fragment: None,
            primitive: wgpu::PrimitiveState { cull_mode: None, ..Default::default() },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::Less),
                stencil: Default::default(),
                bias: wgpu::DepthBiasState { constant: 2, slope_scale: 2.0, clamp: 0.0 },
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview_mask: None,
            cache: None,
        });
        let sky_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("hearth sky"),
            layout: Some(&sky_layout),
            vertex: wgpu::VertexState { module: &shader, entry_point: Some("vs_sky"), compilation_options: Default::default(), buffers: &[] },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_sky"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format: COLOR, blend: None, write_mask: wgpu::ColorWrites::ALL })],
            }),
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH,
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::Always),
                stencil: Default::default(),
                bias: Default::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview_mask: None,
            cache: None,
        });
        let frame_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("frame"),
            size: std::mem::size_of::<GpuFrame>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let shadow_map = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("hearth shadow map"),
            size: wgpu::Extent3d { width: SHADOW_SIZE, height: SHADOW_SIZE, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: DEPTH,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let shadow_view = shadow_map.create_view(&wgpu::TextureViewDescriptor::default());
        let shadow_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("hearth shadow"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });
        let frame_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("frame"),
            layout: &frame_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: frame_buffer.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&shadow_view) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&shadow_sampler) },
            ],
        });
        let shadow_frame_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("shadow frame"),
            layout: &shadow_frame_layout,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: frame_buffer.as_entire_binding() }],
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("hearth textures"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            anisotropy_clamp: 8,
            ..Default::default()
        });
        let white = upload_texture(&device, &queue, &[[255, 255, 255, 255]], 1, 1);
        let (color, depth) = targets(&device, width, height);
        Ok(Renderer {
            device,
            queue,
            pipeline,
            shadow_pipeline,
            sky_pipeline,
            object_layout,
            frame_buffer,
            frame_group,
            shadow_frame_group,
            shadow_map,
            meshes: HashMap::new(),
            textures: HashMap::new(),
            details: HashMap::new(),
            last_triangles: 0,
            white,
            sampler,
            next_mesh: 1,
            width,
            height,
            color,
            depth,
            adapter_name,
        })
    }

    /// Puts a mesh on the graphics card.
    pub fn add_mesh(&mut self, mesh: &MeshData) -> Result<MeshId, String> {
        if mesh.indices.is_empty() || !mesh.indices.len().is_multiple_of(3) {
            return Err("a mesh needs whole triangles".into());
        }
        if let Some(bad) = mesh.indices.iter().find(|&&i| i as usize >= mesh.vertices.len()) {
            return Err(format!("a triangle uses vertex {bad}, but the mesh has {}", mesh.vertices.len()));
        }
        let vertices = self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("vertices"), contents: bytemuck::cast_slice(&mesh.vertices), usage: wgpu::BufferUsages::VERTEX });
        let indices = self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("indices"), contents: bytemuck::cast_slice(&mesh.indices), usage: wgpu::BufferUsages::INDEX });
        let id = MeshId(self.next_mesh);
        self.next_mesh += 1;
        let (lo, hi) = mesh.vertices.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(lo, hi), v| {
            let p = Vec3::from(v.pos);
            (lo.min(p), hi.max(p))
        });
        self.meshes.insert(id, GpuMesh { vertices, indices, count: mesh.indices.len() as u32, bounds: (lo, hi) });
        Ok(id)
    }

    /// Puts a Nanite-style detail model (from hearth-detail) on the graphics card.
    pub fn add_detail(&mut self, detail: hearth_detail::Detail) -> Result<DetailId, String> {
        if detail.clusters.is_empty() {
            return Err("the detail model has no pieces".into());
        }
        let vertices: Vec<Vertex> = detail.vertices.iter().map(|v| Vertex { pos: v.pos, normal: v.normal, uv: v.uv }).collect();
        let (lo, hi) = vertices.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(lo, hi), v| (lo.min(Vec3::from(v.pos)), hi.max(Vec3::from(v.pos))));
        let buffer = self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("detail vertices"), contents: bytemuck::cast_slice(&vertices), usage: wgpu::BufferUsages::VERTEX });
        let id = DetailId(self.next_mesh);
        self.next_mesh += 1;
        self.details.insert(id, GpuDetail { detail, vertices: buffer, bounds: (lo, hi) });
        Ok(id)
    }

    /// Puts a colour texture on the graphics card (RGBA, 8 bits a channel, sRGB, top row first), with its mipmaps
    /// made the light-correct way (hearth-texture).
    pub fn add_texture(&mut self, rgba: &[u8], width: u32, height: u32) -> Result<TextureId, String> {
        if width == 0 || height == 0 || rgba.len() != (width * height * 4) as usize {
            return Err(format!("the texture's pixels don't match its size ({width} x {height})"));
        }
        let pixels: Vec<[u8; 4]> = rgba.as_chunks::<4>().0.to_vec();
        let view = upload_texture(&self.device, &self.queue, &pixels, width, height);
        let id = TextureId(self.next_mesh);
        self.next_mesh += 1;
        self.textures.insert(id, view);
        Ok(id)
    }

    pub fn remove_mesh(&mut self, id: MeshId) {
        self.meshes.remove(&id);
    }

    /// Changes the picture size.
    pub fn resize(&mut self, width: u32, height: u32) {
        if width > 0 && height > 0 && (width, height) != (self.width, self.height) {
            (self.color, self.depth) = targets(&self.device, width, height);
            (self.width, self.height) = (width, height);
        }
    }

    /// The box around everything in the scene.
    fn scene_bounds(&self, scene: &Scene) -> Option<(Vec3, Vec3)> {
        let mut b: Option<(Vec3, Vec3)> = None;
        let boxes = scene.objects.iter().filter_map(|o| self.meshes.get(&o.mesh).map(|m| (m.bounds, o.transform)));
        let detail_boxes = scene.details.iter().filter_map(|o| self.details.get(&o.detail).map(|d| (d.bounds, o.transform)));
        for ((lo, hi), transform) in boxes.chain(detail_boxes) {
            for k in 0..8 {
                let c = Vec3::new(if k & 1 == 0 { lo.x } else { hi.x }, if k & 2 == 0 { lo.y } else { hi.y }, if k & 4 == 0 { lo.z } else { hi.z });
                let p = transform.transform_point3(c);
                b = Some(match b {
                    Some((a, z)) => (a.min(p), z.max(p)),
                    None => (p, p),
                });
            }
        }
        b
    }

    /// The sun's view: an orthographic box around the whole scene, looking along the sun.
    fn sun_view(&self, scene: &Scene) -> Option<Mat4> {
        let sun = scene.sun?;
        let (lo, hi) = self.scene_bounds(scene)?;
        let centre = (lo + hi) * 0.5;
        let radius = ((hi - lo).length() * 0.5).max(0.01);
        let d = sun.direction.normalize_or(Vec3::NEG_Z);
        let up = if d.z.abs() > 0.99 { Vec3::Y } else { Vec3::Z };
        let eye = centre - d * radius * 2.0;
        let view = glam::camera::rh::view::look_at_mat4(eye, centre, up);
        let proj = glam::camera::rh::proj::directx::orthographic(-radius, radius, -radius, radius, 0.01, radius * 4.0);
        Some(proj * view)
    }

    fn frame_data(&self, scene: &Scene) -> GpuFrame {
        let mut lights = [GpuLight { pos_range: [0.0; 4], color: [0.0; 4] }; MAX_LIGHTS];
        for (g, l) in lights.iter_mut().zip(scene.lights.iter().take(MAX_LIGHTS)) {
            *g = GpuLight { pos_range: [l.position.x, l.position.y, l.position.z, l.range.max(0.01)], color: [l.color[0] * l.intensity, l.color[1] * l.intensity, l.color[2] * l.intensity, 1.0] };
        }
        let (sun_dir, sun_color) = match scene.sun {
            Some(s) => {
                let d = s.direction.normalize_or(Vec3::NEG_Z);
                ([d.x, d.y, d.z, 0.0], [s.color[0] * s.intensity, s.color[1] * s.intensity, s.color[2] * s.intensity, 1.0])
            }
            None => ([0.0, 0.0, -1.0, 0.0], [0.0; 4]),
        };
        let e = scene.camera.eye;
        let vp = scene.camera.view_proj(self.width as f32 / self.height as f32);
        let light = if scene.shadows { self.sun_view(scene) } else { None };
        let sky = scene.sky;
        let c3 = |c: [f32; 3], on: bool| [c[0], c[1], c[2], if on { 1.0 } else { 0.0 }];
        GpuFrame {
            view_proj: vp.to_cols_array_2d(),
            inv_view_proj: vp.inverse().to_cols_array_2d(),
            light_view_proj: light.unwrap_or(Mat4::IDENTITY).to_cols_array_2d(),
            camera: [e.x, e.y, e.z, 1.0],
            sun_dir,
            sun_color,
            ambient: [scene.ambient[0], scene.ambient[1], scene.ambient[2], 0.0],
            counts: [scene.lights.len().min(MAX_LIGHTS) as f32, scene.exposure, 0.0, 0.0],
            sky_zenith: c3(sky.map(|s| s.zenith).unwrap_or_default(), sky.is_some()),
            sky_horizon: c3(sky.map(|s| s.horizon).unwrap_or_default(), sky.is_some()),
            sky_ground: c3(sky.map(|s| s.ground).unwrap_or_default(), sky.is_some()),
            shadow: [if light.is_some() { 1.0 } else { 0.0 }, 0.0015, 1.0 / SHADOW_SIZE as f32, 0.0],
            rays: match (scene.god_rays, scene.sun) {
                (Some(g), Some(_)) => [g.density.max(0.0), g.steps.clamp(1, 128) as f32, g.toward_sun.clamp(-0.95, 0.95), g.strength.max(0.0)],
                _ => [0.0; 4],
            },
            rays_more: [scene.god_rays.map(|g| g.distance.max(0.1)).unwrap_or(0.0), 0.0, 0.0, 0.0],
            lights,
        }
    }

    fn object_group(&self, transform: &Mat4, m: &Material) -> Result<wgpu::BindGroup, String> {
        let data = GpuObject {
            model: transform.to_cols_array_2d(),
            normal: transform.inverse().transpose().to_cols_array_2d(),
            color: m.color,
            params: [m.metallic, m.roughness, 0.0, 0.0],
            emissive: [m.emissive[0], m.emissive[1], m.emissive[2], 0.0],
        };
        let buffer = self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("object"), contents: bytemuck::bytes_of(&data), usage: wgpu::BufferUsages::UNIFORM });
        let view = match m.texture {
            Some(t) => self.textures.get(&t).ok_or_else(|| format!("a material uses texture {t:?}, which isn't on the graphics card"))?,
            None => &self.white,
        };
        Ok(self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("object"),
            layout: &self.object_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: buffer.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(view) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&self.sampler) },
            ],
        }))
    }

    /// Everything to draw this frame: (vertices, indices, index count, object bind group).
    fn items(&self, scene: &Scene) -> Result<Vec<(wgpu::Buffer, wgpu::Buffer, u32, wgpu::BindGroup)>, String> {
        let mut items = Vec::with_capacity(scene.objects.len() + scene.details.len());
        for o in &scene.objects {
            let mesh = self.meshes.get(&o.mesh).ok_or_else(|| format!("a thing in the scene uses mesh {:?}, which isn't on the graphics card", o.mesh))?;
            items.push((mesh.vertices.clone(), mesh.indices.clone(), mesh.count, self.object_group(&o.transform, &o.material)?));
        }
        // Detail models: the pieces for this camera, picked now (full detail near, far fewer triangles far away).
        let pixels_per_unit = self.height as f32 * 0.5 / (scene.camera.fov.to_radians() * 0.5).tan();
        for o in &scene.details {
            let d = self.details.get(&o.detail).ok_or_else(|| format!("a thing in the scene uses detail model {:?}, which isn't on the graphics card", o.detail))?;
            let camera = o.transform.inverse().transform_point3(scene.camera.eye);
            let chosen = d.detail.cut_for_camera(camera.to_array(), pixels_per_unit, o.max_pixels.max(0.05));
            let mut indices: Vec<u32> = Vec::new();
            for c in chosen {
                let c = &d.detail.clusters[c];
                indices.extend_from_slice(&d.detail.indices[c.first_index as usize..(c.first_index + c.index_count) as usize]);
            }
            if indices.is_empty() {
                continue;
            }
            let ib = self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("detail indices"), contents: bytemuck::cast_slice(&indices), usage: wgpu::BufferUsages::INDEX });
            items.push((d.vertices.clone(), ib, indices.len() as u32, self.object_group(&o.transform, &o.material)?));
        }
        Ok(items)
    }

    /// Draws the scene into the renderer's picture.
    fn draw(&mut self, scene: &Scene, encoder: &mut wgpu::CommandEncoder) -> Result<(), String> {
        self.queue.write_buffer(&self.frame_buffer, 0, bytemuck::bytes_of(&self.frame_data(scene)));
        let items = self.items(scene)?;
        self.last_triangles = items.iter().map(|i| i.2 as u64 / 3).sum();
        // The sun's shadow map first.
        {
            let shadow_view = self.shadow_map.create_view(&wgpu::TextureViewDescriptor::default());
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("hearth shadow"),
                color_attachments: &[],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &shadow_view,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                occlusion_query_set: None,
                timestamp_writes: None,
                multiview_mask: None,
            });
            if scene.shadows && scene.sun.is_some() {
                pass.set_pipeline(&self.shadow_pipeline);
                pass.set_bind_group(0, &self.shadow_frame_group, &[]);
                for (vb, ib, count, group) in &items {
                    pass.set_bind_group(1, group, &[]);
                    pass.set_vertex_buffer(0, vb.slice(..));
                    pass.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint32);
                    pass.draw_indexed(0..*count, 0, 0..1);
                }
            }
        }
        let color_view = self.color.create_view(&wgpu::TextureViewDescriptor::default());
        let depth_view = self.depth.create_view(&wgpu::TextureViewDescriptor::default());
        let c = scene.clear;
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("hearth main"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &color_view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color { r: c[0] as f64, g: c[1] as f64, b: c[2] as f64, a: c[3] as f64 }), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: &depth_view,
                depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Discard }),
                stencil_ops: None,
            }),
            occlusion_query_set: None,
            timestamp_writes: None,
            multiview_mask: None,
        });
        pass.set_bind_group(0, &self.frame_group, &[]);
        if scene.sky.is_some() {
            pass.set_pipeline(&self.sky_pipeline);
            pass.draw(0..3, 0..1);
        }
        pass.set_pipeline(&self.pipeline);
        for (vb, ib, count, group) in &items {
            pass.set_bind_group(1, group, &[]);
            pass.set_vertex_buffer(0, vb.slice(..));
            pass.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint32);
            pass.draw_indexed(0..*count, 0, 0..1);
        }
        Ok(())
    }

    /// Draws the scene and gives back the picture: RGBA, 8 bits a channel, sRGB, top row first.
    pub fn render(&mut self, scene: &Scene) -> Result<Vec<u8>, String> {
        let mut encoder = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("hearth frame") });
        self.draw(scene, &mut encoder)?;
        // Rows are copied 256 bytes aligned; the padding is taken off after.
        let row = self.width * 4;
        let padded = row.div_ceil(256) * 256;
        let readback = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("readback"),
            size: (padded * self.height) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo { texture: &self.color, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::TexelCopyBufferInfo { buffer: &readback, layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(padded), rows_per_image: Some(self.height) } },
            wgpu::Extent3d { width: self.width, height: self.height, depth_or_array_layers: 1 },
        );
        self.queue.submit(Some(encoder.finish()));
        let slice = readback.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        self.device.poll(wgpu::PollType::wait_indefinitely()).map_err(|e| format!("the graphics card stopped: {e}"))?;
        rx.recv().map_err(|_| "the picture never came back".to_string())?.map_err(|e| format!("the picture couldn't be read: {e}"))?;
        let mut out = Vec::with_capacity((row * self.height) as usize);
        {
            let view = slice.get_mapped_range().map_err(|e| format!("the picture couldn't be read: {e}"))?;
            for y in 0..self.height as usize {
                let start = y * padded as usize;
                out.extend_from_slice(&view[start..start + row as usize]);
            }
        }
        readback.unmap();
        Ok(out)
    }
}

/// A texture with its whole mip chain (made by hearth-texture: averaged as light, sharp without ringing).
fn upload_texture(device: &wgpu::Device, queue: &wgpu::Queue, pixels: &[[u8; 4]], width: u32, height: u32) -> wgpu::TextureView {
    use hearth_texture::mips::{chain, Image, Options};
    let top = Image { width: width as usize, height: height as usize, pixels: pixels.iter().map(|p| [p[0] as f32 / 255.0, p[1] as f32 / 255.0, p[2] as f32 / 255.0, p[3] as f32 / 255.0]).collect() };
    let levels = chain(&top, &Options::default());
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("hearth texture"),
        size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
        mip_level_count: levels.len() as u32,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    for (mip, level) in levels.iter().enumerate() {
        let bytes: Vec<u8> = if mip == 0 {
            pixels.iter().flatten().copied().collect()
        } else {
            level.pixels.iter().flat_map(|p| p.map(|c| (c.clamp(0.0, 1.0) * 255.0 + 0.5) as u8)).collect()
        };
        queue.write_texture(
            wgpu::TexelCopyTextureInfo { texture: &texture, mip_level: mip as u32, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            &bytes,
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(level.width as u32 * 4), rows_per_image: Some(level.height as u32) },
            wgpu::Extent3d { width: level.width as u32, height: level.height as u32, depth_or_array_layers: 1 },
        );
    }
    texture.create_view(&wgpu::TextureViewDescriptor::default())
}

/// Saves an RGBA picture as a PNG.
pub fn save_png(path: &std::path::Path, rgba: &[u8], width: u32, height: u32) -> Result<(), String> {
    if let Some(d) = path.parent() {
        std::fs::create_dir_all(d).map_err(|e| e.to_string())?;
    }
    let f = std::fs::File::create(path).map_err(|e| format!("can't save {}: {e}", path.display()))?;
    let mut enc = png::Encoder::new(std::io::BufWriter::new(f), width, height);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut w = enc.write_header().map_err(|e| e.to_string())?;
    w.write_image_data(rgba).map_err(|e| e.to_string())
}
