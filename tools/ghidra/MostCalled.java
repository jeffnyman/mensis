// SPDX-License-Identifier: GPL-3.0-or-later
// List the functions called from the most places, and decompile the top ones for reading.
// Naming these by hand improves the decompilation of every function that calls them.
//
// Headless:
//   -process <program> -noanalysis -readOnly -postScript MostCalled.java <out-dir> [count] [all]
//
// Writes <out-dir>/most-called.tsv (address, callers, size, name, the first strings it
// references) for the top [count] (default 60) functions, and one .c file per function.
// Thunks are followed to what they call, so a helper reached through jump stubs is counted once.
// By default only unnamed (FUN_*) functions are listed; add "all" to include named ones.
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class MostCalled extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 60;
        boolean all = args.length > 2 && args[2].equalsIgnoreCase("all");
        outDir.mkdirs();

        // callers per real (non-thunk) function, counting each calling function once
        Map<Function, Set<Function>> callers = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            monitor.checkCancelled();
            if (f.isThunk()) {
                continue;
            }
            for (Function callee : f.getCalledFunctions(monitor)) {
                Function real = callee.isThunk() ? callee.getThunkedFunction(true) : callee;
                if (real != null && !real.isExternal()) {
                    callers.computeIfAbsent(real, k -> new HashSet<>()).add(f);
                }
            }
        }
        List<Map.Entry<Function, Set<Function>>> ranked = new ArrayList<>(callers.entrySet());
        ranked.removeIf(e -> !all && (e.getKey().getSymbol().getSource() != SourceType.DEFAULT ||
            !e.getKey().getParentNamespace().isGlobal()));
        ranked.sort((a, b) -> b.getValue().size() - a.getValue().size());

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new File(outDir, "most-called.tsv"), StandardCharsets.UTF_8)) {
            w.println("rank\taddress\tcallers\tsize\tname\tstrings");
            for (int i = 0; i < Math.min(count, ranked.size()); i++) {
                Function f = ranked.get(i).getKey();
                w.printf("%d\t%s\t%d\t%d\t%s\t%s%n", i + 1, f.getEntryPoint(), ranked.get(i).getValue().size(),
                    f.getBody().getNumAddresses(), f.getName(true), strings(f));
                DecompileResults res = di.decompileFunction(f, 60, monitor);
                try (PrintWriter c = new PrintWriter(new File(outDir, String.format("%02d_%s.c", i + 1, f.getEntryPoint())),
                    StandardCharsets.UTF_8)) {
                    c.printf("// #%d  %s @ %s  called from %d functions%n", i + 1, f.getName(true), f.getEntryPoint(),
                        ranked.get(i).getValue().size());
                    c.print(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// " + res.getErrorMessage());
                }
            }
        }
        di.dispose();
        println("MostCalled: wrote " + Math.min(count, ranked.size()) + " functions to " + outDir);
    }

    private String strings(Function f) {
        Set<String> found = new LinkedHashSet<>();
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            for (Reference r : ins.getReferencesFrom()) {
                Data d = currentProgram.getListing().getDataAt(r.getToAddress());
                if (d != null && d.hasStringValue() && found.size() < 3) {
                    String v = String.valueOf(d.getValue()).replaceAll("\\s", " ");
                    found.add(v.length() > 60 ? v.substring(0, 60) + "..." : v);
                }
            }
        }
        return String.join(" | ", found);
    }
}
