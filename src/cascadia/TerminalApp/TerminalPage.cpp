// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "Utils.h"
#include "../../types/inc/utils.hpp"

#include <filesystem>
#include <LibraryResources.h>

#include "TerminalPage.g.cpp"
#include <winrt/Windows.Storage.h>

#include "TabRowControl.h"
#include "ColorHelper.h"
#include "DebugTapConnection.h"
#include "SettingsTab.h"
#include "RenameWindowRequestedArgs.g.cpp"
#include "../inc/WindowingBehavior.h"

#include <til/latch.h>

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace std::chrono_literals;

#define HOOKUP_ACTION(action) _actionDispatch->action({ this, &TerminalPage::_Handle##action });

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace winrt::TerminalApp::implementation
{
    TerminalPage::TerminalPage() :
        _tabs{ winrt::single_threaded_observable_vector<TerminalApp::TabBase>() },
        _mruTabs{ winrt::single_threaded_observable_vector<TerminalApp::TabBase>() },
        _startupActions{ winrt::single_threaded_vector<ActionAndArgs>() },
        _hostingHwnd{}
    {
        InitializeComponent();
    }

    // Method Description:
    // - implements the IInitializeWithWindow interface from shobjidl_core.
    HRESULT TerminalPage::Initialize(HWND hwnd)
    {
        _hostingHwnd = hwnd;
        return S_OK;
    }

    // Function Description:
    // - Recursively check our commands to see if there's a keybinding for
    //   exactly their action. If there is, label that command with the text
    //   corresponding to that key chord.
    // - Will recurse into nested commands as well.
    // Arguments:
    // - settings: The settings who's keybindings we should use to look up the key chords from
    // - commands: The list of commands to label.
    static void _recursiveUpdateCommandKeybindingLabels(CascadiaSettings settings,
                                                        IMapView<winrt::hstring, Command> commands)
    {
        for (const auto& nameAndCmd : commands)
        {
            const auto& command = nameAndCmd.Value();
            if (command.HasNestedCommands())
            {
                _recursiveUpdateCommandKeybindingLabels(settings, command.NestedCommands());
            }
            else
            {
                // If there's a keybinding that's bound to exactly this command,
                // then get the keychord and display it as a
                // part of the command in the UI.
                // We specifically need to do this for nested commands.
                const auto keyChord{ settings.ActionMap().GetKeyBindingForAction(command.ActionAndArgs().Action(), command.ActionAndArgs().Args()) };
                command.RegisterKey(keyChord);
            }
        }
    }

    void TerminalPage::SetSettings(CascadiaSettings settings, bool needRefreshUI)
    {
        _settings = settings;

        // Make sure to _UpdateCommandsForPalette before
        // _RefreshUIForSettingsReload. _UpdateCommandsForPalette will make
        // sure the KeyChordText of Commands is updated, which needs to
        // happen before the Settings UI is reloaded and tries to re-read
        // those values.
        _UpdateCommandsForPalette();
        CommandPalette().SetActionMap(_settings.ActionMap());

        if (needRefreshUI)
        {
            _RefreshUIForSettingsReload();
        }

        // Upon settings update we reload the system settings for scrolling as well.
        // TODO: consider reloading this value periodically.
        _systemRowsToScroll = _ReadSystemRowsToScroll();
    }

    bool TerminalPage::IsElevated() const noexcept
    {
        // use C++11 magic statics to make sure we only do this once.
        // This won't change over the lifetime of the application

        static const bool isElevated = []() {
            // *** THIS IS A SINGLETON ***
            auto result = false;

            // GH#2455 - Make sure to try/catch calls to Application::Current,
            // because that _won't_ be an instance of TerminalApp::App in the
            // LocalTests
            try
            {
                result = ::winrt::Windows::UI::Xaml::Application::Current().as<::winrt::TerminalApp::App>().Logic().IsElevated();
            }
            CATCH_LOG();
            return result;
        }();

        return isElevated;
    }

    void TerminalPage::Create()
    {
        // Hookup the key bindings
        _HookupKeyBindings(_settings.ActionMap());

        _tabContent = this->TabContent();
        _tabRow = this->TabRow();
        _tabView = _tabRow.TabView();
        _rearranging = false;

        const auto isElevated = IsElevated();

        if (_settings.GlobalSettings().UseAcrylicInTabRow())
        {
            const auto res = Application::Current().Resources();

            const auto lightKey = winrt::box_value(L"Light");
            const auto darkKey = winrt::box_value(L"Dark");
            const auto tabViewBackgroundKey = winrt::box_value(L"TabViewBackground");

            for (auto const& dictionary : res.MergedDictionaries())
            {
                // Don't change MUX resources
                if (dictionary.Source())
                {
                    continue;
                }

                for (auto const& kvPair : dictionary.ThemeDictionaries())
                {
                    const auto themeDictionary = kvPair.Value().as<winrt::Windows::UI::Xaml::ResourceDictionary>();

                    if (themeDictionary.HasKey(tabViewBackgroundKey))
                    {
                        const auto backgroundSolidBrush = themeDictionary.Lookup(tabViewBackgroundKey).as<Media::SolidColorBrush>();

                        const til::color backgroundColor = backgroundSolidBrush.Color();

                        const auto acrylicBrush = Media::AcrylicBrush();
                        acrylicBrush.BackgroundSource(Media::AcrylicBackgroundSource::HostBackdrop);
                        acrylicBrush.FallbackColor(backgroundColor);
                        acrylicBrush.TintColor(backgroundColor);
                        acrylicBrush.TintOpacity(0.5);

                        themeDictionary.Insert(tabViewBackgroundKey, acrylicBrush);
                    }
                }
            }
        }

        _tabRow.PointerMoved({ get_weak(), &TerminalPage::_RestorePointerCursorHandler });
        _tabView.CanReorderTabs(!isElevated);
        _tabView.CanDragTabs(!isElevated);
        _tabView.TabDragStarting({ get_weak(), &TerminalPage::_TabDragStarted });
        _tabView.TabDragCompleted({ get_weak(), &TerminalPage::_TabDragCompleted });

        auto tabRowImpl = winrt::get_self<implementation::TabRowControl>(_tabRow);
        _newTabButton = tabRowImpl->NewTabButton();

        if (_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            // Remove the TabView from the page. We'll hang on to it, we need to
            // put it in the titlebar.
            uint32_t index = 0;
            if (this->Root().Children().IndexOf(_tabRow, index))
            {
                this->Root().Children().RemoveAt(index);
            }

            // Inform the host that our titlebar content has changed.
            _SetTitleBarContentHandlers(*this, _tabRow);
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _RegisterActionCallbacks();

        // Hook up inbound connection event handler
        ConptyConnection::NewConnection({ this, &TerminalPage::_OnNewConnection });

        //Event Bindings (Early)
        _newTabButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                page->_OpenNewTerminal(NewTerminalArgs());
            }
        });
        _newTabButton.Drop([weakThis{ get_weak() }](Windows::Foundation::IInspectable const&, winrt::Windows::UI::Xaml::DragEventArgs e) {
            if (auto page{ weakThis.get() })
            {
                page->NewTerminalByDrop(e);
            }
        });
        _tabView.SelectionChanged({ this, &TerminalPage::_OnTabSelectionChanged });
        _tabView.TabCloseRequested({ this, &TerminalPage::_OnTabCloseRequested });
        _tabView.TabItemsChanged({ this, &TerminalPage::_OnTabItemsChanged });

        _CreateNewTabFlyout();

        _UpdateTabWidthMode();

        _tabContent.SizeChanged({ this, &TerminalPage::_OnContentSizeChanged });

        // When the visibility of the command palette changes to "collapsed",
        // the palette has been closed. Toss focus back to the currently active
        // control.
        CommandPalette().RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (CommandPalette().Visibility() == Visibility::Collapsed)
            {
                _FocusActiveControl(nullptr, nullptr);
            }
        });
        CommandPalette().DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
        CommandPalette().CommandLineExecutionRequested({ this, &TerminalPage::_OnCommandLineExecutionRequested });
        CommandPalette().SwitchToTabRequested({ this, &TerminalPage::_OnSwitchToTabRequested });
        CommandPalette().PreviewAction({ this, &TerminalPage::_PreviewActionHandler });

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        // Once the page is actually laid out on the screen, trigger all our
        // startup actions. Things like Panes need to know at least how big the
        // window will be, so they can subdivide that space.
        //
        // _OnFirstLayout will remove this handler so it doesn't get called more than once.
        _layoutUpdatedRevoker = _tabContent.LayoutUpdated(winrt::auto_revoke, { this, &TerminalPage::_OnFirstLayout });

        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();

        // DON'T set up Toasts/TeachingTips here. They should be loaded and
        // initialized the first time they're opened, in whatever method opens
        // them.

        // Setup mouse vanish attributes
        SystemParametersInfoW(SPI_GETMOUSEVANISH, 0, &_shouldMouseVanish, false);

        _tabRow.ShowElevationShield(IsElevated() && _settings.GlobalSettings().ShowAdminShield());

        // Store cursor, so we can restore it, e.g., after mouse vanishing
        // (we'll need to adapt this logic once we make cursor context aware)
        try
        {
            _defaultPointerCursor = CoreWindow::GetForCurrentThread().PointerCursor();
        }
        CATCH_LOG();
    }

    // Method Description;
    // - Checks if the current terminal window should load or save its layout information.
    // Arguments:
    // - settings: The settings to use as this may be called before the page is
    //   fully initialized.
    // Return Value:
    // - true if the ApplicationState should be used.
    bool TerminalPage::ShouldUsePersistedLayout(CascadiaSettings& settings) const
    {
        // GH#5000 Until there is a separate state file for elevated sessions we should just not
        // save at all while in an elevated window.
        return Feature_PersistedWindowLayout::IsEnabled() &&
               !IsElevated() &&
               settings.GlobalSettings().FirstWindowPreference() == FirstWindowPreference::PersistedWindowLayout;
    }

    // Method Description;
    // - Checks if the current window is configured to load a particular layout
    // Arguments:
    // - settings: The settings to use as this may be called before the page is
    //   fully initialized.
    // Return Value:
    // - non-null if there is a particular saved layout to use
    std::optional<uint32_t> TerminalPage::LoadPersistedLayoutIdx(CascadiaSettings& settings) const
    {
        return ShouldUsePersistedLayout(settings) ? _loadFromPersistedLayoutIdx : std::nullopt;
    }

    WindowLayout TerminalPage::LoadPersistedLayout(CascadiaSettings& settings) const
    {
        if (const auto idx = LoadPersistedLayoutIdx(settings))
        {
            const auto i = idx.value();
            const auto layouts = ApplicationState::SharedInstance().PersistedWindowLayouts();
            if (layouts && layouts.Size() > i)
            {
                return layouts.GetAt(i);
            }
        }
        return nullptr;
    }

    winrt::fire_and_forget TerminalPage::NewTerminalByDrop(winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        Windows::Foundation::Collections::IVectorView<Windows::Storage::IStorageItem> items;
        try
        {
            items = co_await e.DataView().GetStorageItemsAsync();
        }
        CATCH_LOG();

        if (items.Size() == 1)
        {
            std::filesystem::path path(items.GetAt(0).Path().c_str());
            if (!std::filesystem::is_directory(path))
            {
                path = path.parent_path();
            }

            NewTerminalArgs args;
            args.StartingDirectory(winrt::hstring{ path.wstring() });
            this->_OpenNewTerminal(args);

            TraceLoggingWrite(
                g_hTerminalAppProvider,
                "NewTabByDragDrop",
                TraceLoggingDescription("Event emitted when the user drag&drops onto the new tab button"),
                TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
        }
    }

    // Method Description:
    // - This method is called once command palette action was chosen for dispatching
    //   We'll use this event to dispatch this command.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnDispatchCommandRequested(const IInspectable& /*sender*/, const Microsoft::Terminal::Settings::Model::Command& command)
    {
        const auto& actionAndArgs = command.ActionAndArgs();
        _actionDispatch->DoAction(actionAndArgs);
    }

    // Method Description:
    // - This method is called once command palette command line was chosen for execution
    //   We'll use this event to create a command line execution command and dispatch it.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnCommandLineExecutionRequested(const IInspectable& /*sender*/, const winrt::hstring& commandLine)
    {
        ExecuteCommandlineArgs args{ commandLine };
        ActionAndArgs actionAndArgs{ ShortcutAction::ExecuteCommandline, args };
        _actionDispatch->DoAction(actionAndArgs);
    }

    // Method Description:
    // - This method is called once on startup, on the first LayoutUpdated event.
    //   We'll use this event to know that we have an ActualWidth and
    //   ActualHeight, so we can now attempt to process our list of startup
    //   actions.
    // - We'll remove this event handler when the event is first handled.
    // - If there are no startup actions, we'll open a single tab with the
    //   default profile.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_OnFirstLayout(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        // Only let this succeed once.
        _layoutUpdatedRevoker.revoke();

        // This event fires every time the layout changes, but it is always the
        // last one to fire in any layout change chain. That gives us great
        // flexibility in finding the right point at which to initialize our
        // renderer (and our terminal). Any earlier than the last layout update
        // and we may not know the terminal's starting size.
        if (_startupState == StartupState::NotInitialized)
        {
            _startupState = StartupState::InStartup;

            // If we are provided with an index, the cases where we have
            // commandline args and startup actions are already handled.
            if (const auto layout = LoadPersistedLayout(_settings))
            {
                if (layout.TabLayout().Size() > 0)
                {
                    _startupActions = layout.TabLayout();
                }
            }

            ProcessStartupActions(_startupActions, true);

            // If we were told that the COM server needs to be started to listen for incoming
            // default application connections, start it now.
            // This MUST be done after we've registered the event listener for the new connections
            // or the COM server might start receiving requests on another thread and dispatch
            // them to nowhere.
            _StartInboundListener();
        }
    }

    // Routine Description:
    // - Will start the listener for inbound console handoffs if we have already determined
    //   that we should do so.
    // NOTE: Must be after TerminalPage::_OnNewConnection has been connected up.
    // Arguments:
    // - <unused> - Looks at _shouldStartInboundListener
    // Return Value:
    // - <none> - May fail fast if setup fails as that would leave us in a weird state.
    void TerminalPage::_StartInboundListener()
    {
        if (_shouldStartInboundListener)
        {
            _shouldStartInboundListener = false;

            try
            {
                winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection::StartInboundListener();
            }
            // If we failed to start the listener, it will throw.
            // We don't want to fail fast here because if a peasant has some trouble with
            // starting the listener, we don't want it to crash and take all its tabs down
            // with it.
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
            }
        }
    }

    // Method Description:
    // - Process all the startup actions in the provided list of startup
    //   actions. We'll do this all at once here.
    // Arguments:
    // - actions: a winrt vector of actions to process. Note that this must NOT
    //   be an IVector&, because we need the collection to be accessible on the
    //   other side of the co_await.
    // - initial: if true, we're parsing these args during startup, and we
    //   should fire an Initialized event.
    // - cwd: If not empty, we should try switching to this provided directory
    //   while processing these actions. This will allow something like `wt -w 0
    //   nt -d .` from inside another directory to work as expected.
    // Return Value:
    // - <none>
    winrt::fire_and_forget TerminalPage::ProcessStartupActions(Windows::Foundation::Collections::IVector<ActionAndArgs> actions,
                                                               const bool initial,
                                                               const winrt::hstring cwd)
    {
        auto weakThis{ get_weak() };

        // Handle it on a subsequent pass of the UI thread.
        co_await winrt::resume_foreground(Dispatcher(), CoreDispatcherPriority::Normal);

        // If the caller provided a CWD, switch to that directory, then switch
        // back once we're done. This looks weird though, because we have to set
        // up the scope_exit _first_. We'll release the scope_exit if we don't
        // actually need it.
        std::wstring originalCwd{ wil::GetCurrentDirectoryW<std::wstring>() };
        auto restoreCwd = wil::scope_exit([&originalCwd]() {
            // ignore errors, we'll just power on through. We'd rather do
            // something rather than fail silently if the directory doesn't
            // actually exist.
            LOG_IF_WIN32_BOOL_FALSE(SetCurrentDirectory(originalCwd.c_str()));
        });
        if (cwd.empty())
        {
            restoreCwd.release();
        }
        else
        {
            // ignore errors, we'll just power on through. We'd rather do
            // something rather than fail silently if the directory doesn't
            // actually exist.
            LOG_IF_WIN32_BOOL_FALSE(SetCurrentDirectory(cwd.c_str()));
        }

        if (auto page{ weakThis.get() })
        {
            for (const auto& action : actions)
            {
                if (auto page{ weakThis.get() })
                {
                    _actionDispatch->DoAction(action);
                }
                else
                {
                    co_return;
                }
            }

            // GH#6586: now that we're done processing all startup commands,
            // focus the active control. This will work as expected for both
            // commandline invocations and for `wt` action invocations.
            if (const auto control = _GetActiveControl())
            {
                control.Focus(FocusState::Programmatic);
            }
        }
        if (initial)
        {
            _CompleteInitialization();
        }
    }

    // Method Description:
    // - Perform and steps that need to be done once our initial state is all
    //   set up. This includes entering fullscreen mode and firing our
    //   Initialized event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_CompleteInitialization()
    {
        _startupState = StartupState::Initialized;
        _InitializedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Show a dialog with "About" information. Displays the app's Display
    //   Name, version, getting started link, documentation link, release
    //   Notes link, and privacy policy link.
    void TerminalPage::_ShowAboutDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            presenter.ShowDialog(FindName(L"AboutDialog").try_as<WUX::Controls::ContentDialog>());
        }
    }

    winrt::hstring TerminalPage::ApplicationDisplayName()
    {
        return CascadiaSettings::ApplicationDisplayName();
    }

    winrt::hstring TerminalPage::ApplicationVersion()
    {
        return CascadiaSettings::ApplicationVersion();
    }

    void TerminalPage::_ThirdPartyNoticesOnClick(const IInspectable& /*sender*/, const Windows::UI::Xaml::RoutedEventArgs& /*eventArgs*/)
    {
        std::filesystem::path currentPath{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
        currentPath.replace_filename(L"NOTICE.html");
        ShellExecute(nullptr, nullptr, currentPath.c_str(), nullptr, nullptr, SW_SHOW);
    }

    // Method Description:
    // - Displays a dialog to warn the user that they are about to close all open windows.
    //   Once the user clicks the OK button, shut down the application.
    //   If cancel is clicked, the dialog will close.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowQuitDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"QuitDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal app using
    //   key binding with multiple tabs opened. Display messages to warn user
    //   that more than 1 tab is opened, and once the user clicks the OK button, remove
    //   all the tabs and shut down and app. If cancel is clicked, the dialog will close
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseWarningDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"CloseAllDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal tab marked as read-only
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseReadOnlyDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"CloseReadOnlyDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste contains the "new line" character which can
    //   have the effect of starting commands without the user's knowledge if
    //   it is pasted on a shell where the "new line" character marks the end
    //   of a command.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowMultiLinePasteWarningDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"MultiLinePasteDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste is very long, in case they did not mean to
    //   paste it but pressed the paste shortcut by accident.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowLargePasteWarningDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"LargePasteDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Builds the flyout (dropdown) attached to the new tab button, and
    //   attaches it to the button. Populates the flyout with one entry per
    //   Profile, displaying the profile's name. Clicking each flyout item will
    //   open a new tab with that profile.
    //   Below the profiles are the static menu items: settings, command palette
    void TerminalPage::_CreateNewTabFlyout()
    {
        auto newTabFlyout = WUX::Controls::MenuFlyout{};
        newTabFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);

        auto actionMap = _settings.ActionMap();
        const auto defaultProfileGuid = _settings.GlobalSettings().DefaultProfile();
        // the number of profiles should not change in the loop for this to work
        auto const profileCount = gsl::narrow_cast<int>(_settings.ActiveProfiles().Size());
        for (int profileIndex = 0; profileIndex < profileCount; profileIndex++)
        {
            const auto profile = _settings.ActiveProfiles().GetAt(profileIndex);
            auto profileMenuItem = WUX::Controls::MenuFlyoutItem{};

            // Add the keyboard shortcuts based on the number of profiles defined
            // Look for a keychord that is bound to the equivalent
            // NewTab(ProfileIndex=N) action
            NewTerminalArgs newTerminalArgs{ profileIndex };
            NewTabArgs newTabArgs{ newTerminalArgs };
            auto profileKeyChord{ actionMap.GetKeyBindingForAction(ShortcutAction::NewTab, newTabArgs) };

            // make sure we find one to display
            if (profileKeyChord)
            {
                _SetAcceleratorForMenuItem(profileMenuItem, profileKeyChord);
            }

            auto profileName = profile.Name();
            profileMenuItem.Text(profileName);

            // If there's an icon set for this profile, set it as the icon for
            // this flyout item.
            if (!profile.Icon().empty())
            {
                const auto iconSource{ IconPathConverter().IconSourceWUX(profile.Icon()) };

                WUX::Controls::IconSourceElement iconElement;
                iconElement.IconSource(iconSource);
                profileMenuItem.Icon(iconElement);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
            }

            if (profile.Guid() == defaultProfileGuid)
            {
                // Contrast the default profile with others in font weight.
                profileMenuItem.FontWeight(FontWeights::Bold());
            }

            auto newTabRun = WUX::Documents::Run();
            newTabRun.Text(RS_(L"NewTabRun/Text"));
            auto newPaneRun = WUX::Documents::Run();
            newPaneRun.Text(RS_(L"NewPaneRun/Text"));
            newPaneRun.FontStyle(FontStyle::Italic);
            auto newWindowRun = WUX::Documents::Run();
            newWindowRun.Text(RS_(L"NewWindowRun/Text"));
            newWindowRun.FontStyle(FontStyle::Italic);

            auto textBlock = WUX::Controls::TextBlock{};
            textBlock.Inlines().Append(newTabRun);
            textBlock.Inlines().Append(WUX::Documents::LineBreak{});
            textBlock.Inlines().Append(newPaneRun);
            textBlock.Inlines().Append(WUX::Documents::LineBreak{});
            textBlock.Inlines().Append(newWindowRun);

            auto toolTip = WUX::Controls::ToolTip{};
            toolTip.Content(textBlock);
            WUX::Controls::ToolTipService::SetToolTip(profileMenuItem, toolTip);

            profileMenuItem.Click([profileIndex, weakThis{ get_weak() }](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    NewTerminalArgs newTerminalArgs{ profileIndex };
                    page->_OpenNewTerminal(newTerminalArgs);
                }
            });
            newTabFlyout.Items().Append(profileMenuItem);
        }

        // add menu separator
        auto separatorItem = WUX::Controls::MenuFlyoutSeparator{};
        newTabFlyout.Items().Append(separatorItem);

        // add static items
        {
            // GH#2455 - Make sure to try/catch calls to Application::Current,
            // because that _won't_ be an instance of TerminalApp::App in the
            // LocalTests
            auto isUwp = false;
            try
            {
                isUwp = ::winrt::Windows::UI::Xaml::Application::Current().as<::winrt::TerminalApp::App>().Logic().IsUwp();
            }
            CATCH_LOG();

            if (!isUwp)
            {
                // Create the settings button.
                auto settingsItem = WUX::Controls::MenuFlyoutItem{};
                settingsItem.Text(RS_(L"SettingsMenuItem"));

                WUX::Controls::SymbolIcon ico{};
                ico.Symbol(WUX::Controls::Symbol::Setting);
                settingsItem.Icon(ico);

                settingsItem.Click({ this, &TerminalPage::_SettingsButtonOnClick });
                newTabFlyout.Items().Append(settingsItem);

                const auto settingsKeyChord{ actionMap.GetKeyBindingForAction(ShortcutAction::OpenSettings, OpenSettingsArgs{ SettingsTarget::SettingsUI }) };
                if (settingsKeyChord)
                {
                    _SetAcceleratorForMenuItem(settingsItem, settingsKeyChord);
                }

                // Create the command palette button.
                auto commandPaletteFlyout = WUX::Controls::MenuFlyoutItem{};
                commandPaletteFlyout.Text(RS_(L"CommandPaletteMenuItem"));

                WUX::Controls::FontIcon commandPaletteIcon{};
                commandPaletteIcon.Glyph(L"\xE945");
                commandPaletteIcon.FontFamily(Media::FontFamily{ L"Segoe MDL2 Assets" });
                commandPaletteFlyout.Icon(commandPaletteIcon);

                commandPaletteFlyout.Click({ this, &TerminalPage::_CommandPaletteButtonOnClick });
                newTabFlyout.Items().Append(commandPaletteFlyout);

                const auto commandPaletteKeyChord{ actionMap.GetKeyBindingForAction(ShortcutAction::ToggleCommandPalette) };
                if (commandPaletteKeyChord)
                {
                    _SetAcceleratorForMenuItem(commandPaletteFlyout, commandPaletteKeyChord);
                }
            }

            // Create the about button.
            auto aboutFlyout = WUX::Controls::MenuFlyoutItem{};
            aboutFlyout.Text(RS_(L"AboutMenuItem"));

            WUX::Controls::SymbolIcon aboutIcon{};
            aboutIcon.Symbol(WUX::Controls::Symbol::Help);
            aboutFlyout.Icon(aboutIcon);

            aboutFlyout.Click({ this, &TerminalPage::_AboutButtonOnClick });
            newTabFlyout.Items().Append(aboutFlyout);
        }

        // Before opening the fly-out set focus on the current tab
        // so no matter how fly-out is closed later on the focus will return to some tab.
        // We cannot do it on closing because if the window loses focus (alt+tab)
        // the closing event is not fired.
        // It is important to set the focus on the tab
        // Since the previous focus location might be discarded in the background,
        // e.g., the command palette will be dismissed by the menu,
        // and then closing the fly-out will move the focus to wrong location.
        newTabFlyout.Opening([this](auto&&, auto&&) {
            _FocusCurrentTab(true);
        });
        _newTabButton.Flyout(newTabFlyout);
    }

    // Function Description:
    // Called when the openNewTabDropdown keybinding is used.
    // Shows the dropdown flyout.
    void TerminalPage::_OpenNewTabDropdown()
    {
        _newTabButton.Flyout().ShowAt(_newTabButton);
    }

    void TerminalPage::_OpenNewTerminal(const NewTerminalArgs newTerminalArgs)
    {
        // if alt is pressed, open a pane
        const CoreWindow window = CoreWindow::GetForCurrentThread();
        const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
        const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
        const bool altPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto rShiftState = window.GetKeyState(VirtualKey::RightShift);
        const auto lShiftState = window.GetKeyState(VirtualKey::LeftShift);
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        // Check for DebugTap
        bool debugTap = this->_settings.GlobalSettings().DebugFeaturesEnabled() &&
                        WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                        WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

        if (altPressed && !debugTap)
        {
            this->_SplitPane(SplitDirection::Automatic,
                             SplitType::Manual,
                             0.5f,
                             newTerminalArgs);
        }
        else if (shiftPressed && !debugTap)
        {
            // Manually fill in the evaluated profile.
            if (newTerminalArgs.ProfileIndex() != nullptr)
            {
                // We want to promote the index to a GUID because there is no "launch to profile index" command.
                const auto profile = _settings.GetProfileForArgs(newTerminalArgs);
                if (profile)
                {
                    newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(profile.Guid()));
                }
            }
            this->_OpenNewWindow(false, newTerminalArgs);
        }
        else
        {
            LOG_IF_FAILED(this->_OpenNewTab(newTerminalArgs));
        }
    }

    winrt::fire_and_forget TerminalPage::_RemoveOnCloseRoutine(Microsoft::UI::Xaml::Controls::TabViewItem tabViewItem, winrt::com_ptr<TerminalPage> page)
    {
        co_await winrt::resume_foreground(page->_tabView.Dispatcher());

        if (auto tab{ _GetTabByTabViewItem(tabViewItem) })
        {
            _RemoveTab(tab);
        }
    }

    // Method Description:
    // - Creates a new connection based on the profile settings
    // Arguments:
    // - the profile we want the settings from
    // - the terminal settings
    // Return value:
    // - the desired connection
    TerminalConnection::ITerminalConnection TerminalPage::_CreateConnectionFromSettings(Profile profile,
                                                                                        TerminalSettings settings)
    {
        TerminalConnection::ITerminalConnection connection{ nullptr };

        winrt::guid connectionType = profile.ConnectionType();
        winrt::guid sessionGuid{};

        if (connectionType == TerminalConnection::AzureConnection::ConnectionType() &&
            TerminalConnection::AzureConnection::IsAzureConnectionAvailable())
        {
            // TODO GH#4661: Replace this with directly using the AzCon when our VT is better
            std::filesystem::path azBridgePath{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
            azBridgePath.replace_filename(L"TerminalAzBridge.exe");
            connection = TerminalConnection::ConptyConnection();
            connection.Initialize(TerminalConnection::ConptyConnection::CreateSettings(azBridgePath.wstring(),
                                                                                       L".",
                                                                                       L"Azure",
                                                                                       nullptr,
                                                                                       ::base::saturated_cast<uint32_t>(settings.InitialRows()),
                                                                                       ::base::saturated_cast<uint32_t>(settings.InitialCols()),
                                                                                       winrt::guid()));
        }

        else
        {
            // profile is guaranteed to exist here
            std::wstring guidWString = Utils::GuidToString(profile.Guid());

            StringMap envMap{};
            envMap.Insert(L"WT_PROFILE_ID", guidWString);
            envMap.Insert(L"WSLENV", L"WT_PROFILE_ID");

            // Update the path to be relative to whatever our CWD is.
            //
            // Refer to the examples in
            // https://en.cppreference.com/w/cpp/filesystem/path/append
            //
            // We need to do this here, to ensure we tell the ConptyConnection
            // the correct starting path. If we're being invoked from another
            // terminal instance (e.g. wt -w 0 -d .), then we have switched our
            // CWD to the provided path. We should treat the StartingDirectory
            // as relative to the current CWD.
            //
            // The connection must be informed of the current CWD on
            // construction, because the connection might not spawn the child
            // process until later, on another thread, after we've already
            // restored the CWD to it's original value.
            winrt::hstring newWorkingDirectory{ settings.StartingDirectory() };
            if (newWorkingDirectory.size() <= 1 ||
                !(newWorkingDirectory[0] == L'~' || newWorkingDirectory[0] == L'/'))
            { // We only want to resolve the new WD against the CWD if it doesn't look like a Linux path (see GH#592)
                std::wstring cwdString{ wil::GetCurrentDirectoryW<std::wstring>() };
                std::filesystem::path cwd{ cwdString };
                cwd /= settings.StartingDirectory().c_str();
                newWorkingDirectory = winrt::hstring{ cwd.wstring() };
            }

            auto conhostConn = TerminalConnection::ConptyConnection();
            conhostConn.Initialize(TerminalConnection::ConptyConnection::CreateSettings(settings.Commandline(),
                                                                                        newWorkingDirectory,
                                                                                        settings.StartingTitle(),
                                                                                        envMap.GetView(),
                                                                                        ::base::saturated_cast<uint32_t>(settings.InitialRows()),
                                                                                        ::base::saturated_cast<uint32_t>(settings.InitialCols()),
                                                                                        winrt::guid()));

            sessionGuid = conhostConn.Guid();
            connection = conhostConn;
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "ConnectionCreated",
            TraceLoggingDescription("Event emitted upon the creation of a connection"),
            TraceLoggingGuid(connectionType, "ConnectionTypeGuid", "The type of the connection"),
            TraceLoggingGuid(profile.Guid(), "ProfileGuid", "The profile's GUID"),
            TraceLoggingGuid(sessionGuid, "SessionGuid", "The WT_SESSION's GUID"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));

        return connection;
    }

    // Method Description:
    // - Called when the settings button is clicked. Launches a background
    //   thread to open the settings file in the default JSON editor.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_SettingsButtonOnClick(const IInspectable&,
                                              const RoutedEventArgs&)
    {
        const CoreWindow window = CoreWindow::GetForCurrentThread();

        // check alt state
        const auto rAltState{ window.GetKeyState(VirtualKey::RightMenu) };
        const auto lAltState{ window.GetKeyState(VirtualKey::LeftMenu) };
        const bool altPressed{ WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                               WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down) };

        // check shift state
        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto lShiftState{ window.GetKeyState(VirtualKey::LeftShift) };
        const auto rShiftState{ window.GetKeyState(VirtualKey::RightShift) };
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        auto target{ SettingsTarget::SettingsUI };
        if (shiftPressed)
        {
            target = SettingsTarget::SettingsFile;
        }
        else if (altPressed)
        {
            target = SettingsTarget::DefaultsFile;
        }
        _LaunchSettings(target);
    }

    // Method Description:
    // - Called when the command palette button is clicked. Opens the command palette.
    void TerminalPage::_CommandPaletteButtonOnClick(const IInspectable&,
                                                    const RoutedEventArgs&)
    {
        CommandPalette().EnableCommandPaletteMode(CommandPaletteLaunchMode::Action);
        CommandPalette().Visibility(Visibility::Visible);
    }

    // Method Description:
    // - Called when the about button is clicked. See _ShowAboutDialog for more info.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_AboutButtonOnClick(const IInspectable&,
                                           const RoutedEventArgs&)
    {
        _ShowAboutDialog();
    }

    // Method Description:
    // Called when the users pressed keyBindings while CommandPalette is open.
    // Arguments:
    // - e: the KeyRoutedEventArgs containing info about the keystroke.
    // Return Value:
    // - <none>
    void TerminalPage::_KeyDownHandler(Windows::Foundation::IInspectable const& /*sender*/, Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        const auto key = e.OriginalKey();
        const auto scanCode = e.KeyStatus().ScanCode;
        const auto coreWindow = CoreWindow::GetForCurrentThread();
        const auto ctrlDown = WI_IsFlagSet(coreWindow.GetKeyState(winrt::Windows::System::VirtualKey::Control), CoreVirtualKeyStates::Down);
        const auto altDown = WI_IsFlagSet(coreWindow.GetKeyState(winrt::Windows::System::VirtualKey::Menu), CoreVirtualKeyStates::Down);
        const auto shiftDown = WI_IsFlagSet(coreWindow.GetKeyState(winrt::Windows::System::VirtualKey::Shift), CoreVirtualKeyStates::Down);

        winrt::Microsoft::Terminal::Control::KeyChord kc{ ctrlDown, altDown, shiftDown, false, static_cast<int32_t>(key), static_cast<int32_t>(scanCode) };
        if (const auto cmd{ _settings.ActionMap().GetActionByKeyChord(kc) })
        {
            if (CommandPalette().Visibility() == Visibility::Visible && cmd.ActionAndArgs().Action() != ShortcutAction::ToggleCommandPalette)
            {
                CommandPalette().Visibility(Visibility::Collapsed);
            }
            _actionDispatch->DoAction(cmd.ActionAndArgs());
            e.Handled(true);
        }
    }

    // Method Description:
    // - Configure the AppKeyBindings to use our ShortcutActionDispatch and the updated ActionMap
    //    as the object to handle dispatching ShortcutAction events.
    // Arguments:
    // - bindings: An IActionMapView object to wire up with our event handlers
    void TerminalPage::_HookupKeyBindings(const IActionMapView& actionMap) noexcept
    {
        _bindings->SetDispatch(*_actionDispatch);
        _bindings->SetActionMap(actionMap);
    }

    // Method Description:
    // - Register our event handlers with our ShortcutActionDispatch. The
    //   ShortcutActionDispatch is responsible for raising the appropriate
    //   events for an ActionAndArgs. WE'll handle each possible event in our
    //   own way.
    // Arguments:
    // - <none>
    void TerminalPage::_RegisterActionCallbacks()
    {
        // Hook up the ShortcutActionDispatch object's events to our handlers.
        // They should all be hooked up here, regardless of whether or not
        // there's an actual keychord for them.
#define ON_ALL_ACTIONS(action) HOOKUP_ACTION(action);
        ALL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
    }

    // Method Description:
    // - Get the title of the currently focused terminal control. If this tab is
    //   the focused tab, then also bubble this title to any listeners of our
    //   TitleChanged event.
    // Arguments:
    // - tab: the Tab to update the title for.
    void TerminalPage::_UpdateTitle(const TerminalTab& tab)
    {
        auto newTabTitle = tab.Title();

        if (tab == _GetFocusedTab())
        {
            _TitleChangedHandlers(*this, newTabTitle);
        }
    }

    // Method Description:
    // - Connects event handlers to the TermControl for events that we want to
    //   handle. This includes:
    //    * the Copy and Paste events, for setting and retrieving clipboard data
    //      on the right thread
    // Arguments:
    // - term: The newly created TermControl to connect the events for
    void TerminalPage::_RegisterTerminalEvents(TermControl term)
    {
        term.RaiseNotice({ this, &TerminalPage::_ControlNoticeRaisedHandler });

        // Add an event handler when the terminal's selection wants to be copied.
        // When the text buffer data is retrieved, we'll copy the data into the Clipboard
        term.CopyToClipboard({ this, &TerminalPage::_CopyToClipboardHandler });

        // Add an event handler when the terminal wants to paste data from the Clipboard.
        term.PasteFromClipboard({ this, &TerminalPage::_PasteFromClipboardHandler });

        term.OpenHyperlink({ this, &TerminalPage::_OpenHyperlinkHandler });

        term.HidePointerCursor({ get_weak(), &TerminalPage::_HidePointerCursorHandler });
        term.RestorePointerCursor({ get_weak(), &TerminalPage::_RestorePointerCursorHandler });
        // Add an event handler for when the terminal or tab wants to set a
        // progress indicator on the taskbar
        term.SetTaskbarProgress({ get_weak(), &TerminalPage::_SetTaskbarProgressHandler });

        term.ConnectionStateChanged({ get_weak(), &TerminalPage::_ConnectionStateChangedHandler });
    }

    // Method Description:
    // - Connects event handlers to the TerminalTab for events that we want to
    //   handle. This includes:
    //    * the TitleChanged event, for changing the text of the tab
    //    * the Color{Selected,Cleared} events to change the color of a tab.
    // Arguments:
    // - hostingTab: The Tab that's hosting this TermControl instance
    void TerminalPage::_RegisterTabEvents(TerminalTab& hostingTab)
    {
        auto weakTab{ hostingTab.get_weak() };
        auto weakThis{ get_weak() };
        // PropertyChanged is the generic mechanism by which the Tab
        // communicates changes to any of its observable properties, including
        // the Title
        hostingTab.PropertyChanged([weakTab, weakThis](auto&&, const WUX::Data::PropertyChangedEventArgs& args) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };
            if (page && tab)
            {
                if (args.PropertyName() == L"Title")
                {
                    page->_UpdateTitle(*tab);
                }
                else if (args.PropertyName() == L"Content")
                {
                    if (*tab == page->_GetFocusedTab())
                    {
                        page->_tabContent.Children().Clear();
                        page->_tabContent.Children().Append(tab->Content());

                        tab->Focus(FocusState::Programmatic);
                    }
                }
            }
        });

        // react on color changed events
        hostingTab.ColorSelected([weakTab, weakThis](auto&& color) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab && (tab->FocusState() != FocusState::Unfocused))
            {
                page->_SetNonClientAreaColors(color);
            }
        });

        hostingTab.ColorCleared([weakTab, weakThis]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab && (tab->FocusState() != FocusState::Unfocused))
            {
                page->_ClearNonClientAreaColors();
            }
        });

        // Add an event handler for when the terminal or tab wants to set a
        // progress indicator on the taskbar
        hostingTab.TaskbarProgressChanged({ get_weak(), &TerminalPage::_SetTaskbarProgressHandler });

        // TODO GH#3327: Once we support colorizing the NewTab button based on
        // the color of the tab, we'll want to make sure to call
        // _ClearNewTabButtonColor here, to reset it to the default (for the
        // newly created tab).
        // remove any colors left by other colored tabs
        // _ClearNewTabButtonColor();
    }

    // Method Description:
    // - Helper to manually exit "zoom" when certain actions take place.
    //   Anything that modifies the state of the pane tree should probably
    //   un-zoom the focused pane first, so that the user can see the full pane
    //   tree again. These actions include:
    //   * Splitting a new pane
    //   * Closing a pane
    //   * Moving focus between panes
    //   * Resizing a pane
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UnZoomIfNeeded()
    {
        if (const auto activeTab{ _GetFocusedTabImpl() })
        {
            if (activeTab->IsZoomed())
            {
                // Remove the content from the tab first, so Pane::UnZoom can
                // re-attach the content to the tree w/in the pane
                _tabContent.Children().Clear();
                // In ExitZoom, we'll change the Tab's Content(), triggering the
                // content changed event, which will re-attach the tab's new content
                // root to the tree.
                activeTab->ExitZoom();
            }
        }
    }

    // Method Description:
    // - Attempt to move focus between panes, as to focus the child on
    //   the other side of the separator. See Pane::NavigateFocus for details.
    // - Moves the focus of the currently focused tab.
    // Arguments:
    // - direction: The direction to move the focus in.
    // Return Value:
    // - Whether changing the focus succeeded. This allows a keychord to propagate
    //   to the terminal when no other panes are present (GH#6219)
    bool TerminalPage::_MoveFocus(const FocusDirection& direction)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            return terminalTab->NavigateFocus(direction);
        }
        return false;
    }

    // Method Description:
    // - Attempt to swap the positions of the focused pane with another pane.
    //   See Pane::SwapPane for details.
    // Arguments:
    // - direction: The direction to move the focused pane in.
    // Return Value:
    // - true if panes were swapped.
    bool TerminalPage::_SwapPane(const FocusDirection& direction)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            return terminalTab->SwapPane(direction);
        }
        return false;
    }

    TermControl TerminalPage::_GetActiveControl()
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            return terminalTab->GetActiveTerminalControl();
        }
        return nullptr;
    }
    // Method Description:
    // - Warn the user that they are about to close all open windows, then
    //   signal that we want to close everything.
    fire_and_forget TerminalPage::RequestQuit()
    {
        if (!_displayingCloseDialog)
        {
            _displayingCloseDialog = true;
            ContentDialogResult warningResult = co_await _ShowQuitDialog();
            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }

            _QuitRequestedHandlers(nullptr, nullptr);
        }
    }

    // Method Description:
    // - Saves the window position and tab layout to the application state
    // - This does not create the InitialPosition field, that needs to be
    //   added externally.
    // Arguments:
    // - <none>
    // Return Value:
    // - the window layout
    WindowLayout TerminalPage::GetWindowLayout()
    {
        if (_startupState != StartupState::Initialized)
        {
            return nullptr;
        }

        std::vector<ActionAndArgs> actions;

        for (auto tab : _tabs)
        {
            if (auto terminalTab = _GetTerminalTabImpl(tab))
            {
                auto tabActions = terminalTab->BuildStartupActions();
                actions.insert(actions.end(), std::make_move_iterator(tabActions.begin()), std::make_move_iterator(tabActions.end()));
            }
            else if (tab.try_as<SettingsTab>())
            {
                ActionAndArgs action;
                action.Action(ShortcutAction::OpenSettings);
                OpenSettingsArgs args{ SettingsTarget::SettingsUI };
                action.Args(args);

                actions.emplace_back(std::move(action));
            }
        }

        // if the focused tab was not the last tab, restore that
        auto idx = _GetFocusedTabIndex();
        if (idx && idx != _tabs.Size() - 1)
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::SwitchToTab);
            SwitchToTabArgs switchToTabArgs{ idx.value() };
            action.Args(switchToTabArgs);

            actions.emplace_back(std::move(action));
        }

        // If the user set a custom name, save it
        if (_WindowName != L"")
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::RenameWindow);
            RenameWindowArgs args{ _WindowName };
            action.Args(args);

            actions.emplace_back(std::move(action));
        }

        WindowLayout layout{};
        layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));

        // Only save the content size because the tab size will be added on load.
        const float contentWidth = ::base::saturated_cast<float>(_tabContent.ActualWidth());
        const float contentHeight = ::base::saturated_cast<float>(_tabContent.ActualHeight());
        const winrt::Windows::Foundation::Size windowSize{ contentWidth, contentHeight };

        layout.InitialSize(windowSize);

        return layout;
    }

    // Method Description:
    // - Close the terminal app. If there is more
    //   than one tab opened, show a warning dialog.
    // Arguments:
    // - bypassDialog: if true a dialog won't be shown even if the user would
    //   normally get confirmation. This is used in the case where the user
    //   has already been prompted by the Quit action.
    fire_and_forget TerminalPage::CloseWindow(bool bypassDialog)
    {
        if (!bypassDialog &&
            _HasMultipleTabs() &&
            _settings.GlobalSettings().ConfirmCloseAllTabs() &&
            !_displayingCloseDialog)
        {
            _displayingCloseDialog = true;
            ContentDialogResult warningResult = co_await _ShowCloseWarningDialog();
            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        if (ShouldUsePersistedLayout(_settings))
        {
            // Don't delete the ApplicationState when all of the tabs are removed.
            // If there is still a monarch living they will get the event that
            // a window closed and trigger a new save without this window.
            _maintainStateOnTabClose = true;
        }

        _RemoveAllTabs();
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a number of lines.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    // - rowsToScroll: a number of lines to move the viewport. If not provided we will use a system default.
    void TerminalPage::_Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            uint32_t realRowsToScroll;
            if (rowsToScroll == nullptr)
            {
                // The magic value of WHEEL_PAGESCROLL indicates that we need to scroll the entire page
                realRowsToScroll = _systemRowsToScroll == WHEEL_PAGESCROLL ?
                                       terminalTab->GetActiveTerminalControl().ViewHeight() :
                                       _systemRowsToScroll;
            }
            else
            {
                // use the custom value specified in the command
                realRowsToScroll = rowsToScroll.Value();
            }
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, realRowsToScroll);
            terminalTab->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Moves the currently active pane on the currently active tab to the
    //   specified tab. If the tab index is greater than the number of
    //   tabs, then a new tab will be created for the pane. Similarly, if a pane
    //   is the last remaining pane on a tab, that tab will be closed upon moving.
    // - No move will occur if the tabIdx is the same as the current tab, or if
    //   the specified tab is not a host of terminals (such as the settings tab).
    // Arguments:
    // - tabIdx: The target tab index.
    // Return Value:
    // - true if the pane was successfully moved to the new tab.
    bool TerminalPage::_MovePane(const uint32_t tabIdx)
    {
        auto focusedTab{ _GetFocusedTabImpl() };

        if (!focusedTab)
        {
            return false;
        }

        // If we are trying to move from the current tab to the current tab do nothing.
        if (_GetFocusedTabIndex() == tabIdx)
        {
            return false;
        }

        // Moving the pane from the current tab might close it, so get the next
        // tab before its index changes.
        if (_tabs.Size() > tabIdx)
        {
            auto targetTab = _GetTerminalTabImpl(_tabs.GetAt(tabIdx));
            // if the selected tab is not a host of terminals (e.g. settings)
            // don't attempt to add a pane to it.
            if (!targetTab)
            {
                return false;
            }
            auto pane = focusedTab->DetachPane();
            targetTab->AttachPane(pane);
            _SetFocusedTab(*targetTab);
        }
        else
        {
            auto pane = focusedTab->DetachPane();
            _CreateNewTabFromPane(pane);
        }

        return true;
    }

    // Method Description:
    // - Split the focused pane either horizontally or vertically, and place the
    //   given TermControl into the newly created pane.
    // Arguments:
    // - splitDirection: one value from the TerminalApp::SplitDirection enum, indicating how the
    //   new pane should be split from its parent.
    // - splitMode: value from TerminalApp::SplitType enum, indicating the profile to be used in the newly split pane.
    // - newTerminalArgs: An object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See CascadiaSettings::BuildSettings for more details.
    void TerminalPage::_SplitPane(const SplitDirection splitDirection,
                                  const SplitType splitMode,
                                  const float splitSize,
                                  const NewTerminalArgs& newTerminalArgs)
    {
        const auto focusedTab{ _GetFocusedTabImpl() };

        // Do nothing if no TerminalTab is focused
        if (!focusedTab)
        {
            return;
        }

        _SplitPane(*focusedTab, splitDirection, splitMode, splitSize, newTerminalArgs);
    }

    // Method Description:
    // - Split the focused pane of the given tab, either horizontally or vertically, and place the
    //   given TermControl into the newly created pane.
    // Arguments:
    // - tab: The tab that is going to be split.
    // - splitDirection: one value from the TerminalApp::SplitDirection enum, indicating how the
    //   new pane should be split from its parent.
    // - splitMode: value from TerminalApp::SplitType enum, indicating the profile to be used in the newly split pane.
    // - newTerminalArgs: An object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See CascadiaSettings::BuildSettings for more details.
    void TerminalPage::_SplitPane(TerminalTab& tab,
                                  const SplitDirection splitDirection,
                                  const SplitType splitMode,
                                  const float splitSize,
                                  const NewTerminalArgs& newTerminalArgs)
    {
        try
        {
            TerminalSettingsCreateResult controlSettings{ nullptr };
            Profile profile{ nullptr };

            if (splitMode == SplitType::Duplicate)
            {
                profile = tab.GetFocusedProfile();
                if (profile)
                {
                    // TODO GH#5047 If we cache the NewTerminalArgs, we no longer need to do this.
                    profile = GetClosestProfileForDuplicationOfProfile(profile);
                    controlSettings = TerminalSettings::CreateWithProfile(_settings, profile, *_bindings);
                    const auto workingDirectory = tab.GetActiveTerminalControl().WorkingDirectory();
                    const auto validWorkingDirectory = !workingDirectory.empty();
                    if (validWorkingDirectory)
                    {
                        controlSettings.DefaultSettings().StartingDirectory(workingDirectory);
                    }
                }
                // TODO: GH#5047 - In the future, we should get the Profile of
                // the focused pane, and use that to build a new instance of the
                // settings so we can duplicate this tab/pane.
                //
                // Currently, if the profile doesn't exist anymore in our
                // settings, we'll silently do nothing.
                //
                // In the future, it will be preferable to just duplicate the
                // current control's settings, but we can't do that currently,
                // because we won't be able to create a new instance of the
                // connection without keeping an instance of the original Profile
                // object around.
            }
            if (!profile)
            {
                profile = _settings.GetProfileForArgs(newTerminalArgs);
                controlSettings = TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs, *_bindings);
            }

            const auto controlConnection = _CreateConnectionFromSettings(profile, controlSettings.DefaultSettings());

            const float contentWidth = ::base::saturated_cast<float>(_tabContent.ActualWidth());
            const float contentHeight = ::base::saturated_cast<float>(_tabContent.ActualHeight());
            const winrt::Windows::Foundation::Size availableSpace{ contentWidth, contentHeight };

            auto realSplitType = splitDirection;
            if (realSplitType == SplitDirection::Automatic)
            {
                realSplitType = tab.PreCalculateAutoSplit(availableSpace);
            }

            const auto canSplit = tab.PreCalculateCanSplit(realSplitType, splitSize, availableSpace);
            if (!canSplit)
            {
                return;
            }

            auto newControl = _InitControl(controlSettings, controlConnection);

            // Hookup our event handlers to the new terminal
            _RegisterTerminalEvents(newControl);

            _UnZoomIfNeeded();

            tab.SplitPane(realSplitType, splitSize, profile, newControl);

            // After GH#6586, the control will no longer focus itself
            // automatically when it's finished being laid out. Manually focus
            // the control here instead.
            if (_startupState == StartupState::Initialized)
            {
                _GetActiveControl().Focus(FocusState::Programmatic);
            }
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Switches the split orientation of the currently focused pane.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ToggleSplitOrientation()
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            terminalTab->ToggleSplitOrientation();
        }
    }

    // Method Description:
    // - Attempt to move a separator between panes, as to resize each child on
    //   either size of the separator. See Pane::ResizePane for details.
    // - Moves a separator on the currently focused tab.
    // Arguments:
    // - direction: The direction to move the separator in.
    // Return Value:
    // - <none>
    void TerminalPage::_ResizePane(const ResizeDirection& direction)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            terminalTab->ResizePane(direction);
        }
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a page. The page length will be dependent on the terminal view height.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    void TerminalPage::_ScrollPage(ScrollDirection scrollDirection)
    {
        // Do nothing if for some reason, there's no terminal tab in focus. We don't want to crash.
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            if (const auto& control{ _GetActiveControl() })
            {
                const auto termHeight = control.ViewHeight();
                auto scrollDelta = _ComputeScrollDelta(scrollDirection, termHeight);
                terminalTab->Scroll(scrollDelta);
            }
        }
    }

    void TerminalPage::_ScrollToBufferEdge(ScrollDirection scrollDirection)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, INT_MAX);
            terminalTab->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Gets the title of the currently focused terminal control. If there
    //   isn't a control selected for any reason, returns "Windows Terminal"
    // Arguments:
    // - <none>
    // Return Value:
    // - the title of the focused control if there is one, else "Windows Terminal"
    hstring TerminalPage::Title()
    {
        if (_settings.GlobalSettings().ShowTitleInTitlebar())
        {
            auto selectedIndex = _tabView.SelectedIndex();
            if (selectedIndex >= 0)
            {
                try
                {
                    if (auto focusedControl{ _GetActiveControl() })
                    {
                        return focusedControl.Title();
                    }
                }
                CATCH_LOG();
            }
        }
        return { L"Windows Terminal" };
    }

    // Method Description:
    // - Handles the special case of providing a text override for the UI shortcut due to VK_OEM issue.
    //      Looks at the flags from the KeyChord modifiers and provides a concatenated string value of all
    //      in the same order that XAML would put them as well.
    // Return Value:
    // - a string representation of the key modifiers for the shortcut
    //NOTE: This needs to be localized with https://github.com/microsoft/terminal/issues/794 if XAML framework issue not resolved before then
    static std::wstring _FormatOverrideShortcutText(VirtualKeyModifiers modifiers)
    {
        std::wstring buffer{ L"" };

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Control))
        {
            buffer += L"Ctrl+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Shift))
        {
            buffer += L"Shift+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Menu))
        {
            buffer += L"Alt+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Windows))
        {
            buffer += L"Win+";
        }

        return buffer;
    }

    // Method Description:
    // - Takes a MenuFlyoutItem and a corresponding KeyChord value and creates the accelerator for UI display.
    //   Takes into account a special case for an error condition for a comma
    // Arguments:
    // - MenuFlyoutItem that will be displayed, and a KeyChord to map an accelerator
    void TerminalPage::_SetAcceleratorForMenuItem(WUX::Controls::MenuFlyoutItem& menuItem,
                                                  const KeyChord& keyChord)
    {
#ifdef DEP_MICROSOFT_UI_XAML_708_FIXED
        // work around https://github.com/microsoft/microsoft-ui-xaml/issues/708 in case of VK_OEM_COMMA
        if (keyChord.Vkey() != VK_OEM_COMMA)
        {
            // use the XAML shortcut to give us the automatic capabilities
            auto menuShortcut = Windows::UI::Xaml::Input::KeyboardAccelerator{};

            // TODO: Modify this when https://github.com/microsoft/terminal/issues/877 is resolved
            menuShortcut.Key(static_cast<Windows::System::VirtualKey>(keyChord.Vkey()));

            // add the modifiers to the shortcut
            menuShortcut.Modifiers(keyChord.Modifiers());

            // add to the menu
            menuItem.KeyboardAccelerators().Append(menuShortcut);
        }
        else // we've got a comma, so need to just use the alternate method
#endif
        {
            // extract the modifier and key to a nice format
            auto overrideString = _FormatOverrideShortcutText(keyChord.Modifiers());
            auto mappedCh = MapVirtualKeyW(keyChord.Vkey(), MAPVK_VK_TO_CHAR);
            if (mappedCh != 0)
            {
                menuItem.KeyboardAcceleratorTextOverride(overrideString + gsl::narrow_cast<wchar_t>(mappedCh));
            }
        }
    }

    // Method Description:
    // - Calculates the appropriate size to snap to in the given direction, for
    //   the given dimension. If the global setting `snapToGridOnResize` is set
    //   to `false`, this will just immediately return the provided dimension,
    //   effectively disabling snapping.
    // - See Pane::CalcSnappedDimension
    float TerminalPage::CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
    {
        if (_settings && _settings.GlobalSettings().SnapToGridOnResize())
        {
            if (const auto terminalTab{ _GetFocusedTabImpl() })
            {
                return terminalTab->CalcSnappedDimension(widthOrHeight, dimension);
            }
        }
        return dimension;
    }

    // Method Description:
    // - Place `copiedData` into the clipboard as text. Triggered when a
    //   terminal control raises it's CopyToClipboard event.
    // Arguments:
    // - copiedData: the new string content to place on the clipboard.
    winrt::fire_and_forget TerminalPage::_CopyToClipboardHandler(const IInspectable /*sender*/,
                                                                 const CopyToClipboardEventArgs copiedData)
    {
        co_await winrt::resume_foreground(Dispatcher(), CoreDispatcherPriority::High);

        DataPackage dataPack = DataPackage();
        dataPack.RequestedOperation(DataPackageOperation::Copy);

        // The EventArgs.Formats() is an override for the global setting "copyFormatting"
        //   iff it is set
        bool useGlobal = copiedData.Formats() == nullptr;
        auto copyFormats = useGlobal ?
                               _settings.GlobalSettings().CopyFormatting() :
                               copiedData.Formats().Value();

        // copy text to dataPack
        dataPack.SetText(copiedData.Text());

        if (WI_IsFlagSet(copyFormats, CopyFormat::HTML))
        {
            // copy html to dataPack
            const auto htmlData = copiedData.Html();
            if (!htmlData.empty())
            {
                dataPack.SetHtmlFormat(htmlData);
            }
        }

        if (WI_IsFlagSet(copyFormats, CopyFormat::RTF))
        {
            // copy rtf data to dataPack
            const auto rtfData = copiedData.Rtf();
            if (!rtfData.empty())
            {
                dataPack.SetRtf(rtfData);
            }
        }

        try
        {
            Clipboard::SetContent(dataPack);
            Clipboard::Flush();
        }
        CATCH_LOG();
    }

    // Function Description:
    // - This function is called when the `TermControl` requests that we send
    //   it the clipboard's content.
    // - Retrieves the data from the Windows Clipboard and converts it to text.
    // - Shows warnings if the clipboard is too big or contains multiple lines
    //   of text.
    // - Sends the text back to the TermControl through the event's
    //   `HandleClipboardData` member function.
    // - Does some of this in a background thread, as to not hang/crash the UI thread.
    // Arguments:
    // - eventArgs: the PasteFromClipboard event sent from the TermControl
    fire_and_forget TerminalPage::_PasteFromClipboardHandler(const IInspectable /*sender*/,
                                                             const PasteFromClipboardEventArgs eventArgs)
    {
        const DataPackageView data = Clipboard::GetContent();

        // This will switch the execution of the function to a background (not
        // UI) thread. This is IMPORTANT, because the getting the clipboard data
        // will crash on the UI thread, because the main thread is a STA.
        co_await winrt::resume_background();

        try
        {
            hstring text = L"";
            if (data.Contains(StandardDataFormats::Text()))
            {
                text = co_await data.GetTextAsync();
            }
            // Windows Explorer's "Copy address" menu item stores a StorageItem in the clipboard, and no text.
            else if (data.Contains(StandardDataFormats::StorageItems()))
            {
                Windows::Foundation::Collections::IVectorView<Windows::Storage::IStorageItem> items = co_await data.GetStorageItemsAsync();
                if (items.Size() > 0)
                {
                    Windows::Storage::IStorageItem item = items.GetAt(0);
                    text = item.Path();
                }
            }

            bool warnMultiLine = _settings.GlobalSettings().WarnAboutMultiLinePaste();
            if (warnMultiLine)
            {
                const auto isNewLineLambda = [](auto c) { return c == L'\n' || c == L'\r'; };
                const auto hasNewLine = std::find_if(text.cbegin(), text.cend(), isNewLineLambda) != text.cend();
                warnMultiLine = hasNewLine;
            }

            constexpr const std::size_t minimumSizeForWarning = 1024 * 5; // 5 KiB
            const bool warnLargeText = text.size() > minimumSizeForWarning &&
                                       _settings.GlobalSettings().WarnAboutLargePaste();

            if (warnMultiLine || warnLargeText)
            {
                co_await winrt::resume_foreground(Dispatcher());

                if (warnMultiLine)
                {
                    const auto focusedTab = _GetFocusedTabImpl();
                    // Do not warn about multi line pasting if the current tab has bracketed paste enabled.
                    warnMultiLine = warnMultiLine && !focusedTab->GetActiveTerminalControl().BracketedPasteEnabled();
                }

                // We have to initialize the dialog here to be able to change the text of the text block within it
                FindName(L"MultiLinePasteDialog").try_as<WUX::Controls::ContentDialog>();
                ClipboardText().Text(text);

                // The vertical offset on the scrollbar does not reset automatically, so reset it manually
                ClipboardContentScrollViewer().ScrollToVerticalOffset(0);

                ContentDialogResult warningResult = ContentDialogResult::Primary;
                if (warnMultiLine)
                {
                    warningResult = co_await _ShowMultiLinePasteWarningDialog();
                }
                else if (warnLargeText)
                {
                    warningResult = co_await _ShowLargePasteWarningDialog();
                }

                // Clear the clipboard text so it doesn't lie around in memory
                ClipboardText().Text(L"");

                if (warningResult != ContentDialogResult::Primary)
                {
                    // user rejected the paste
                    co_return;
                }
            }

            eventArgs.HandleClipboardData(text);
        }
        CATCH_LOG();
    }

    void TerminalPage::_OpenHyperlinkHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs)
    {
        try
        {
            auto parsed = winrt::Windows::Foundation::Uri(eventArgs.Uri().c_str());
            if (_IsUriSupported(parsed))
            {
                ShellExecute(nullptr, L"open", eventArgs.Uri().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            else
            {
                _ShowCouldNotOpenDialog(RS_(L"UnsupportedSchemeText"), eventArgs.Uri());
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _ShowCouldNotOpenDialog(RS_(L"InvalidUriText"), eventArgs.Uri());
        }
    }

    // Method Description:
    // - Opens up a dialog box explaining why we could not open a URI
    // Arguments:
    // - The reason (unsupported scheme, invalid uri, potentially more in the future)
    // - The uri
    void TerminalPage::_ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto unopenedUriDialog = FindName(L"CouldNotOpenUriDialog").try_as<WUX::Controls::ContentDialog>();

            // Insert the reason and the URI
            CouldNotOpenUriReason().Text(reason);
            UnopenedUri().Text(uri);

            // Show the dialog
            presenter.ShowDialog(unopenedUriDialog);
        }
    }

    // Method Description:
    // - Determines if the given URI is currently supported
    // Arguments:
    // - The parsed URI
    // Return value:
    // - True if we support it, false otherwise
    bool TerminalPage::_IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri)
    {
        if (parsedUri.SchemeName() == L"http" || parsedUri.SchemeName() == L"https")
        {
            return true;
        }
        if (parsedUri.SchemeName() == L"file")
        {
            const auto host = parsedUri.Host();
            // If no hostname was provided or if the hostname was "localhost", Host() will return an empty string
            // and we allow it
            if (host == L"")
            {
                return true;
            }
            // TODO: by the OSC 8 spec, if a hostname (other than localhost) is provided, we _should_ be
            // comparing that value against what is returned by GetComputerNameExW and making sure they match.
            // However, ShellExecute does not seem to be happy with file URIs of the form
            //          file://{hostname}/path/to/file.ext
            // and so while we could do the hostname matching, we do not know how to actually open the URI
            // if its given in that form. So for now we ignore all hostnames other than localhost
        }
        return false;
    }

    // Important! Don't take this eventArgs by reference, we need to extend the
    // lifetime of it to the other side of the co_await!
    winrt::fire_and_forget TerminalPage::_ControlNoticeRaisedHandler(const IInspectable /*sender*/,
                                                                     const Microsoft::Terminal::Control::NoticeEventArgs eventArgs)
    {
        auto weakThis = get_weak();
        co_await winrt::resume_foreground(Dispatcher());
        if (auto page = weakThis.get())
        {
            winrt::hstring message = eventArgs.Message();

            winrt::hstring title;

            switch (eventArgs.Level())
            {
            case NoticeLevel::Debug:
                title = RS_(L"NoticeDebug"); //\xebe8
                break;
            case NoticeLevel::Info:
                title = RS_(L"NoticeInfo"); // \xe946
                break;
            case NoticeLevel::Warning:
                title = RS_(L"NoticeWarning"); //\xe7ba
                break;
            case NoticeLevel::Error:
                title = RS_(L"NoticeError"); //\xe783
                break;
            }

            page->_ShowControlNoticeDialog(title, message);
        }
    }

    void TerminalPage::_ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto controlNoticeDialog = FindName(L"ControlNoticeDialog").try_as<WUX::Controls::ContentDialog>();

            ControlNoticeDialog().Title(winrt::box_value(title));

            // Insert the message
            NoticeMessage().Text(message);

            // Show the dialog
            presenter.ShowDialog(controlNoticeDialog);
        }
    }

    // Method Description:
    // - Copy text from the focused terminal to the Windows Clipboard
    // Arguments:
    // - singleLine: if enabled, copy contents as a single line of text
    // - formats: dictate which formats need to be copied
    // Return Value:
    // - true iff we we able to copy text (if a selection was active)
    bool TerminalPage::_CopyText(const bool singleLine, const Windows::Foundation::IReference<CopyFormat>& formats)
    {
        if (const auto& control{ _GetActiveControl() })
        {
            return control.CopySelectionToClipboard(singleLine, formats);
        }
        return false;
    }

    // Method Description:
    // - Send an event (which will be caught by AppHost) to set the progress indicator on the taskbar
    // Arguments:
    // - sender (not used)
    // - eventArgs: the arguments specifying how to set the progress indicator
    winrt::fire_and_forget TerminalPage::_SetTaskbarProgressHandler(const IInspectable /*sender*/, const IInspectable /*eventArgs*/)
    {
        co_await resume_foreground(Dispatcher());
        _SetTaskbarProgressHandlers(*this, nullptr);
    }

    // Method Description:
    // - Paste text from the Windows Clipboard to the focused terminal
    void TerminalPage::_PasteText()
    {
        if (const auto& control{ _GetActiveControl() })
        {
            control.PasteTextFromClipboard();
        }
    }

    // Function Description:
    // - Called when the settings button is clicked. ShellExecutes the settings
    //   file, as to open it in the default editor for .json files. Does this in
    //   a background thread, as to not hang/crash the UI thread.
    fire_and_forget TerminalPage::_LaunchSettings(const SettingsTarget target)
    {
        if (target == SettingsTarget::SettingsUI)
        {
            _OpenSettingsUI();
        }
        else
        {
            // This will switch the execution of the function to a background (not
            // UI) thread. This is IMPORTANT, because the Windows.Storage API's
            // (used for retrieving the path to the file) will crash on the UI
            // thread, because the main thread is a STA.
            co_await winrt::resume_background();

            auto openFile = [](const auto& filePath) {
                HINSTANCE res = ShellExecute(nullptr, nullptr, filePath.c_str(), nullptr, nullptr, SW_SHOW);
                if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
                {
                    ShellExecute(nullptr, nullptr, L"notepad", filePath.c_str(), nullptr, SW_SHOW);
                }
            };

            switch (target)
            {
            case SettingsTarget::DefaultsFile:
                openFile(CascadiaSettings::DefaultSettingsPath());
                break;
            case SettingsTarget::SettingsFile:
                openFile(CascadiaSettings::SettingsPath());
                break;
            case SettingsTarget::AllFiles:
                openFile(CascadiaSettings::DefaultSettingsPath());
                openFile(CascadiaSettings::SettingsPath());
                break;
            }
        }
    }

    // Method Description:
    // - Called when our tab content size changes. This updates each tab with
    //   the new size, so they have a chance to update each of their panes with
    //   the new size.
    // Arguments:
    // - e: the SizeChangedEventArgs with the new size of the tab content area.
    // Return Value:
    // - <none>
    void TerminalPage::_OnContentSizeChanged(const IInspectable& /*sender*/, Windows::UI::Xaml::SizeChangedEventArgs const& e)
    {
        const auto newSize = e.NewSize();
        _ResizeTabContent(newSize);
    }

    // Method Description:
    // - Responds to the TabView control's Tab Closing event by removing
    //      the indicated tab from the set and focusing another one.
    //      The event is cancelled so App maintains control over the
    //      items in the tabview.
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabCloseRequested(const IInspectable& /*sender*/, const MUX::Controls::TabViewTabCloseRequestedEventArgs& eventArgs)
    {
        const auto tabViewItem = eventArgs.Tab();
        if (auto tab{ _GetTabByTabViewItem(tabViewItem) })
        {
            _HandleCloseTabRequested(tab);
        }
    }

    TermControl TerminalPage::_InitControl(const TerminalSettingsCreateResult& settings, const ITerminalConnection& connection)
    {
        // Give term control a child of the settings so that any overrides go in the child
        // This way, when we do a settings reload we just update the parent and the overrides remain
        const auto child = TerminalSettings::CreateWithParent(settings);
        TermControl term{ child.DefaultSettings(), connection };

        term.UnfocusedAppearance(child.UnfocusedSettings()); // It is okay for the unfocused settings to be null

        return term;
    }

    // Method Description:
    // - Hook up keybindings, and refresh the UI of the terminal.
    //   This includes update the settings of all the tabs according
    //   to their profiles, update the title and icon of each tab, and
    //   finally create the tab flyout
    void TerminalPage::_RefreshUIForSettingsReload()
    {
        // Re-wire the keybindings to their handlers, as we'll have created a
        // new AppKeyBindings object.
        _HookupKeyBindings(_settings.ActionMap());

        // Refresh UI elements

        // Mapping by GUID isn't _excellent_ because the defaults profile doesn't have a stable GUID; however,
        // when we stabilize its guid this will become fully safe.
        std::unordered_map<winrt::guid, std::pair<Profile, TerminalSettingsCreateResult>> profileGuidSettingsMap;
        const auto profileDefaults{ _settings.ProfileDefaults() };
        const auto allProfiles{ _settings.AllProfiles() };

        profileGuidSettingsMap.reserve(allProfiles.Size() + 1);

        // Include the Defaults profile for consideration
        profileGuidSettingsMap.insert_or_assign(profileDefaults.Guid(), std::pair{ profileDefaults, nullptr });
        for (const auto& newProfile : allProfiles)
        {
            // Avoid creating a TerminalSettings right now. They're not totally cheap, and we suspect that users with many
            // panes may not be using all of their profiles at the same time. Lazy evaluation is king!
            profileGuidSettingsMap.insert_or_assign(newProfile.Guid(), std::pair{ newProfile, nullptr });
        }

        for (const auto& tab : _tabs)
        {
            if (auto terminalTab{ _GetTerminalTabImpl(tab) })
            {
                terminalTab->UpdateSettings();

                // Manually enumerate the panes in each tab; this will let us recycle TerminalSettings
                // objects but only have to iterate one time.
                terminalTab->GetRootPane()->WalkTree([&](auto&& pane) {
                    if (const auto profile{ pane->GetProfile() })
                    {
                        const auto found{ profileGuidSettingsMap.find(profile.Guid()) };
                        // GH#2455: If there are any panes with controls that had been
                        // initialized with a Profile that no longer exists in our list of
                        // profiles, we'll leave it unmodified. The profile doesn't exist
                        // anymore, so we can't possibly update its settings.
                        if (found != profileGuidSettingsMap.cend())
                        {
                            auto& pair{ found->second };
                            if (!pair.second)
                            {
                                pair.second = TerminalSettings::CreateWithProfile(_settings, pair.first, *_bindings);
                            }
                            pane->UpdateSettings(pair.second, pair.first);
                        }
                    }
                    return false;
                });

                // Update the icon of the tab for the currently focused profile in that tab.
                // Only do this for TerminalTabs. Other types of tabs won't have multiple panes
                // and profiles so the Title and Icon will be set once and only once on init.
                _UpdateTabIcon(*terminalTab);

                // Force the TerminalTab to re-grab its currently active control's title.
                terminalTab->UpdateTitle();
            }
            else if (auto settingsTab = tab.try_as<TerminalApp::SettingsTab>())
            {
                settingsTab.UpdateSettings(_settings);
            }

            auto tabImpl{ winrt::get_self<TabBase>(tab) };
            tabImpl->SetActionMap(_settings.ActionMap());
        }

        // repopulate the new tab button's flyout with entries for each
        // profile, which might have changed
        _UpdateTabWidthMode();
        _CreateNewTabFlyout();

        // Reload the current value of alwaysOnTop from the settings file. This
        // will let the user hot-reload this setting, but any runtime changes to
        // the alwaysOnTop setting will be lost.
        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();
        _AlwaysOnTopChangedHandlers(*this, nullptr);

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        _tabRow.ShowElevationShield(IsElevated() && _settings.GlobalSettings().ShowAdminShield());
    }

    // This is a helper to aid in sorting commands by their `Name`s, alphabetically.
    static bool _compareSchemeNames(const ColorScheme& lhs, const ColorScheme& rhs)
    {
        std::wstring leftName{ lhs.Name() };
        std::wstring rightName{ rhs.Name() };
        return leftName.compare(rightName) < 0;
    }

    // Method Description:
    // - Takes a mapping of names->commands and expands them
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    IMap<winrt::hstring, Command> TerminalPage::_ExpandCommands(IMapView<winrt::hstring, Command> commandsToExpand,
                                                                IVectorView<Profile> profiles,
                                                                IMapView<winrt::hstring, ColorScheme> schemes)
    {
        IVector<SettingsLoadWarnings> warnings{ winrt::single_threaded_vector<SettingsLoadWarnings>() };

        std::vector<ColorScheme> sortedSchemes;
        sortedSchemes.reserve(schemes.Size());

        for (const auto& nameAndScheme : schemes)
        {
            sortedSchemes.push_back(nameAndScheme.Value());
        }
        std::sort(sortedSchemes.begin(),
                  sortedSchemes.end(),
                  _compareSchemeNames);

        IMap<winrt::hstring, Command> copyOfCommands = winrt::single_threaded_map<winrt::hstring, Command>();
        for (const auto& nameAndCommand : commandsToExpand)
        {
            copyOfCommands.Insert(nameAndCommand.Key(), nameAndCommand.Value());
        }

        Command::ExpandCommands(copyOfCommands,
                                profiles,
                                { sortedSchemes },
                                warnings);

        return copyOfCommands;
    }
    // Method Description:
    // - Repopulates the list of commands in the command palette with the
    //   current commands in the settings. Also updates the keybinding labels to
    //   reflect any matching keybindings.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateCommandsForPalette()
    {
        IMap<winrt::hstring, Command> copyOfCommands = _ExpandCommands(_settings.GlobalSettings().ActionMap().NameMap(),
                                                                       _settings.ActiveProfiles().GetView(),
                                                                       _settings.GlobalSettings().ColorSchemes());

        _recursiveUpdateCommandKeybindingLabels(_settings, copyOfCommands.GetView());

        // Update the command palette when settings reload
        auto commandsCollection = winrt::single_threaded_vector<Command>();
        for (const auto& nameAndCommand : copyOfCommands)
        {
            commandsCollection.Append(nameAndCommand.Value());
        }

        CommandPalette().SetCommands(commandsCollection);
    }

    // Method Description:
    // - Sets the initial actions to process on startup. We'll make a copy of
    //   this list, and process these actions when we're loaded.
    // - This function will have no effective result after Create() is called.
    // Arguments:
    // - actions: a list of Actions to process on startup.
    // Return Value:
    // - <none>
    void TerminalPage::SetStartupActions(std::vector<ActionAndArgs>& actions)
    {
        // The fastest way to copy all the actions out of the std::vector and
        // put them into a winrt::IVector is by making a copy, then moving the
        // copy into the winrt vector ctor.
        auto listCopy = actions;
        _startupActions = winrt::single_threaded_vector<ActionAndArgs>(std::move(listCopy));
    }

    // Routine Description:
    // - Notifies this Terminal Page that it should start the incoming connection
    //   listener for command-line tools attempting to join this Terminal
    //   through the default application channel.
    // Arguments:
    // - isEmbedding - True if COM started us to be a server. False if we're doing it of our own accord.
    // Return Value:
    // - <none>
    void TerminalPage::SetInboundListener(bool isEmbedding)
    {
        _shouldStartInboundListener = true;
        _isEmbeddingInboundListener = isEmbedding;

        // If the page has already passed the NotInitialized state,
        // then it is ready-enough for us to just start this immediately.
        if (_startupState != StartupState::NotInitialized)
        {
            _StartInboundListener();
        }
    }

    winrt::TerminalApp::IDialogPresenter TerminalPage::DialogPresenter() const
    {
        return _dialogPresenter.get();
    }

    void TerminalPage::DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter)
    {
        _dialogPresenter = dialogPresenter;
    }

    // Method Description:
    // - Get the combined taskbar state for the page. This is the combination of
    //   all the states of all the tabs, which are themselves a combination of
    //   all their panes. Taskbar states are given a priority based on the rules
    //   in:
    //   https://docs.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist3-setprogressstate
    //   under "How the Taskbar Button Chooses the Progress Indicator for a Group"
    // Arguments:
    // - <none>
    // Return Value:
    // - A TaskbarState object representing the combined taskbar state and
    //   progress percentage of all our tabs.
    winrt::TerminalApp::TaskbarState TerminalPage::TaskbarState() const
    {
        auto state{ winrt::make<winrt::TerminalApp::implementation::TaskbarState>() };

        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTerminalTabImpl(tab) })
            {
                auto tabState{ tabImpl->GetCombinedTaskbarState() };
                // lowest priority wins
                if (tabState.Priority() < state.Priority())
                {
                    state = tabState;
                }
            }
        }

        return state;
    }

    // Method Description:
    // - This is the method that App will call when the titlebar
    //   has been clicked. It dismisses any open flyouts.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::TitlebarClicked()
    {
        if (_newTabButton && _newTabButton.Flyout())
        {
            _newTabButton.Flyout().Hide();
        }
        _DismissTabContextMenus();
    }

    // Method Description:
    // - Called when the user tries to do a search using keybindings.
    //   This will tell the current focused terminal control to create
    //   a search box and enable find process.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_Find()
    {
        if (const auto& control{ _GetActiveControl() })
        {
            control.CreateSearchBoxControl();
        }
    }

    // Method Description:
    // - Toggles borderless mode. Hides the tab row, and raises our
    //   FocusModeChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFocusMode()
    {
        _SetFocusMode(!_isInFocusMode);
    }

    void TerminalPage::_SetFocusMode(const bool inFocusMode)
    {
        const bool newInFocusMode = inFocusMode;
        if (newInFocusMode != FocusMode())
        {
            _isInFocusMode = newInFocusMode;
            _UpdateTabView();
            _FocusModeChangedHandlers(*this, nullptr);
        }
    }

    // Method Description:
    // - Toggles fullscreen mode. Hides the tab row, and raises our
    //   FullscreenChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFullscreen()
    {
        _isFullscreen = !_isFullscreen;
        _UpdateTabView();
        _FullscreenChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Toggles always on top mode. Raises our AlwaysOnTopChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleAlwaysOnTop()
    {
        _isAlwaysOnTop = !_isAlwaysOnTop;
        _AlwaysOnTopChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Sets the tab split button color when a new tab color is selected
    // Arguments:
    // - color: The color of the newly selected tab, used to properly calculate
    //          the foreground color of the split button (to match the font
    //          color of the tab)
    // - accentColor: the actual color we are going to use to paint the tab row and
    //                split button, so that there is some contrast between the tab
    //                and the non-client are behind it
    // Return Value:
    // - <none>
    void TerminalPage::_SetNewTabButtonColor(const Windows::UI::Color& color, const Windows::UI::Color& accentColor)
    {
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        bool IsBrightColor = ColorHelper::IsBrightColor(color);
        bool isLightAccentColor = ColorHelper::IsBrightColor(accentColor);
        winrt::Windows::UI::Color pressedColor{};
        winrt::Windows::UI::Color hoverColor{};
        winrt::Windows::UI::Color foregroundColor{};
        const float hoverColorAdjustment = 5.f;
        const float pressedColorAdjustment = 7.f;

        if (IsBrightColor)
        {
            foregroundColor = winrt::Windows::UI::Colors::Black();
        }
        else
        {
            foregroundColor = winrt::Windows::UI::Colors::White();
        }

        if (isLightAccentColor)
        {
            hoverColor = ColorHelper::Darken(accentColor, hoverColorAdjustment);
            pressedColor = ColorHelper::Darken(accentColor, pressedColorAdjustment);
        }
        else
        {
            hoverColor = ColorHelper::Lighten(accentColor, hoverColorAdjustment);
            pressedColor = ColorHelper::Lighten(accentColor, pressedColorAdjustment);
        }

        Media::SolidColorBrush backgroundBrush{ accentColor };
        Media::SolidColorBrush backgroundHoverBrush{ hoverColor };
        Media::SolidColorBrush backgroundPressedBrush{ pressedColor };
        Media::SolidColorBrush foregroundBrush{ foregroundColor };

        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackground"), backgroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPointerOver"), backgroundHoverBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPressed"), backgroundPressedBrush);

        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForeground"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPointerOver"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPressed"), foregroundBrush);

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);
    }

    // Method Description:
    // - Clears the tab split button color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // - Clears the tab row color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ClearNewTabButtonColor()
    {
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        winrt::hstring keys[] = {
            L"SplitButtonBackground",
            L"SplitButtonBackgroundPointerOver",
            L"SplitButtonBackgroundPressed",
            L"SplitButtonForeground",
            L"SplitButtonForegroundPointerOver",
            L"SplitButtonForegroundPressed"
        };

        // simply clear any of the colors in the split button's dict
        for (auto keyString : keys)
        {
            auto key = winrt::box_value(keyString);
            if (_newTabButton.Resources().HasKey(key))
            {
                _newTabButton.Resources().Remove(key);
            }
        }

        const auto res = Application::Current().Resources();

        const auto defaultBackgroundKey = winrt::box_value(L"TabViewItemHeaderBackground");
        const auto defaultForegroundKey = winrt::box_value(L"SystemControlForegroundBaseHighBrush");
        winrt::Windows::UI::Xaml::Media::SolidColorBrush backgroundBrush;
        winrt::Windows::UI::Xaml::Media::SolidColorBrush foregroundBrush;

        // TODO: Related to GH#3917 - I think if the system is set to "Dark"
        // theme, but the app is set to light theme, then this lookup still
        // returns to us the dark theme brushes. There's gotta be a way to get
        // the right brushes...
        // See also GH#5741
        if (res.HasKey(defaultBackgroundKey))
        {
            winrt::Windows::Foundation::IInspectable obj = res.Lookup(defaultBackgroundKey);
            backgroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            backgroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::Black() };
        }

        if (res.HasKey(defaultForegroundKey))
        {
            winrt::Windows::Foundation::IInspectable obj = res.Lookup(defaultForegroundKey);
            foregroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            foregroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::White() };
        }

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);
    }

    // Method Description:
    // - Sets the tab split button color when a new tab color is selected
    // - This method could also set the color of the title bar and tab row
    // in the future
    // Arguments:
    // - selectedTabColor: The color of the newly selected tab
    // Return Value:
    // - <none>
    void TerminalPage::_SetNonClientAreaColors(const Windows::UI::Color& /*selectedTabColor*/)
    {
        // TODO GH#3327: Look at what to do with the NC area when we have XAML theming
    }

    // Method Description:
    // - Clears the tab split button color when the tab's color is cleared
    // - This method could also clear the color of the title bar and tab row
    // in the future
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ClearNonClientAreaColors()
    {
        // TODO GH#3327: Look at what to do with the NC area when we have XAML theming
    }

    // Function Description:
    // - This is a helper method to get the commandline out of a
    //   ExecuteCommandline action, break it into subcommands, and attempt to
    //   parse it into actions. This is used by _HandleExecuteCommandline for
    //   processing commandlines in the current WT window.
    // Arguments:
    // - args: the ExecuteCommandlineArgs to synthesize a list of startup actions for.
    // Return Value:
    // - an empty list if we failed to parse, otherwise a list of actions to execute.
    std::vector<ActionAndArgs> TerminalPage::ConvertExecuteCommandlineToActions(const ExecuteCommandlineArgs& args)
    {
        ::TerminalApp::AppCommandlineArgs appArgs;
        if (appArgs.ParseArgs(args) == 0)
        {
            return appArgs.GetStartupActions();
        }

        return {};
    }

    void TerminalPage::_FocusActiveControl(IInspectable /*sender*/,
                                           IInspectable /*eventArgs*/)
    {
        _FocusCurrentTab(false);
    }

    bool TerminalPage::FocusMode() const
    {
        return _isInFocusMode;
    }

    bool TerminalPage::Fullscreen() const
    {
        return _isFullscreen;
    }

    // Method Description:
    // - Returns true if we're currently in "Always on top" mode. When we're in
    //   always on top mode, the window should be on top of all other windows.
    //   If multiple windows are all "always on top", they'll maintain their own
    //   z-order, with all the windows on top of all other non-topmost windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if we should be in "always on top" mode
    bool TerminalPage::AlwaysOnTop() const
    {
        return _isAlwaysOnTop;
    }

    HRESULT TerminalPage::_OnNewConnection(const ConptyConnection& connection)
    {
        // We need to be on the UI thread in order for _OpenNewTab to run successfully.
        // HasThreadAccess will return true if we're currently on a UI thread and false otherwise.
        // When we're on a COM thread, we'll need to dispatch the calls to the UI thread
        // and wait on it hence the locking mechanism.
        if (!Dispatcher().HasThreadAccess())
        {
            til::latch latch{ 1 };
            HRESULT finalVal = S_OK;

            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [&]() {
                // Re-running ourselves under the dispatcher will cause us to take the first branch above.
                finalVal = _OnNewConnection(connection);
                latch.count_down();
            });

            latch.wait();
            return finalVal;
        }

        try
        {
            NewTerminalArgs newTerminalArgs{};
            // TODO GH#10952: When we pass the actual commandline (or originating application), the
            // settings model can choose the right settings based on command matching, or synthesize
            // a profile from the registry/link settings (TODO GH#9458).
            // TODO GH#9458: Get and pass the LNK/EXE filenames.
            // Passing in a commandline forces GetProfileForArgs to use Base Layer instead of Default Profile;
            // in the future, it can make a better decision based on the value we pull out of the process handle.
            // TODO GH#5047: When we hang on to the N.T.A., try not to spawn "default... .exe" :)
            newTerminalArgs.Commandline(connection.Commandline());
            const auto profile{ _settings.GetProfileForArgs(newTerminalArgs) };
            const auto settings{ TerminalSettings::CreateWithProfile(_settings, profile, *_bindings) };

            _CreateNewTabWithProfileAndSettings(profile, settings, connection);

            // Request a summon of this window to the foreground
            _SummonWindowRequestedHandlers(*this, nullptr);
            return S_OK;
        }
        CATCH_RETURN()
    }

    // Method Description:
    // - Creates a settings UI tab and focuses it. If there's already a settings UI tab open,
    //   just focus the existing one.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_OpenSettingsUI()
    {
        // If we're holding the settings tab's switch command, don't create a new one, switch to the existing one.
        if (!_settingsTab)
        {
            winrt::Microsoft::Terminal::Settings::Editor::MainPage sui{ _settings };
            if (_hostingHwnd)
            {
                sui.SetHostingWindow(reinterpret_cast<uint64_t>(*_hostingHwnd));
            }

            // GH#8767 - let unhandled keys in the SUI try to run commands too.
            sui.KeyDown({ this, &TerminalPage::_KeyDownHandler });

            sui.OpenJson([weakThis{ get_weak() }](auto&& /*s*/, winrt::Microsoft::Terminal::Settings::Model::SettingsTarget e) {
                if (auto page{ weakThis.get() })
                {
                    page->_LaunchSettings(e);
                }
            });

            auto newTabImpl = winrt::make_self<SettingsTab>(sui);

            // Add the new tab to the list of our tabs.
            _tabs.Append(*newTabImpl);
            _mruTabs.Append(*newTabImpl);

            newTabImpl->SetDispatch(*_actionDispatch);
            newTabImpl->SetActionMap(_settings.ActionMap());

            // Give the tab its index in the _tabs vector so it can manage its own SwitchToTab command.
            _UpdateTabIndices();

            // Don't capture a strong ref to the tab. If the tab is removed as this
            // is called, we don't really care anymore about handling the event.
            auto weakTab = make_weak(newTabImpl);

            auto tabViewItem = newTabImpl->TabViewItem();
            _tabView.TabItems().Append(tabViewItem);

            tabViewItem.PointerPressed({ this, &TerminalPage::_OnTabClick });

            // When the tab requests close, try to close it (prompt for approval, if required)
            newTabImpl->CloseRequested([weakTab, weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
                auto page{ weakThis.get() };
                auto tab{ weakTab.get() };

                if (page && tab)
                {
                    page->_HandleCloseTabRequested(*tab);
                }
            });

            // When the tab is closed, remove it from our list of tabs.
            newTabImpl->Closed([tabViewItem, weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
                if (auto page{ weakThis.get() })
                {
                    page->_settingsTab = nullptr;
                    page->_RemoveOnCloseRoutine(tabViewItem, page);
                }
            });

            _settingsTab = *newTabImpl;

            // This kicks off TabView::SelectionChanged, in response to which
            // we'll attach the terminal's Xaml control to the Xaml root.
            _tabView.SelectedItem(tabViewItem);
        }
        else
        {
            _tabView.SelectedItem(_settingsTab.TabViewItem());
        }
    }

    // Method Description:
    // - Returns a com_ptr to the implementation type of the given tab if it's a TerminalTab.
    //   If the tab is not a TerminalTab, returns nullptr.
    // Arguments:
    // - tab: the projected type of a Tab
    // Return Value:
    // - If the tab is a TerminalTab, a com_ptr to the implementation type.
    //   If the tab is not a TerminalTab, nullptr
    winrt::com_ptr<TerminalTab> TerminalPage::_GetTerminalTabImpl(const TerminalApp::TabBase& tab)
    {
        if (auto terminalTab = tab.try_as<TerminalApp::TerminalTab>())
        {
            winrt::com_ptr<TerminalTab> tabImpl;
            tabImpl.copy_from(winrt::get_self<TerminalTab>(terminalTab));
            return tabImpl;
        }
        else
        {
            return nullptr;
        }
    }

    // Method Description:
    // - Computes the delta for scrolling the tab's viewport.
    // Arguments:
    // - scrollDirection - direction (up / down) to scroll
    // - rowsToScroll - the number of rows to scroll
    // Return Value:
    // - delta - Signed delta, where a negative value means scrolling up.
    int TerminalPage::_ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll)
    {
        return scrollDirection == ScrollUp ? -1 * rowsToScroll : rowsToScroll;
    }

    // Method Description:
    // - Reads system settings for scrolling (based on the step of the mouse scroll).
    // Upon failure fallbacks to default.
    // Return Value:
    // - The number of rows to scroll or a magic value of WHEEL_PAGESCROLL
    // indicating that we need to scroll an entire view height
    uint32_t TerminalPage::_ReadSystemRowsToScroll()
    {
        uint32_t systemRowsToScroll;
        if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &systemRowsToScroll, 0))
        {
            LOG_LAST_ERROR();

            // If SystemParametersInfoW fails, which it shouldn't, fall back to
            // Windows' default value.
            return DefaultRowsToScroll;
        }

        return systemRowsToScroll;
    }

    // Method Description:
    // - Displays a dialog stating the "Touch Keyboard and Handwriting Panel
    //   Service" is disabled.
    void TerminalPage::ShowKeyboardServiceWarning() const
    {
        if (!_IsMessageDismissed(InfoBarMessage::KeyboardServiceWarning))
        {
            if (const auto keyboardServiceWarningInfoBar = FindName(L"KeyboardServiceWarningInfoBar").try_as<MUX::Controls::InfoBar>())
            {
                keyboardServiceWarningInfoBar.IsOpen(true);
            }
        }
    }

    // Function Description:
    // - Helper function to get the OS-localized name for the "Touch Keyboard
    //   and Handwriting Panel Service". If we can't open up the service for any
    //   reason, then we'll just return the service's key, "TabletInputService".
    // Return Value:
    // - The OS-localized name for the TabletInputService
    winrt::hstring _getTabletServiceName()
    {
        auto isUwp = false;
        try
        {
            isUwp = ::winrt::Windows::UI::Xaml::Application::Current().as<::winrt::TerminalApp::App>().Logic().IsUwp();
        }
        CATCH_LOG();

        if (isUwp)
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        wil::unique_schandle hManager{ OpenSCManager(nullptr, nullptr, 0) };

        if (LOG_LAST_ERROR_IF(!hManager.is_valid()))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        DWORD cchBuffer = 0;
        GetServiceDisplayName(hManager.get(), TabletInputServiceKey.data(), nullptr, &cchBuffer);
        std::wstring buffer;
        cchBuffer += 1; // Add space for a null
        buffer.resize(cchBuffer);

        if (LOG_LAST_ERROR_IF(!GetServiceDisplayName(hManager.get(),
                                                     TabletInputServiceKey.data(),
                                                     buffer.data(),
                                                     &cchBuffer)))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }
        return winrt::hstring{ buffer };
    }

    // Method Description:
    // - Return the fully-formed warning message for the
    //   "KeyboardServiceDisabled" InfoBar. This InfoBar is used to warn the user
    //   if the keyboard service is disabled, and uses the OS localization for
    //   the service's actual name. It's bound to the bar in XAML.
    // Return Value:
    // - The warning message, including the OS-localized service name.
    winrt::hstring TerminalPage::KeyboardServiceDisabledText()
    {
        const winrt::hstring serviceName{ _getTabletServiceName() };
        const winrt::hstring text{ fmt::format(std::wstring_view(RS_(L"KeyboardServiceWarningText")), serviceName) };
        return text;
    }

    // Method Description:
    // - Hides cursor if required
    // Return Value:
    // - <none>
    void TerminalPage::_HidePointerCursorHandler(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        if (_shouldMouseVanish && !_isMouseHidden)
        {
            if (auto window{ CoreWindow::GetForCurrentThread() })
            {
                try
                {
                    window.PointerCursor(nullptr);
                    _isMouseHidden = true;
                }
                CATCH_LOG();
            }
        }
    }

    // Method Description:
    // - Restores cursor if required
    // Return Value:
    // - <none>
    void TerminalPage::_RestorePointerCursorHandler(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        if (_isMouseHidden)
        {
            if (auto window{ CoreWindow::GetForCurrentThread() })
            {
                try
                {
                    window.PointerCursor(_defaultPointerCursor);
                    _isMouseHidden = false;
                }
                CATCH_LOG();
            }
        }
    }

    // Method Description:
    // - Update the RequestedTheme of the specified FrameworkElement and all its
    //   Parent elements. We need to do this so that we can actually theme all
    //   of the elements of the TeachingTip. See GH#9717
    // Arguments:
    // - element: The TeachingTip to set the theme on.
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element)
    {
        auto theme{ _settings.GlobalSettings().Theme() };
        while (element)
        {
            element.RequestedTheme(theme);
            element = element.Parent().try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
        }
    }

    // Method Description:
    // - Display the name and ID of this window in a TeachingTip. If the window
    //   has no name, the name will be presented as "<unnamed-window>".
    // - This can be invoked by either:
    //   * An identifyWindow action, that displays the info only for the current
    //     window
    //   * An identifyWindows action, that displays the info for all windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    winrt::fire_and_forget TerminalPage::IdentifyWindow()
    {
        auto weakThis{ get_weak() };
        co_await winrt::resume_foreground(Dispatcher());
        if (auto page{ weakThis.get() })
        {
            // If we haven't ever loaded the TeachingTip, then do so now and
            // create the toast for it.
            if (page->_windowIdToast == nullptr)
            {
                if (MUX::Controls::TeachingTip tip{ page->FindName(L"WindowIdToast").try_as<MUX::Controls::TeachingTip>() })
                {
                    page->_windowIdToast = std::make_shared<Toast>(tip);
                    // Make sure to use the weak ref when setting up this
                    // callback.
                    tip.Closed({ page->get_weak(), &TerminalPage::_FocusActiveControl });
                }
            }
            _UpdateTeachingTipTheme(WindowIdToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

            if (page->_windowIdToast != nullptr)
            {
                page->_windowIdToast->Open();
            }
        }
    }

    // WindowName is a otherwise generic WINRT_OBSERVABLE_PROPERTY, but it needs
    // to raise a PropertyChanged for WindowNameForDisplay, instead of
    // WindowName.
    winrt::hstring TerminalPage::WindowName() const noexcept
    {
        return _WindowName;
    }

    winrt::fire_and_forget TerminalPage::WindowName(const winrt::hstring& value)
    {
        const bool oldIsQuakeMode = IsQuakeWindow();
        const bool changed = _WindowName != value;
        if (changed)
        {
            _WindowName = value;
        }
        auto weakThis{ get_weak() };
        // On the foreground thread, raise property changed notifications, and
        // display the success toast.
        co_await resume_foreground(Dispatcher());
        if (auto page{ weakThis.get() })
        {
            if (changed)
            {
                page->_PropertyChangedHandlers(*this, WUX::Data::PropertyChangedEventArgs{ L"WindowName" });
                page->_PropertyChangedHandlers(*this, WUX::Data::PropertyChangedEventArgs{ L"WindowNameForDisplay" });

                // DON'T display the confirmation if this is the name we were
                // given on startup!
                if (page->_startupState == StartupState::Initialized)
                {
                    page->IdentifyWindow();

                    // If we're entering quake mode, or leaving it
                    if (IsQuakeWindow() != oldIsQuakeMode)
                    {
                        // If we're entering Quake Mode from ~Focus Mode, then this will enter Focus Mode
                        // If we're entering Quake Mode from Focus Mode, then this will do nothing
                        // If we're leaving Quake Mode (we're already in Focus Mode), then this will do nothing
                        _SetFocusMode(true);
                        _IsQuakeWindowChangedHandlers(*this, nullptr);
                    }
                }
            }
        }
    }

    // WindowId is a otherwise generic WINRT_OBSERVABLE_PROPERTY, but it needs
    // to raise a PropertyChanged for WindowIdForDisplay, instead of
    // WindowId.
    uint64_t TerminalPage::WindowId() const noexcept
    {
        return _WindowId;
    }
    void TerminalPage::WindowId(const uint64_t& value)
    {
        if (_WindowId != value)
        {
            _WindowId = value;
            _PropertyChangedHandlers(*this, WUX::Data::PropertyChangedEventArgs{ L"WindowIdForDisplay" });
        }
    }

    void TerminalPage::SetPersistedLayoutIdx(const uint32_t idx)
    {
        _loadFromPersistedLayoutIdx = idx;
    }

    void TerminalPage::SetNumberOfOpenWindows(const uint64_t num)
    {
        _numOpenWindows = num;
    }

    // Method Description:
    // - Returns a label like "Window: 1234" for the ID of this window
    // Arguments:
    // - <none>
    // Return Value:
    // - a string for displaying the name of the window.
    winrt::hstring TerminalPage::WindowIdForDisplay() const noexcept
    {
        return winrt::hstring{ fmt::format(L"{}: {}",
                                           std::wstring_view(RS_(L"WindowIdLabel")),
                                           _WindowId) };
    }

    // Method Description:
    // - Returns a label like "<unnamed window>" when the window has no name, or the name of the window.
    // Arguments:
    // - <none>
    // Return Value:
    // - a string for displaying the name of the window.
    winrt::hstring TerminalPage::WindowNameForDisplay() const noexcept
    {
        return _WindowName.empty() ?
                   winrt::hstring{ fmt::format(L"<{}>", RS_(L"UnnamedWindowName")) } :
                   _WindowName;
    }

    // Method Description:
    // - Called when an attempt to rename the window has failed. This will open
    //   the toast displaying a message to the user that the attempt to rename
    //   the window has failed.
    // - This will load the RenameFailedToast TeachingTip the first time it's called.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    winrt::fire_and_forget TerminalPage::RenameFailed()
    {
        auto weakThis{ get_weak() };
        co_await winrt::resume_foreground(Dispatcher());
        if (auto page{ weakThis.get() })
        {
            // If we haven't ever loaded the TeachingTip, then do so now and
            // create the toast for it.
            if (page->_windowRenameFailedToast == nullptr)
            {
                if (MUX::Controls::TeachingTip tip{ page->FindName(L"RenameFailedToast").try_as<MUX::Controls::TeachingTip>() })
                {
                    page->_windowRenameFailedToast = std::make_shared<Toast>(tip);
                    // Make sure to use the weak ref when setting up this
                    // callback.
                    tip.Closed({ page->get_weak(), &TerminalPage::_FocusActiveControl });
                }
            }
            _UpdateTeachingTipTheme(RenameFailedToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

            if (page->_windowRenameFailedToast != nullptr)
            {
                page->_windowRenameFailedToast->Open();
            }
        }
    }

    // Method Description:
    // - Called when the user hits the "Ok" button on the WindowRenamer TeachingTip.
    // - Will raise an event that will bubble up to the monarch, asking if this
    //   name is acceptable.
    //   - If it is, we'll eventually get called back in TerminalPage::WindowName(hstring).
    //   - If not, then TerminalPage::RenameFailed will get called.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerActionClick(const IInspectable& /*sender*/,
                                                 const IInspectable& /*eventArgs*/)
    {
        auto newName = WindowRenamerTextBox().Text();
        _RequestWindowRename(newName);
    }

    void TerminalPage::_RequestWindowRename(const winrt::hstring& newName)
    {
        auto request = winrt::make<implementation::RenameWindowRequestedArgs>(newName);
        // The WindowRenamer is _not_ a Toast - we want it to stay open until
        // the user dismisses it.
        if (WindowRenamer())
        {
            WindowRenamer().IsOpen(false);
        }
        _RenameWindowRequestedHandlers(*this, request);
        // We can't just use request.Successful here, because the handler might
        // (will) be handling this asynchronously, so when control returns to
        // us, this hasn't actually been handled yet. We'll get called back in
        // RenameFailed if this fails.
        //
        // Theoretically we could do a IAsyncOperation<RenameWindowResult> kind
        // of thing with co_return winrt::make<RenameWindowResult>(false).
    }

    // Method Description:
    // - Manually handle Enter and Escape for committing and dismissing a window
    //   rename. This is highly similar to the TabHeaderControl's KeyUp handler.
    // Arguments:
    // - e: the KeyRoutedEventArgs describing the key that was released
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerKeyUp(const IInspectable& sender,
                                           winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        if (e.OriginalKey() == Windows::System::VirtualKey::Enter)
        {
            // User is done making changes, close the rename box
            _WindowRenamerActionClick(sender, nullptr);
        }
        else if (e.OriginalKey() == Windows::System::VirtualKey::Escape)
        {
            // User wants to discard the changes they made
            WindowRenamerTextBox().Text(WindowName());
            WindowRenamer().IsOpen(false);
        }
    }

    bool TerminalPage::IsQuakeWindow() const noexcept
    {
        return WindowName() == QuakeWindowName;
    }

    // Method Description:
    // - This function stops people from duplicating the base profile, because
    //   it gets ~ ~ weird ~ ~ when they do. Remove when TODO GH#5047 is done.
    Profile TerminalPage::GetClosestProfileForDuplicationOfProfile(const Profile& profile) const noexcept
    {
        if (profile == _settings.ProfileDefaults())
        {
            return _settings.FindProfile(_settings.GlobalSettings().DefaultProfile());
        }
        return profile;
    }

    // Method Description:
    // - Handles the change of connection state.
    // If the connection state is failure show information bar suggesting to configure termination behavior
    // (unless user asked not to show this message again)
    // Arguments:
    // - sender: the ICoreState instance containing the connection state
    // Return Value:
    // - <none>
    winrt::fire_and_forget TerminalPage::_ConnectionStateChangedHandler(const IInspectable& sender, const IInspectable& /*args*/) const
    {
        if (const auto coreState{ sender.try_as<winrt::Microsoft::Terminal::Control::ICoreState>() })
        {
            const auto newConnectionState = coreState.ConnectionState();
            if (newConnectionState == ConnectionState::Failed && !_IsMessageDismissed(InfoBarMessage::CloseOnExitInfo))
            {
                co_await winrt::resume_foreground(Dispatcher());
                if (const auto infoBar = FindName(L"CloseOnExitInfoBar").try_as<MUX::Controls::InfoBar>())
                {
                    infoBar.IsOpen(true);
                }
            }
        }
    }

    // Method Description:
    // - Persists the user's choice not to show information bar guiding to configure termination behavior.
    // Then hides this information buffer.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_CloseOnExitInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        _DismissMessage(InfoBarMessage::CloseOnExitInfo);
        if (const auto infoBar = FindName(L"CloseOnExitInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // Method Description:
    // - Persists the user's choice not to show information bar warning about "Touch keyboard and Handwriting Panel Service" disabled
    // Then hides this information buffer.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_KeyboardServiceWarningInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        _DismissMessage(InfoBarMessage::KeyboardServiceWarning);
        if (const auto infoBar = FindName(L"KeyboardServiceWarningInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // Method Description:
    // - Checks whether information bar message was dismissed earlier (in the application state)
    // Arguments:
    // - message: message to look for in the state
    // Return Value:
    // - true, if the message was dismissed
    bool TerminalPage::_IsMessageDismissed(const InfoBarMessage& message)
    {
        if (const auto dismissedMessages{ ApplicationState::SharedInstance().DismissedMessages() })
        {
            for (const auto& dismissedMessage : dismissedMessages)
            {
                if (dismissedMessage == message)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Method Description:
    // - Persists the user's choice to dismiss information bar message (in application state)
    // Arguments:
    // - message: message to dismiss
    // Return Value:
    // - <none>
    void TerminalPage::_DismissMessage(const InfoBarMessage& message)
    {
        auto dismissedMessages = ApplicationState::SharedInstance().DismissedMessages();
        if (!dismissedMessages)
        {
            dismissedMessages = winrt::single_threaded_vector<InfoBarMessage>();
        }

        dismissedMessages.Append(message);
        ApplicationState::SharedInstance().DismissedMessages(dismissedMessages);
    }
}
