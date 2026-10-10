// Bounded initial analysis for the verified Steam executable.
// @category ApexRadiance
import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import ghidra.program.model.listing.Program;

public class ApexAnalysisSetup extends GhidraScript {
    @Override
    public void run() throws Exception {
        Options options = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
        int disabled = 0;
        for (String name : options.getOptionNames()) {
            if (name.endsWith(" Constant Reference Analyzer")) {
                options.setBoolean(name, false);
                println("Disabled memory-intensive initial analyzer: " + name);
                disabled++;
            }
        }
        if (disabled == 0) {
            throw new IllegalStateException("Constant reference analyzer option was not found; review the Ghidra version.");
        }
    }
}
