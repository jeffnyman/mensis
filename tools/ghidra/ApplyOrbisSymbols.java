// SPDX-License-Identifier: GPL-3.0-or-later
// Apply the symbol list written by tools/orbis_prep.py to a PS4 executable in Ghidra.
//
// Run it before auto-analysis (headless: -preScript ApplyOrbisSymbols.java <symbols.tsv>),
// or from the Script Manager in the GUI, where it asks for the file.
//
// Each line of the TSV is: address, kind, name, library.
//   IMPORT       a 1-byte RET stub standing in for a system-library function -> function
//   IMPORT_DATA  a stub standing in for a system-library object            -> label
//   PLT          the game's "jmp [rip+GOT]" trampoline -> function, made a thunk of the IMPORT
//   GOT          the GOT slot the trampoline reads     -> label + pointer
//   ENTRY        the entry point                       -> function + entry point
//   PROCPARAM    the SCE process-parameter block       -> label
//   FUNC         a function start from .eh_frame_hdr   -> disassembled, unnamed function
//
// Library names go into a namespace per library (libkernel::sceKernelOpen), so the
// decompiler output shows where each call goes.
//@category PS4

import java.io.BufferedReader;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class ApplyOrbisSymbols extends GhidraScript {

    private final Map<String, Namespace> namespaces = new HashMap<>();

    private Namespace ns(String lib) throws Exception {
        if (lib == null || lib.isEmpty()) {
            return currentProgram.getGlobalNamespace();
        }
        Namespace n = namespaces.get(lib);
        if (n == null) {
            SymbolTable st = currentProgram.getSymbolTable();
            n = st.getNamespace(lib, currentProgram.getGlobalNamespace());
            if (n == null) {
                n = st.createNameSpace(currentProgram.getGlobalNamespace(), lib, SourceType.IMPORTED);
            }
            namespaces.put(lib, n);
        }
        return n;
    }

    private Function function(Address a, String name, Namespace n) throws Exception {
        Function f = getFunctionAt(a);
        if (f == null) {
            disassemble(a);
            f = createFunction(a, name);
        }
        if (f == null) {
            printerr("could not create function at " + a + " for " + name);
            return null;
        }
        f.setName(name, SourceType.IMPORTED);
        f.setParentNamespace(n);
        return f;
    }

    @Override
    protected void run() throws Exception {
        File tsv;
        String[] args = getScriptArgs();
        if (args.length > 0) {
            tsv = new File(args[0]);
        } else {
            tsv = askFile("Symbols TSV from orbis_prep.py", "Apply");
        }

        Map<String, Function> imports = new HashMap<>();
        int nImport = 0, nData = 0, nPlt = 0, nGot = 0, nFunc = 0;

        // Two passes: imports first, so PLT trampolines can be made thunks of them.
        for (int pass = 0; pass < 2; pass++) {
            try (BufferedReader r = Files.newBufferedReader(tsv.toPath(), StandardCharsets.UTF_8)) {
                String line;
                while ((line = r.readLine()) != null) {
                    if (line.startsWith("#") || line.isBlank()) {
                        continue;
                    }
                    String[] c = line.split("\t", -1);
                    Address a = toAddr(Long.decode(c[0]));
                    String kind = c[1], name = c[2], lib = c.length > 3 ? c[3] : "";
                    boolean importsPass = kind.startsWith("IMPORT");
                    if ((pass == 0) != importsPass) {
                        continue;
                    }
                    switch (kind) {
                        case "IMPORT" -> {
                            Function f = function(a, name, ns(lib));
                            if (f != null) {
                                imports.put(lib + "::" + name, f);
                                nImport++;
                            }
                        }
                        case "IMPORT_DATA" -> {
                            createLabel(a, name, ns(lib), true, SourceType.IMPORTED);
                            nData++;
                        }
                        case "PLT" -> {
                            Function f = function(a, name, ns(lib));
                            Function target = imports.get(lib + "::" + name);
                            if (f != null && target != null) {
                                f.setThunkedFunction(target);
                            }
                            nPlt++;
                        }
                        case "GOT" -> {
                            createLabel(a, name, ns(lib), true, SourceType.IMPORTED);
                            clearListing(a, a.add(7));
                            createData(a, PointerDataType.dataType);
                            nGot++;
                        }
                        case "ENTRY" -> {
                            function(a, name, currentProgram.getGlobalNamespace());
                            addEntryPoint(a);
                        }
                        case "PROCPARAM" -> createLabel(a, name, true, SourceType.IMPORTED);
                        case "FUNC" -> {
                            if (getFunctionAt(a) == null) {
                                disassemble(a);
                                if (createFunction(a, null) != null) {
                                    nFunc++;
                                }
                            }
                            if (nFunc % 10000 == 0) {
                                monitor.setMessage("ApplyOrbisSymbols: " + nFunc + " functions");
                            }
                        }
                        default -> printerr("unknown kind " + kind + " at " + a);
                    }
                }
            }
        }
        println(String.format(
            "ApplyOrbisSymbols: %d imports, %d data imports, %d PLT thunks, %d GOT slots, %d functions from .eh_frame_hdr",
            nImport, nData, nPlt, nGot, nFunc));
    }
}
