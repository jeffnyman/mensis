// SPDX-License-Identifier: GPL-3.0-or-later
// Delete a file (program, Version Tracking session, ...) from the current Ghidra project.
//
// Headless, with any program as the -process target:
//   -process <any program> -noanalysis -readOnly -postScript DeleteProjectFile.java /path/in/project
//@category PS4

import ghidra.app.script.GhidraScript;
import ghidra.framework.model.DomainFile;

public class DeleteProjectFile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String path : getScriptArgs()) {
            DomainFile f = state.getProject().getProjectData().getFile(path);
            if (f == null) {
                println("DeleteProjectFile: nothing at " + path);
                continue;
            }
            f.delete();
            println("DeleteProjectFile: deleted " + path);
        }
    }
}
