using System.Runtime.InteropServices;

namespace ApexSetup;

// What the graphics cards of this PC offer, asked from the Vulkan driver (vulkan-1.dll). DXVK and the RTX Remix bridge (which draws
// through DXVK) need Vulkan 1.3 and the device extensions DXVK requires: without one, DXVK finds no adapter and the game closes
// at start (an Intel Iris Xe on a 2023 driver: Vulkan 1.3 but no VK_KHR_load_store_op_none).
public static class Gpu
{
    public static readonly Version Needed = new(1, 3);

    // The extensions DXVK 3.1.1 marks required (src/dxvk/dxvk_device_info.cpp, ENABLE_EXT / ENABLE_EXT_FEATURE with true). Setup
    // installs DXVK's latest release: check this list again when DXVK publishes a new version. Setup asks the 64-bit driver; the
    // game's 32-bit DXVK asks the 32-bit one, which ships with it.
    public static readonly string[] DxvkExtensions =
    {
        "VK_KHR_swapchain", "VK_EXT_depth_clip_enable", "VK_EXT_robustness2", "VK_KHR_load_store_op_none", "VK_KHR_maintenance5", "VK_KHR_maintenance6",
    };

    // The best card: one DXVK can use if there is one, else the highest Vulkan version. Missing: DXVK's required extensions that
    // card lacks. (null, null, []) when no Vulkan driver or card is found. Throws when the driver could not be asked: unknown, not a no.
    public static (Version? Version, string? Card, string[] Missing) Vulkan()
    {
        nint instance;
        var info = new InstanceCreateInfo { SType = 1 }; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
        try
        {
            if (vkCreateInstance(ref info, 0, out instance) is var r && r != 0)
                return r == -9 ? (null, null, Array.Empty<string>()) : throw new InvalidOperationException($"vkCreateInstance {r}"); // -9: VK_ERROR_INCOMPATIBLE_DRIVER
        }
        catch (DllNotFoundException) { return (null, null, Array.Empty<string>()); }
        try
        {
            uint n = 0;
            Ok(vkEnumeratePhysicalDevices(instance, ref n, null));
            var devices = new nint[n];
            if (n > 0) Ok(vkEnumeratePhysicalDevices(instance, ref n, devices));
            (Version? Version, string? Card, string[] Missing) best = (null, null, Array.Empty<string>());
            var props = new byte[1024]; // VkPhysicalDeviceProperties: 824 bytes on x64
            foreach (var d in devices.Take((int)n))
            {
                vkGetPhysicalDeviceProperties(d, props);
                var v = Decode(BitConverter.ToUInt32(props, 0)); // apiVersion
                var name = System.Text.Encoding.UTF8.GetString(props, 20, 256).Split('\0')[0]; // deviceName
                var missing = Lacking(Extensions(d));
                if (best.Version is null || Better((v, missing), (best.Version, best.Missing))) best = (v, name, missing);
            }
            return best;
        }
        finally { vkDestroyInstance(instance, 0); }
    }

    // Vulkan 1.3 with an extension missing is an old driver, not an old card (Iris Xe gets VK_KHR_load_store_op_none from
    // driver 101.6556, 2025): the vendor's driver page for that card
    public static string DriverPage(string? card) =>
        card?.Contains("Intel", StringComparison.OrdinalIgnoreCase) == true ? "https://www.intel.com/content/www/us/en/support/detect.html"
        : card?.Contains("AMD", StringComparison.OrdinalIgnoreCase) == true || card?.Contains("Radeon", StringComparison.OrdinalIgnoreCase) == true ? "https://www.amd.com/en/support/download/drivers.html"
        : "https://www.nvidia.com/en-us/drivers/";

    public static string[] Lacking(ICollection<string> extensions) => DxvkExtensions.Where(e => !extensions.Contains(e)).ToArray();

    // a card DXVK can use beats one it cannot; between equals, the higher Vulkan version
    public static bool Better((Version V, string[] Missing) a, (Version V, string[] Missing) b)
    {
        bool Usable((Version V, string[] Missing) c) => c.V >= Needed && c.Missing.Length == 0;
        return Usable(a) != Usable(b) ? Usable(a) : a.V > b.V;
    }

    static HashSet<string> Extensions(nint device)
    {
        uint n = 0;
        Ok(vkEnumerateDeviceExtensionProperties(device, 0, ref n, null));
        var raw = new byte[n * 260]; // VkExtensionProperties: char extensionName[256], uint32 specVersion
        if (n > 0) Ok(vkEnumerateDeviceExtensionProperties(device, 0, ref n, raw));
        var set = new HashSet<string>();
        for (var i = 0; i < n; i++) set.Add(System.Text.Encoding.UTF8.GetString(raw, i * 260, 256).Split('\0')[0]);
        return set;
    }

    // a failed query is "unknown", never "this card cannot" (VK_INCOMPLETE, 5, is a success code)
    static void Ok(int r) { if (r < 0) throw new InvalidOperationException($"Vulkan query {r}"); }

    // VK_MAKE_API_VERSION: variant 3 bits, major 7, minor 10, patch 12
    public static Version Decode(uint v) => new((int)(v >> 22) & 0x7F, (int)(v >> 12) & 0x3FF);

    [StructLayout(LayoutKind.Sequential)]
    struct InstanceCreateInfo
    {
        public uint SType; public nint PNext; public uint Flags; public nint ApplicationInfo;
        public uint LayerCount; public nint Layers; public uint ExtensionCount; public nint Extensions;
    }

    [DllImport("vulkan-1.dll")] static extern int vkCreateInstance(ref InstanceCreateInfo info, nint allocator, out nint instance);
    [DllImport("vulkan-1.dll")] static extern int vkEnumeratePhysicalDevices(nint instance, ref uint count, nint[]? devices);
    [DllImport("vulkan-1.dll")] static extern void vkGetPhysicalDeviceProperties(nint device, byte[] properties);
    [DllImport("vulkan-1.dll")] static extern int vkEnumerateDeviceExtensionProperties(nint device, nint layerName, ref uint count, byte[]? properties);
    [DllImport("vulkan-1.dll")] static extern void vkDestroyInstance(nint instance, nint allocator);
}
