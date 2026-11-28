/*
 * Hearth Engine: talking to Hearth Script from the game, Script Canvas and Lua.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

#include <AzCore/Component/ComponentBus.h>
#include <AzCore/std/string/string.h>

namespace HearthRust
{
    //! Asks an entity's Hearth Script to do things.
    class HearthScriptRequests : public AZ::ComponentBus
    {
    public:
        //! Runs the script's `when event(...)` with these values (a JSON list, like `["Gym key", 2]`).
        //! Returns "" when it worked, or the mistake in plain words.
        virtual AZStd::string Fire(const AZStd::string& event, const AZStd::string& argsJson) = 0;
        //! Loads the script file again now (it also reloads by itself within a second of being saved).
        virtual AZStd::string Reload() = 0;
    };
    using HearthScriptRequestBus = AZ::EBus<HearthScriptRequests>;

    //! What an entity's Hearth Script tells the game.
    class HearthScriptNotifications : public AZ::ComponentBus
    {
    public:
        //! The script called `send(name, values...)`: the game (or Script Canvas) does what that means.
        virtual void OnScriptSent([[maybe_unused]] const AZStd::string& name, [[maybe_unused]] const AZStd::string& argsJson) {}
        //! A mistake in the script (the last good version keeps running).
        virtual void OnScriptMistake([[maybe_unused]] const AZStd::string& message) {}
        //! A line from the script's `say(...)`.
        virtual void OnScriptSaid([[maybe_unused]] const AZStd::string& line) {}
    };
    using HearthScriptNotificationBus = AZ::EBus<HearthScriptNotifications>;
} // namespace HearthRust
