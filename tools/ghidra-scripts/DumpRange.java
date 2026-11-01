// Disassemble and print a guest address range: DumpRange.java <start-hex> <end-hex> <out-file>
// Used headless against default.xex (loaded with XEXLoaderWV) to inspect codegen problem sites.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import java.io.PrintWriter;

public class DumpRange extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] a = getScriptArgs();
    Address start = toAddr(Long.parseLong(a[0], 16));
    Address end = toAddr(Long.parseLong(a[1], 16));
    try (PrintWriter out = new PrintWriter(a[2])) {
      for (Address p = start; p.compareTo(end) < 0; p = p.add(4)) {
        if (getInstructionAt(p) == null && getInstructionContaining(p) == null) {
          disassemble(p);
        }
      }
      for (Address p = start; p.compareTo(end) < 0; p = p.add(4)) {
        Instruction ins = getInstructionAt(p);
        String bytes = String.format("%08X", getInt(p));
        out.printf("%s  %s  %s%n", p, bytes, ins == null ? "<data>" : ins.toString());
      }
    }
  }
}
