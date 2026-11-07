// SPDX-License-Identifier: GPL-3.0-or-later
// Write one line per function of the current program, for comparing builds outside Ghidra.
//
// Headless (one program in memory at a time, so it is light):
//   -process "eboot-1.*.ghidra.elf" -noanalysis -readOnly -postScript ExportFunctionHashes.java <out-dir>
//
// Writes <out-dir>/<program name>.functions.tsv with columns:
//   address       entry point
//   size          bytes in the function body
//   instructions  instruction count
//   full_hash     FID full hash: the instructions with every address-dependent operand masked,
//                 so the same code at a different address hashes the same
//   specific_hash FID specific hash: also covers constant operands, catching changed numbers
//                 (a damage multiplier, a buffer size) that leave the full hash alone
//   calls         number of distinct functions called
//   strings       up to four strings the function references, " | "-separated
// Thunks are skipped; they are identical by construction.
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.hash.FidHashQuad;
import ghidra.feature.fid.service.FidService;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;

public class ExportFunctionHashes extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : askDirectory("Output", "Write").getPath());
        outDir.mkdirs();
        File out = new File(outDir, currentProgram.getName().replace(".ghidra.elf", "") + ".functions.tsv");
        FidService fid = new FidService();
        Listing listing = currentProgram.getListing();
        int n = 0;

        try (PrintWriter w = new PrintWriter(out, StandardCharsets.UTF_8)) {
            w.println("address\tsize\tinstructions\tfull_hash\tspecific_hash\tcalls\tstrings");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isThunk() || f.isExternal()) {
                    continue;
                }
                monitor.checkCancelled();
                String full = "", specific = "";
                try {
                    FidHashQuad q = fid.hashFunction(f);
                    if (q != null) {
                        full = String.format("%016x", q.getFullHash());
                        specific = String.format("%016x", q.getSpecificHash());
                    }
                }
                catch (Exception e) {
                    // too small or unhashable: leave blank
                }
                int count = 0;
                Set<String> strs = new LinkedHashSet<>();
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    count++;
                    if (strs.size() >= 4) {
                        continue;
                    }
                    for (Reference r : ins.getReferencesFrom()) {
                        Data d = listing.getDataAt(r.getToAddress());
                        if (d != null && d.hasStringValue()) {
                            String v = String.valueOf(d.getValue()).replaceAll("\\s", " ");
                            strs.add(v.length() > 70 ? v.substring(0, 70) + "..." : v);
                        }
                    }
                }
                int calls = f.getCalledFunctions(monitor).size();
                w.printf("%s\t%d\t%d\t%s\t%s\t%d\t%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
                    count, full, specific, calls, String.join(" | ", strs));
                if (++n % 20000 == 0) {
                    monitor.setMessage("ExportFunctionHashes: " + n);
                }
            }
        }
        println("ExportFunctionHashes: " + n + " functions -> " + out);
    }
}
