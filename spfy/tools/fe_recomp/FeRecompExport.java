// Ghidra script: export SWIttsFe-en-US.dll's functions, basic blocks, block
// successors and resolved computed-flow targets as JSON lines, the input to
// recomp.py. Run on the analysed program (image base 07dd0000); writes
// C:/tmp/fe_recomp/ghidra_export.jsonl (gzip it into data/ to keep it).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.block.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.TaskMonitor;
import java.io.*;
import java.util.*;

public class FeRecompExport extends GhidraScript {
    private static String hex(Address a) { return String.format("%08x", a.getOffset()); }

    @Override
    public void run() throws Exception {
        File out = new File("C:/tmp/fe_recomp/ghidra_export.jsonl");
        out.getParentFile().mkdirs();
        PrintWriter w = new PrintWriter(new BufferedWriter(new OutputStreamWriter(new FileOutputStream(out), "UTF-8")));
        Listing listing = currentProgram.getListing();
        BasicBlockModel bbm = new BasicBlockModel(currentProgram);
        ReferenceManager rm = currentProgram.getReferenceManager();
        int nf = 0, nb = 0, ni = 0;
        FunctionIterator it = listing.getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            StringBuilder sb = new StringBuilder();
            sb.append("{\"fn\":\"").append(hex(f.getEntryPoint())).append("\",\"name\":\"")
              .append(f.getName().replace("\\", "\\\\").replace("\"", "'")).append("\",\"cc\":\"")
              .append(f.getCallingConventionName()).append("\",\"ranges\":[");
            boolean first = true;
            for (AddressRange r : f.getBody()) {
                if (!first) sb.append(",");
                first = false;
                sb.append("[\"").append(hex(r.getMinAddress())).append("\",\"").append(hex(r.getMaxAddress())).append("\"]");
            }
            sb.append("],\"blocks\":[");
            CodeBlockIterator bi = bbm.getCodeBlocksContaining(f.getBody(), TaskMonitor.DUMMY);
            first = true;
            while (bi.hasNext()) {
                CodeBlock b = bi.next();
                if (!first) sb.append(",");
                first = false;
                nb++;
                sb.append("{\"start\":\"").append(hex(b.getMinAddress())).append("\",\"end\":\"").append(hex(b.getMaxAddress())).append("\",\"succ\":[");
                CodeBlockReferenceIterator di = b.getDestinations(TaskMonitor.DUMMY);
                boolean f2 = true;
                while (di.hasNext()) {
                    CodeBlockReference d = di.next();
                    if (!f2) sb.append(",");
                    f2 = false;
                    sb.append("[\"").append(hex(d.getDestinationAddress())).append("\",\"").append(d.getFlowType().toString()).append("\"]");
                }
                sb.append("]}");
            }
            sb.append("],\"computed\":[");
            first = true;
            InstructionIterator ii = listing.getInstructions(f.getBody(), true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                ni++;
                FlowType ft = ins.getFlowType();
                if (ft.isComputed()) {
                    if (!first) sb.append(",");
                    first = false;
                    sb.append("{\"at\":\"").append(hex(ins.getAddress())).append("\",\"kind\":\"").append(ft.toString()).append("\",\"targets\":[");
                    boolean f3 = true;
                    for (Reference ref : rm.getReferencesFrom(ins.getAddress())) {
                        if (!ref.getReferenceType().isFlow()) continue;
                        if (!f3) sb.append(",");
                        f3 = false;
                        sb.append("\"").append(hex(ref.getToAddress())).append("\"");
                    }
                    sb.append("]}");
                }
            }
            sb.append("]}");
            w.println(sb.toString());
            nf++;
        }
        w.close();
        println("functions " + nf + " blocks " + nb + " instructions " + ni + " -> " + out);
    }
}
