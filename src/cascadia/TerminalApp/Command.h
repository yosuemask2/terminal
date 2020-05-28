﻿// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "Command.g.h"
#include "..\inc\cppwinrt_utils.h"

namespace winrt::TerminalApp::implementation
{
    struct Command : CommandT<Command>
    {
        Command() = default;

        // DECLARE_GETSET_PROPERTY(winrt::hstring, Name);

        WINRT_CALLBACK(PropertyChanged, Windows::UI::Xaml::Data::PropertyChangedEventHandler);
        OBSERVABLE_GETSET_PROPERTY(winrt::hstring, Name, _PropertyChangedHandlers);
        // OBSERVABLE_GETSET_PROPERTY(winrt::hstring, IconPath, _propertyChanged);
        // OBSERVABLE_GETSET_PROPERTY(winrt::TerminalApp::ActionAndArgs, Action, _propertyChanged);
        GETSET_PROPERTY(winrt::hstring, IconPath);
        GETSET_PROPERTY(winrt::TerminalApp::ActionAndArgs, Action);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(Command);
}
