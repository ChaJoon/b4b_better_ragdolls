using System.Security.Cryptography;
using System.Text.RegularExpressions;
using CUE4Parse.FileProvider;
using CUE4Parse.FileProvider.Vfs;
using CUE4Parse.UE4.Versions;

// pakx: offline reader for Back 4 Blood's retail paks (no game running). CUE4Parse (NuGet, GAME_Back4Blood) plus
// the pak AES key (built into b4bmod.py). Same bytes as the dev agent's `dumpassets`
// (453/453 in docs/investigations/model-mods-paks.md). Normally run through modkit/b4bmod.py.
// --paks reads the game's own paks only (pakchunk<N>...-WindowsNoEditor.pak, 61 in this build): other paks people put
// in that folder (Nexus mods such as deck-size or weapon mods) are not the game, often not in its v9 format, and their
// `_P` names would override retail files in the listing and in what `extract` gives a mod maker.
const string Usage = @"pakx key     --paks <Paks dir> --aes <hex>                       check the key against the pak indexes
pakx list    --paks <Paks dir> --aes <hex> [<regex>]             every file path (size, path), TSV with the pak name
pakx extract --paks <Paks dir> --aes <hex> --out <dir> <regex>
                                                                 write every matching file to <dir>/Gobi/Content/...
--paks reads only the game's own paks (pakchunk*-WindowsNoEditor.pak); other paks in that folder (mods) are skipped.
list/extract also take --pak <file.pak> instead of --paks: one pak on its own (e.g. a mod someone made; B4B v9 or
v8 footer, the v8 one is read as v9 without changing the file).
<regex> is matched (case-insensitive) against paths like Gobi/Content/Characters/Heroes/Holly/.../x_SKM.uasset.
Oodle: CUE4Parse decompresses with its open-source managed decoder (OodleSharp, MIT); no native Oodle is used.";

try { return Run(args); }
catch (UsageException e) { Console.Error.WriteLine($"pakx: {e.Message}\n\n{Usage}"); return 2; }
catch (PakxException e) { Console.Error.WriteLine($"pakx: {e.Message}"); return 1; }

static int Run(string[] args)
{
    if (args.Length == 0 || args[0] is "-h" or "--help" or "help") { Console.WriteLine(Usage); return 0; }
    string cmd = args[0];
    var named = new Dictionary<string, string>();
    var pos = new List<string>();
    for (int i = 1; i < args.Length; i++)
    {
        if (args[i] is "--paks" or "--pak" or "--aes" or "--out")
        {
            if (i + 1 >= args.Length) throw new UsageException($"{args[i]} needs a value");
            named[args[i]] = args[++i];
        }
        else pos.Add(args[i]);
    }
    string one = named.GetValueOrDefault("--pak");
    string paks = named.GetValueOrDefault("--paks") ?? (one != null ? null : throw new UsageException("--paks <Paks dir> is required"));
    if (paks != null && !Directory.Exists(paks)) throw new PakxException($"no such folder: {paks}");
    if (one != null && !File.Exists(one)) throw new PakxException($"no such file: {one}");
    if (one != null && cmd == "key") throw new UsageException("key checks the game's Paks folder: give --paks");
    byte[] key = ParseKey(named.GetValueOrDefault("--aes") ?? throw new UsageException("--aes <key> is required"));

    switch (cmd)
    {
        case "key":
        {
            var (ok, n) = CheckKey(paks, key);
            Console.WriteLine($"key OK: it decrypts all {ok} encrypted pak indexes in {paks} (index SHA1 matches)");
            var other = GamePaks(paks).other;
            if (other.Count > 0)
                Console.WriteLine($"other paks: {other.Count} not read (not the game's own; mods): {string.Join(", ", other.Select(Path.GetFileName))}");
            return 0;
        }
        case "list":
        case "extract":
        {
            if (paks != null) CheckKey(paks, key);   // "wrong key" before CUE4Parse gets to it
            Regex re = new(pos.Count > 0 ? pos[0] : (cmd == "list" ? "." : throw new UsageException("<regex> is required")),
                           RegexOptions.IgnoreCase);
            string outDir = cmd == "extract" ? named.GetValueOrDefault("--out") ?? throw new UsageException("--out <dir> is required") : null;
            AbstractVfsFileProvider pp;
            if (one != null)
            {
                var sp = new StreamedFileProvider("pakx", true, new VersionContainer(EGame.GAME_Back4Blood));
                sp.Initialize();
                sp.RegisterVfs(Path.GetFileName(one), new[] { OpenPak(one) });
                pp = sp;
            }
            else
            {
                // the game's own paks, each registered as DefaultFileProvider registers a pak it finds (RegisterVfs(FileInfo):
                // a random-access file archive), so the readers are the ones the 453/453 check used; mods are left out
                var gp = new StreamedFileProvider("pakx", new VersionContainer(EGame.GAME_Back4Blood), StringComparer.OrdinalIgnoreCase);
                gp.Initialize();
                foreach (var p in GamePaks(paks).game) gp.RegisterVfs(new FileInfo(p));
                pp = gp;
            }
            pp.SubmitKey(new CUE4Parse.UE4.Objects.Core.Misc.FGuid(), new CUE4Parse.Encryption.Aes.FAesKey(key));
            int n = 0;
            long bytes = 0;
            foreach (var k in pp.Files.Keys.OrderBy(k => k, StringComparer.Ordinal))
            {
                var f = pp.Files[k];
                if (!re.IsMatch(f.Path)) continue;
                n++;
                if (cmd == "list")
                {
                    string pak = f is CUE4Parse.UE4.VirtualFileSystem.VfsEntry v ? Path.GetFileName(v.Vfs.Name) : "";
                    Console.WriteLine($"{f.Path}\t{f.Size}\t{pak}");
                    continue;
                }
                var outp = Path.Combine(outDir, f.Path.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(outp)!);
                var data = pp.SaveAsset(f);
                File.WriteAllBytes(outp, data);
                bytes += data.Length;
            }
            Console.Error.WriteLine(cmd == "list" ? $"{n} files" : $"{n} files, {bytes / 1048576.0:F1} MB -> {outDir}");
            return 0;
        }
        default: throw new UsageException($"unknown command {cmd}");
    }
}

// One pak file as CUE4Parse reads it. B4B paks end in a 222-byte footer (v9); some community paks carry the v8 one
// (221 bytes, no bIndexIsFrozen byte, same entries), which the game also mounts: shown as v9 (frozen = 0), file untouched.
static Stream OpenPak(string path)
{
    var fs = File.OpenRead(path);
    var ft = new byte[222];
    if (fs.Length < 222) throw new PakxException($"{Path.GetFileName(path)}: too small for a pak");
    fs.Seek(-222, SeekOrigin.End); fs.ReadExactly(ft);
    if (BitConverter.ToUInt32(ft, 4) == 0x18772) { fs.Seek(0, SeekOrigin.Begin); return fs; }
    if (BitConverter.ToUInt32(ft, 5) == 0x18772 && BitConverter.ToUInt32(ft, 1) == 8)
    {
        var v9 = new byte[222];
        Array.Copy(ft, 1, v9, 0, 61);          // Version .. IndexOffset
        v9[0] = 9; v9[61] = 0;                 // bIndexIsFrozen
        Array.Copy(ft, 62, v9, 62, 160);       // CompressionMethods
        return new FooterStream(fs, fs.Length - 221, v9);
    }
    if (Enumerable.Range(0, 218).Any(i => BitConverter.ToUInt32(ft, i) == 0x5A6F12E1))
        throw new PakxException($"{Path.GetFileName(path)} is a stock Unreal pak, not a Back 4 Blood one (the game can't mount it either)");
    throw new PakxException($"{Path.GetFileName(path)} is not a Back 4 Blood pak (footer magic)");
}

static byte[] ParseKey(string s)
{
    s = s.Trim();
    if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];
    if (s.Length != 64 || !s.All(Uri.IsHexDigit))
        throw new PakxException("the AES key must be 64 hex digits (0x... as in the community key lists)");
    return Convert.FromHexString(s);
}

// B4B pak footer (222 bytes): Version u32, Magic u32 0x18772, KeyGuid[16], bEncryptedIndex u8, IndexHash[20] (SHA1 of
// the plain index), IndexSize u64, IndexOffset u64, ... (docs/investigations/model-mods-paks.md §3). The index is
// AES-256-ECB, so a key is right exactly when SHA1(decrypt(index)) == IndexHash.
static (int ok, int total) CheckKey(string paks, byte[] key)
{
    using var aes = Aes.Create();
    aes.Key = key;
    int ok = 0, seen = 0;
    foreach (var p in GamePaks(paks).game)
    {
        using var fs = File.OpenRead(p);
        if (fs.Length < 222) continue;
        var ft = new byte[222];
        fs.Seek(-222, SeekOrigin.End); fs.ReadExactly(ft);
        if (BitConverter.ToUInt32(ft, 4) != 0x18772) throw new PakxException($"{Path.GetFileName(p)} is not a Back 4 Blood pak (footer magic)");
        seen++;
        if (ft[24] == 0) continue;
        long size = BitConverter.ToInt64(ft, 45), off = BitConverter.ToInt64(ft, 53);
        if (size <= 0 || size % 16 != 0 || off < 0 || off + size > fs.Length) throw new PakxException($"{Path.GetFileName(p)}: bad index bounds");
        var idx = new byte[size];
        fs.Seek(off, SeekOrigin.Begin); fs.ReadExactly(idx);
        var plain = aes.DecryptEcb(idx, PaddingMode.None);
        if (!SHA1.HashData(plain).AsSpan().SequenceEqual(ft.AsSpan(25, 20)))
            throw new PakxException($"wrong AES key: it does not decrypt {Path.GetFileName(p)} (index SHA1 mismatch). " +
                                    "Back 4 Blood has one key for every copy of the game; see the modkit README, \"The AES key\".");
        ok++;
    }
    if (seen == 0) throw new PakxException($"no pakchunk*-WindowsNoEditor.pak files in {paks} (expected <game>/Gobi/Content/Paks)");
    return (ok, seen);
}

// The game's own paks in a Paks folder (pakchunk<N>[...]-WindowsNoEditor.pak; the setup script's rule for "a game
// file"), and every other .pak there (mods), both sorted.
static (List<string> game, List<string> other) GamePaks(string paks)
{
    var name = new Regex(@"^pakchunk[0-9]+[^-]*-WindowsNoEditor\.pak$", RegexOptions.IgnoreCase);
    var all = Directory.GetFiles(paks, "*.pak").OrderBy(p => p, StringComparer.Ordinal).ToList();
    var game = all.Where(p => name.IsMatch(Path.GetFileName(p))).ToList();
    return (game, all.Except(game).ToList());
}

// The first `body` bytes of `inner`, then `tail`: a read-only view of a pak with another footer.
class FooterStream(Stream inner, long body, byte[] tail) : Stream
{
    long pos;
    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => body + tail.Length;
    public override long Position { get => pos; set => pos = value; }
    public override void Flush() { }
    public override long Seek(long o, SeekOrigin so) => pos = so switch { SeekOrigin.Begin => o, SeekOrigin.Current => pos + o, _ => Length + o };
    public override void SetLength(long v) => throw new NotSupportedException();
    public override void Write(byte[] b, int o, int c) => throw new NotSupportedException();
    public override int Read(byte[] b, int o, int c)
    {
        int n = 0;
        while (c > 0 && pos < Length)
        {
            int k;
            if (pos < body)
            {
                inner.Seek(pos, SeekOrigin.Begin);
                k = inner.Read(b, o, (int)Math.Min(c, body - pos));
                if (k <= 0) break;
            }
            else
            {
                k = (int)Math.Min(c, Length - pos);
                Array.Copy(tail, pos - body, b, o, k);
            }
            pos += k; o += k; c -= k; n += k;
        }
        return n;
    }
    protected override void Dispose(bool d) { if (d) inner.Dispose(); base.Dispose(d); }
}

class UsageException(string m) : Exception(m);
class PakxException(string m) : Exception(m);
