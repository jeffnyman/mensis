// SPDX-License-Identifier: GPL-3.0-or-later
// List the strings matching a regular expression, who references them, and (optionally)
// decompile a few of the referencing functions. A tool for exploring before writing a namer.
//
// Headless:
//   -process <program> -noanalysis -readOnly -postScript FindStringUses.java <out-file> <decompile-count> <regex> [regex ...]
//
// Several regexes are OR'd together. An argument of the form @file reads one regex per line from
// that file instead. analyzeHeadless.bat passes its arguments through cmd.exe, which treats
// | < > ^ & specially, so any pattern using those belongs in a file.
//
// For each defined string (ASCII or UTF-16) whose value matches any regex, writes the string's
// address and value, then one line per reference: the referencing address, the containing
// function, and the instruction. The first <decompile-count> distinct referencing functions
// are decompiled at the end of the file.
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class FindStringUses extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        int decompileCount = Integer.parseInt(args[1]);
        java.util.List<String> pats = new java.util.ArrayList<>();
        for (String a : java.util.Arrays.copyOfRange(args, 2, args.length)) {
            if (a.startsWith("@")) {
                for (String line : java.nio.file.Files.readAllLines(new File(a.substring(1)).toPath(), StandardCharsets.UTF_8)) {
                    if (!line.isBlank()) {
                        pats.add(line.strip());
                    }
                }
            }
            else {
                pats.add(a);
            }
        }
        Pattern pat = Pattern.compile(String.join("|", pats));
        Set<Function> funcs = new LinkedHashSet<>();
        int strings = 0, refs = 0;

        try (PrintWriter w = new PrintWriter(out, StandardCharsets.UTF_8)) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                if (!d.hasStringValue()) {
                    continue;
                }
                String v = String.valueOf(d.getValue());
                if (!pat.matcher(v).find()) {
                    continue;
                }
                strings++;
                w.printf("%s  %s  \"%s\"%n", d.getAddress(), d.getDataType().getName(), v);
                for (Reference r : getReferencesTo(d.getAddress())) {
                    refs++;
                    Function f = getFunctionContaining(r.getFromAddress());
                    Instruction ins = getInstructionAt(r.getFromAddress());
                    w.printf("    <- %s  %-28s %s%n", r.getFromAddress(),
                        f == null ? "(no function)" : f.getName(), ins == null ? r.getReferenceType() : ins);
                    if (f != null) {
                        funcs.add(f);
                    }
                }
            }
            w.printf("%n%d strings, %d references, %d functions%n", strings, refs, funcs.size());

            if (decompileCount > 0) {
                DecompInterface di = new DecompInterface();
                di.openProgram(currentProgram);
                int n = 0;
                for (Function f : funcs) {
                    if (n++ >= decompileCount) {
                        break;
                    }
                    DecompileResults res = di.decompileFunction(f, 60, monitor);
                    w.printf("%n// ===== %s @ %s =====%n%s", f.getName(), f.getEntryPoint(),
                        res.decompileCompleted() ? res.getDecompiledFunction().getC() : res.getErrorMessage());
                }
                di.dispose();
            }
        }
        println("FindStringUses: " + strings + " strings, " + refs + " references -> " + out);
    }
}
