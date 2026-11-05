# Pixel diff of two screenshots, title bar excluded.
# Usage: powershell -File tools\native_diff.ps1 -A a.png -B b.png [-Curve]
# Prints mean absolute error, percent of pixels off by more than 8/255, mean RGB of each.
# -Curve prints the median of B per 16-wide bin of A, which shows a tone curve between them.
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [switch]$Curve)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Text;
using System.Collections.Generic; using System.Runtime.InteropServices;
public static class NgDiff {
  static byte[] Load(string p, out int w, out int h, out int s) {
    using (var b = new Bitmap(p)) {
      w = b.Width; h = b.Height;
      var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      s = d.Stride; var o = new byte[s * h]; Marshal.Copy(d.Scan0, o, 0, o.Length); b.UnlockBits(d); return o;
    }
  }
  public static string Run(string pa, string pb, bool curve) {
    int wa, ha, sa, wb, hb, sb;
    var a = Load(pa, out wa, out ha, out sa); var b = Load(pb, out wb, out hb, out sb);
    int w = Math.Min(wa, wb), h = Math.Min(ha, hb); long sum = 0, over = 0, n = 0;
    var ma = new long[3]; var mb = new long[3];
    var bins = new List<int>[3, 16];
    for (int c = 0; c < 3; c++) for (int i = 0; i < 16; i++) bins[c, i] = new List<int>();
    for (int y = 32; y < h; y++) for (int x = 0; x < w; x++) {
      int mx = 0;
      for (int c = 0; c < 3; c++) {
        int va = a[y * sa + x * 4 + 2 - c], vb = b[y * sb + x * 4 + 2 - c], d = Math.Abs(va - vb);
        sum += d; ma[c] += va; mb[c] += vb; if (d > mx) mx = d;
        if (curve) bins[c, va >> 4].Add(vb);
      }
      if (mx > 8) over++; n++;
    }
    var s = new StringBuilder();
    s.AppendFormat("mae={0:F2}/255 over8={1:F1}% meanA={2:F1},{3:F1},{4:F1} meanB={5:F1},{6:F1},{7:F1}",
      sum / (double)(n * 3), over * 100.0 / n, ma[0] / (double)n, ma[1] / (double)n, ma[2] / (double)n,
      mb[0] / (double)n, mb[1] / (double)n, mb[2] / (double)n);
    if (curve) for (int c = 0; c < 3; c++) {
      s.Append("\n").Append("RGB"[c]).Append(':');
      for (int i = 0; i < 16; i++) { var l = bins[c, i];
        if (l.Count == 0) { s.Append(" -"); continue; }
        l.Sort(); s.AppendFormat(" {0}={1}", i * 16 + 8, l[l.Count / 2]); }
    }
    return s.ToString();
  }
}
'@
[NgDiff]::Run((Resolve-Path $A).Path, (Resolve-Path $B).Path, [bool]$Curve)
