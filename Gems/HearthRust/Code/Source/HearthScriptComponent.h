/*
 * Hearth Script on an entity: runs a .hs file, gives it the entity to move, and passes game events to it.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/std/string/string.h>

#include <HearthRust/HearthRustBus.h>

struct HsEngine;

namespace HearthRust
{
    class HearthScriptComponent
        : public AZ::Component
        , private AZ::TickBus::Handler
        , private HearthScriptRequestBus::Handler
    {
    public:
        AZ_COMPONENT(HearthScriptComponent, "{0F4B7D63-2E4A-4C58-8E0B-7B4E2A61C3D7}");
        static void Reflect(AZ::ReflectContext* context);

        ~HearthScriptComponent() override;

    protected:
        void Activate() override;
        void Deactivate() override;

    private:
        void OnTick(float deltaTime, AZ::ScriptTimePoint time) override;

        AZStd::string Fire(const AZStd::string& event, const AZStd::string& argsJson) override;
        AZStd::string Reload() override;

        AZStd::string FullPath() const;
        void Report(const char* mistake);
        void PassOnOutput();
        static const char* Native(void* user, const char* name, const char* argsJson);
        const char* RunNative(const AZStd::string& name, const AZStd::string& argsJson);

        AZStd::string m_script;       // the .hs file, from the project folder ("Scripts/door.hs")
        HsEngine* m_engine = nullptr;
        AZ::u64 m_modified = 0;
        float m_sinceCheck = 0.0f;
        float m_time = 0.0f;
        bool m_started = false;
        AZStd::string m_reply;        // what a game function gives back (kept until the next call)
        AZStd::string m_lastMistake;
    };
} // namespace HearthRust
