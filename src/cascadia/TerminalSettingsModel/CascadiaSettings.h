/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- CascadiaSettings.h

Abstract:
- This class acts as the container for all app settings. It's composed of two
        parts: Globals, which are app-wide settings, and Profiles, which contain
        a set of settings that apply to a single instance of the terminal.
  Also contains the logic for serializing and deserializing this object.

Author(s):
- Mike Griese - March 2019

--*/
#pragma once

#include "CascadiaSettings.g.h"

#include "GlobalAppSettings.h"
#include "Profile.h"

namespace winrt::Microsoft::Terminal::Settings::Model
{
    class IDynamicProfileGenerator;
}

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    winrt::com_ptr<Profile> CreateChild(const winrt::com_ptr<Profile>& parent);

    class SettingsTypedDeserializationException final : public std::runtime_error
    {
    public:
        SettingsTypedDeserializationException(const char* message) noexcept :
            std::runtime_error(message) {}
    };

    struct ParsedSettings
    {
        winrt::com_ptr<implementation::GlobalAppSettings> globals;
        winrt::com_ptr<implementation::Profile> baseLayerProfile;
        std::vector<winrt::com_ptr<implementation::Profile>> profiles;
        std::unordered_map<winrt::guid, winrt::com_ptr<implementation::Profile>> profilesByGuid;
    };

    struct SettingsLoader
    {
        static SettingsLoader Default(const std::string_view& userJSON, const std::string_view& inboxJSON);
        SettingsLoader(const std::string_view& userJSON, const std::string_view& inboxJSON);

        void GenerateProfiles();
        void ApplyRuntimeInitialSettings();
        void MergeInboxIntoUserSettings();
        void FindFragmentsAndMergeIntoUserSettings();
        void FinalizeLayering();
        bool DisableDeletedProfiles();

        ParsedSettings inboxSettings;
        ParsedSettings userSettings;
        bool duplicateProfile = false;

    private:
        static std::pair<size_t, size_t> _lineAndColumnFromPosition(const std::string_view& string, const size_t position);
        static void _rethrowSerializationExceptionWithLocationInfo(const JsonUtils::DeserializationError& e, const std::string_view& settingsString);
        static Json::Value _parseJSON(const std::string_view& content);
        static const Json::Value& _getJSONValue(const Json::Value& json, const std::string_view& key) noexcept;
        gsl::span<const winrt::com_ptr<implementation::Profile>> _getNonUserOriginProfiles() const;
        void _parse(const OriginTag origin, const winrt::hstring& source, const std::string_view& content, ParsedSettings& settings, bool updatesKeyAllowed = false);
        void _appendProfile(winrt::com_ptr<implementation::Profile>&& profile, ParsedSettings& settings);
        static void _addParentProfile(const winrt::com_ptr<implementation::Profile>& profile, ParsedSettings& settings);
        void _executeGenerator(const IDynamicProfileGenerator& generator);

        std::unordered_set<std::wstring_view> _ignoredNamespaces;
        // See _getNonUserOriginProfiles().
        size_t _userProfileCount = 0;
    };

    struct CascadiaSettings : CascadiaSettingsT<CascadiaSettings>
    {
    public:
        static Model::CascadiaSettings LoadDefaults();
        static Model::CascadiaSettings LoadAll();
        static Model::CascadiaSettings LoadUniversal();

        static winrt::hstring SettingsPath();
        static winrt::hstring DefaultSettingsPath();
        static winrt::hstring ApplicationDisplayName();
        static winrt::hstring ApplicationVersion();

        CascadiaSettings() noexcept = default;
        CascadiaSettings(const winrt::hstring& userJSON, const winrt::hstring& inboxJSON);
        CascadiaSettings(const std::string_view& userJSON, const std::string_view& inboxJSON = {});
        explicit CascadiaSettings(SettingsLoader&& loader);

        // user settings
        Model::CascadiaSettings Copy() const;
        Model::GlobalAppSettings GlobalSettings() const;
        winrt::Windows::Foundation::Collections::IObservableVector<Model::Profile> AllProfiles() const noexcept;
        winrt::Windows::Foundation::Collections::IObservableVector<Model::Profile> ActiveProfiles() const noexcept;
        Model::ActionMap ActionMap() const noexcept;
        void WriteSettingsToDisk() const;
        Json::Value ToJson() const;
        Model::Profile ProfileDefaults() const;
        Model::Profile CreateNewProfile();
        Model::Profile FindProfile(const winrt::guid& guid) const noexcept;
        Model::ColorScheme GetColorSchemeForProfile(const Model::Profile& profile) const;
        void UpdateColorSchemeReferences(const winrt::hstring& oldName, const winrt::hstring& newName);
        Model::Profile GetProfileForArgs(const Model::NewTerminalArgs& newTerminalArgs) const;
        Model::Profile GetProfileByName(const winrt::hstring& name) const;
        Model::Profile GetProfileByIndex(uint32_t index) const;
        Model::Profile DuplicateProfile(const Model::Profile& source);

        // load errors
        winrt::Windows::Foundation::Collections::IVectorView<Model::SettingsLoadWarnings> Warnings() const;
        winrt::Windows::Foundation::IReference<Model::SettingsLoadErrors> GetLoadingError() const;
        winrt::hstring GetSerializationErrorMessage() const;

        // defterm
        static bool IsDefaultTerminalAvailable() noexcept;
        winrt::Windows::Foundation::Collections::IObservableVector<Model::DefaultTerminal> DefaultTerminals() const noexcept;
        Model::DefaultTerminal CurrentDefaultTerminal() noexcept;
        void CurrentDefaultTerminal(const Model::DefaultTerminal& terminal);

    private:
        static const std::filesystem::path& _settingsPath();
        static std::wstring _normalizeCommandLine(LPCWSTR commandLine);

        winrt::com_ptr<implementation::Profile> _createNewProfile(const std::wstring_view& name) const;
        Model::Profile _getProfileForCommandLine(const winrt::hstring& commandLine) const;

        void _resolveDefaultProfile() const;

        void _validateSettings();
        void _validateAllSchemesExist();
        void _validateMediaResources();
        void _validateKeybindings() const;
        void _validateColorSchemesInCommands() const;
        bool _hasInvalidColorScheme(const Model::Command& command) const;

        // user settings
        winrt::com_ptr<implementation::GlobalAppSettings> _globals;
        winrt::com_ptr<implementation::Profile> _baseLayerProfile;
        winrt::Windows::Foundation::Collections::IObservableVector<Model::Profile> _allProfiles;
        winrt::Windows::Foundation::Collections::IObservableVector<Model::Profile> _activeProfiles;

        // load errors
        winrt::Windows::Foundation::Collections::IVector<Model::SettingsLoadWarnings> _warnings;
        winrt::Windows::Foundation::IReference<Model::SettingsLoadErrors> _loadError;
        winrt::hstring _deserializationErrorMessage;

        // defterm
        Model::DefaultTerminal _currentDefaultTerminal{ nullptr };

        // GetProfileForArgs cache
        mutable std::once_flag _commandLinesCacheOnce;
        mutable std::vector<std::pair<std::wstring, Model::Profile>> _commandLinesCache;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(CascadiaSettings);
}
