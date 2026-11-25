using System.Text.Json.Serialization;

namespace ApexSetup;

// The JSON Setup keeps in Game\Bin (setup.json, restore points), generated at build time so the trimmed exe keeps
// everything it reads and writes. Same names and layout as the reflection serializer wrote before, checked by the tests.
[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(Manifest))]
[JsonSerializable(typeof(RestorePoint))]
[JsonSerializable(typeof(LauncherSettings))]
[JsonSerializable(typeof(BackupChoices))]
[JsonSerializable(typeof(ModNotesFile))]
[JsonSerializable(typeof(ModProfilesFile))]
[JsonSerializable(typeof(MergeRecord))]
[JsonSerializable(typeof(List<SessionInfo>))]
[JsonSerializable(typeof(Dictionary<string, List<SizePoint>>))]
[JsonSerializable(typeof(Dictionary<string, List<string>>))]
[JsonSerializable(typeof(ConflictResult))]
[JsonSerializable(typeof(GalleryData))]
public partial class SetupJson : JsonSerializerContext;
