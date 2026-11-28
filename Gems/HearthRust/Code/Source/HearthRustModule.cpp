/*
 * Hearth Engine's Rust parts as a gem.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/Module/Module.h>

#include "HearthDetailComponent.h"
#include "HearthScriptComponent.h"

namespace HearthRust
{
    class HearthRustModule : public AZ::Module
    {
    public:
        AZ_RTTI(HearthRustModule, "{6C2E8E52-7B53-4E52-9A84-61E7E36A2D11}", AZ::Module);
        AZ_CLASS_ALLOCATOR(HearthRustModule, AZ::SystemAllocator);

        HearthRustModule()
        {
            m_descriptors.insert(
                m_descriptors.end(),
                {
                    HearthScriptComponent::CreateDescriptor(),
                    HearthDetailComponent::CreateDescriptor(),
                });
        }
    };
} // namespace HearthRust

#if defined(O3DE_GEM_NAME)
AZ_DECLARE_MODULE_CLASS(AZ_JOIN(Gem_, O3DE_GEM_NAME), HearthRust::HearthRustModule)
#else
AZ_DECLARE_MODULE_CLASS(Gem_HearthRust, HearthRust::HearthRustModule)
#endif
