/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "MiniAudioPlaybackComponentController.h"

#include <AzCore/Asset/AssetSerializer.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Serialization/EditContext.h>

#include "AzCore/Math/MathUtils.h"
#include <miniaudio.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/string/conversions.h>
#include <AzFramework/Entity/EntityContextBus.h>
#include <AzFramework/Entity/GameEntityContextBus.h>

namespace MiniAudio
{
    // Hearth Engine: every playback controller that's active, for the Audio window's list and buses.
    namespace
    {
        AZStd::mutex& HearthLiveMutex()
        {
            static AZStd::mutex mutex;
            return mutex;
        }
        AZStd::unordered_set<MiniAudioPlaybackComponentController*>& HearthLive()
        {
            static AZStd::unordered_set<MiniAudioPlaybackComponentController*> live;
            return live;
        }

        //! Which bus a sound plays on when the project hasn't said: by the words in its name.
        AZStd::string HearthDefaultBus(const AZStd::string& name)
        {
            AZStd::string lower = name;
            AZStd::to_lower(lower.begin(), lower.end());
            auto has = [&lower](const char* word) { return lower.find(word) != AZStd::string::npos; };
            if (has("music") || has("theme") || has("song")) return "Music";
            if (has("step") || has("foot") || has("breath") || has("player") || has("land") || has("jump")) return "Player";
            if (has("hum") || has("zap") || has("light") || has("flicker") || has("buzz") || has("crackle")) return "Lights";
            if (has("entity") || has("monster") || has("smiler") || has("hound") || has("creature")) return "Entities";
            return "Ambience";
        }

        float HearthDecibelsToGain(double decibels)
        {
            return decibels <= -60.0 ? 0.0f : ma_volume_db_to_linear(float(decibels));
        }

        void hearth_audioRefresh([[maybe_unused]] const AZ::ConsoleCommandContainer& arguments)
        {
            AZStd::lock_guard<AZStd::mutex> lock(HearthLiveMutex());
            for (MiniAudioPlaybackComponentController* controller : HearthLive())
            {
                controller->HearthApplyVolume();
            }
        }
        AZ_CONSOLEFREEFUNC(hearth_audioRefresh, AZ::ConsoleFunctorFlags::Null, "Hearth Engine: apply the Audio window's bus volumes to every sound");

        void hearth_audioStats(const AZ::ConsoleCommandContainer& arguments)
        {
            if (arguments.empty())
            {
                return;
            }
            AZStd::string json = "{\"sounds\":[";
            {
                AZStd::lock_guard<AZStd::mutex> lock(HearthLiveMutex());
                bool first = true;
                for (MiniAudioPlaybackComponentController* controller : HearthLive())
                {
                    if (!first)
                    {
                        json += ",";
                    }
                    first = false;
                    controller->HearthDescribe(json);
                }
            }
            float global = 1.0f;
            if (auto* audio = MiniAudioInterface::Get())
            {
                global = audio->GetGlobalVolume();
            }
            json += AZStd::string::format("],\"globalVolume\":%.3f}", global);
            const AZStd::string path(arguments[0]);
            AZ::IO::SystemFile file;
            if (file.Open(path.c_str(), AZ::IO::SystemFile::SF_OPEN_CREATE | AZ::IO::SystemFile::SF_OPEN_CREATE_PATH | AZ::IO::SystemFile::SF_OPEN_WRITE_ONLY))
            {
                file.Write(json.c_str(), json.size());
            }
        }
        AZ_CONSOLEFREEFUNC(hearth_audioStats, AZ::ConsoleFunctorFlags::Null, "Hearth Engine: write every active sound's state as JSON to the given file");

        //! The Audio window's play button: play the first sound with this name once (stem, as in hearth_audioStats).
        void hearth_audioPlay(const AZ::ConsoleCommandContainer& arguments)
        {
            if (arguments.empty())
            {
                return;
            }
            const AZStd::string stem(arguments[0]);
            MiniAudioPlaybackComponentController* found = nullptr;
            {
                AZStd::lock_guard<AZStd::mutex> lock(HearthLiveMutex());
                for (MiniAudioPlaybackComponentController* controller : HearthLive())
                {
                    if (controller->HearthSoundName() == stem)
                    {
                        found = controller;
                        break;
                    }
                }
            }
            if (found)
            {
                found->Play();
            }
        }
        AZ_CONSOLEFREEFUNC(hearth_audioPlay, AZ::ConsoleFunctorFlags::Null, "Hearth Engine: play the first sound with the given name (the Audio window's play button)");

        AZStd::string HearthEscape(const AZStd::string& text)
        {
            AZStd::string out;
            for (char c : text)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                }
                if (static_cast<unsigned char>(c) >= 0x20)
                {
                    out += c;
                }
            }
            return out;
        }
    } // namespace

    AZStd::string MiniAudioPlaybackComponentController::HearthSoundName() const
    {
        // "assets/sounds/level0/step_carpet_1.wav.miniaudio" -> "step_carpet_1"
        AZStd::string hint = m_config.m_sound.GetHint();
        const size_t slash = hint.find_last_of("/\\");
        if (slash != AZStd::string::npos)
        {
            hint = hint.substr(slash + 1);
        }
        const size_t dot = hint.find('.');
        return dot != AZStd::string::npos ? hint.substr(0, dot) : hint;
    }

    AZStd::string MiniAudioPlaybackComponentController::HearthBusName() const
    {
        const AZStd::string name = HearthSoundName();
        AZ::SettingsRegistryInterface::FixedValueString bus;
        if (auto* registry = AZ::SettingsRegistry::Get())
        {
            if (registry->Get(bus, AZStd::string::format("/Hearth/Audio/Assign/%s", name.c_str())) && !bus.empty())
            {
                return AZStd::string(bus.c_str());
            }
        }
        return HearthDefaultBus(name);
    }

    float MiniAudioPlaybackComponentController::HearthBusGain() const
    {
        double bus = 0.0;
        double master = 0.0;
        if (auto* registry = AZ::SettingsRegistry::Get())
        {
            registry->Get(bus, AZStd::string::format("/Hearth/Audio/Buses/%s", HearthBusName().c_str()));
            registry->Get(master, "/Hearth/Audio/Buses/Master");
        }
        return HearthDecibelsToGain(bus) * HearthDecibelsToGain(master);
    }

    void MiniAudioPlaybackComponentController::HearthApplyVolume()
    {
        if (m_sound)
        {
            ma_sound_set_volume(m_sound.get(), (m_config.m_volume / 100.f) * HearthBusGain());
        }
    }

    void MiniAudioPlaybackComponentController::HearthDescribe(AZStd::string& json) const
    {
        AZStd::string entityName;
        AZ::ComponentApplicationBus::BroadcastResult(entityName, &AZ::ComponentApplicationRequests::GetEntityName, m_entityComponentIdPair.GetEntityId());
        AzFramework::EntityContextId owner = AzFramework::EntityContextId::CreateNull();
        AzFramework::EntityIdContextQueryBus::EventResult(owner, m_entityComponentIdPair.GetEntityId(), &AzFramework::EntityIdContextQueries::GetOwningContextId);
        AzFramework::EntityContextId game = AzFramework::EntityContextId::CreateNull();
        AzFramework::GameEntityContextRequestBus::BroadcastResult(game, &AzFramework::GameEntityContextRequestBus::Events::GetGameEntityContextId);
        const bool playing = m_sound && ma_sound_is_playing(m_sound.get());
        float seconds = 0.0f;
        if (m_sound)
        {
            ma_sound_get_length_in_seconds(m_sound.get(), &seconds);
        }
        json += AZStd::string::format(
            "{\"sound\":\"%s\",\"hint\":\"%s\",\"entity\":\"%s\",\"game\":%s,\"bus\":\"%s\",\"volume\":%.1f,\"gain\":%.4f,"
            "\"loaded\":%s,\"playing\":%s,\"loop\":%s,\"spatial\":%s,\"maxDistance\":%.1f,\"seconds\":%.3f,\"requests\":%u,\"heard\":%u}",
            HearthEscape(HearthSoundName()).c_str(), HearthEscape(m_config.m_sound.GetHint()).c_str(), HearthEscape(entityName).c_str(),
            (!game.IsNull() && owner == game) ? "true" : "false", HearthEscape(HearthBusName()).c_str(), m_config.m_volume, HearthBusGain(),
            m_sound ? "true" : "false", playing ? "true" : "false", m_config.m_loop ? "true" : "false",
            m_config.m_enableSpatialization ? "true" : "false", m_config.m_maximumDistance, seconds, m_hearthRequests, m_hearthHeard);
    }

    MiniAudioPlaybackComponentController::MiniAudioPlaybackComponentController()
    {
    }

    // placement of this destructor is intentional.  It forces unique_ptr<ma_sound> to declare its destructor here
    // instead of in the header before inclusion of the giant <miniaudio.h> file
    MiniAudioPlaybackComponentController::~MiniAudioPlaybackComponentController()
    {
    }

    MiniAudioPlaybackComponentController::MiniAudioPlaybackComponentController(const MiniAudioPlaybackComponentConfig& config)
    {
        m_config = config;
    }

    void MiniAudioPlaybackComponentController::Reflect(AZ::ReflectContext* context)
    {
        MiniAudioPlaybackComponentConfig::Reflect(context);

        if (auto serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<MiniAudioPlaybackComponentController>()
                ->Field("Config", &MiniAudioPlaybackComponentController::m_config)
                ->Version(1);
        }
    }

    void MiniAudioPlaybackComponentController::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("MiniAudioPlaybackComponent"));
    }

    void MiniAudioPlaybackComponentController::Activate(const AZ::EntityComponentIdPair& entityComponentIdPair)
    {
        m_entityComponentIdPair = entityComponentIdPair;

        m_config.m_innerAngleInRadians = AZ::DegToRad(m_config.m_innerAngleInDegrees);
        m_config.m_outerAngleInRadians = AZ::DegToRad(m_config.m_outerAngleInDegrees);

        MiniAudioPlaybackRequestBus::Handler::BusConnect(m_entityComponentIdPair.GetEntityId());
        {
            AZStd::lock_guard<AZStd::mutex> lock(HearthLiveMutex());
            HearthLive().insert(this);
        }
        OnConfigurationUpdated();
    }

    void MiniAudioPlaybackComponentController::SetConfiguration(const MiniAudioPlaybackComponentConfig& config)
    {
        m_config = config;
        OnConfigurationUpdated();
    }

    const MiniAudioPlaybackComponentConfig& MiniAudioPlaybackComponentController::GetConfiguration() const
    {
        return m_config;
    }

    void MiniAudioPlaybackComponentController::Deactivate()
    {
        {
            AZStd::lock_guard<AZStd::mutex> lock(HearthLiveMutex());
            HearthLive().erase(this);
        }
        m_entityMovedHandler.Disconnect();
        UnloadSound();
        m_config.m_sound.Release();

        MiniAudioPlaybackRequestBus::Handler::BusDisconnect();
        AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    }

    void MiniAudioPlaybackComponentController::Play()
    {
        ++m_hearthRequests;
        if (m_sound)
        {
            const bool started = ma_sound_start(m_sound.get()) == MA_SUCCESS;
            float global = 1.0f;
            if (auto* audio = MiniAudioInterface::Get())
            {
                global = audio->GetGlobalVolume();
            }
            // Heard: it started, and its volume, bus and the global volume let something come out.
            if (started && (m_config.m_volume / 100.f) * HearthBusGain() * global > 0.001f)
            {
                ++m_hearthHeard;
            }
        }
    }

    void MiniAudioPlaybackComponentController::Stop()
    {
        if (m_sound)
        {
            ma_sound_stop(m_sound.get());
            ma_sound_seek_to_pcm_frame(m_sound.get(), 0);
        }
    }

    void MiniAudioPlaybackComponentController::Pause()
    {
        if (m_sound)
        {
            ma_sound_stop(m_sound.get());
        }
    }

    float MiniAudioPlaybackComponentController::GetVolumePercentage() const
    {
        return m_config.m_volume; // the sound's own volume, without its bus
    }

    void MiniAudioPlaybackComponentController::SetVolumePercentage(float volume)
    {
        m_config.m_volume = AZ::GetClamp(volume, 0.f, 100.f);
        HearthApplyVolume();
    }

    float MiniAudioPlaybackComponentController::GetVolumeDecibels() const
    {
        return ma_volume_linear_to_db(m_config.m_volume / 100.f);
    }

    void MiniAudioPlaybackComponentController::SetVolumeDecibels(float volumeDecibels)
    {
        m_config.m_volume = ma_volume_db_to_linear(volumeDecibels) * 100.f;
        HearthApplyVolume();
    }

    void MiniAudioPlaybackComponentController::SetLooping(bool loop)
    {
        m_config.m_loop = loop;
        if (m_sound)
        {
            ma_sound_set_looping(m_sound.get(), loop);
        }
    }

    bool MiniAudioPlaybackComponentController::IsLooping() const
    {
        if (m_sound)
        {
            return ma_sound_is_looping(m_sound.get());
        }

        return false;
    }

    AZ::Data::Asset<SoundAsset> MiniAudioPlaybackComponentController::GetSoundAsset() const
    {
        return m_config.m_sound;
    }

    void MiniAudioPlaybackComponentController::SetSoundAsset(AZ::Data::Asset<SoundAsset> soundAsset)
    {
        if (m_config.m_sound.GetId() != soundAsset.GetId())
        {
            UnloadSound();
            m_config.m_sound = soundAsset;
            OnConfigurationUpdated();
        }
    }

    SoundAssetRef MiniAudioPlaybackComponentController::GetSoundAssetRef() const
    {
        SoundAssetRef ref;
        ref.SetAsset(GetSoundAsset());
        return ref;
    }

    void MiniAudioPlaybackComponentController::SetSoundAssetRef(const SoundAssetRef& soundAssetRef)
    {
        SetSoundAsset(soundAssetRef.GetAsset());
    }

    float MiniAudioPlaybackComponentController::GetInnerAngleInRadians() const
    {
        return m_config.m_innerAngleInRadians;
    }

    void MiniAudioPlaybackComponentController::SetInnerAngleInRadians(float innerAngleInRadians)
    {
        m_config.m_innerAngleInRadians = innerAngleInRadians;
        m_config.m_innerAngleInDegrees = AZ::RadToDeg(m_config.m_innerAngleInRadians);
        OnConfigurationUpdated();
    }

    float MiniAudioPlaybackComponentController::GetInnerAngleInDegrees() const
    {
        return m_config.m_innerAngleInDegrees;
    }

    void MiniAudioPlaybackComponentController::SetInnerAngleInDegrees(float innerAngleInDegrees)
    {
        m_config.m_innerAngleInDegrees = innerAngleInDegrees;
        m_config.m_innerAngleInRadians = AZ::DegToRad(m_config.m_innerAngleInDegrees);
        OnConfigurationUpdated();
    }

    float MiniAudioPlaybackComponentController::GetOuterAngleInRadians() const
    {
        return m_config.m_outerAngleInRadians;
    }

    void MiniAudioPlaybackComponentController::SetOuterAngleInRadians(float outerAngleInRadians)
    {
        m_config.m_outerAngleInRadians = outerAngleInRadians;
        m_config.m_outerAngleInDegrees = AZ::RadToDeg(m_config.m_outerAngleInRadians);
        OnConfigurationUpdated();
    }

    float MiniAudioPlaybackComponentController::GetOuterAngleInDegrees() const
    {
        return m_config.m_outerAngleInDegrees;
    }

    void MiniAudioPlaybackComponentController::SetOuterAngleInDegrees(float outerAngleInDegrees)
    {
        m_config.m_outerAngleInDegrees = outerAngleInDegrees;
        m_config.m_outerAngleInRadians = AZ::DegToRad(m_config.m_outerAngleInDegrees);
        OnConfigurationUpdated();
    }

    float MiniAudioPlaybackComponentController::GetOuterVolumePercentage() const
    {
        return m_config.m_outerVolume;
    }

    void MiniAudioPlaybackComponentController::SetOuterVolumePercentage(float outerVolume)
    {
        m_config.m_outerVolume = AZ::GetClamp(outerVolume, 0.f, 100.f);
        OnConfigurationUpdated();
    }

    float MiniAudioPlaybackComponentController::GetOuterVolumeDecibels() const
    {
        return ma_volume_linear_to_db(m_config.m_outerVolume / 100.f);
    }

    void MiniAudioPlaybackComponentController::SetOuterVolumeDecibels(float outerVolumeDecibels)
    {
        m_config.m_outerVolume = ma_volume_db_to_linear(outerVolumeDecibels) * 100.f;
        OnConfigurationUpdated();
    }

    bool MiniAudioPlaybackComponentController::GetFixedDirecion() const
    {
        return m_config.m_fixedDirection;
    }

    void MiniAudioPlaybackComponentController::SetFixedDirecion(bool fixedDirection)
    {
        m_config.m_fixedDirection = fixedDirection;
    }

    float MiniAudioPlaybackComponentController::GetDirectionalAttenuationFactor() const
    {
        return ma_sound_get_directional_attenuation_factor(m_sound.get());
    }

    void MiniAudioPlaybackComponentController::SetDirectionalAttenuationFactor(float directionalAttenuationFactor)
    {
        m_config.m_directionalAttenuationFactor = directionalAttenuationFactor;
        OnConfigurationUpdated();
    }

    AZ::Vector3 MiniAudioPlaybackComponentController::GetDirection() const
    {
        ma_vec3f direction = ma_sound_get_direction(m_sound.get());
        return AZ::Vector3(direction.x, direction.y, direction.z);
    }

    void MiniAudioPlaybackComponentController::SetDirection(const AZ::Vector3& direction)
    {
        m_config.m_direction = direction;
        ma_sound_set_direction(m_sound.get(), m_config.m_direction.GetX(), m_config.m_direction.GetY(), m_config.m_direction.GetZ());
    }

    void MiniAudioPlaybackComponentController::OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData> asset)
    {
        AZ::Data::AssetBus::MultiHandler::BusDisconnect(asset.GetId());

        // Re-assign the sound before attempting to load it if it was
        // released and the asset is now ready.
        // This can happen in the Editor when returning from game mode
        if (!m_config.m_sound.IsReady())
        {
            m_config.m_sound = asset;
        }

        LoadSound();
    }

    void MiniAudioPlaybackComponentController::OnWorldTransformChanged(const AZ::Transform& world)
    {
        if (m_sound)
        {
            ma_sound_set_position(
                m_sound.get(), world.GetTranslation().GetX(), world.GetTranslation().GetY(), world.GetTranslation().GetZ());

            // Set the forward direction for the sound source
            if (!m_config.m_fixedDirection)
            {
                AZ::Transform worldTm = AZ::Transform::CreateIdentity();
                AZ::TransformBus::EventResult(worldTm, m_entityComponentIdPair.GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
                ma_sound_set_direction(m_sound.get(), worldTm.GetBasisY().GetX(), worldTm.GetBasisY().GetY(), worldTm.GetBasisY().GetZ());
            }
        }
    }

    void MiniAudioPlaybackComponentController::OnConfigurationUpdated()
    {
        if (m_config.m_sound.IsReady() == false)
        {
            AZ::Data::AssetBus::MultiHandler::BusConnect(m_config.m_sound.GetId());
            m_config.m_sound.QueueLoad();
        }
        else
        {
            LoadSound();
        }
    }

    void MiniAudioPlaybackComponentController::LoadSound()
    {
        if (ma_engine* engine = MiniAudioInterface::Get()->GetSoundEngine())
        {
            if (GetConfiguration().m_sound.IsReady())
            {
                AZ::Data::AssetId::FixedString soundName = GetConfiguration().m_sound.GetId().ToFixedString();

                const auto& assetBuffer = GetConfiguration().m_sound->m_data;
                if (assetBuffer.empty())
                {
                    return;
                }

                ma_result result = ma_resource_manager_register_encoded_data(
                    ma_engine_get_resource_manager(engine), soundName.c_str(), assetBuffer.data(), assetBuffer.size());
                if (result != MA_SUCCESS)
                {
                    // An error occurred.
                    return;
                }

                if (m_sound)
                {
                    ma_sound_uninit(m_sound.get());
                    m_sound.reset();
                }
                m_sound = AZStd::make_unique<ma_sound>();

                const ma_uint32 flags = MA_SOUND_FLAG_DECODE;
                result = ma_sound_init_from_file(engine, soundName.c_str(), flags, nullptr, nullptr, m_sound.get());
                if (result != MA_SUCCESS)
                {
                    // An error occurred.
                    return;
                }

                HearthApplyVolume();
                ma_sound_set_looping(m_sound.get(), m_config.m_loop);

                ma_sound_set_spatialization_enabled(m_sound.get(), m_config.m_enableSpatialization);
                if (m_config.m_enableSpatialization)
                {
                    ma_sound_set_min_distance(m_sound.get(), m_config.m_minimumDistance);
                    ma_sound_set_max_distance(m_sound.get(), m_config.m_maximumDistance);
                    ma_sound_set_attenuation_model(m_sound.get(), static_cast<ma_attenuation_model>(m_config.m_attenuationModel));
                    ma_sound_set_directional_attenuation_factor(m_sound.get(), m_config.m_directionalAttenuationFactor);

                    // Set the forward direction for the sound source
                    if (!m_config.m_fixedDirection)
                    {
                        AZ::Transform worldTm = AZ::Transform::CreateIdentity();
                        AZ::TransformBus::EventResult(
                            worldTm, m_entityComponentIdPair.GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
                        ma_sound_set_direction(
                            m_sound.get(), worldTm.GetBasisY().GetX(), worldTm.GetBasisY().GetY(), worldTm.GetBasisY().GetZ());
                    }
                    else
                    {
                        ma_sound_set_direction(
                            m_sound.get(), m_config.m_direction.GetX(), m_config.m_direction.GetY(), m_config.m_direction.GetZ());
                    }

                    ma_sound_set_cone(
                        m_sound.get(), m_config.m_innerAngleInRadians, m_config.m_outerAngleInRadians, (m_config.m_outerVolume / 100.f));
                }

                if (m_config.m_autoFollowEntity)
                {
                    m_entityMovedHandler.Disconnect();
                    AZ::TransformBus::Event(
                        m_entityComponentIdPair.GetEntityId(),
                        &AZ::TransformBus::Events::BindTransformChangedEventHandler,
                        m_entityMovedHandler);

                    AZ::Transform worldTm = AZ::Transform::CreateIdentity();
                    AZ::TransformBus::EventResult(worldTm, m_entityComponentIdPair.GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
                    OnWorldTransformChanged(worldTm);
                }
                else
                {
                    m_entityMovedHandler.Disconnect();
                }

                // Automatically play after the sound loads if requested
                // This will play automatically in Editor and Game
                if (m_config.m_autoplayOnActivate)
                {
                    Play();
                }
            }
        }
    }

    void MiniAudioPlaybackComponentController::UnloadSound()
    {
        if (ma_engine* engine = MiniAudioInterface::Get()->GetSoundEngine())
        {
            if (m_sound)
            {
                ma_sound_stop(m_sound.get());
                ma_sound_uninit(m_sound.get());
                m_sound.reset();
            }

            AZ::Data::AssetId soundId = GetConfiguration().m_sound.GetId();
            if (soundId.IsValid())
            {
                ma_resource_manager_unregister_data(ma_engine_get_resource_manager(engine), soundId.ToFixedString().c_str());
            }
        }
    }
} // namespace MiniAudio
