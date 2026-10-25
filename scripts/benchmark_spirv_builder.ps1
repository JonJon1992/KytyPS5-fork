param(
    [Parameter(Mandatory = $true)][string]$BaselineExe,
    [Parameter(Mandatory = $true)][string]$CandidateExe,
    [Parameter(Mandatory = $true)][string]$OutCsv,
    [ValidateRange(0, 63)][int]$Cpu = 2
)

# Run identical Release binaries in ABBA order, pinned to one logical CPU.
# Round 0 warms the process; the remaining rounds measure declaration cache hits.
$ErrorActionPreference = 'Stop'
$baseline = (Resolve-Path -LiteralPath $BaselineExe).Path
$candidate = (Resolve-Path -LiteralPath $CandidateExe).Path
$rows = @()
$run = 0
for ($cycle = 0; $cycle -lt 3; $cycle++) {
    foreach ($variant in @('baseline', 'candidate', 'candidate', 'baseline')) {
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = if ($variant -eq 'baseline') { $baseline } else { $candidate }
        $start.Arguments = '--benchmark'
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $proc = [Diagnostics.Process]::Start($start)
        try {
            $proc.ProcessorAffinity = [IntPtr]([Int64]1 -shl $Cpu)
            $output = $proc.StandardOutput.ReadToEndAsync()
            $errors = $proc.StandardError.ReadToEndAsync()
            $proc.WaitForExit()
            if ($proc.ExitCode -ne 0) { throw "Benchmark $variant failed: $($errors.Result)" }
            $samples = @($output.Result | ConvertFrom-Csv)
            if ($samples.Count -ne 9) { throw "Unexpected benchmark output from $variant" }
            foreach ($sample in $samples) {
                if ([int]$sample.round -eq 0) { continue }
                $rows += [pscustomobject]@{
                    run = $run; cycle = $cycle; variant = $variant; round = $sample.round
                    lookups = $sample.lookups; allocations = $sample.allocations
                    ns_per_lookup = $sample.ns_per_lookup; checksum = $sample.checksum
                }
            }
        } finally { $proc.Dispose() }
        $run++
    }
}
if (@($rows.checksum | Select-Object -Unique).Count -ne 1) { throw 'Benchmark checksums differ' }
$rows | Export-Csv -LiteralPath $OutCsv -NoTypeInformation
foreach ($variant in @('baseline', 'candidate')) {
    $selected = @($rows | Where-Object variant -eq $variant)
    $values = @($selected | ForEach-Object {
        [double]::Parse($_.ns_per_lookup, [Globalization.CultureInfo]::InvariantCulture)
    } | Sort-Object)
    $middle = [int]($values.Count / 2)
    $median = ($values[$middle - 1] + $values[$middle]) / 2
    '{0}: samples={1} median_ns={2:F3} allocations_per_million={3}' -f $variant, $values.Count, $median, $selected[0].allocations
}
