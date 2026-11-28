/*
 * Nanite-style detail on an entity: shows a model (.glb, .obj or .hdetail) at full detail up close and with far
 * fewer triangles far away, piece by piece, with no cracks.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/string/string.h>

#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>
#include <Atom/RPI.Reflect/Buffer/BufferAsset.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>

struct HdDetail;

namespace HearthRust
{
    struct DetailLoad;

    class HearthDetailComponent
        : public AZ::Component
        , private AZ::TickBus::Handler
        , private AZ::TransformNotificationBus::Handler
    {
    public:
        AZ_COMPONENT(HearthDetailComponent, "{5A8C1E44-9F27-4B6D-A3C2-2E7F9B1D6C85}");
        static void Reflect(AZ::ReflectContext* context);

        ~HearthDetailComponent() override;

    protected:
        void Activate() override;
        void Deactivate() override;

    private:
        void OnTick(float deltaTime, AZ::ScriptTimePoint time) override;
        void OnTransformChanged(const AZ::Transform& local, const AZ::Transform& world) override;

        bool MakeVertexBuffers();
        bool ShowSelection();
        void Free();

        struct Stream
        {
            const char* m_semantic = "";
            AZ::Data::Asset<AZ::RPI::BufferAsset> m_asset;
            AZ::RHI::BufferViewDescriptor m_view;
        };

        AZStd::string m_model;                                                  // from the project folder
        AZStd::string m_materialPath = "materials/presets/pbr/default_grid.azmaterial";
        float m_maxPixels = 1.0f;                                               // how big a mistake may look

        AZStd::shared_ptr<DetailLoad> m_load;
        HdDetail* m_detail = nullptr;
        AZStd::vector<Stream> m_streams;
        AZ::Aabb m_aabb = AZ::Aabb::CreateNull();
        AZ::Data::Asset<AZ::RPI::MaterialAsset> m_materialAsset;
        AZ::Data::Instance<AZ::RPI::Material> m_material;
        AZ::Render::MeshFeatureProcessorInterface* m_meshes = nullptr;
        AZ::Render::MeshFeatureProcessorInterface::MeshHandle m_mesh;
        AZ::Transform m_world = AZ::Transform::CreateIdentity();
        float m_sinceSelect = 0.0f;
        bool m_reported = false;
    };
} // namespace HearthRust
