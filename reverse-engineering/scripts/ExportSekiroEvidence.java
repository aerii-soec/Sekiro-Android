// Export metadata for the analyzed program and bounded evidence for selected functions.
//@category Sekiro

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;

public class ExportSekiroEvidence extends GhidraScript {
    private static void row(BufferedWriter out, Object... values) throws IOException {
        for (int i = 0; i < values.length; i++) {
            if (i > 0) {
                out.write('\t');
            }
            out.write(String.valueOf(values[i]).replace('\t', ' ').replace('\n', ' ').replace('\r', ' '));
        }
        out.newLine();
    }

    @Override
    public void run() throws Exception {
        var args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("Pass the report output directory and optional function addresses");
        }

        var output = Files.createDirectories(Path.of(args[0]));
        var functions = currentProgram.getFunctionManager();
        var references = currentProgram.getReferenceManager();
        var symbols = currentProgram.getSymbolTable();
        var roots = new LinkedHashSet<Function>();
        var focus = functions.getFunctionAt(toAddr("14001054d"));
        if (focus != null) {
            roots.add(focus);
        }

        var dataAddresses = new LinkedHashSet<Address>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("data:")) {
                dataAddresses.add(toAddr(args[i].substring(5)));
                continue;
            }
            var function = functions.getFunctionAt(toAddr(args[i]));
            if (function == null) {
                printf("No function defined at %s%n", args[i]);
            } else {
                roots.add(function);
            }
        }

        try (var metadata = Files.newBufferedWriter(output.resolve("program.tsv"));
             var inventory = Files.newBufferedWriter(output.resolve("functions.tsv"));
             var imports = Files.newBufferedWriter(output.resolve("imports.tsv"));
             var edges = Files.newBufferedWriter(output.resolve("startup-edges.tsv"))) {
            row(metadata, "field", "value");
            row(metadata, "name", currentProgram.getName());
            row(metadata, "format", currentProgram.getExecutableFormat());
            row(metadata, "sha256", currentProgram.getExecutableSHA256());
            row(metadata, "language", currentProgram.getLanguageID());
            row(metadata, "image_base", currentProgram.getImageBase());
            var entryPoints = symbols.getExternalEntryPointIterator();
            while (entryPoints.hasNext()) {
                var address = entryPoints.next();
                row(metadata, "external_entry_point", address);
                var symbol = symbols.getPrimarySymbol(address);
                if (symbol != null && symbol.getName().equals("entry")) {
                    var function = functions.getFunctionAt(address);
                    if (function != null) {
                        roots.add(function);
                    }
                }
            }

            row(inventory, "address", "name", "body_bytes", "thunk");
            var allFunctions = functions.getFunctions(true);
            while (allFunctions.hasNext()) {
                monitor.checkCancelled();
                var function = allFunctions.next();
                row(inventory, function.getEntryPoint(), function.getName(),
                    function.getBody().getNumAddresses(), function.isThunk());
            }

            row(imports, "library", "symbol", "caller", "reference_site", "reference_type");
            var externalSymbols = symbols.getExternalSymbols();
            while (externalSymbols.hasNext()) {
                monitor.checkCancelled();
                Symbol symbol = externalSymbols.next();
                var sites = new LinkedHashSet<Address>();
                sites.add(symbol.getAddress());
                var function = functions.getFunctionAt(symbol.getAddress());
                if (function != null) {
                    var thunks = function.getFunctionThunkAddresses(true);
                    if (thunks != null) {
                        for (Address thunk : thunks) {
                            sites.add(thunk);
                        }
                    }
                }
                boolean found = false;
                for (Address site : sites) {
                    ReferenceIterator incoming = references.getReferencesTo(site);
                    while (incoming.hasNext()) {
                        Reference reference = incoming.next();
                        var caller = functions.getFunctionContaining(reference.getFromAddress());
                        row(imports, symbol.getParentNamespace().getName(), symbol.getName(),
                            caller == null ? "" : caller.getEntryPoint(),
                            reference.getFromAddress(), reference.getReferenceType());
                        found = true;
                    }
                }
                if (!found) {
                    row(imports, symbol.getParentNamespace().getName(), symbol.getName(), "", "", "");
                }
            }

            row(edges, "caller", "call_site", "callee", "callee_name");
            var neighbors = new LinkedHashSet<Function>();
            for (Function root : roots) {
                var insns = currentProgram.getListing().getInstructions(root.getBody(), true);
                while (insns.hasNext()) {
                    monitor.checkCancelled();
                    Instruction instruction = insns.next();
                    for (Reference ref : instruction.getReferencesFrom()) {
                        if (!ref.getReferenceType().isCall()) {
                            continue;
                        }
                        var callee = functions.getFunctionAt(ref.getToAddress());
                        row(edges, root.getEntryPoint(), instruction.getAddress(), ref.getToAddress(),
                            callee == null ? "" : callee.getName());
                        if (focus != null && root.equals(focus) && callee != null && !callee.isExternal()) {
                            neighbors.add(callee);
                        }
                    }
                }
            }
            if (focus != null) {
                try (var incoming = Files.newBufferedWriter(output.resolve("focus-xrefs.tsv"))) {
                    row(incoming, "from_address", "caller", "type");
                    var xrefs = references.getReferencesTo(focus.getEntryPoint());
                    while (xrefs.hasNext()) {
                        monitor.checkCancelled();
                        var reference = xrefs.next();
                        var caller = functions.getFunctionContaining(reference.getFromAddress());
                        row(incoming, reference.getFromAddress(),
                            caller == null ? "" : caller.getEntryPoint(), reference.getReferenceType());
                    }
                }
            }

            try (var callers = Files.newBufferedWriter(output.resolve("root-xrefs.tsv"))) {
                row(callers, "root", "from_address", "caller", "type");
                for (Function root : roots) {
                    var xrefs = references.getReferencesTo(root.getEntryPoint());
                    while (xrefs.hasNext()) {
                        monitor.checkCancelled();
                        var reference = xrefs.next();
                        var caller = functions.getFunctionContaining(reference.getFromAddress());
                        row(callers, root.getEntryPoint(), reference.getFromAddress(),
                            caller == null ? "" : caller.getEntryPoint(), reference.getReferenceType());
                    }
                }
            }
            try (var dataXrefs = Files.newBufferedWriter(output.resolve("data-xrefs.tsv"))) {
                row(dataXrefs, "address", "from_address", "caller", "reference_type");
                for (Address address : dataAddresses) {
                    var incoming = references.getReferencesTo(address);
                    while (incoming.hasNext()) {
                        monitor.checkCancelled();
                        var reference = incoming.next();
                        var caller = functions.getFunctionContaining(reference.getFromAddress());
                        row(dataXrefs, address, reference.getFromAddress(),
                            caller == null ? "" : caller.getEntryPoint(), reference.getReferenceType());
                    }
                }
            }
            // Bound the amount of decompilation, including when a helper is unusually large.
            var selected = new ArrayList<>(roots);
            selected.addAll(neighbors.stream().filter(f -> !roots.contains(f)).limit(8).toList());
            exportDecompilation(output, selected);
        }
        Files.writeString(output.resolve("complete.txt"), "Export finished." + System.lineSeparator());
    }

    private void exportDecompilation(Path output, Iterable<Function> functions) throws Exception {
        var decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) {
                throw new IOException(decompiler.getLastMessage());
            }
            for (var function : functions) {
                monitor.checkCancelled();
                var address = function.getEntryPoint().toString();
                var result = decompiler.decompileFunction(function, 20, monitor);
                var header = "/* Ghidra decompiler output for " + address + ". */\n";
                if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    Files.writeString(output.resolve(address + ".pseudocode.txt"),
                        header + result.getDecompiledFunction().getC());
                } else {
                    Files.writeString(output.resolve(address + ".pseudocode.txt"),
                        header + result.getErrorMessage());
                }
                try (var assembly = Files.newBufferedWriter(output.resolve(address + ".mnemonics.tsv"))) {
                    row(assembly, "address", "mnemonic");
                    var insns = currentProgram.getListing().getInstructions(function.getBody(), true);
                    while (insns.hasNext()) {
                        monitor.checkCancelled();
                        var insn = insns.next();
                        row(assembly, insn.getAddress(), insn.getMnemonicString().toUpperCase(Locale.ROOT));
                    }
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
