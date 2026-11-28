/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include "HearthScriptComponent.h"

#include <AzCore/Component/TransformBus.h>
#include <AzCore/IO/FileIO.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/JSON/document.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Utils/Utils.h>

#include <hearth_ffi.h>

namespace HearthRust
{
    // Script Canvas and Lua can listen for what scripts send.
    class HearthScriptNotificationHandler
        : public HearthScriptNotificationBus::Handler
        , public AZ::BehaviorEBusHandler
    {
    public:
        AZ_EBUS_BEHAVIOR_BINDER(
            HearthScriptNotificationHandler,
            "{9D1F3C52-5C1B-4C2A-8E9D-3A6B1F7E2C40}",
            AZ::SystemAllocator,
            OnScriptSent,
            OnScriptMistake,
            OnScriptSaid);

        void OnScriptSent(const AZStd::string& name, const AZStd::string& argsJson) override
        {
            Call(FN_OnScriptSent, name, argsJson);
        }
        void OnScriptMistake(const AZStd::string& message) override
        {
            Call(FN_OnScriptMistake, message);
        }
        void OnScriptSaid(const AZStd::string& line) override
        {
            Call(FN_OnScriptSaid, line);
        }
    };

    void HearthScriptComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<HearthScriptComponent, AZ::Component>()->Version(1)->Field("Script", &HearthScriptComponent::m_script);
            if (auto* edit = serialize->GetEditContext())
            {
                edit->Class<HearthScriptComponent>("Hearth Script", "Runs a Hearth Script (.hs) file on this entity.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "Hearth")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu, AZ_CRC_CE("Game"))
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &HearthScriptComponent::m_script, "Script",
                        "The .hs file, from the project folder (for example Scripts/door.hs). Saving it reloads it.");
            }
        }
        if (auto* behavior = azrtti_cast<AZ::BehaviorContext*>(context))
        {
            behavior->EBus<HearthScriptRequestBus>("HearthScriptRequestBus")
                ->Attribute(AZ::Script::Attributes::Category, "Hearth")
                ->Event("Fire", &HearthScriptRequestBus::Events::Fire)
                ->Event("Reload", &HearthScriptRequestBus::Events::Reload);
            behavior->EBus<HearthScriptNotificationBus>("HearthScriptNotificationBus")
                ->Attribute(AZ::Script::Attributes::Category, "Hearth")
                ->Handler<HearthScriptNotificationHandler>();
        }
    }

    HearthScriptComponent::~HearthScriptComponent()
    {
        if (m_engine)
        {
            hs_free(m_engine);
            m_engine = nullptr;
        }
    }

    AZStd::string HearthScriptComponent::FullPath() const
    {
        AZ::IO::FixedMaxPath path = AZ::Utils::GetProjectPath();
        path /= m_script.c_str();
        return AZStd::string(path.c_str());
    }

    void HearthScriptComponent::Report(const char* mistake)
    {
        if (!mistake || !*mistake)
        {
            return;
        }
        // Each mistake is shown once, not every frame.
        if (m_lastMistake != mistake)
        {
            m_lastMistake = mistake;
            AZ_Warning("Hearth Script", false, "%s", mistake);
        }
        HearthScriptNotificationBus::Event(GetEntityId(), &HearthScriptNotifications::OnScriptMistake, AZStd::string(mistake));
    }

    void HearthScriptComponent::PassOnOutput()
    {
        const char* out = hs_take_output(m_engine);
        if (!out || !*out)
        {
            return;
        }
        AZStd::string all(out);
        size_t start = 0;
        while (start <= all.size())
        {
            size_t end = all.find('\n', start);
            if (end == AZStd::string::npos)
            {
                end = all.size();
            }
            AZStd::string line = all.substr(start, end - start);
            AZ_TracePrintf("Hearth Script", "%s\n", line.c_str());
            HearthScriptNotificationBus::Event(GetEntityId(), &HearthScriptNotifications::OnScriptSaid, line);
            start = end + 1;
        }
    }

    AZStd::string HearthScriptComponent::Reload()
    {
        if (!m_engine || m_script.empty())
        {
            return "no script is set";
        }
        const AZStd::string path = FullPath();
        auto text = AZ::Utils::ReadFile<AZStd::string>(path);
        if (!text.IsSuccess())
        {
            AZStd::string why = AZStd::string::format("can't open %s", path.c_str());
            Report(why.c_str());
            return why;
        }
        m_modified = AZ::IO::FileIOBase::GetInstance() ? AZ::IO::FileIOBase::GetInstance()->ModificationTime(path.c_str()) : 0;
        const char* mistake = hs_load(m_engine, text.GetValue().c_str());
        PassOnOutput();
        if (mistake)
        {
            AZStd::string why(mistake);
            Report(why.c_str());
            return why;
        }
        m_lastMistake.clear();
        return {};
    }

    void HearthScriptComponent::Activate()
    {
        m_engine = hs_new(m_script.c_str());
        // What a script can do with its entity. More come from the game through send().
        for (const char* name : { "position", "move_to", "send", "time" })
        {
            hs_register(m_engine, name, &HearthScriptComponent::Native, this);
        }
        Reload();
        m_started = false;
        m_time = 0.0f;
        HearthScriptRequestBus::Handler::BusConnect(GetEntityId());
        AZ::TickBus::Handler::BusConnect();
    }

    void HearthScriptComponent::Deactivate()
    {
        AZ::TickBus::Handler::BusDisconnect();
        HearthScriptRequestBus::Handler::BusDisconnect();
        if (m_engine)
        {
            hs_free(m_engine);
            m_engine = nullptr;
        }
    }

    void HearthScriptComponent::OnTick(float deltaTime, [[maybe_unused]] AZ::ScriptTimePoint time)
    {
        if (!m_engine)
        {
            return;
        }
        m_time += deltaTime;
        if (!m_started)
        {
            m_started = true;
            Report(hs_fire(m_engine, "start", "[]"));
            PassOnOutput();
        }
        if (hs_listens(m_engine, "tick"))
        {
            const AZStd::string args = AZStd::string::format("[%f]", deltaTime);
            Report(hs_fire(m_engine, "tick", args.c_str()));
            PassOnOutput();
        }
        // Saved again? Load it (the last good version keeps running if the new one has a mistake).
        m_sinceCheck += deltaTime;
        if (m_sinceCheck >= 1.0f && AZ::IO::FileIOBase::GetInstance())
        {
            m_sinceCheck = 0.0f;
            const AZ::u64 modified = AZ::IO::FileIOBase::GetInstance()->ModificationTime(FullPath().c_str());
            if (modified != 0 && modified != m_modified)
            {
                Reload();
            }
        }
    }

    AZStd::string HearthScriptComponent::Fire(const AZStd::string& event, const AZStd::string& argsJson)
    {
        if (!m_engine)
        {
            return "the script isn't running";
        }
        const char* mistake = hs_fire(m_engine, event.c_str(), argsJson.empty() ? "[]" : argsJson.c_str());
        PassOnOutput();
        Report(mistake);
        return mistake ? AZStd::string(mistake) : AZStd::string();
    }

    const char* HearthScriptComponent::Native(void* user, const char* name, const char* argsJson)
    {
        return static_cast<HearthScriptComponent*>(user)->RunNative(name ? name : "", argsJson ? argsJson : "[]");
    }

    const char* HearthScriptComponent::RunNative(const AZStd::string& name, const AZStd::string& argsJson)
    {
        rapidjson::Document args;
        args.Parse(argsJson.c_str());
        auto number = [&args](rapidjson::SizeType i, float& out) -> bool
        {
            if (!args.IsArray() || i >= args.Size() || !args[i].IsNumber())
            {
                return false;
            }
            out = static_cast<float>(args[i].GetDouble());
            return true;
        };
        auto fail = [this](const char* why) -> const char*
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.StartObject();
            writer.Key("error");
            writer.String(why);
            writer.EndObject();
            m_reply = buffer.GetString();
            return m_reply.c_str();
        };

        if (name == "position")
        {
            AZ::Vector3 p = AZ::Vector3::CreateZero();
            AZ::TransformBus::EventResult(p, GetEntityId(), &AZ::TransformBus::Events::GetWorldTranslation);
            m_reply = AZStd::string::format("{\"ok\":[%f,%f,%f]}", static_cast<double>(p.GetX()), static_cast<double>(p.GetY()), static_cast<double>(p.GetZ()));
            return m_reply.c_str();
        }
        if (name == "move_to")
        {
            float x = 0, y = 0, z = 0;
            if (!number(0, x) || !number(1, y) || !number(2, z))
            {
                return fail("move_to needs three numbers: move_to(x, y, z)");
            }
            AZ::TransformBus::Event(GetEntityId(), &AZ::TransformBus::Events::SetWorldTranslation, AZ::Vector3(x, y, z));
            m_reply = "{\"ok\":null}";
            return m_reply.c_str();
        }
        if (name == "time")
        {
            m_reply = AZStd::string::format("{\"ok\":%f}", static_cast<double>(m_time));
            return m_reply.c_str();
        }
        if (name == "send")
        {
            if (!args.IsArray() || args.Empty() || !args[0].IsString())
            {
                return fail("send needs a name first: send(\"door_opened\", ...)");
            }
            const AZStd::string what = args[0].GetString();
            rapidjson::Value rest(rapidjson::kArrayType);
            for (rapidjson::SizeType i = 1; i < args.Size(); ++i)
            {
                rest.PushBack(rapidjson::Value(args[i], args.GetAllocator()), args.GetAllocator());
            }
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            rest.Accept(writer);
            HearthScriptNotificationBus::Event(GetEntityId(), &HearthScriptNotifications::OnScriptSent, what, AZStd::string(buffer.GetString()));
            m_reply = "{\"ok\":null}";
            return m_reply.c_str();
        }
        return fail("the game has no function by that name");
    }
} // namespace HearthRust
