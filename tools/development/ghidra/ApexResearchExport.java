// Export a private function index and selected Steam engine decompilations.
// Output is for local research only; do not commit generated game code.
//@category ApexRadiance
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.io.BufferedWriter;

public class ApexResearchExport extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Pass one private output directory.");
        if (!currentProgram.getLanguageID().toString().startsWith("x86:LE:32") ||
            currentProgram.getImageBase().getOffset() != 0x400000L)
            throw new IllegalStateException("Expected an x86 Steam image based at 0x00400000.");
        Path output = Path.of(args[0]);
        Files.createDirectories(output);
        int count = 0;
        try (BufferedWriter writer = Files.newBufferedWriter(output.resolve("functions.tsv"), StandardCharsets.UTF_8)) {
            writer.write("entry\tname\tsignature\n");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext()) {
                monitor.checkCancelled();
                Function fn = functions.next();
                writer.write(fn.getEntryPoint() + "\t" + fn.getName().replace('\t',' ') + "\t" + fn.getSignature().toString().replace('\t',' ') + "\n");
                count++;
            }
        }
        // Addresses documented in docs/engine and the codec harnesses, Steam only.
        long[] addresses = {0x004eb3b0L,0x004ec0a0L,0x005d1960L,0x006152f0L,0x006154b0L,
            0x006a18b0L,0x006a31d0L,0x006a3c90L,0x006a3f80L,0x006ac070L,
            0x006bc020L,0x006c5c20L,0x006e4130L,0x00c292b0L,0x00c62d40L};
        DecompInterface decompiler = new DecompInterface();
        int exported = 0;
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Cannot open decompiler.");
            for (long address : addresses) {
                monitor.checkCancelled();
                Function fn = currentProgram.getFunctionManager().getFunctionAt(toAddr(address));
                if (fn == null) { println("No function at " + Long.toHexString(address)); continue; }
                DecompileResults result = decompiler.decompileFunction(fn, 60, monitor);
                if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                    println("Decompilation failed at " + fn.getEntryPoint() + ": " + result.getErrorMessage());
                    continue;
                }
                Files.writeString(output.resolve(String.format("fn_%08x.c", address)),
                    "// Private Ghidra output; inferred types and control flow require verification.\n" + result.getDecompiledFunction().getC(), StandardCharsets.UTF_8);
                exported++;
            }
        } finally { decompiler.dispose(); }
        Files.writeString(output.resolve("summary.txt"), "Functions indexed: " + count + "\nSelected decompilations: " + exported + "/" + addresses.length + "\n", StandardCharsets.UTF_8);
        println("Indexed " + count + " functions; exported " + exported + " selected engine functions.");
    }
}
