// SPDX-License-Identifier: GPL-3.0-or-later
// Summarise an analysed PS4 executable and decompile a few landmark functions.
//
// Headless: -process <program> -noanalysis -postScript OrbisReport.java <out-dir>
//
// Writes <out-dir>/report.txt (function counts, most-used system imports) and one .c file per
// landmark function: the entry point, every function that calls a few chosen system imports,
// and every function that references a few chosen UTF-16 path strings.
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class OrbisReport extends GhidraScript {

    // System imports whose callers make good landmarks.
    private static final String[] IMPORT_LANDMARKS = {
        "sceGnmSubmitAndFlipCommandBuffers", "scePadReadState", "scePadRead", "sceAvPlayerInit",
        "sceSaveDataMount2", "sceSaveDataMount", "sceNpTrophyUnlockTrophy",
    };
    // UTF-16 strings whose users make good landmarks.
    private static final String[] STRING_LANDMARKS = {
        "movie:/%s.mp4", "param:/DrawParam/default_DrawParam.parambnd",
        "parampatch:/GameParam/GameParamPatch.parambnd", "dbgai:/ai/out/%06d_battle.lua",
    };

    private DecompInterface decomp;
    private File outDir;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        outDir = new File(args.length > 0 ? args[0] : askDirectory("Output", "Write").getPath());
        outDir.mkdirs();

        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        try (PrintWriter rep = new PrintWriter(new File(outDir, "report.txt"), StandardCharsets.UTF_8)) {
            int total = 0, thunks = 0, named = 0;
            Map<String, Integer> callers = new TreeMap<>();
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                total++;
                if (f.isThunk()) {
                    thunks++;
                    Function t = f.getThunkedFunction(true);
                    if (!t.getParentNamespace().isGlobal()) {  // system imports live in lib namespaces
                        Set<Function> c = f.getCallingFunctions(monitor);
                        callers.merge(t.getParentNamespace().getName() + "::" + t.getName(), c.size(), Integer::sum);
                    }
                } else if (f.getSymbol().getSource() != SourceType.DEFAULT) {
                    named++;
                }
            }
            rep.printf("program        %s%n", currentProgram.getName());
            rep.printf("image base     %s%n", currentProgram.getImageBase());
            rep.printf("functions      %d (%d thunks, %d other named, %d unnamed FUN_*)%n",
                total, thunks, named, total - thunks - named);
            rep.printf("%nmost-called system imports (number of distinct calling functions):%n");
            List<Map.Entry<String, Integer>> top = new ArrayList<>(callers.entrySet());
            top.sort(Comparator.comparing((Map.Entry<String, Integer> e) -> e.getValue()).reversed());
            for (Map.Entry<String, Integer> e : top.subList(0, Math.min(40, top.size()))) {
                rep.printf("  %6d  %s%n", e.getValue(), e.getKey());
            }

            rep.printf("%nlandmark functions:%n");
            Set<Function> done = new LinkedHashSet<>();
            Function entry = getFunctionAt(currentProgram.getSymbolTable().getSymbols("_start").next().getAddress());
            emit(rep, entry, "entry point", done);
            for (String imp : IMPORT_LANDMARKS) {
                for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                    if (f.isThunk() && f.getName().equals(imp)) {
                        for (Function c : f.getCallingFunctions(monitor)) {
                            emit(rep, c, "calls " + imp, done);
                        }
                        break;
                    }
                }
            }
            for (String s : STRING_LANDMARKS) {
                byte[] needle = (s + "\0").getBytes(StandardCharsets.UTF_16LE);
                Address a = currentProgram.getMemory().findBytes(currentProgram.getMinAddress(), needle, null, true, monitor);
                if (a == null) {
                    rep.printf("  (string not found: %s)%n", s);
                    continue;
                }
                for (Reference r : getReferencesTo(a)) {
                    Function c = getFunctionContaining(r.getFromAddress());
                    if (c != null) {
                        emit(rep, c, "uses \"" + s + "\" @" + a, done);
                    }
                }
            }
        }
        decomp.dispose();
        println("OrbisReport: wrote " + outDir);
    }

    private void emit(PrintWriter rep, Function f, String why, Set<Function> done) throws Exception {
        if (f == null || !done.add(f) || done.size() > 40) {
            return;
        }
        DecompileResults res = decomp.decompileFunction(f, 120, monitor);
        String name = f.getName() + "_" + f.getEntryPoint();
        rep.printf("  %-40s %-8s %5d bytes  %s%n", f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses(), why);
        try (PrintWriter w = new PrintWriter(new File(outDir, name + ".c"), StandardCharsets.UTF_8)) {
            w.printf("// %s @ %s  (%s)%n", f.getName(), f.getEntryPoint(), why);
            w.print(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed: " + res.getErrorMessage());
        }
    }
}
