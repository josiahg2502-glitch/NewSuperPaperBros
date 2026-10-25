using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Translator.Core.Build;

/// <summary>
/// Save states: makes one translated function resumable at
/// its call sites, so a restored state can rebuild the native call stack a guest thread had at a VI
/// suspend. Applied only to the functions listed in the project (those seen on suspended threads).
/// <list type="bullet">
/// <item>Entry: while <c>SaveStateResume::Active()</c>, jump to the call site whose return address the
/// state recorded for this frame (<c>SaveStateResume::Enter</c>).</item>
/// <item>Before every call: write every register local to ctx (the coherent call boundary the rebuild
/// relies on), then the resume label.</item>
/// <item>After every call: reload r1 and the callee-saved registers from ctx (the callee's epilogue put
/// them there). On the resumed call only, restore the remaining locals from the registers the thread had
/// when it entered the HLE wait (<c>SaveStateResume::YieldEntry</c>): the values this frame kept native.</item>
/// </list>
/// Throws on any call form it does not understand rather than emitting a function that cannot resume.
/// </summary>
public static partial class ResumableFunctionRewriter
{
    [GeneratedRegex(@"^\s+(?:uint32_t|PPC_FPR|uint64_t|double|float) (r\d+|f\d+|cr|xer|ctr) = ctx->(gpr\[\d+\]|fpr\[\d+\]|cr|xer|ctr);\s*$")]
    private static partial Regex DeclarationRegex();

    [GeneratedRegex(@"^(\s*)(InvokeDirectCpu<0x[0-9A-Fa-f]{8}u>\(ctx\)|MKW_STATIC_TRANSLATED_CALL\(0x[0-9A-Fa-f]{8}u, \w+, ctx\)|InvokeIndirectCpu\((.+), ctx\));\s*$")]
    private static partial Regex CallRegex();

    [GeneratedRegex(@"^\s*ctx->lr = 0x([0-9A-Fa-f]{8})u;\s*$")]
    private static partial Regex LinkRegex();

    [GeneratedRegex(@"^\s*(r\d+|f\d+|cr|xer|ctr) = ctx->(?:gpr\[\d+\]|fpr\[\d+\]|cr|xer|ctr);\s*$")]
    private static partial Regex ReloadRegex();

    [GeneratedRegex(@"^\s*goto loc_[0-9A-Fa-f]{8};\s*$")]
    private static partial Regex EntryGotoRegex();

    private static readonly string[] UnsupportedForms =
        ["fused_boundary_ctx", "cached_", "MKW_STATE_FREE", "InvokeDirectJump"];

    /// <summary>r1 and the PowerPC callee-saved registers: restored into ctx by the callee's epilogue.</summary>
    private static bool LiveAfterCall(string register) =>
        register == "r1" ||
        (register.Length > 1 && (register[0] == 'r' || register[0] == 'f') &&
         int.TryParse(register.AsSpan(1), NumberStyles.None, CultureInfo.InvariantCulture, out var index) && index >= 14);

    public static string Rewrite(string source, uint entry)
    {
        foreach (var form in UnsupportedForms)
        {
            if (source.Contains(form, StringComparison.Ordinal))
                throw new InvalidOperationException($"Resumable 0x{entry:X8}: unsupported call form '{form}'.");
        }

        var newline = source.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var lines = source.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n').ToList();
        var header = $"void func_{entry:X8}(CpuContext* MKW_RESTRICT ctx)";
        var headerIndex = lines.FindIndex(line => line.Contains(header, StringComparison.Ordinal));
        if (headerIndex < 0)
            throw new InvalidOperationException($"Resumable 0x{entry:X8}: function header not found.");
        var gotoIndex = lines.FindIndex(headerIndex, line => EntryGotoRegex().IsMatch(line));
        if (gotoIndex < 0)
            throw new InvalidOperationException($"Resumable 0x{entry:X8}: entry goto not found.");

        var locals = new List<(string Register, string Field)>();
        for (var i = headerIndex + 1; i < gotoIndex; ++i)
        {
            var match = DeclarationRegex().Match(lines[i]);
            if (match.Success) locals.Add((match.Groups[1].Value, match.Groups[2].Value));
        }

        var output = new List<string>();
        var returnAddresses = new List<uint>();
        for (var i = 0; i < lines.Count; ++i)
        {
            var line = lines[i];
            var call = i > gotoIndex ? CallRegex().Match(line) : Match.Empty;
            if (!call.Success)
            {
                output.Add(line);
                continue;
            }

            var pad = call.Groups[1].Value;
            uint? returnAddress = null;
            for (var back = output.Count - 1; back >= 0 && output.Count - back < 120; --back)
            {
                if (CallRegex().IsMatch(output[back])) break;
                var link = LinkRegex().Match(output[back]);
                if (link.Success)
                {
                    returnAddress = uint.Parse(link.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                    break;
                }
            }
            // No ctx->lr: the translator omits it when the callee never saves LR, so that callee is
            // never on a captured chain and the call needs no resume point.
            if (returnAddress is not { } ret)
            {
                output.Add(line);
                continue;
            }
            if (returnAddresses.Contains(ret))
                throw new InvalidOperationException($"Resumable 0x{entry:X8}: two calls return to 0x{ret:X8}.");
            returnAddresses.Add(ret);

            foreach (var (register, field) in locals) output.Add($"{pad}ctx->{field} = {register};");
            output.Add($"mkw_resume_{ret:X8}:");
            output.Add(call.Groups[3].Success
                ? $"{pad}InvokeIndirectCpu(SaveStateResume::Target({call.Groups[3].Value}), ctx);"
                : line);

            // The call's own reloads first (results and clobbers, exactly as the normal path reads them).
            var reloaded = new HashSet<string>(StringComparer.Ordinal);
            while (i + 1 < lines.Count && ReloadRegex().Match(lines[i + 1]) is { Success: true } reload)
            {
                reloaded.Add(reload.Groups[1].Value);
                output.Add(lines[++i]);
            }
            foreach (var (register, field) in locals)
            {
                if (LiveAfterCall(register) && !reloaded.Contains(register)) output.Add($"{pad}{register} = ctx->{field};");
            }
            var restore = locals.Where(local => !LiveAfterCall(local.Register) && !reloaded.Contains(local.Register)).ToList();
            output.Add($"{pad}if (__builtin_expect(mkw_resumed, 0)) {{");
            output.Add($"{pad}    mkw_resumed = false;");
            if (restore.Count > 0)
            {
                output.Add($"{pad}    const CpuContext& mkw_entry = SaveStateResume::YieldEntry();");
                foreach (var (register, field) in restore) output.Add($"{pad}    {register} = mkw_entry.{field};");
            }
            output.Add($"{pad}}}");
        }

        if (returnAddresses.Count == 0)
            throw new InvalidOperationException($"Resumable 0x{entry:X8}: no call sites.");
        var dispatch = new StringBuilder();
        dispatch.Append($"    bool mkw_resumed = false;{newline}");
        dispatch.Append($"    if (__builtin_expect(SaveStateResume::Active(), 0)) {{{newline}");
        dispatch.Append($"        mkw_resumed = true;{newline}");
        dispatch.Append($"        switch (SaveStateResume::Enter(0x{entry:X8}u)) {{{newline}");
        foreach (var ret in returnAddresses) dispatch.Append($"        case 0x{ret:X8}u: goto mkw_resume_{ret:X8};{newline}");
        dispatch.Append($"        default: SaveStateResume::Mismatch(0x{entry:X8}u);{newline}");
        dispatch.Append($"        }}{newline}");
        dispatch.Append("    }");
        output.Insert(output.FindIndex(headerIndex, line => EntryGotoRegex().IsMatch(line)), dispatch.ToString());
        return string.Join(newline, output);
    }

    /// <summary>The project's resumable_functions.inc: one hex guest address per line, C syntax
    /// (<c>0x8001D9A0,  // main</c>), so the runtime can #include the same file.</summary>
    public static HashSet<uint> LoadList(string path)
    {
        var set = new HashSet<uint>();
        foreach (var raw in File.ReadAllLines(path))
        {
            var text = raw.Split("//")[0].Trim().TrimEnd(',').Trim();
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) text = text[2..];
            if (text.Length > 0) set.Add(uint.Parse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture));
        }
        return set;
    }
}
