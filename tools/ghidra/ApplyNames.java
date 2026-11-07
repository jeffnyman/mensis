// SPDX-License-Identifier: GPL-3.0-or-later
// Apply a hand-written name list (tools/names/*.tsv) to the current program.
//
// Headless:
//   -process <program> -noanalysis -postScript ApplyNames.java <names.tsv>
//
// TSV columns: address, kind (function | global), name, confidence, evidence. Lines starting
// with '#' are comments. Names may contain '::' to place them in a namespace.
//
// Names are applied as USER_DEFINED, so they replace names given by analysis or by
// NameFromStrings.java. Each item gets a comment "[mensis:hand/<confidence>] <evidence>".
//@category PS4

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {
    @Override
    protected void run() throws Exception {
        int functions = 0, globals = 0, missing = 0;
        boolean header = true;
        for (String line : Files.readAllLines(new File(getScriptArgs()[0]).toPath(), StandardCharsets.UTF_8)) {
            if (line.startsWith("#") || line.isBlank()) {
                continue;
            }
            if (header) {  // column names
                header = false;
                continue;
            }
            String[] c = line.split("\t", -1);
            Address a = toAddr(Long.parseUnsignedLong(c[0], 16));
            String kind = c[1], full = c[2], note = "[mensis:hand/" + c[3] + "] " + c[4];
            int sep = full.lastIndexOf("::");
            String name = sep < 0 ? full : full.substring(sep + 2);
            Namespace ns = sep < 0 ? currentProgram.getGlobalNamespace()
                : NamespaceUtils.createNamespaceHierarchy(full.substring(0, sep), null, currentProgram, SourceType.USER_DEFINED);
            if (kind.equals("function")) {
                Function f = getFunctionAt(a);
                if (f == null) {
                    printerr("ApplyNames: no function at " + a + " for " + full);
                    missing++;
                    continue;
                }
                f.setParentNamespace(ns);
                f.setName(name, SourceType.USER_DEFINED);
                f.setComment(note);
                functions++;
            }
            else {
                createLabel(a, name, ns, true, SourceType.USER_DEFINED);
                setEOLComment(a, note);
                globals++;
            }
        }
        println("ApplyNames: " + functions + " functions, " + globals + " globals" +
            (missing > 0 ? ", " + missing + " missing" : ""));
    }
}
