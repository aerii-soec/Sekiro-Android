// Inspect selected existing Ghidra references and decompile a bounded set of targets.
//@category Sekiro
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;

public class ExportVerticalSliceEvidence extends GhidraScript {
    @Override
    public void run() throws Exception {
        var args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("Specify an output directory and analysis addresses");
        }
        var output = Files.createDirectories(Path.of(args[0]));
        var listing = currentProgram.getListing();
        var functions = currentProgram.getFunctionManager();
        var references = currentProgram.getReferenceManager();
        var targets = new LinkedHashSet<Function>();
        try (var report = Files.newBufferedWriter(output.resolve("references.tsv"))) {
            report.write("seed\taddress\tfrom\tfunction\tref_type\n");
            for (int i = 1; i < args.length; i++) {
                monitor.checkCancelled();
                var specification = args[i].split(":", 2);
                var address = toAddr(specification[1]);
                if (specification[0].equals("function")) {
                    var function = functions.getFunctionAt(address);
                    if (function != null) {
                        targets.add(function);
                    }
                    continue;
                }
                var addresses = new LinkedHashSet<Address>();
                addresses.add(address);
                var datum = listing.getDataContaining(address);
                if (datum != null) {
                    addresses.add(datum.getMinAddress());
                }
                for (Address candidate : addresses) {
                    var incoming = references.getReferencesTo(candidate);
                    int seen = 0;
                    while (incoming.hasNext() && seen++ < 24) {
                        var ref = incoming.next();
                        var owner = functions.getFunctionContaining(ref.getFromAddress());
                        var caller = owner == null ? "" : owner.getEntryPoint().toString();
                        report.write(specification[0] + "\t" + candidate + "\t" + ref.getFromAddress()
                            + "\t" + caller + "\t" + ref.getReferenceType() + "\n");
                        if (owner != null && targets.size() < 16) {
                            targets.add(owner);
                        }
                    }
                }
            }
        }
        var decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) {
                throw new IllegalStateException(decompiler.getLastMessage());
            }
            for (var function : targets) {
                monitor.checkCancelled();
                var result = decompiler.decompileFunction(function, 12, monitor);
                var decompiled = result.getDecompiledFunction();
                if (result.decompileCompleted() && decompiled != null) {
                    Files.writeString(output.resolve(function.getEntryPoint() + ".pseudocode.txt"),
                        "/* Ghidra output */\n" + decompiled.getC());
                }
            }
        } finally {
            decompiler.dispose();
        }
        Files.writeString(output.resolve("complete.txt"), currentProgram.getExecutableSHA256() + "\n");
    }
}
