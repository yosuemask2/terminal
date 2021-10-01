// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CascadiaSettings.h"
#include "CascadiaSettings.g.cpp"

#include <LibraryResources.h>
#include <VersionHelpers.h>

#include <shellapi.h>
#include <shlwapi.h>

using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Settings;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Windows::Foundation::Collections;
using namespace Microsoft::Console;

winrt::com_ptr<Profile> Model::implementation::CreateChild(const winrt::com_ptr<Profile>& parent)
{
    auto profile = winrt::make_self<Profile>();
    profile->Origin(OriginTag::User);
    profile->Name(parent->Name());
    profile->Guid(parent->Guid());
    profile->Hidden(parent->Hidden());
    profile->InsertParent(parent);
    return profile;
}

Model::CascadiaSettings CascadiaSettings::Copy() const
{
    const auto settings{ winrt::make_self<CascadiaSettings>() };

    // user settings
    {
        std::vector<Model::Profile> allProfiles;
        std::vector<Model::Profile> activeProfiles;
        allProfiles.reserve(_allProfiles.Size());
        activeProfiles.reserve(_activeProfiles.Size());

        // Clone the graph of profiles.
        // _baseLayerProfile is part of the graph
        // and thus needs to be handled here as well.
        {
            std::vector<winrt::com_ptr<Profile>> sourceProfiles;
            std::vector<winrt::com_ptr<Profile>> targetProfiles;
            sourceProfiles.reserve(allProfiles.size());
            targetProfiles.reserve(allProfiles.size());

            for (const auto& profile : _allProfiles)
            {
                winrt::com_ptr<Profile> profileImpl;
                profileImpl.copy_from(winrt::get_self<Profile>(profile));
                sourceProfiles.emplace_back(std::move(profileImpl));
            }

            // Profiles are basically a directed acyclic graph. Cloning it without creating duplicated nodes,
            // requires us to "intern" visited profiles. Thus the "visited" map contains a cache of
            // previously cloned profiles/sub-graphs. It maps from source-profile-pointer to cloned-profile.
            std::unordered_map<const Profile*, winrt::com_ptr<Profile>> visited;
            // I'm just gonna estimate that each profile has 3 parents at most on average:
            // * base layer
            // * fragment
            // * inbox defaults
            visited.reserve(sourceProfiles.size() * 3);

            // _baseLayerProfile is part of the profile graph.
            // In order to get a reference to the clone, we need to copy it explicitly.
            settings->_baseLayerProfile = _baseLayerProfile->CopyInheritanceGraph(visited);
            Profile::CopyInheritanceGraphs(visited, sourceProfiles, targetProfiles);

            for (const auto& profile : targetProfiles)
            {
                allProfiles.emplace_back(*profile);
                if (!profile->Hidden())
                {
                    activeProfiles.emplace_back(*profile);
                }
            }
        }

        settings->_globals = _globals->Copy();
        settings->_allProfiles = winrt::single_threaded_observable_vector(std::move(allProfiles));
        settings->_activeProfiles = winrt::single_threaded_observable_vector(std::move(activeProfiles));
    }

    // load errors
    {
        std::vector<Model::SettingsLoadWarnings> warnings{ _warnings.Size() };
        _warnings.GetMany(0, warnings);

        settings->_warnings = winrt::single_threaded_vector(std::move(warnings));
        settings->_loadError = _loadError;
        settings->_deserializationErrorMessage = _deserializationErrorMessage;
    }

    // defterm
    settings->_currentDefaultTerminal = _currentDefaultTerminal;

    return *settings;
}

// Method Description:
// - Finds a profile that matches the given GUID. If there is no profile in this
//      settings object that matches, returns nullptr.
// Arguments:
// - guid: the GUID of the profile to return.
// Return Value:
// - a strong reference to the profile matching the given guid, or nullptr
//      if there is no match.
Model::Profile CascadiaSettings::FindProfile(const winrt::guid& guid) const noexcept
{
    for (const auto& profile : _allProfiles)
    {
        if (profile.Guid() == guid)
        {
            return profile;
        }
    }
    return nullptr;
}

// Method Description:
// - Returns an iterable collection of all of our Profiles.
// Arguments:
// - <none>
// Return Value:
// - an iterable collection of all of our Profiles.
IObservableVector<Model::Profile> CascadiaSettings::AllProfiles() const noexcept
{
    return _allProfiles;
}

// Method Description:
// - Returns an iterable collection of all of our non-hidden Profiles.
// Arguments:
// - <none>
// Return Value:
// - an iterable collection of all of our Profiles.
IObservableVector<Model::Profile> CascadiaSettings::ActiveProfiles() const noexcept
{
    return _activeProfiles;
}

// Method Description:
// - Returns the globally configured keybindings
// Arguments:
// - <none>
// Return Value:
// - the globally configured keybindings
Model::ActionMap CascadiaSettings::ActionMap() const noexcept
{
    return _globals->ActionMap();
}

// Method Description:
// - Get a reference to our global settings
// Arguments:
// - <none>
// Return Value:
// - a reference to our global settings
Model::GlobalAppSettings CascadiaSettings::GlobalSettings() const
{
    return *_globals;
}

// Method Description:
// - Get a reference to our profiles.defaults object
// Arguments:
// - <none>
// Return Value:
// - a reference to our profile.defaults object
Model::Profile CascadiaSettings::ProfileDefaults() const
{
    return *_baseLayerProfile;
}

// Method Description:
// - Create a new profile based off the default profile settings.
// Arguments:
// - <none>
// Return Value:
// - a reference to the new profile
Model::Profile CascadiaSettings::CreateNewProfile()
{
    if (_allProfiles.Size() == std::numeric_limits<uint32_t>::max())
    {
        // Shouldn't really happen
        return nullptr;
    }

    std::wstring newName;
    for (uint32_t candidateIndex = 0, count = _allProfiles.Size() + 1; candidateIndex < count; candidateIndex++)
    {
        // There is a theoretical unsigned integer wraparound, which is OK
        newName = fmt::format(L"Profile {}", count + candidateIndex);
        if (std::none_of(begin(_allProfiles), end(_allProfiles), [&](auto&& profile) { return profile.Name() == newName; }))
        {
            break;
        }
    }

    const auto newProfile = _createNewProfile(newName);
    _allProfiles.Append(*newProfile);
    _activeProfiles.Append(*newProfile);
    return *newProfile;
}

// Method Description:
// - Duplicate a new profile based off another profile's settings
// - This differs from Profile::Copy because it also copies over settings
//   that were not defined in the json (for example, settings that were
//   defined in one of the parents)
// - This will not duplicate settings that were defined in profiles.defaults
//   however, because we do not want the json blob generated from the new profile
//   to contain those settings
// Arguments:
// - source: the Profile object we are duplicating (must not be null)
// Return Value:
// - a reference to the new profile
Model::Profile CascadiaSettings::DuplicateProfile(const Model::Profile& source)
{
    THROW_HR_IF_NULL(E_INVALIDARG, source);

    auto newName = fmt::format(L"{} ({})", source.Name(), RS_(L"CopySuffix"));

    // Check if this name already exists and if so, append a number
    for (uint32_t candidateIndex = 0, count = _allProfiles.Size() + 1; candidateIndex < count; ++candidateIndex)
    {
        if (std::none_of(begin(_allProfiles), end(_allProfiles), [&](auto&& profile) { return profile.Name() == newName; }))
        {
            break;
        }
        // There is a theoretical unsigned integer wraparound, which is OK
        newName = fmt::format(L"{} ({} {})", source.Name(), RS_(L"CopySuffix"), candidateIndex + 2);
    }

    const auto duplicated = _createNewProfile(newName);

    static constexpr auto isProfilesDefaultsOrigin = [](const auto& profile) -> bool {
        return profile && profile.Origin() != OriginTag::ProfilesDefaults;
    };

    static constexpr auto isProfilesDefaultsOriginSub = [](const auto& sub) -> bool {
        return sub && isProfilesDefaultsOrigin(sub.SourceProfile());
    };

#define NEEDS_DUPLICATION(settingName) source.Has##settingName() || isProfilesDefaultsOrigin(source.settingName##OverrideSource())
#define NEEDS_DUPLICATION_SUB(source, settingName) source.Has##settingName() || isProfilesDefaultsOriginSub(source.settingName##OverrideSource())

#define DUPLICATE_SETTING_MACRO(settingName)           \
    if (NEEDS_DUPLICATION(settingName))                \
    {                                                  \
        duplicated->settingName(source.settingName()); \
    }

#define DUPLICATE_SETTING_MACRO_SUB(source, target, settingName) \
    if (NEEDS_DUPLICATION_SUB(source, settingName))              \
    {                                                            \
        target.settingName(source.settingName());                \
    }

    // If the source is hidden and the Settings UI creates a
    // copy of it we don't want the copy to be hidden as well.
    // --> Don't do DUPLICATE_SETTING_MACRO(Hidden);
    DUPLICATE_SETTING_MACRO(Icon);
    DUPLICATE_SETTING_MACRO(CloseOnExit);
    DUPLICATE_SETTING_MACRO(TabTitle);
    DUPLICATE_SETTING_MACRO(TabColor);
    DUPLICATE_SETTING_MACRO(SuppressApplicationTitle);
    DUPLICATE_SETTING_MACRO(UseAcrylic);
    DUPLICATE_SETTING_MACRO(ScrollState);
    DUPLICATE_SETTING_MACRO(Padding);
    DUPLICATE_SETTING_MACRO(Commandline);
    DUPLICATE_SETTING_MACRO(StartingDirectory);
    DUPLICATE_SETTING_MACRO(AntialiasingMode);
    DUPLICATE_SETTING_MACRO(ForceFullRepaintRendering);
    DUPLICATE_SETTING_MACRO(SoftwareRendering);
    DUPLICATE_SETTING_MACRO(HistorySize);
    DUPLICATE_SETTING_MACRO(SnapOnInput);
    DUPLICATE_SETTING_MACRO(AltGrAliasing);
    DUPLICATE_SETTING_MACRO(BellStyle);

    {
        const auto font = source.FontInfo();
        const auto target = duplicated->FontInfo();
        DUPLICATE_SETTING_MACRO_SUB(font, target, FontFace);
        DUPLICATE_SETTING_MACRO_SUB(font, target, FontSize);
        DUPLICATE_SETTING_MACRO_SUB(font, target, FontWeight);
        DUPLICATE_SETTING_MACRO_SUB(font, target, FontFeatures);
        DUPLICATE_SETTING_MACRO_SUB(font, target, FontAxes);
    }

    {
        const auto appearance = source.DefaultAppearance();
        const auto target = duplicated->DefaultAppearance();
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, ColorSchemeName);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, Foreground);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, Background);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, SelectionBackground);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, CursorColor);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, PixelShaderPath);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, IntenseTextStyle);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, BackgroundImagePath);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, BackgroundImageOpacity);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, BackgroundImageStretchMode);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, BackgroundImageAlignment);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, RetroTerminalEffect);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, CursorShape);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, CursorHeight);
        DUPLICATE_SETTING_MACRO_SUB(appearance, target, Opacity);
    }

    // UnfocusedAppearance is treated as a single setting,
    // but requires a little more legwork to duplicate properly
    if (NEEDS_DUPLICATION(UnfocusedAppearance))
    {
        // It is alright to simply call CopyAppearance here instead of needing a separate function
        // like DuplicateAppearance since UnfocusedAppearance is treated as a single setting.
        const auto unfocusedAppearance = AppearanceConfig::CopyAppearance(
            winrt::get_self<AppearanceConfig>(source.UnfocusedAppearance()),
            winrt::weak_ref<Model::Profile>(*duplicated));

        // Make sure to add the default appearance of the duplicated profile as a parent to the duplicate's UnfocusedAppearance
        winrt::com_ptr<AppearanceConfig> defaultAppearance;
        defaultAppearance.copy_from(winrt::get_self<AppearanceConfig>(duplicated->DefaultAppearance()));
        unfocusedAppearance->InsertParent(defaultAppearance);

        duplicated->UnfocusedAppearance(*unfocusedAppearance);
    }

    if (source.HasConnectionType())
    {
        duplicated->ConnectionType(source.ConnectionType());
    }

    _allProfiles.Append(*duplicated);
    _activeProfiles.Append(*duplicated);
    return *duplicated;
}

// Method Description:
// - Gets our list of warnings we found during loading. These are things that we
//   knew were bad when we called `_ValidateSettings` last.
// Return Value:
// - a reference to our list of warnings.
IVectorView<Model::SettingsLoadWarnings> CascadiaSettings::Warnings() const
{
    return _warnings.GetView();
}

winrt::Windows::Foundation::IReference<Model::SettingsLoadErrors> CascadiaSettings::GetLoadingError() const
{
    return _loadError;
}

winrt::hstring CascadiaSettings::GetSerializationErrorMessage() const
{
    return _deserializationErrorMessage;
}

// As used by CreateNewProfile and DuplicateProfile this function
// creates a new Profile instance with a random UUID and a given name.
winrt::com_ptr<Profile> CascadiaSettings::_createNewProfile(const std::wstring_view& name) const
{
    // Technically there's Utils::CreateV5Uuid which we could use, but I wanted
    // truly globally unique UUIDs for profiles created through the settings UI.
    GUID guid{};
    LOG_IF_FAILED(CoCreateGuid(&guid));

    auto profile = CreateChild(_baseLayerProfile);
    profile->Guid(guid);
    profile->Name(winrt::hstring{ name });
    return profile;
}

// Method Description:
// - Attempts to validate this settings structure. If there are critical errors
//   found, they'll be thrown as a SettingsLoadError. Non-critical errors, such
//   as not finding the default profile, will only result in an error. We'll add
//   all these warnings to our list of warnings, and the application can chose
//   to display these to the user.
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::_validateSettings()
{
    _validateAllSchemesExist();
    _validateMediaResources();
    _validateKeybindings();
    _validateColorSchemesInCommands();
}

// Method Description:
// - Ensures that every profile has a valid "color scheme" set. If any profile
//   has a colorScheme set to a value which is _not_ the name of an actual color
//   scheme, we'll set the color table of the profile to something reasonable.
// Arguments:
// - <none>
// Return Value:
// - <none>
// - Appends a SettingsLoadWarnings::UnknownColorScheme to our list of warnings if
//   we find any such duplicate.
void CascadiaSettings::_validateAllSchemesExist()
{
    const auto colorSchemes = _globals->ColorSchemes();
    bool foundInvalidScheme = false;

    for (const auto& profile : _allProfiles)
    {
        for (const auto& appearance : std::array{ profile.DefaultAppearance(), profile.UnfocusedAppearance() })
        {
            if (appearance && !colorSchemes.HasKey(appearance.ColorSchemeName()))
            {
                // Clear the user set color scheme. We'll just fallback instead.
                appearance.ClearColorSchemeName();
                foundInvalidScheme = true;
            }
        }
    }

    if (foundInvalidScheme)
    {
        _warnings.Append(SettingsLoadWarnings::UnknownColorScheme);
    }
}

// Method Description:
// - Ensures that all specified images resources (icons and background images) are valid URIs.
//   This does not verify that the icon or background image files are encoded as an image.
// Arguments:
// - <none>
// Return Value:
// - <none>
// - Appends a SettingsLoadWarnings::InvalidBackgroundImage to our list of warnings if
//   we find any invalid background images.
// - Appends a SettingsLoadWarnings::InvalidIconImage to our list of warnings if
//   we find any invalid icon images.
void CascadiaSettings::_validateMediaResources()
{
    bool invalidBackground{ false };
    bool invalidIcon{ false };

    for (auto profile : _allProfiles)
    {
        if (const auto path = profile.DefaultAppearance().ExpandedBackgroundImagePath(); !path.empty())
        {
            // Attempt to convert the path to a URI, the ctor will throw if it's invalid/unparseable.
            // This covers file paths on the machine, app data, URLs, and other resource paths.
            try
            {
                winrt::Windows::Foundation::Uri imagePath{ path };
            }
            catch (...)
            {
                // reset background image path
                profile.DefaultAppearance().ClearBackgroundImagePath();
                invalidBackground = true;
            }
        }

        if (profile.UnfocusedAppearance())
        {
            if (const auto path = profile.UnfocusedAppearance().ExpandedBackgroundImagePath(); !path.empty())
            {
                // Attempt to convert the path to a URI, the ctor will throw if it's invalid/unparseable.
                // This covers file paths on the machine, app data, URLs, and other resource paths.
                try
                {
                    winrt::Windows::Foundation::Uri imagePath{ path };
                }
                catch (...)
                {
                    // reset background image path
                    profile.UnfocusedAppearance().ClearBackgroundImagePath();
                    invalidBackground = true;
                }
            }
        }

        // Anything longer than 2 wchar_t's _isn't_ an emoji or symbol,
        // so treat it as an invalid path.
        if (const auto icon = profile.Icon(); icon.size() > 2)
        {
            const auto iconPath{ wil::ExpandEnvironmentStringsW<std::wstring>(icon.c_str()) };
            try
            {
                winrt::Windows::Foundation::Uri imagePath{ iconPath };
            }
            catch (...)
            {
                profile.ClearIcon();
                invalidIcon = true;
            }
        }
    }

    if (invalidBackground)
    {
        _warnings.Append(SettingsLoadWarnings::InvalidBackgroundImage);
    }

    if (invalidIcon)
    {
        _warnings.Append(SettingsLoadWarnings::InvalidIcon);
    }
}

// Method Description:
// - Helper to get the GUID of a profile, given an optional index and a possible
//   "profile" value to override that.
// - First, we'll try looking up the profile for the given index. This will
//   either get us the GUID of the Nth profile, or the GUID of the default
//   profile.
// - Then, if there was a Profile set in the NewTerminalArgs, we'll use that to
//   try and look the profile up by either GUID or name.
// Arguments:
// - index: if provided, the index in the list of profiles to get the GUID for.
//   If omitted, instead use the default profile's GUID
// - newTerminalArgs: An object that may contain a profile name or GUID to
//   actually use. If the Profile value is not a guid, we'll treat it as a name,
//   and attempt to look the profile up by name instead.
// Return Value:
// - the GUID of the profile corresponding to this combination of index and NewTerminalArgs
Model::Profile CascadiaSettings::GetProfileForArgs(const Model::NewTerminalArgs& newTerminalArgs) const
{
    if (newTerminalArgs)
    {
        if (const auto name = newTerminalArgs.Profile(); !name.empty())
        {
            if (auto profile = GetProfileByName(name))
            {
                return profile;
            }
        }

        if (const auto index = newTerminalArgs.ProfileIndex())
        {
            if (auto profile = GetProfileByIndex(gsl::narrow<uint32_t>(index.Value())))
            {
                return profile;
            }
        }

        if (const auto commandLine = newTerminalArgs.Commandline(); !commandLine.empty())
        {
            if (auto profile = _getProfileForCommandLine(commandLine))
            {
                return profile;
            }
        }
    }

    if constexpr (Feature_ShowProfileDefaultsInSettings::IsEnabled())
    {
        // If the user has access to the "Defaults" profile, and no profile was otherwise specified,
        // what we do is dependent on whether there was a commandline.
        // If there was a commandline (case 1), we we'll launch in the "Defaults" profile.
        // If there wasn't a commandline or there wasn't a NewTerminalArgs (case 2), we'll
        //   launch in the user's actual default profile.
        // Case 2 above could be the result of a "nt" or "sp" invocation that doesn't specify anything.
        // TODO GH#10952: Detect the profile based on the commandline (add matching support)
        return (!newTerminalArgs || newTerminalArgs.Commandline().empty()) ?
                   FindProfile(GlobalSettings().DefaultProfile()) :
                   ProfileDefaults();
    }
    else
    {
        // For compatibility with the stable version's behavior, return the default by GUID in all other cases.
        return FindProfile(GlobalSettings().DefaultProfile());
    }
}

// The method does some crude command line matching for our console hand-off support.
// If you have hand-off enabled and start PowerShell from the start menu we might be called with
//   "C:\Program Files\PowerShell\7\pwsh.exe -WorkingDirectory ~"
// This function then checks all known user profiles for one that's compatible with the commandLine.
// In this case we might have a profile with the command line
//   "C:\Program Files\PowerShell\7\pwsh.exe"
// This function will then match this profile return it.
//
// If no matching profile could be found a nullptr will be returned.
Model::Profile CascadiaSettings::_getProfileForCommandLine(const winrt::hstring& commandLine) const
{
    // We're going to cache all the command lines we got, as
    // _normalizeCommandLine is a relatively heavy operation.
    std::call_once(_commandLinesCacheOnce, [this]() {
        _commandLinesCache.reserve(_allProfiles.Size());

        for (const auto& profile : _allProfiles)
        {
            if (const auto cmd = profile.Commandline(); !cmd.empty())
            {
                _commandLinesCache.emplace_back(_normalizeCommandLine(cmd.c_str()), profile);
            }
        }

        // We're trying to find the command line with the longest common prefix below.
        // Given the commandLine "foo.exe -bar -baz" and these two user profiles:
        // * "foo.exe"
        // * "foo.exe -bar"
        // we want to choose the second one. By sorting the _commandLinesCache in a descending order
        // by command line length, we can return from this function the moment we found a matching
        // profile as there cannot possibly be any other profile anymore with a longer command line.
        std::stable_sort(_commandLinesCache.begin(), _commandLinesCache.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first.size() > rhs.first.size();
        });
    });

    const auto needle = _normalizeCommandLine(commandLine.c_str());

    // til::starts_with(string, prefix) will always return false if prefix.size() > string.size().
    // --> Using binary search we can safely skip all items in _commandLinesCache where .first.size() > needle.size().
    const auto end = _commandLinesCache.end();
    auto it = std::lower_bound(_commandLinesCache.begin(), end, needle, [&](const auto& lhs, const auto& rhs) {
        return lhs.first.size() > rhs.size();
    });

    // `it` is now at a position where it->first.size() <= needle.size().
    // Hopefully we'll now find a command line with matching prefix.
    for (; it != end; ++it)
    {
        if (til::starts_with(needle, it->first))
        {
            return it->second;
        }
    }

    return nullptr;
}

// Given a commandLine like the following:
// * "C:\WINDOWS\System32\cmd.exe"
// * "pwsh -WorkingDirectory ~"
// * "C:\Program Files\PowerShell\7\pwsh.exe"
// * "C:\Program Files\PowerShell\7\pwsh.exe -WorkingDirectory ~"
//
// This function returns:
// * "C:\Windows\System32\cmd.exe"
// * "C:\Program Files\PowerShell\7\pwsh.exe\0-WorkingDirectory\0~"
// * "C:\Program Files\PowerShell\7\pwsh.exe"
// * "C:\Program Files\PowerShell\7\pwsh.exe\0-WorkingDirectory\0~"
//
// The resulting strings are then used for comparisons in _getProfileForCommandLine().
// For instance a resulting string of
//   "C:\Program Files\PowerShell\7\pwsh.exe"
// is considered a compatible profile with
//   "C:\Program Files\PowerShell\7\pwsh.exe -WorkingDirectory ~"
// as it shares the same (normalized) prefix.
std::wstring CascadiaSettings::_normalizeCommandLine(LPCWSTR commandLine)
{
    // Turn "%SystemRoot%\System32\cmd.exe" into "C:\WINDOWS\System32\cmd.exe".
    // We do this early, as environment variables might occur anywhere in the commandLine.
    std::wstring normalized;
    THROW_IF_FAILED(wil::ExpandEnvironmentStringsW(commandLine, normalized));

    // One of the most important things this function does is to strip quotes.
    // That way the commandLine "foo.exe -bar" and "\"foo.exe\" \"-bar\"" appear identical.
    // We'll abuse CommandLineToArgvW for that as it's close to what CreateProcessW uses.
    int argc = 0;
    const auto argv = CommandLineToArgvW(normalized.c_str(), &argc);
    THROW_LAST_ERROR_IF(!argc);
    const auto argvRelease = wil::scope_exit([=]() { LocalFree(argv); });

    // The given commandLine should start with an executable name or path.
    // For instance given the following argv arrays:
    // * {"C:\WINDOWS\System32\cmd.exe"}
    // * {"pwsh", "-WorkingDirectory", "~"}
    // * {"C:\Program", "Files\PowerShell\7\pwsh.exe"}
    //               ^^^^
    //   Notice how there used to be a space in the path, which was split by ExpandEnvironmentStringsW().
    //   CreateProcessW() supports such atrocities, so we got to do the same.
    // * {"C:\Program Files\PowerShell\7\pwsh.exe", "-WorkingDirectory", "~"}
    //
    // This loop tries to resolve relative paths, as well as executable names in %PATH%
    // into absolute paths and normalizes them. The results for the above would be:
    // * "C:\Windows\System32\cmd.exe"
    // * "C:\Program Files\PowerShell\7\pwsh.exe"
    // * "C:\Program Files\PowerShell\7\pwsh.exe"
    // * "C:\Program Files\PowerShell\7\pwsh.exe"
    for (;;)
    {
        // CreateProcessW uses RtlGetExePath to get the lpPath for SearchPathW.
        // The difference between the behavior of SearchPathW if lpPath is nullptr and what RtlGetExePath returns
        // seems to be mostly whether SafeProcessSearchMode is respected and the support for relative paths.
        // Windows Terminal makes the use relative paths rather impractical which is why we simply dropped the call to RtlGetExePath.
        const auto status = wil::SearchPathW(nullptr, argv[0], L".exe", normalized);

        if (status == S_OK)
        {
            // ExpandEnvironmentStringsW() might have returned a string that's not in the canonical capitalization.
            // For instance %SystemRoot% is set to C:\WINDOWS on my system (ugh), even though the path is actually C:\Windows.
            // We need to fix this as case-sensitive path comparisons will fail otherwise (Windows supports case-sensitive file systems).
            auto path = std::filesystem::canonical(std::move(normalized));
            // std::filesystem::path has no way to extract the internal path.
            // So about that.... I own you, computer. Give me that path.
            normalized = std::move(const_cast<std::wstring&>(path.native()));
            break;
        }

        // If the file path couldn't be found by SearchPathW this could be the result of us being given a commandLine
        // like "C:\foo bar\baz.exe -arg" which is resolved to the argv array {"C:\foo", "bar\baz.exe", "-arg"}.
        // Just like CreateProcessW() we thus try to concatenate arguments until we successfully resolve a valid path.
        // Of course we can only do that if we have at least 2 remaining arguments in argv.
        // All other error types aren't handled at the moment.
        if (argc < 2 || status != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            break;
        }

        // As described in the comment right above, we concatenate arguments in an attempt to resolve a valid path.
        // The code below turns argv from {"C:\foo", "bar\baz.exe", "-arg"} into {"C:\foo bar\baz.exe", "-arg"}.
        // The code abuses the fact that CommandLineToArgvW allocates all arguments back-to-back on the heap separated by '\0'.
        argv[1][-1] = L' ';
        --argc;
    }

    // We've (hopefully) finished resolving the path to the executable.
    // We're now going to append all remaining arguments to the resulting string.
    // If argv is {"C:\Program Files\PowerShell\7\pwsh.exe", "-WorkingDirectory", "~"},
    // then we'll get "C:\Program Files\PowerShell\7\pwsh.exe\0-WorkingDirectory\0~"
    if (argc > 1)
    {
        // normalized contains a canonical form of argv[0] at this point.
        // -1 allows us to include the \0 between argv[0] and argv[1] in the call to append().
        const auto beg = argv[1] - 1;
        const auto lastArg = argv[argc - 1];
        const auto end = lastArg + wcslen(lastArg);
        normalized.append(beg, end);
    }

    return normalized;
}

// Method Description:
// - Helper to get a profile given a name that could be a guid or an actual name.
// Arguments:
// - name: a guid string _or_ the name of a profile
// Return Value:
// - the GUID of the profile corresponding to this name
Model::Profile CascadiaSettings::GetProfileByName(const winrt::hstring& name) const
{
    // First, try and parse the "name" as a GUID. If it's a
    // GUID, and the GUID of one of our profiles, then use that as the
    // profile GUID instead. If it's not, then try looking it up as a
    // name of a profile. If it's still not that, then just ignore it.
    if (!name.empty())
    {
        // Do a quick heuristic check - is the profile 38 chars long (the
        // length of a GUID string), and does it start with '{'? Because if
        // it doesn't, it's _definitely_ not a GUID.
        if (name.size() == 38 && name[0] == L'{')
        {
            const auto newGUID{ Utils::GuidFromString(name.c_str()) };
            if (auto profile = FindProfile(newGUID))
            {
                return profile;
            }
        }

        // Here, we were unable to use the profile string as a GUID to
        // lookup a profile. Instead, try using the string to look the
        // Profile up by name.
        for (auto profile : _allProfiles)
        {
            if (profile.Name() == name)
            {
                return profile;
            }
        }
    }

    return nullptr;
}

// Method Description:
// - Helper to get the profile at the given index in the list of profiles.
// - Returns a nullptr if the index is out of bounds.
// Arguments:
// - index: The profile index in ActiveProfiles()
// Return Value:
// - the Nth profile
Model::Profile CascadiaSettings::GetProfileByIndex(uint32_t index) const
{
    return index < _activeProfiles.Size() ? _activeProfiles.GetAt(index) : nullptr;
}

// Method Description:
// - If there were any warnings we generated while parsing the user's
//   keybindings, add them to the list of warnings here. If there were warnings
//   generated in this way, we'll add a AtLeastOneKeybindingWarning, which will
//   act as a header for the other warnings
// - GH#3522
//   With variable args to keybindings, it's possible that a user
//   set a keybinding without all the required args for an action.
//   Display a warning if an action didn't have a required arg.
//   This will also catch other keybinding warnings, like from GH#4239.
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::_validateKeybindings() const
{
    const auto keybindingWarnings = _globals->KeybindingsWarnings();

    if (!keybindingWarnings.empty())
    {
        _warnings.Append(SettingsLoadWarnings::AtLeastOneKeybindingWarning);
        for (auto warning : keybindingWarnings)
        {
            _warnings.Append(warning);
        }
    }
}

// Method Description:
// - Ensures that every "setColorScheme" command has a valid "color scheme" set.
// Arguments:
// - <none>
// Return Value:
// - <none>
// - Appends a SettingsLoadWarnings::InvalidColorSchemeInCmd to our list of warnings if
//   we find any command with an invalid color scheme.
void CascadiaSettings::_validateColorSchemesInCommands() const
{
    bool foundInvalidScheme{ false };
    for (const auto& nameAndCmd : _globals->ActionMap().NameMap())
    {
        if (_hasInvalidColorScheme(nameAndCmd.Value()))
        {
            foundInvalidScheme = true;
            break;
        }
    }

    if (foundInvalidScheme)
    {
        _warnings.Append(SettingsLoadWarnings::InvalidColorSchemeInCmd);
    }
}

bool CascadiaSettings::_hasInvalidColorScheme(const Model::Command& command) const
{
    bool invalid{ false };
    if (command.HasNestedCommands())
    {
        for (const auto& nested : command.NestedCommands())
        {
            if (_hasInvalidColorScheme(nested.Value()))
            {
                invalid = true;
                break;
            }
        }
    }
    else if (const auto& actionAndArgs = command.ActionAndArgs())
    {
        if (const auto& realArgs = actionAndArgs.Args().try_as<Model::SetColorSchemeArgs>())
        {
            const auto cmdImpl{ winrt::get_self<Command>(command) };
            // no need to validate iterable commands on color schemes
            // they will be expanded to commands with a valid scheme name
            if (cmdImpl->IterateOn() != ExpandCommandType::ColorSchemes &&
                !_globals->ColorSchemes().HasKey(realArgs.SchemeName()))
            {
                invalid = true;
            }
        }
    }

    return invalid;
}

// Method Description:
// - Lookup the color scheme for a given profile. If the profile doesn't exist,
//   or the scheme name listed in the profile doesn't correspond to a scheme,
//   this will return `nullptr`.
// Arguments:
// - profileGuid: the GUID of the profile to find the scheme for.
// Return Value:
// - a non-owning pointer to the scheme.
Model::ColorScheme CascadiaSettings::GetColorSchemeForProfile(const Model::Profile& profile) const
{
    if (!profile)
    {
        return nullptr;
    }
    const auto schemeName = profile.DefaultAppearance().ColorSchemeName();
    return _globals->ColorSchemes().TryLookup(schemeName);
}

// Method Description:
// - updates all references to that color scheme with the new name
// Arguments:
// - oldName: the original name for the color scheme
// - newName: the new name for the color scheme
// Return Value:
// - <none>
void CascadiaSettings::UpdateColorSchemeReferences(const winrt::hstring& oldName, const winrt::hstring& newName)
{
    // update profiles.defaults, if necessary
    if (_baseLayerProfile &&
        _baseLayerProfile->DefaultAppearance().HasColorSchemeName() &&
        _baseLayerProfile->DefaultAppearance().ColorSchemeName() == oldName)
    {
        _baseLayerProfile->DefaultAppearance().ColorSchemeName(newName);
    }

    // update all profiles referencing this color scheme
    for (const auto& profile : _allProfiles)
    {
        const auto defaultAppearance = profile.DefaultAppearance();
        if (defaultAppearance.HasColorSchemeName() && defaultAppearance.ColorSchemeName() == oldName)
        {
            defaultAppearance.ColorSchemeName(newName);
        }

        if (profile.UnfocusedAppearance())
        {
            if (profile.UnfocusedAppearance().HasColorSchemeName() && profile.UnfocusedAppearance().ColorSchemeName() == oldName)
            {
                profile.UnfocusedAppearance().ColorSchemeName(newName);
            }
        }
    }
}

winrt::hstring CascadiaSettings::ApplicationDisplayName()
{
    try
    {
        const auto package{ winrt::Windows::ApplicationModel::Package::Current() };
        return package.DisplayName();
    }
    CATCH_LOG();

    return RS_(L"ApplicationDisplayNameUnpackaged");
}

winrt::hstring CascadiaSettings::ApplicationVersion()
{
    try
    {
        const auto package{ winrt::Windows::ApplicationModel::Package::Current() };
        const auto version{ package.Id().Version() };
        winrt::hstring formatted{ wil::str_printf<std::wstring>(L"%u.%u.%u.%u", version.Major, version.Minor, version.Build, version.Revision) };
        return formatted;
    }
    CATCH_LOG();

    // Get the product version the old-fashioned way from the localized version compartment.
    //
    // We explicitly aren't using VS_FIXEDFILEINFO here, because our build pipeline puts
    // a non-standard version number into the localized version field.
    // For instance the fixed file info might contain "1.12.2109.13002",
    // while the localized field might contain "1.11.210830001-release1.11".
    try
    {
        struct LocalizationInfo
        {
            WORD language, codepage;
        };
        // Use the current module instance handle for TerminalApp.dll, nullptr for WindowsTerminal.exe
        auto filename{ wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle()) };
        auto size{ GetFileVersionInfoSizeExW(0, filename.c_str(), nullptr) };
        THROW_LAST_ERROR_IF(size == 0);
        auto versionBuffer{ std::make_unique<std::byte[]>(size) };
        THROW_IF_WIN32_BOOL_FALSE(GetFileVersionInfoExW(0, filename.c_str(), 0, size, versionBuffer.get()));

        // Get the list of Version localizations
        LocalizationInfo* pVarLocalization{ nullptr };
        UINT varLen{ 0 };
        THROW_IF_WIN32_BOOL_FALSE(VerQueryValueW(versionBuffer.get(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&pVarLocalization), &varLen));
        THROW_HR_IF(E_UNEXPECTED, varLen < sizeof(*pVarLocalization)); // there must be at least one translation

        // Get the product version from the localized version compartment
        // We're using String/ProductVersion here because our build pipeline puts more rich information in it (like the branch name)
        // than in the unlocalized numeric version fields.
        WCHAR* pProductVersion{ nullptr };
        UINT versionLen{ 0 };
        const auto localizedVersionName{ wil::str_printf<std::wstring>(L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                                                                       pVarLocalization->language ? pVarLocalization->language : 0x0409, // well-known en-US LCID
                                                                       pVarLocalization->codepage) };
        THROW_IF_WIN32_BOOL_FALSE(VerQueryValueW(versionBuffer.get(), localizedVersionName.c_str(), reinterpret_cast<void**>(&pProductVersion), &versionLen));
        return { pProductVersion };
    }
    CATCH_LOG();

    return RS_(L"ApplicationVersionUnknown");
}

// Method Description:
// - Determines if we're on an OS platform that supports
//   the default terminal handoff functionality.
// Arguments:
// - <none>
// Return Value:
// - True if OS supports default terminal. False otherwise.
bool CascadiaSettings::IsDefaultTerminalAvailable() noexcept
{
    OSVERSIONINFOEXW osver{};
    osver.dwOSVersionInfoSize = sizeof(osver);
    osver.dwBuildNumber = 22000;

    DWORDLONG dwlConditionMask = 0;
    VER_SET_CONDITION(dwlConditionMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

    return VerifyVersionInfoW(&osver, VER_BUILDNUMBER, dwlConditionMask) != FALSE;
}

// Method Description:
// - Returns an iterable collection of all available terminals.
// Arguments:
// - <none>
// Return Value:
// - an iterable collection of all available terminals that could be the default.
IObservableVector<Model::DefaultTerminal> CascadiaSettings::DefaultTerminals() const noexcept
{
    const auto available = DefaultTerminal::Available();
    std::vector<Model::DefaultTerminal> terminals{ available.Size(), nullptr };
    available.GetMany(0, terminals);
    return winrt::single_threaded_observable_vector(std::move(terminals));
}

// Method Description:
// - Returns the currently selected default terminal application.
// - DANGER! This will be null unless you've called
//   CascadiaSettings::RefreshDefaultTerminals. At the time of this comment (May

//   2021), only the Launch page in the settings UI calls that method, so this
//   value is unset unless you've navigated to that page.
// Arguments:
// - <none>
// Return Value:
// - the selected default terminal application
Settings::Model::DefaultTerminal CascadiaSettings::CurrentDefaultTerminal() noexcept
{
    if (!_currentDefaultTerminal)
    {
        _currentDefaultTerminal = DefaultTerminal::Current();
    }
    return _currentDefaultTerminal;
}

// Method Description:
// - Sets the current default terminal application
// Arguments:
// - terminal - Terminal from `DefaultTerminals` list to set as default
// Return Value:
// - <none>
void CascadiaSettings::CurrentDefaultTerminal(const Model::DefaultTerminal& terminal)
{
    _currentDefaultTerminal = terminal;
}
