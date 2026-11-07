// SPDX-License-Identifier: GPL-3.0-or-later
// Decompile the functions at (or containing) the given addresses.
//
// Headless:
//   -process <program> -noanalysis -readOnly -postScript Decompile.java <out> <address> [address ...]
//
// <out> is a file, or, if it ends in '/' or '\', a directory that gets one <address>.c per
// function. An address may be followed by ":callers" to also decompile up to three of its
// callers (file mode only). An argument "@list.txt" reads addresses from a file, one per line
// (first whitespace-separated field; '#' lines skipped).
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
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
        boolean dirMode = args[0].endsWith("/") || args[0].endsWith("\\");
        File out = new File(args[0]);
        if (dirMode) {
            out.mkdirs();
        }
        List<String> targets = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                for (String line : Files.readAllLines(new File(args[i].substring(1)).toPath(), StandardCharsets.UTF_8)) {
                    String t = line.strip();
                    if (!t.isEmpty() && !t.startsWith("#")) {
                        targets.add(t.split("\\s+")[0]);
                    }
                }
            }
            else {
                targets.add(args[i]);
            }
        }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter single = dirMode ? null : new PrintWriter(out, StandardCharsets.UTF_8);
        int n = 0;
        try {
            for (String t : targets) {
                String[] parts = t.split(":");
                Function f = getFunctionContaining(toAddr(Long.parseUnsignedLong(parts[0].replace("0x", ""), 16)));
                if (f == null) {
                    printerr("Decompile: no function at " + parts[0]);
                    continue;
                }
                List<Function> todo = new ArrayList<>(List.of(f));
                if (!dirMode && parts.length > 1 && parts[1].equals("callers")) {
                    todo.addAll(f.getCallingFunctions(monitor).stream().limit(3).toList());
                }
                for (Function g : todo) {
                    DecompileResults res = di.decompileFunction(g, 90, monitor);
                    String text = String.format("// ===== %s @ %s%s =====%n%s", g.getName(true), g.getEntryPoint(),
                        g == f ? "" : "  (calls " + f.getName() + ")",
                        res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// " + res.getErrorMessage());
                    if (dirMode) {
                        try (PrintWriter w = new PrintWriter(new File(out, g.getEntryPoint() + ".c"), StandardCharsets.UTF_8)) {
                            w.print(text);
                        }
                    }
                    else {
                        single.printf("%n%s", text);
                    }
                    n++;
                }
            }
        }
        finally {
            if (single != null) {
                single.close();
            }
            di.dispose();
        }
        println("Decompile: " + n + " functions -> " + out);
    }
}
