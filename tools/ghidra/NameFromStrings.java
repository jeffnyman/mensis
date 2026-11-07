// SPDX-License-Identifier: GPL-3.0-or-later
// Name functions and globals in an FD4-engine (FromSoftware) PS4 executable from the strings the
// engine and its middleware leave next to them.
//
// Headless:
//   -process <program> -noanalysis -postScript NameFromStrings.java <out.tsv> [dry]
// With "dry" nothing is changed; <out.tsv> still lists what would be named. Without it, add
// -readOnly only if you don't want the result saved.
//
// Rules, most certain first:
//
//   step      FD4 state machines register their steps in tables of 24-byte records
//             { code*, 0, L"Class::STEP_Name" }, filled in by static initialisers:
//                 _DAT_05957950 = FUN_0198c2b0;  _DAT_05957958 = 0;
//                 _DAT_05957960 = L"MapRes::STEP_Init";
//             The function stored 0x10 bytes before the name gets that name, in a namespace
//             for the class. These names are the developers' own.
//
//   havok     Havok's profiler macros (HK_TIMER_BEGIN, HK_TIMER_BEGIN_LIST) leave markers such as
//             "TthkbWorld::step" or "LthkpWorld::getClosestPoints" inside the function they time.
//             A function that references exactly one distinct marker is named after it (minus the
//             Tt/Lt prefix). Several markers usually mean link-time inlining, so those are left.
//             Scaleform's renderer markers ("HAL::BeginFrame") are treated the same way.
//
//   singleton FD4's singleton accessor asserts when the instance is missing (FD4Singleton.h and
//             FD4DerivedSingleton.h):
//                 if (DAT_0593b118 == 0) FUN_024b55b0("...FD4DerivedSingleton.h", 0xaa, ..., "SprjFile");
//             The compiler moves the assert out of line, so the null check is found by following
//             the branches into the assert's basic block; the global read there is the instance
//             pointer of the named class and becomes g_<Class> (given a clear majority of votes,
//             and only one global per class). The function called is the assert handler and
//             becomes FD4_SingletonAssert (a descriptive name, not one found in the binary).
//
// Names are applied with SourceType.ANALYSIS, so any name a person sets later wins, and every
// named function or global gets a plate/EOL comment saying which rule named it and why.
// Functions that already have a non-default name are left alone.
//@category PS4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.block.BasicBlockModel;
import ghidra.program.model.block.CodeBlock;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class NameFromStrings extends GhidraScript {

    private static final Pattern STEP = Pattern.compile("^([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)::(_?STEP_[A-Za-z0-9_]+)$");
    private static final Pattern HAVOK = Pattern.compile("^(?:Tt|Lt)([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_~][A-Za-z0-9_]*)$");
    private static final Pattern SCALEFORM = Pattern.compile("^(HAL)::([A-Za-z_][A-Za-z0-9_]*)$");
    private static final Pattern IDENT = Pattern.compile("^[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*$");

    private PrintWriter log;
    private BasicBlockModel blockModel;
    private boolean dry;
    private final Map<String, Integer> counts = new LinkedHashMap<>();

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        dry = args.length > 1 && args[1].equalsIgnoreCase("dry");
        try (PrintWriter w = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8)) {
            log = w;
            log.println("rule\taddress\tname\tevidence\tapplied");
            blockModel = new BasicBlockModel(currentProgram);
            Map<Address, String> strings = collectStrings();
            stepRule(strings);
            markerRule(strings);
            singletonRule(strings);
        }
        println("NameFromStrings" + (dry ? " (dry run)" : "") + ": " + counts);
    }

    /** All defined strings, by address. */
    private Map<Address, String> collectStrings() {
        Map<Address, String> m = new HashMap<>();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (d.hasStringValue()) {
                m.put(d.getAddress(), String.valueOf(d.getValue()));
            }
        }
        return m;
    }

    // ---- rule: step tables --------------------------------------------------------------------

    private void stepRule(Map<Address, String> strings) throws Exception {
        // Functions that reference any step name are the table initialisers.
        Set<Function> inits = new HashSet<>();
        for (Map.Entry<Address, String> e : strings.entrySet()) {
            if (STEP.matcher(e.getValue()).matches()) {
                for (Reference r : getReferencesTo(e.getKey())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) {
                        inits.add(f);
                    }
                }
            }
        }
        for (Function init : inits) {
            Map<Address, Address> stores = absoluteStores(init);
            for (Map.Entry<Address, Address> s : stores.entrySet()) {
                String v = strings.get(s.getValue());
                if (v == null) {
                    continue;
                }
                Matcher m = STEP.matcher(v);
                if (!m.matches()) {
                    continue;
                }
                Address code = stores.get(s.getKey().subtract(0x10));
                Function target = code == null ? null : getFunctionAt(code);
                if (target == null) {
                    log.printf("step\t%s\t%s\tno function stored at %s-0x10\t0%n", s.getKey(), v, s.getKey());
                    continue;
                }
                nameFunction("step", target, m.group(1), m.group(2),
                    "step table entry " + s.getKey() + " written by " + init.getName());
            }
        }
    }

    /**
     * For one function, map each absolute address written with "MOV [abs], reg" to the address
     * that reg was last loaded with by "LEA reg, [addr]". This is how static initialisers fill
     * tables of pointers.
     */
    private Map<Address, Address> absoluteStores(Function f) {
        Map<Register, Address> lea = new HashMap<>();
        Map<Address, Address> stores = new HashMap<>();
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            String mn = ins.getMnemonicString();
            if (mn.equals("LEA")) {
                Object[] dst = ins.getOpObjects(0);
                Reference[] refs = ins.getReferencesFrom();
                if (dst.length == 1 && dst[0] instanceof Register reg && refs.length > 0) {
                    lea.put(reg, refs[0].getToAddress());
                }
                continue;
            }
            if (mn.equals("MOV")) {
                Object[] src = ins.getOpObjects(1);
                if (src.length == 1 && src[0] instanceof Register reg && lea.containsKey(reg)) {
                    for (Reference r : ins.getReferencesFrom()) {
                        if (r.getReferenceType().isWrite()) {
                            stores.put(r.getToAddress(), lea.get(reg));
                        }
                    }
                }
                Object[] dst = ins.getOpObjects(0);
                if (dst.length == 1 && dst[0] instanceof Register reg) {
                    lea.remove(reg);  // overwritten by something else
                }
            }
        }
        return stores;
    }

    // ---- rule: profiler markers ---------------------------------------------------------------

    private void markerRule(Map<Address, String> strings) throws Exception {
        Map<Function, Set<String>> perFunction = new HashMap<>();
        Map<Function, Address> evidence = new HashMap<>();
        for (Map.Entry<Address, String> e : strings.entrySet()) {
            String v = e.getValue();
            if (!HAVOK.matcher(v).matches() && !SCALEFORM.matcher(v).matches()) {
                continue;
            }
            for (Reference r : getReferencesTo(e.getKey())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) {
                    perFunction.computeIfAbsent(f, k -> new HashSet<>()).add(v);
                    evidence.put(f, e.getKey());
                }
            }
        }
        for (Map.Entry<Function, Set<String>> e : perFunction.entrySet()) {
            Function f = e.getKey();
            if (e.getValue().size() != 1) {
                log.printf("marker\t%s\t\t%d markers: %s\t0%n", f.getEntryPoint(), e.getValue().size(),
                    String.join(" / ", e.getValue()));
                continue;
            }
            String v = e.getValue().iterator().next();
            Matcher m = HAVOK.matcher(v);
            String rule = "havok";
            if (!m.matches()) {
                m = SCALEFORM.matcher(v);
                m.matches();
                rule = "scaleform";
            }
            nameFunction(rule, f, m.group(1), m.group(2), "profiler marker \"" + v + "\" at " + evidence.get(f));
        }
    }

    // ---- rule: singleton asserts --------------------------------------------------------------

    private void singletonRule(Map<Address, String> strings) throws Exception {
        Map<Address, Map<String, Integer>> votes = new HashMap<>();
        Map<Address, Integer> handlers = new HashMap<>();
        for (Map.Entry<Address, String> e : strings.entrySet()) {
            if (!e.getValue().endsWith("FD4DerivedSingleton.h") && !e.getValue().endsWith("FD4Singleton.h")) {
                continue;
            }
            for (Reference r : getReferencesTo(e.getKey())) {
                Instruction at = getInstructionAt(r.getFromAddress());
                if (at == null) {
                    continue;
                }
                // The assert's arguments are loaded just before its CALL, in any order. Find the
                // CALL ahead of the path string, then take the window from a few instructions
                // before the path string up to the CALL.
                Instruction call = at;
                for (int i = 0; i < 10 && call != null && !call.getMnemonicString().equals("CALL"); i++) {
                    call = call.getNext();
                }
                if (call == null || !call.getMnemonicString().equals("CALL")) {
                    continue;
                }
                Address handler = call.getFlows().length == 1 ? call.getFlows()[0] : null;
                Instruction first = at;
                for (int i = 0; i < 6 && first.getPrevious() != null; i++) {
                    first = first.getPrevious();
                }
                String cls = null;
                Instruction earliestArg = at;
                for (Instruction ins = first; ins != null && !ins.equals(call); ins = ins.getNext()) {
                    for (Reference rr : ins.getReferencesFrom()) {
                        String s = strings.get(rr.getToAddress());
                        if (s != null && !s.equals(e.getValue()) && IDENT.matcher(s).matches()) {
                            cls = s;
                            if (ins.getAddress().compareTo(earliestArg.getAddress()) < 0) {
                                earliestArg = ins;
                            }
                        }
                    }
                }
                // The null check is wherever control comes from: the compiler usually moves the
                // assert into an out-of-line block, so look at each branch into the block that
                // holds it (and at the conditional branch just before it, if it falls through),
                // and take the nearest read of a writable global before that branch.
                Address global = null;
                CodeBlock block = blockModel.getFirstCodeBlockContaining(earliestArg.getAddress(), monitor);
                List<Instruction> sources = new ArrayList<>();
                if (block != null) {
                    for (Reference in : getReferencesTo(block.getFirstStartAddress())) {
                        if (in.getReferenceType().isJump()) {
                            Instruction src = getInstructionAt(in.getFromAddress());
                            if (src != null) {
                                sources.add(src);
                            }
                        }
                    }
                    Instruction before = getInstructionBefore(block.getFirstStartAddress());
                    if (before != null && before.getFlowType().isConditional()) {
                        sources.add(before);
                    }
                }
                Map<Address, Integer> seen = new HashMap<>();
                for (Instruction src : sources) {
                    Instruction ins = src;
                    for (int i = 0; i < 6 && ins != null; i++, ins = ins.getPrevious()) {
                        Address g = readGlobal(ins, strings);
                        if (g != null) {
                            seen.merge(g, 1, Integer::sum);
                            break;
                        }
                    }
                }
                if (seen.size() == 1) {
                    global = seen.keySet().iterator().next();
                }
                log.printf("singleton-site\t%s\t%s\tglobal %s, handler %s\t0%n", at.getAddress(),
                    cls == null ? "" : cls, global, handler);
                if (cls != null && global != null) {
                    votes.computeIfAbsent(global, k -> new HashMap<>()).merge(cls, 1, Integer::sum);
                }
                if (handler != null) {
                    handlers.merge(handler, 1, Integer::sum);
                }
            }
        }
        // A global gets a class name only with a clear majority (at least 2 votes and 90%), and
        // each class name goes to only one global: the one with the most votes for it.
        Map<String, Address> bestFor = new HashMap<>();
        Map<String, Integer> bestVotes = new HashMap<>();
        Map<Address, String> winner = new HashMap<>();
        for (Map.Entry<Address, Map<String, Integer>> v : votes.entrySet()) {
            Map.Entry<String, Integer> top = v.getValue().entrySet().stream()
                .max(Map.Entry.comparingByValue()).get();
            int total = v.getValue().values().stream().mapToInt(Integer::intValue).sum();
            if (top.getValue() < 2 || top.getValue() * 10 < total * 9) {
                log.printf("singleton\t%s\t\tno clear majority %s\t0%n", v.getKey(), v.getValue());
                continue;
            }
            winner.put(v.getKey(), top.getKey());
            if (top.getValue() > bestVotes.getOrDefault(top.getKey(), 0)) {
                bestVotes.put(top.getKey(), top.getValue());
                bestFor.put(top.getKey(), v.getKey());
            }
        }
        for (Map.Entry<Address, String> w : winner.entrySet()) {
            String cls = w.getValue();
            int n = votes.get(w.getKey()).get(cls);
            if (!bestFor.get(cls).equals(w.getKey())) {
                log.printf("singleton\t%s\t\t%s also claimed by %s with more votes (%d here)\t0%n",
                    w.getKey(), cls, bestFor.get(cls), n);
                continue;
            }
            nameGlobal(w.getKey(), "g_" + cls.replace("::", "_"),
                "singleton instance: null-checked before " + n + " singleton asserts naming " + cls);
        }
        handlers.entrySet().stream().max(Map.Entry.comparingByValue()).ifPresent(h -> {
            try {
                Function f = getFunctionAt(h.getKey());
                if (f != null) {
                    nameFunction("singleton", f, null, "FD4_SingletonAssert",
                        "called by " + h.getValue() + " FD4Singleton.h / FD4DerivedSingleton.h asserts (descriptive name)");
                }
            }
            catch (Exception ex) {
                printerr(ex.toString());
            }
        });
    }

    /** The writable global an instruction reads (MOV reg,[g] / CMP [g],0), if any. */
    private Address readGlobal(Instruction ins, Map<Address, String> strings) {
        for (Reference rr : ins.getReferencesFrom()) {
            if (rr.getReferenceType().isRead() && isWritableData(rr.getToAddress()) &&
                !strings.containsKey(rr.getToAddress())) {
                return rr.getToAddress();
            }
        }
        return null;
    }

    private boolean isWritableData(Address a) {
        MemoryBlock b = currentProgram.getMemory().getBlock(a);
        return b != null && b.isWrite() && !b.isExecute();
    }

    // ---- applying -----------------------------------------------------------------------------

    private void nameFunction(String rule, Function f, String cls, String name, String why) throws Exception {
        String full = cls == null ? name : cls + "::" + name;
        boolean free = f.getSymbol().getSource() == SourceType.DEFAULT;
        log.printf("%s\t%s\t%s\t%s\t%d%n", rule, f.getEntryPoint(), full, why, free && !dry ? 1 : 0);
        if (!free) {
            counts.merge(rule + " (already named)", 1, Integer::sum);
            return;
        }
        counts.merge(rule, 1, Integer::sum);
        if (dry) {
            return;
        }
        Namespace ns = cls == null ? currentProgram.getGlobalNamespace()
            : NamespaceUtils.createNamespaceHierarchy(cls, null, currentProgram, SourceType.ANALYSIS);
        f.setParentNamespace(ns);
        f.setName(name, SourceType.ANALYSIS);
        String plate = f.getComment();
        String note = "[mensis:" + rule + "] " + why;
        f.setComment(plate == null ? note : plate + "\n" + note);
    }

    private void nameGlobal(Address a, String name, String why) throws Exception {
        Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
        boolean free = s == null || s.getSource() == SourceType.DEFAULT;
        log.printf("singleton\t%s\t%s\t%s\t%d%n", a, name, why, free && !dry ? 1 : 0);
        if (!free) {
            counts.merge("singleton global (already named)", 1, Integer::sum);
            return;
        }
        counts.merge("singleton global", 1, Integer::sum);
        if (dry) {
            return;
        }
        createLabel(a, name, true, SourceType.ANALYSIS);
        setEOLComment(a, "[mensis:singleton] " + why);
    }
}
