// SPDX-License-Identifier: GPL-3.0-or-later
// List what references the given addresses (code or data, defined or not), and decompile the
// referencing functions. Useful for data Ghidra hasn't typed, e.g. an alphabet string inside a
// larger blob, which FindStringUses.java can't see.
//
// Headless:
//   -process <program> -noanalysis -readOnly -postScript RefsTo.java <out-file> <decompile-count> <address> [address ...]
//
// If Ghidra's reference manager has nothing for an address, the code is also scanned for
// instructions whose operands reference it (RIP-relative LEA/MOV that analysis didn't record).
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;

public class RefsTo extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        int decompileCount = Integer.parseInt(args[1]);
        Set<Function> funcs = new LinkedHashSet<>();
        try (PrintWriter w = new PrintWriter(out, StandardCharsets.UTF_8)) {
            for (int i = 2; i < args.length; i++) {
                Address target = toAddr(Long.parseUnsignedLong(args[i].replace("0x", ""), 16));
                w.printf("%s%n", target);
                int n = 0;
                for (Reference r : getReferencesTo(target)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    w.printf("    <- %s  %s  %s%n", r.getFromAddress(), f == null ? "(no function)" : f.getName(true), r.getReferenceType());
                    if (f != null) {
                        funcs.add(f);
                    }
                    n++;
                }
                if (n == 0) {
                    // fall back to scanning operands for the address as an immediate/displacement result
                    InstructionIterator it = currentProgram.getListing().getInstructions(true);
                    while (it.hasNext() && !monitor.isCancelled()) {
                        Instruction ins = it.next();
                        for (int op = 0; op < ins.getNumOperands(); op++) {
                            for (Object o : ins.getOpObjects(op)) {
                                if (o instanceof Address a && a.equals(target)
                                    || o instanceof Scalar s && s.getUnsignedValue() == target.getOffset()) {
                                    Function f = getFunctionContaining(ins.getAddress());
                                    w.printf("    <~ %s  %s  %s%n", ins.getAddress(), f == null ? "(no function)" : f.getName(true), ins);
                                    if (f != null) {
                                        funcs.add(f);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            DecompInterface di = new DecompInterface();
            di.openProgram(currentProgram);
            int k = 0;
            for (Function f : funcs) {
                if (k++ >= decompileCount) {
                    break;
                }
                DecompileResults res = di.decompileFunction(f, 90, monitor);
                w.printf("%n// ===== %s @ %s =====%n%s", f.getName(true), f.getEntryPoint(),
                    res.decompileCompleted() ? res.getDecompiledFunction().getC() : res.getErrorMessage());
            }
            di.dispose();
        }
        println("RefsTo: " + funcs.size() + " referencing functions -> " + out);
    }
}
