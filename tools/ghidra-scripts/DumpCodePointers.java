// List every aligned 32-bit word in non-executable initialized memory that points into
// the code range, with the instruction words at target-4 and target.
// Usage: DumpCodePointers.java <code-start-hex> <code-end-hex> <out-file>
// Output lines: <ptr-location> <target> <word-before-target> <word-at-target>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import java.io.PrintWriter;

public class DumpCodePointers extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] a = getScriptArgs();
    long lo = Long.parseLong(a[0], 16), hi = Long.parseLong(a[1], 16);
    try (PrintWriter out = new PrintWriter(a[2])) {
      for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
        if (!b.isInitialized() || b.isExecute()) continue;
        long start = (b.getStart().getOffset() + 3) & ~3L, end = b.getEnd().getOffset();
        byte[] buf = new byte[(int) (end - b.getStart().getOffset() + 1)];
        b.getBytes(b.getStart(), buf);
        int base = (int) (start - b.getStart().getOffset());
        for (int i = base; i + 4 <= buf.length; i += 4) {
          long v = ((buf[i] & 0xFFL) << 24) | ((buf[i + 1] & 0xFFL) << 16)
              | ((buf[i + 2] & 0xFFL) << 8) | (buf[i + 3] & 0xFFL);
          if (v < lo || v >= hi || (v & 3) != 0) continue;
          Address t = toAddr(v);
          int before = 0;
          try { before = getInt(t.subtract(4)); } catch (Exception e) { before = 0; }
          out.printf("%08X %08X %08X %08X%n", b.getStart().getOffset() + i, v, before, getInt(t));
        }
      }
    }
  }
}
