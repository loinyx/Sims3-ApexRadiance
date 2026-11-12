using System.Runtime.InteropServices;

namespace ApexSetup;

// The best Vulkan version a graphics card of this PC supports, asked from the Vulkan driver (vulkan-1.dll).
// The RTX Remix bridge draws through DXVK 2, which needs Vulkan 1.3.
public static class Gpu
{
    public static readonly Version Needed = new(1, 3);

    // (version, card name) of the card with the highest version; (null, null) when no Vulkan driver or card is found.
    // Throws when the driver could not be asked: unknown, not a no.
    public static (Version? Version, string? Card) Vulkan()
    {
        nint instance;
        var info = new InstanceCreateInfo { SType = 1 }; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
        try
        {
            if (vkCreateInstance(ref info, 0, out instance) is var r && r != 0)
                return r == -9 ? (null, null) : throw new InvalidOperationException($"vkCreateInstance {r}"); // -9: VK_ERROR_INCOMPATIBLE_DRIVER
        }
        catch (DllNotFoundException) { return (null, null); }
        try
        {
            uint n = 0;
            vkEnumeratePhysicalDevices(instance, ref n, null);
            var devices = new nint[n];
            if (n > 0) vkEnumeratePhysicalDevices(instance, ref n, devices);
            (Version? Version, string? Card) best = (null, null);
            var props = new byte[1024]; // VkPhysicalDeviceProperties: 824 bytes on x64
            foreach (var d in devices.Take((int)n))
            {
                vkGetPhysicalDeviceProperties(d, props);
                var v = Decode(BitConverter.ToUInt32(props, 0)); // apiVersion
                var name = System.Text.Encoding.UTF8.GetString(props, 20, 256).Split('\0')[0]; // deviceName
                if (best.Version is null || v > best.Version) best = (v, name);
            }
            return best;
        }
        finally { vkDestroyInstance(instance, 0); }
    }

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
    [DllImport("vulkan-1.dll")] static extern void vkDestroyInstance(nint instance, nint allocator);
}
