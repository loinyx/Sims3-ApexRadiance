using System.Text.Json.Serialization;

namespace ApexSetup;

// The JSON Setup keeps in Game\Bin (setup.json, restore points), generated at build time so the trimmed exe keeps
// everything it reads and writes. Same names and layout as the reflection serializer wrote before, checked by the tests.
[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(Manifest))]
[JsonSerializable(typeof(RestorePoint))]
public partial class SetupJson : JsonSerializerContext;
