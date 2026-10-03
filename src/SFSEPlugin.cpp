#ifdef USE_SFSE_PLUGIN

#include <cstdint>
#include <windows.h>

// Mirrors SFSEPluginVersionData / SFSEInterface from ianpatt/sfse sfse/PluginAPI.h.
// Declared locally because this build uses sdk-lite rather than CommonLibSF.
namespace
{
    struct SFSEInterface
    {
        std::uint32_t sfseVersion;
        std::uint32_t runtimeVersion;
        std::uint32_t interfaceVersion;
        void* (*QueryInterface)(std::uint32_t id);
    };

    struct SFSEPluginVersionData
    {
        std::uint32_t dataVersion;
        std::uint32_t pluginVersion;
        char          name[256];
        char          author[256];
        std::uint32_t addressIndependence;
        std::uint32_t structureIndependence;
        std::uint32_t compatibleVersions[16];
        std::uint32_t seVersionRequired;
        std::uint32_t reservedNonBreaking;
        std::uint32_t reservedBreaking;
    };

    constexpr std::uint32_t kAddressIndependence_Signatures         = 1 << 0;
    constexpr std::uint32_t kStructureIndependence_1_14_70_Layout = 1 << 3;
} // namespace

void StartVR();

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
    1, // kVersion
    1,
    "Starfield VR",
    "mutars, AetherianArtificer",
    kAddressIndependence_Signatures,
    kStructureIndependence_1_14_70_Layout,
    { 0 },
    0,
    0,
    0,
};

__declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface*)
{
    StartVR();
    return true;
}
}

#endif
