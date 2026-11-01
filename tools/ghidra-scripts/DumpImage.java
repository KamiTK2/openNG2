// Write each initialized memory block's raw bytes to <out-dir>\<start-hex>_<x|d>.bin
// (x = executable). Gives Python tools the decrypted/decompressed XEX image.
// Usage: DumpImage.java <out-dir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import java.io.File;
import java.io.FileOutputStream;

public class DumpImage extends GhidraScript {
  @Override
  public void run() throws Exception {
    File dir = new File(getScriptArgs()[0]);
    dir.mkdirs();
    for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
      if (!b.isInitialized()) continue;
      byte[] buf = new byte[(int) b.getSize()];
      b.getBytes(b.getStart(), buf);
      String name = String.format("%08X_%s.bin", b.getStart().getOffset(), b.isExecute() ? "x" : "d");
      try (FileOutputStream f = new FileOutputStream(new File(dir, name))) {
        f.write(buf);
      }
    }
  }
}
