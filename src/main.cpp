#include "func_finder.hpp"
#include "globals.hpp"
#include "plugin.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT void PLUGIN_EXIT()
{
    if (g_pToggleGroupHook) {
        g_pToggleGroupHook->unhook();
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pToggleGroupHook);
        g_pToggleGroupHook = nullptr;
    }

    HyprlandAPI::addNotification(PHANDLE, "[dwindle-autogroup] Unloaded successfully!", s_notifyColor, 5000);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    PHANDLE = handle;

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Loading Hyprland functions");

    g_pNodeFromWindow = (nodeFromWindowFuncT)findHyprlandFunction(
        "getNodeFromWindow",
        "Layout::Tiled::CDwindleAlgorithm::getNodeFromWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>)");

    auto pToggleGroup = findHyprlandFunction(
        "toggleGroup",
        "CKeybindManager::toggleGroup(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >)");

    if (!g_pNodeFromWindow || !pToggleGroup) {
        g_pNodeFromWindow = nullptr;
        return s_pluginDescription;
    }

    Log::logger->log(Log::INFO, "[dwindle-autogroup] Registering function hooks");

    g_pToggleGroupHook = HyprlandAPI::createFunctionHook(PHANDLE, pToggleGroup, (void*)&newToggleGroup);
    g_pToggleGroupHook->hook();

    HyprlandAPI::addNotification(PHANDLE, "[dwindle-autogroup] Initialized successfully!", s_notifyColor, 5000);
    return s_pluginDescription;
}
