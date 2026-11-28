/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include "HearthDetailComponent.h"

#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/ViewportContextBus.h>
#include <Atom/RPI.Reflect/Asset/AssetUtils.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/std/parallel/lock.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/parallel/thread.h>
#include <AzCore/Utils/Utils.h>

#include <hearth_ffi.h>

namespace HearthRust
{
    //! Building the detail levels of a big model takes a moment, so it happens off the game's thread. If the
    //! component goes away first, whoever finishes last frees the result.
    struct DetailLoad
    {
        AZStd::mutex m_lock;
        bool m_done = false;
        bool m_abandoned = false;
        HdDetail* m_detail = nullptr;
        AZStd::string m_error;
    };

    static constexpr AZ::RPI::ModelMaterialSlot::StableId DetailMaterialSlot = 1;

    void HearthDetailComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<HearthDetailComponent, AZ::Component>()
                ->Version(1)
                ->Field("Model", &HearthDetailComponent::m_model)
                ->Field("Material", &HearthDetailComponent::m_materialPath)
                ->Field("MaxPixels", &HearthDetailComponent::m_maxPixels);
            if (auto* edit = serialize->GetEditContext())
            {
                edit->Class<HearthDetailComponent>(
                        "Detail Mesh", "Shows a model with Nanite-style detail: full detail up close, far fewer triangles far away, no cracks.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "Hearth")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu, AZ_CRC_CE("Game"))
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(AZ::Edit::UIHandlers::Default, &HearthDetailComponent::m_model, "Model",
                        "A .glb (like Hearth Forge makes), .obj or .hdetail file, from the project folder.")
                    ->DataElement(AZ::Edit::UIHandlers::Default, &HearthDetailComponent::m_materialPath, "Material",
                        "The material's product path (for example materials/presets/pbr/default_grid.azmaterial).")
                    ->DataElement(AZ::Edit::UIHandlers::Default, &HearthDetailComponent::m_maxPixels, "Detail",
                        "How many pixels a simplification may move the shape on screen. Smaller: more detail, more triangles.")
                    ->Attribute(AZ::Edit::Attributes::Min, 0.1f)
                    ->Attribute(AZ::Edit::Attributes::Max, 16.0f);
            }
        }
    }

    HearthDetailComponent::~HearthDetailComponent()
    {
        Free();
    }

    void HearthDetailComponent::Activate()
    {
        AZ::IO::FixedMaxPath path = AZ::Utils::GetProjectPath();
        path /= m_model.c_str();
        const AZStd::string full(path.c_str());

        auto load = AZStd::make_shared<DetailLoad>();
        m_load = load;
        AZStd::thread worker(
            [load, full]()
            {
                HdDetail* d = hd_open(full.c_str());
                if (!d)
                {
                    const char* why = hd_last_error();
                    load->m_error = why ? why : "the model couldn't be opened";
                }
                AZStd::lock_guard<AZStd::mutex> hold(load->m_lock);
                load->m_detail = d;
                load->m_done = true;
                if (load->m_abandoned && load->m_detail)
                {
                    hd_free(load->m_detail);
                    load->m_detail = nullptr;
                }
            });
        worker.detach();

        m_materialAsset = AZ::RPI::AssetUtils::LoadAssetByProductPath<AZ::RPI::MaterialAsset>(m_materialPath.c_str());
        if (m_materialAsset.IsReady())
        {
            m_material = AZ::RPI::Material::FindOrCreate(m_materialAsset);
        }
        else
        {
            AZ_Warning("Detail Mesh", false, "Can't load the material %s", m_materialPath.c_str());
        }
        AZ::TransformBus::EventResult(m_world, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
        m_meshes = AZ::RPI::Scene::GetFeatureProcessorForEntity<AZ::Render::MeshFeatureProcessorInterface>(GetEntityId());
        m_reported = false;
        AZ::TransformNotificationBus::Handler::BusConnect(GetEntityId());
        AZ::TickBus::Handler::BusConnect();
    }

    void HearthDetailComponent::Deactivate()
    {
        AZ::TickBus::Handler::BusDisconnect();
        AZ::TransformNotificationBus::Handler::BusDisconnect();
        Free();
    }

    void HearthDetailComponent::Free()
    {
        if (m_meshes && m_mesh.IsValid())
        {
            m_meshes->ReleaseMesh(m_mesh);
        }
        m_streams.clear();
        m_material = {};
        m_materialAsset = {};
        if (m_detail)
        {
            hd_free(m_detail);
            m_detail = nullptr;
        }
        if (m_load)
        {
            AZStd::lock_guard<AZStd::mutex> hold(m_load->m_lock);
            m_load->m_abandoned = true;
            if (m_load->m_done && m_load->m_detail)
            {
                hd_free(m_load->m_detail);
                m_load->m_detail = nullptr;
            }
        }
        m_load.reset();
    }

    void HearthDetailComponent::OnTransformChanged([[maybe_unused]] const AZ::Transform& local, const AZ::Transform& world)
    {
        m_world = world;
        if (m_meshes && m_mesh.IsValid())
        {
            m_meshes->SetTransform(m_mesh, m_world);
        }
    }

    static AZ::Data::Asset<AZ::RPI::BufferAsset> MakeBuffer(
        const void* data, uint32_t count, uint32_t elementSize, AZ::RHI::Format format, AZ::RHI::BufferViewDescriptor& view)
    {
        view = AZ::RHI::BufferViewDescriptor::CreateTyped(0, count, format);
        AZ::RHI::BufferDescriptor descriptor;
        descriptor.m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly | AZ::RHI::BufferBindFlags::ShaderRead;
        descriptor.m_byteCount = static_cast<uint64_t>(count) * elementSize;
        descriptor.m_alignment = elementSize;

        AZ::RPI::BufferAssetCreator creator;
        creator.Begin(AZ::Uuid::CreateRandom());
        creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
        creator.SetBuffer(data, descriptor.m_byteCount, descriptor);
        creator.SetBufferViewDescriptor(view);
        AZ::Data::Asset<AZ::RPI::BufferAsset> asset;
        if (!creator.End(asset))
        {
            return {};
        }
        return asset;
    }

    bool HearthDetailComponent::MakeVertexBuffers()
    {
        const uint32_t count = hd_vertices(m_detail, nullptr);
        if (count == 0)
        {
            return false;
        }
        AZStd::vector<float> all(static_cast<size_t>(count) * 12);
        hd_vertices(m_detail, all.data());

        AZStd::vector<float> positions, normals, uvs, tangents, bitangents;
        positions.reserve(count * 3);
        normals.reserve(count * 3);
        uvs.reserve(count * 2);
        tangents.reserve(count * 4);
        bitangents.reserve(count * 3);
        for (uint32_t i = 0; i < count; ++i)
        {
            const float* v = &all[static_cast<size_t>(i) * 12];
            positions.insert(positions.end(), { v[0], v[1], v[2] });
            normals.insert(normals.end(), { v[3], v[4], v[5] });
            uvs.insert(uvs.end(), { v[6], v[7] });
            tangents.insert(tangents.end(), { v[8], v[9], v[10], v[11] });
            const AZ::Vector3 n(v[3], v[4], v[5]);
            const AZ::Vector3 t(v[8], v[9], v[10]);
            const AZ::Vector3 b = n.Cross(t) * v[11];
            bitangents.insert(bitangents.end(), { b.GetX(), b.GetY(), b.GetZ() });
        }

        float box[6];
        hd_bounds(m_detail, box);
        m_aabb = AZ::Aabb::CreateFromMinMax(AZ::Vector3(box[0], box[1], box[2]), AZ::Vector3(box[3], box[4], box[5]));

        auto add = [this, count](const char* semantic, const AZStd::vector<float>& data, uint32_t floats, AZ::RHI::Format format)
        {
            Stream s;
            s.m_semantic = semantic;
            s.m_asset = MakeBuffer(data.data(), count, floats * sizeof(float), format, s.m_view);
            if (s.m_asset)
            {
                m_streams.push_back(s);
            }
        };
        m_streams.clear();
        add("POSITION", positions, 3, AZ::RHI::Format::R32G32B32_FLOAT);
        add("NORMAL", normals, 3, AZ::RHI::Format::R32G32B32_FLOAT);
        add("TANGENT", tangents, 4, AZ::RHI::Format::R32G32B32A32_FLOAT);
        add("BITANGENT", bitangents, 3, AZ::RHI::Format::R32G32B32_FLOAT);
        add("UV", uvs, 2, AZ::RHI::Format::R32G32_FLOAT);
        return m_streams.size() == 5;
    }

    bool HearthDetailComponent::ShowSelection()
    {
        uint32_t indexCount = 0;
        const uint32_t* indices = hd_selected(m_detail, &indexCount);
        if (!indices || indexCount == 0 || !m_meshes || !m_materialAsset)
        {
            return false;
        }
        AZ::RHI::BufferViewDescriptor indexView;
        auto indexAsset = MakeBuffer(indices, indexCount, sizeof(uint32_t), AZ::RHI::Format::R32_UINT, indexView);
        if (!indexAsset)
        {
            return false;
        }

        AZ::RPI::ModelLodAssetCreator lod;
        lod.Begin(AZ::Data::AssetId(AZ::Uuid::CreateRandom()));
        lod.SetLodIndexBuffer(indexAsset);
        for (const Stream& s : m_streams)
        {
            lod.AddLodStreamBuffer(s.m_asset);
        }
        lod.BeginMesh();
        lod.SetMeshAabb(m_aabb);
        lod.SetMeshMaterialSlot(DetailMaterialSlot);
        lod.SetMeshIndexBuffer(AZ::RPI::BufferAssetView(indexAsset, indexView));
        for (const Stream& s : m_streams)
        {
            lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(AZ::Name(s.m_semantic)), AZ::Name(), AZ::RPI::BufferAssetView(s.m_asset, s.m_view));
        }
        lod.EndMesh();
        AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;
        if (!lod.End(lodAsset))
        {
            return false;
        }

        AZ::RPI::ModelAssetCreator model;
        model.Begin(AZ::Data::AssetId(AZ::Uuid::CreateRandom()));
        model.SetName("HearthDetail");
        model.AddLodAsset(AZStd::move(lodAsset));
        AZ::RPI::ModelMaterialSlot slot;
        slot.m_stableId = DetailMaterialSlot;
        slot.m_defaultMaterialAsset = m_materialAsset;
        model.AddMaterialSlot(slot);
        AZ::Data::Asset<AZ::RPI::ModelAsset> modelAsset;
        if (!model.End(modelAsset))
        {
            return false;
        }

        // The new version goes up before the old one comes down, so nothing blinks.
        AZ::Render::MeshHandleDescriptor descriptor(modelAsset, m_material);
        descriptor.m_entityId = GetEntityId();
        auto next = m_meshes->AcquireMesh(descriptor);
        m_meshes->SetTransform(next, m_world);
        if (m_mesh.IsValid())
        {
            m_meshes->ReleaseMesh(m_mesh);
        }
        m_mesh = AZStd::move(next);
        return true;
    }

    void HearthDetailComponent::OnTick(float deltaTime, [[maybe_unused]] AZ::ScriptTimePoint time)
    {
        if (!m_detail)
        {
            if (!m_load)
            {
                return;
            }
            AZStd::string error;
            {
                AZStd::lock_guard<AZStd::mutex> hold(m_load->m_lock);
                if (!m_load->m_done)
                {
                    return;
                }
                m_detail = m_load->m_detail;
                m_load->m_detail = nullptr;
                error = m_load->m_error;
            }
            m_load.reset();
            if (!m_detail)
            {
                AZ_Warning("Detail Mesh", m_reported, "%s", error.c_str());
                m_reported = true;
                AZ::TickBus::Handler::BusDisconnect();
                return;
            }
            if (!MakeVertexBuffers())
            {
                AZ_Warning("Detail Mesh", false, "%s has no vertices", m_model.c_str());
                AZ::TickBus::Handler::BusDisconnect();
                return;
            }
            AZ_TracePrintf("Detail Mesh", "%s: %s\n", m_model.c_str(), hd_describe(m_detail));
            m_sinceSelect = 1.0f;
        }

        // Ten times a second is plenty: pieces only change as the camera moves.
        m_sinceSelect += deltaTime;
        if (m_sinceSelect < 0.1f)
        {
            return;
        }
        m_sinceSelect = 0.0f;

        auto* contexts = AZ::RPI::ViewportContextRequests::Get();
        auto context = contexts ? contexts->GetDefaultViewportContext() : nullptr;
        if (!context || !context->GetDefaultView())
        {
            return;
        }
        auto view = context->GetDefaultView();
        const AZ::Vector3 cameraWorld = view->GetCameraTransform().GetTranslation();
        const AZ::Vector3 camera = m_world.GetInverse().TransformPoint(cameraWorld);
        const float height = static_cast<float>(context->GetViewportSize().m_height);
        // Pixels per unit, one unit away. The error and the distance scale together, so the model's size cancels.
        const float pixelsPerUnit = AZ::GetMax(1.0f, height * 0.5f * view->GetViewToClipMatrix().GetElement(1, 1));
        if (hd_select(m_detail, camera.GetX(), camera.GetY(), camera.GetZ(), pixelsPerUnit, m_maxPixels))
        {
            ShowSelection();
        }
    }
} // namespace HearthRust
