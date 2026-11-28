//! Atom in Rust: Hearth Engine's renderer, on wgpu (Vulkan on Linux, any graphics card, or Mesa's software
//! driver when there's none, which is how GitHub renders the test pictures).
//!
//! What it draws today: meshes with physically based materials (colour, metal, roughness, glow), a sun, up to 16
//! point lights, ambient light, depth, ACES tone mapping, into a window or an image. Next: textures, shadows, the
//! Nanite-style detail from hearth-detail, then the rest of what Atom does.

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
}

impl Default for Material {
    fn default() -> Material {
        Material { color: [0.8, 0.8, 0.8, 1.0], metallic: 0.0, roughness: 0.5, emissive: [0.0; 3] }
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

#[derive(Clone, Debug)]
pub struct Scene {
    pub camera: Camera,
    pub sun: Option<Sun>,
    pub ambient: [f32; 3],
    pub lights: Vec<PointLight>,
    pub objects: Vec<Object>,
    /// Linear background colour.
    pub clear: [f32; 4],
    pub exposure: f32,
}

impl Default for Scene {
    fn default() -> Scene {
        Scene { camera: Camera::default(), sun: None, ambient: [0.03; 3], lights: Vec::new(), objects: Vec::new(), clear: [0.0, 0.0, 0.0, 1.0], exposure: 1.0 }
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
    camera: [f32; 4],
    sun_dir: [f32; 4],
    sun_color: [f32; 4],
    ambient: [f32; 4],
    counts: [f32; 4],
    lights: [GpuLight; MAX_LIGHTS],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuObject {
    model: [[f32; 4]; 4],
    normal: [[f32; 4]; 4],
    color: [f32; 4],
    params: [f32; 4],
    emissive: [f32; 4],
}

struct GpuMesh {
    vertices: wgpu::Buffer,
    indices: wgpu::Buffer,
    count: u32,
}

pub struct Renderer {
    device: wgpu::Device,
    queue: wgpu::Queue,
    pipeline: wgpu::RenderPipeline,
    object_layout: wgpu::BindGroupLayout,
    frame_buffer: wgpu::Buffer,
    frame_group: wgpu::BindGroup,
    meshes: HashMap<MeshId, GpuMesh>,
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
        let frame_layout = uniform_layout(&device, "frame", wgpu::ShaderStages::VERTEX_FRAGMENT);
        let object_layout = uniform_layout(&device, "object", wgpu::ShaderStages::VERTEX_FRAGMENT);
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("hearth pbr"), bind_group_layouts: &[Some(&frame_layout), Some(&object_layout)], immediate_size: 0 });
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
        let frame_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("frame"),
            size: std::mem::size_of::<GpuFrame>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let frame_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("frame"),
            layout: &frame_layout,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: frame_buffer.as_entire_binding() }],
        });
        let (color, depth) = targets(&device, width, height);
        Ok(Renderer { device, queue, pipeline, object_layout, frame_buffer, frame_group, meshes: HashMap::new(), next_mesh: 1, width, height, color, depth, adapter_name })
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
        self.meshes.insert(id, GpuMesh { vertices, indices, count: mesh.indices.len() as u32 });
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
        GpuFrame {
            view_proj: scene.camera.view_proj(self.width as f32 / self.height as f32).to_cols_array_2d(),
            camera: [e.x, e.y, e.z, 1.0],
            sun_dir,
            sun_color,
            ambient: [scene.ambient[0], scene.ambient[1], scene.ambient[2], 0.0],
            counts: [scene.lights.len().min(MAX_LIGHTS) as f32, scene.exposure, 0.0, 0.0],
            lights,
        }
    }

    /// Draws the scene into the renderer's picture.
    fn draw(&mut self, scene: &Scene, encoder: &mut wgpu::CommandEncoder) -> Result<(), String> {
        self.queue.write_buffer(&self.frame_buffer, 0, bytemuck::bytes_of(&self.frame_data(scene)));
        let mut groups = Vec::with_capacity(scene.objects.len());
        for o in &scene.objects {
            if !self.meshes.contains_key(&o.mesh) {
                return Err(format!("a thing in the scene uses mesh {:?}, which isn't on the graphics card", o.mesh));
            }
            let m = &o.material;
            let data = GpuObject {
                model: o.transform.to_cols_array_2d(),
                normal: o.transform.inverse().transpose().to_cols_array_2d(),
                color: m.color,
                params: [m.metallic, m.roughness, 0.0, 0.0],
                emissive: [m.emissive[0], m.emissive[1], m.emissive[2], 0.0],
            };
            let buffer = self.device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: Some("object"), contents: bytemuck::bytes_of(&data), usage: wgpu::BufferUsages::UNIFORM });
            let group = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("object"),
                layout: &self.object_layout,
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: buffer.as_entire_binding() }],
            });
            groups.push((group, buffer));
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
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &self.frame_group, &[]);
        for (o, (group, _)) in scene.objects.iter().zip(&groups) {
            let mesh = &self.meshes[&o.mesh];
            pass.set_bind_group(1, group, &[]);
            pass.set_vertex_buffer(0, mesh.vertices.slice(..));
            pass.set_index_buffer(mesh.indices.slice(..), wgpu::IndexFormat::Uint32);
            pass.draw_indexed(0..mesh.count, 0, 0..1);
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
