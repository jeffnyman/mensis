// SPDX-License-Identifier: GPL-3.0-or-later
// Decompile the functions at (or containing) the given addresses into one file.
//
// Headless:
//   -process <program> -noanalysis -readOnly -postScript Decompile.java <out-file> <address> [address ...]
//
// An address may be followed by ":callers" to also decompile up to three of its callers.
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8)) {
            for (int i = 1; i < args.length; i++) {
                String[] parts = args[i].split(":");
                Function f = getFunctionContaining(toAddr(Long.parseUnsignedLong(parts[0].replace("0x", ""), 16)));
                if (f == null) {
                    w.printf("// no function at %s%n", parts[0]);
                    continue;
                }
                List<Function> todo = new ArrayList<>(List.of(f));
                if (parts.length > 1 && parts[1].equals("callers")) {
                    todo.addAll(f.getCallingFunctions(monitor).stream().limit(3).toList());
                }
                for (Function g : todo) {
                    DecompileResults res = di.decompileFunction(g, 60, monitor);
                    w.printf("%n// ===== %s @ %s%s =====%n%s", g.getName(true), g.getEntryPoint(),
                        g == f ? "" : "  (calls " + f.getName() + ")",
                        res.decompileCompleted() ? res.getDecompiledFunction().getC() : res.getErrorMessage());
                }
            }
        }
        di.dispose();
        println("Decompile: wrote " + args[0]);
    }
}
