// Bounded initial analysis for the verified Steam executable.
// @category ApexRadiance
import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import ghidra.program.model.listing.Program;

public class ApexAnalysisSetup extends GhidraScript {
    @Override
    public void run() throws Exception {
        Options options = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
        boolean foundX86ConstantReferences = false;
        boolean foundStackAnalyzer = false;
        boolean foundSwitchAnalyzer = false;
        for (String name : options.getOptionNames()) {
            if (name.equals("Decompiler Switch Analysis")) {
                options.setBoolean(name, false);
                println("Disabled memory-intensive initial analyzer: " + name);
                foundSwitchAnalyzer = true;
            }
            else if (name.equals("Stack")) {
                options.setBoolean(name, false);
                println("Disabled memory-intensive initial analyzer: " + name);
                foundStackAnalyzer = true;
            }
            else if (name.endsWith(" Constant Reference Analyzer")) {
                options.setBoolean(name, false);
                println("Disabled memory-intensive initial analyzer: " + name);
                foundX86ConstantReferences = true;
            }
        }
        if (!foundX86ConstantReferences || !foundStackAnalyzer || !foundSwitchAnalyzer) {
            throw new IllegalStateException("Expected Ghidra analyzers were not found; review the Ghidra version.");
        }
    }
}
