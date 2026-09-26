#requires -Version 5.1
<#
.SYNOPSIS
    Annotate what was observed, what is observed now, and what is expected next.
.DESCRIPTION
    Creates a local JSON record and a readable Markdown report for each run.
    Machine observations come from Windows; human and assistant statements are
    supplied as notes. Expectations are checked on the NEXT run, never reported
    as facts before they are observed. -Verify checks every stored record and
    the links between them without taking a new snapshot. This script does not
    change system state beyond writing its own records.
.EXAMPLE
    .\PowerShell-Forward-Pass.ps1 -HumanNote 'I will start my renderer' `
        -AssistantProposal 'Check whether it stays running' -ExpectedProcess renderer
.EXAMPLE
    .\PowerShell-Forward-Pass.ps1 -DataDirectory "$env:LOCALAPPDATA\ForwardPass"
.EXAMPLE
    .\PowerShell-Forward-Pass.ps1 -Verify
.EXAMPLE
    .\PowerShell-Forward-Pass.ps1 `
        -GraphGroup 'runtime::capture::Capture state' `
        -GraphGroup 'runtime::verify::Verify chain' `
        -GraphLink 'runtime::capture::verify::forwards-to' `
        -CategoryAnnotation 'integrity::runtime::capture::Writes an atomic record' `
        -CategoryAnnotation 'integrity::runtime::verify::Checks the digest chain'
.EXAMPLE
    .\PowerShell-Forward-Pass.ps1 `
        -ScriptEnvironment 'september-forward' `
        -SeptemberMatrix 'forward::prior::current::0.75::Prior becomes current' `
        -SeptemberMatrix 'forward::current::next::0.90::Current develops forward' `
        -GavornRicht 'forward::operator::advance::May advance verified cells'
#>
[CmdletBinding()]
param(
    [string]$DataDirectory = (Join-Path $env:LOCALAPPDATA 'ForwardPass'),
    [switch]$Verify,
    [string]$HumanNote = '',
    [string]$AssistantProposal = '',
    [string[]]$ExpectedProcess = @(),
    [string[]]$ExpectedService = @(),
    [string[]]$GraphGroup = @(),
    [string[]]$GraphLink = @(),
    [string[]]$CategoryAnnotation = @(),
    [string[]]$SeptemberMatrix = @(),
    [string[]]$GavornRicht = @(),
    [string]$ScriptEnvironment = 'forward-pass'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Digest([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Split-AnnotationSpec(
    [string]$Value,
    [int]$PartCount,
    [string]$Kind
) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Kind entry is empty"
    }
    $parts = @($Value -split '::', $PartCount)
    if ($parts.Count -ne $PartCount) {
        throw "$Kind entry must contain $PartCount fields separated by '::': $Value"
    }
    for ($i = 0; $i -lt $parts.Count; $i++) {
        $parts[$i] = [string]$parts[$i].Trim()
        if ([string]::IsNullOrWhiteSpace($parts[$i])) {
            throw "$Kind entry contains an empty field: $Value"
        }
    }
    return $parts
}

function Assert-AnnotationId([string]$Value, [string]$Kind) {
    if ($Value -cnotmatch '^[A-Za-z][A-Za-z0-9_.-]{0,63}$') {
        throw "$Kind '$Value' must begin with a letter and contain only letters, digits, dot, underscore, or hyphen"
    }
}

function Get-GraphNodeKey([string]$GroupId, [string]$NodeId) {
    return '{0}::{1}' -f $GroupId.ToLowerInvariant(), $NodeId.ToLowerInvariant()
}

function New-AnnotationGraph(
    [string[]]$GroupSpecs,
    [string[]]$LinkSpecs,
    [string[]]$AnnotationSpecs
) {
    $nodes = @()
    $nodeIndex = @{}
    $groupIndex = @{}
    foreach ($spec in @($GroupSpecs)) {
        $parts = @(Split-AnnotationSpec $spec 3 'GraphGroup')
        $groupId, $nodeId, $label = $parts
        Assert-AnnotationId $groupId 'Graph group id'
        Assert-AnnotationId $nodeId 'Graph node id'
        $key = Get-GraphNodeKey $groupId $nodeId
        if ($nodeIndex.ContainsKey($key)) {
            throw "Duplicate graph node '$groupId/$nodeId'"
        }
        $nodeIndex[$key] = $true
        $groupIndex[$groupId.ToLowerInvariant()] = $true
        $nodes += [pscustomobject][ordered]@{
            GroupId = $groupId
            NodeId = $nodeId
            Label = $label
        }
    }

    $links = @()
    $linkIndex = @{}
    foreach ($spec in @($LinkSpecs)) {
        $parts = @(Split-AnnotationSpec $spec 4 'GraphLink')
        $groupId, $fromNode, $toNode, $relation = $parts
        Assert-AnnotationId $groupId 'Graph group id'
        Assert-AnnotationId $fromNode 'Graph source node id'
        Assert-AnnotationId $toNode 'Graph target node id'
        if ($fromNode -ieq $toNode) {
            throw "Graph link '$groupId/$fromNode' cannot target itself"
        }
        $fromKey = Get-GraphNodeKey $groupId $fromNode
        $toKey = Get-GraphNodeKey $groupId $toNode
        if (-not $nodeIndex.ContainsKey($fromKey) -or
            -not $nodeIndex.ContainsKey($toKey)) {
            throw "Graph link '$groupId/$fromNode->$toNode' references a node outside its declared group"
        }
        $linkKey = '{0}::{1}::{2}::{3}' -f
            $groupId.ToLowerInvariant(), $fromNode.ToLowerInvariant(),
            $toNode.ToLowerInvariant(), $relation.ToLowerInvariant()
        if ($linkIndex.ContainsKey($linkKey)) {
            throw "Duplicate graph link '$groupId/$fromNode->$toNode/$relation'"
        }
        $linkIndex[$linkKey] = $true
        $links += [pscustomobject][ordered]@{
            GroupId = $groupId
            FromNode = $fromNode
            ToNode = $toNode
            Relation = $relation
        }
    }

    $annotations = @()
    $annotationIndex = @{}
    $annotatedNodeIndex = @{}
    foreach ($spec in @($AnnotationSpecs)) {
        $parts = @(Split-AnnotationSpec $spec 4 'CategoryAnnotation')
        $categoryId, $groupId, $nodeId, $text = $parts
        Assert-AnnotationId $categoryId 'Category id'
        Assert-AnnotationId $groupId 'Annotation group id'
        Assert-AnnotationId $nodeId 'Annotation node id'
        $nodeKey = Get-GraphNodeKey $groupId $nodeId
        if (-not $nodeIndex.ContainsKey($nodeKey)) {
            throw "Category annotation '$categoryId' references unknown graph node '$groupId/$nodeId'"
        }
        $annotationKey = '{0}::{1}::{2}::{3}' -f
            $categoryId.ToLowerInvariant(), $groupId.ToLowerInvariant(),
            $nodeId.ToLowerInvariant(), $text.ToLowerInvariant()
        if ($annotationIndex.ContainsKey($annotationKey)) {
            throw "Duplicate category annotation '$categoryId/$groupId/$nodeId'"
        }
        $annotationIndex[$annotationKey] = $true
        $annotatedNodeIndex[$nodeKey] = $true
        $annotations += [pscustomobject][ordered]@{
            CategoryId = $categoryId
            GroupId = $groupId
            NodeId = $nodeId
            RichtDivision = "$groupId/$categoryId"
            Text = $text
        }
    }

    if ($nodes.Count -eq 0 -and ($links.Count -gt 0 -or $annotations.Count -gt 0)) {
        throw 'Graph links and category annotations require at least one GraphGroup node'
    }
    foreach ($node in $nodes) {
        $nodeKey = Get-GraphNodeKey $node.GroupId $node.NodeId
        if (-not $annotatedNodeIndex.ContainsKey($nodeKey)) {
            throw "Graph node '$($node.GroupId)/$($node.NodeId)' has no category annotation"
        }
    }

    $groups = @($nodes | Group-Object GroupId | Sort-Object Name | ForEach-Object {
        $groupId = [string]$_.Name
        [pscustomobject][ordered]@{
            GroupId = $groupId
            Nodes = @($_.Group | Sort-Object NodeId)
            Links = @($links | Where-Object { $_.GroupId -ieq $groupId } |
                Sort-Object FromNode, ToNode, Relation)
        }
    })
    $divisions = @($annotations | Group-Object RichtDivision | Sort-Object Name |
        ForEach-Object {
            [pscustomobject][ordered]@{
                RichtDivision = [string]$_.Name
                AnnotationCount = [int]$_.Count
                NodeCount = [int](@($_.Group.NodeId | Sort-Object -Unique).Count)
            }
        })
    return [ordered]@{
        Status = if ($nodes.Count -gt 0) { 'Validated and divided' } else { 'No graph annotations supplied' }
        GroupCount = [int]$groups.Count
        NodeCount = [int]$nodes.Count
        LinkCount = [int]$links.Count
        CategoryCount = [int](@($annotations.CategoryId | Sort-Object -Unique).Count)
        DivisionCount = [int]$divisions.Count
        Groups = $groups
        CategoryAnnotations = @($annotations | Sort-Object GroupId, CategoryId, NodeId, Text)
        RichtDivisions = $divisions
    }
}

function Test-StoredAnnotationGraph($Graph) {
    if ($null -eq $Graph) { throw 'schema 3 record has no annotation graph' }
    $groupSpecs = @()
    $linkSpecs = @()
    foreach ($group in @($Graph.Groups)) {
        foreach ($node in @($group.Nodes)) {
            $groupSpecs += '{0}::{1}::{2}' -f $group.GroupId, $node.NodeId, $node.Label
        }
        foreach ($link in @($group.Links)) {
            $linkSpecs += '{0}::{1}::{2}::{3}' -f
                $group.GroupId, $link.FromNode, $link.ToNode, $link.Relation
        }
    }
    $annotationSpecs = @($Graph.CategoryAnnotations | ForEach-Object {
        '{0}::{1}::{2}::{3}' -f $_.CategoryId, $_.GroupId, $_.NodeId, $_.Text
    })
    $rebuilt = New-AnnotationGraph `
        -GroupSpecs @($groupSpecs) `
        -LinkSpecs @($linkSpecs) `
        -AnnotationSpecs @($annotationSpecs)
    if ([int]$Graph.GroupCount -ne [int]$rebuilt.GroupCount -or
        [int]$Graph.NodeCount -ne [int]$rebuilt.NodeCount -or
        [int]$Graph.LinkCount -ne [int]$rebuilt.LinkCount -or
        [int]$Graph.CategoryCount -ne [int]$rebuilt.CategoryCount -or
        [int]$Graph.DivisionCount -ne [int]$rebuilt.DivisionCount) {
        throw 'stored annotation graph counts do not match its contents'
    }
    $storedCanonical = $Graph | ConvertTo-Json -Depth 12 -Compress
    $rebuiltCanonical = $rebuilt | ConvertTo-Json -Depth 12 -Compress
    if ($storedCanonical -cne $rebuiltCanonical) {
        throw 'stored annotation graph structure does not match its validated reconstruction'
    }
}

function ConvertTo-MatrixWeight([string]$Value) {
    $weight = 0.0
    $valid = [double]::TryParse(
        $Value,
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$weight
    )
    if (-not $valid -or [double]::IsNaN($weight) -or
        [double]::IsInfinity($weight) -or $weight -lt 0.0 -or
        $weight -gt 1.0) {
        throw "September matrix weight '$Value' must be a finite number from 0 through 1"
    }
    return [double]$weight
}

function New-SeptemberMatrixAnnotation(
    [string[]]$CellSpecs,
    [string[]]$RichtSpecs,
    [string]$PriorBinding
) {
    if ([string]::IsNullOrWhiteSpace($PriorBinding)) {
        throw 'September matrix annotation requires a prior binding'
    }
    if ($PriorBinding -cne 'GENESIS' -and
        $PriorBinding -cnotmatch '^[0-9a-f]{64}$') {
        throw 'September matrix prior binding must be GENESIS or a SHA-256 digest'
    }

    $cells = @()
    $cellIndex = @{}
    $matrixIndex = @{}
    foreach ($spec in @($CellSpecs)) {
        $parts = @(Split-AnnotationSpec $spec 5 'SeptemberMatrix')
        $matrixId, $rowId, $columnId, $weightText, $text = $parts
        Assert-AnnotationId $matrixId 'September matrix id'
        Assert-AnnotationId $rowId 'September matrix row id'
        Assert-AnnotationId $columnId 'September matrix column id'
        $weight = ConvertTo-MatrixWeight $weightText
        $key = '{0}::{1}::{2}' -f
            $matrixId.ToLowerInvariant(), $rowId.ToLowerInvariant(),
            $columnId.ToLowerInvariant()
        if ($cellIndex.ContainsKey($key)) {
            throw "Duplicate September matrix cell '$matrixId/$rowId/$columnId'"
        }
        $cellIndex[$key] = $true
        $matrixIndex[$matrixId.ToLowerInvariant()] = $matrixId
        $cells += [pscustomobject][ordered]@{
            MatrixId = $matrixId
            RowId = $rowId
            ColumnId = $columnId
            Weight = $weight
            Annotation = $text
        }
    }

    $richts = @()
    $richtIndex = @{}
    foreach ($spec in @($RichtSpecs)) {
        $parts = @(Split-AnnotationSpec $spec 4 'GavornRicht')
        $matrixId, $authorityId, $rightId, $rule = $parts
        Assert-AnnotationId $matrixId 'Gavornricht matrix id'
        Assert-AnnotationId $authorityId 'Gavornricht authority id'
        Assert-AnnotationId $rightId 'Gavornricht right id'
        if (-not $matrixIndex.ContainsKey($matrixId.ToLowerInvariant())) {
            throw "Gavornricht '$authorityId/$rightId' references unknown September matrix '$matrixId'"
        }
        $key = '{0}::{1}::{2}' -f
            $matrixId.ToLowerInvariant(), $authorityId.ToLowerInvariant(),
            $rightId.ToLowerInvariant()
        if ($richtIndex.ContainsKey($key)) {
            throw "Duplicate gavornricht '$matrixId/$authorityId/$rightId'"
        }
        $richtIndex[$key] = $true
        $richts += [pscustomobject][ordered]@{
            MatrixId = $matrixId
            AuthorityId = $authorityId
            RightId = $rightId
            Rule = $rule
        }
    }
    if ($cells.Count -eq 0 -and $richts.Count -gt 0) {
        throw 'Gavornricht entries require at least one September matrix cell'
    }

    $matrices = @($cells | Group-Object MatrixId | Sort-Object Name |
        ForEach-Object {
            $matrixId = [string]$_.Name
            $matrixCells = @($_.Group | Sort-Object RowId, ColumnId)
            $matrixRichts = @($richts |
                Where-Object { $_.MatrixId -ieq $matrixId } |
                Sort-Object AuthorityId, RightId)
            if ($matrixRichts.Count -eq 0) {
                throw "September matrix '$matrixId' has no gavornricht governance annotation"
            }
            $weightSum = 0.0
            foreach ($cell in $matrixCells) { $weightSum += [double]$cell.Weight }
            [pscustomobject][ordered]@{
                MatrixId = $matrixId
                PriorBinding = $PriorBinding
                RowCategories = @($matrixCells.RowId | Sort-Object -Unique)
                ColumnCategories = @($matrixCells.ColumnId | Sort-Object -Unique)
                CellCount = [int]$matrixCells.Count
                WeightSum = [double][Math]::Round($weightSum, 6)
                Cells = $matrixCells
                GavornRichts = $matrixRichts
            }
        })
    return [ordered]@{
        Status = if ($matrices.Count -gt 0) { 'Coalesced with prior governance' } else { 'No September matrices supplied' }
        PriorBinding = $PriorBinding
        MatrixCount = [int]$matrices.Count
        CellCount = [int]$cells.Count
        GavornRichtCount = [int]$richts.Count
        Matrices = $matrices
    }
}

function Test-StoredSeptemberMatrix($Annotation, [string]$ExpectedPriorBinding) {
    if ($null -eq $Annotation) {
        throw 'schema 4 record has no September matrix annotation'
    }
    if ([string]$Annotation.PriorBinding -cne $ExpectedPriorBinding) {
        throw 'September matrix prior binding does not match the record chain'
    }
    $cellSpecs = @()
    $richtSpecs = @()
    foreach ($matrix in @($Annotation.Matrices)) {
        foreach ($cell in @($matrix.Cells)) {
            $weight = ([double]$cell.Weight).ToString(
                'R', [Globalization.CultureInfo]::InvariantCulture)
            $cellSpecs += '{0}::{1}::{2}::{3}::{4}' -f
                $matrix.MatrixId, $cell.RowId, $cell.ColumnId,
                $weight, $cell.Annotation
        }
        foreach ($richt in @($matrix.GavornRichts)) {
            $richtSpecs += '{0}::{1}::{2}::{3}' -f
                $matrix.MatrixId, $richt.AuthorityId,
                $richt.RightId, $richt.Rule
        }
    }
    $rebuilt = New-SeptemberMatrixAnnotation `
        -CellSpecs @($cellSpecs) `
        -RichtSpecs @($richtSpecs) `
        -PriorBinding $ExpectedPriorBinding
    $storedCanonical = $Annotation | ConvertTo-Json -Depth 12 -Compress
    $rebuiltCanonical = $rebuilt | ConvertTo-Json -Depth 12 -Compress
    if ($storedCanonical -cne $rebuiltCanonical) {
        throw 'stored September matrix does not match its validated reconstruction'
    }
}

function New-ScriptEnvironmentAnnotation(
    [string]$Name,
    $AnnotationGraph,
    $SeptemberAnnotation
) {
    Assert-AnnotationId $Name 'Script environment id'
    if ($null -eq $AnnotationGraph -or $null -eq $SeptemberAnnotation) {
        throw 'script environment requires graph and September annotations'
    }
    $graphCanonical = $AnnotationGraph | ConvertTo-Json -Depth 12 -Compress
    $matrixCanonical = $SeptemberAnnotation | ConvertTo-Json -Depth 12 -Compress
    $graphDigest = Get-Digest $graphCanonical
    $matrixDigest = Get-Digest $matrixCanonical
    $mediumSpecs = @(
        [ordered]@{
            MediumId = 'IntegrityRecord'
            Projection = 'Canonical JSON payload'
            DataSource = 'AnnotationGraph;SeptemberMatrixAnnotation'
            AnnotationHandling = 'DataOnly'
        }
        [ordered]@{
            MediumId = 'MarkdownReport'
            Projection = 'Readable graph, matrix, and governance tables'
            DataSource = 'AnnotationGraph;SeptemberMatrixAnnotation'
            AnnotationHandling = 'DataOnly'
        }
        [ordered]@{
            MediumId = 'ConsoleSummary'
            Projection = 'Counts and shared commitment'
            DataSource = 'AnnotationGraph;SeptemberMatrixAnnotation'
            AnnotationHandling = 'DataOnly'
        }
    )
    $commitmentSource = [ordered]@{
        Name = $Name
        Mode = 'BlindThrough'
        PriorBinding = [string]$SeptemberAnnotation.PriorBinding
        GraphSha256 = $graphDigest
        SeptemberMatrixSha256 = $matrixDigest
        Mediums = $mediumSpecs
    }
    $commitmentCanonical = $commitmentSource |
        ConvertTo-Json -Depth 6 -Compress
    $coalescedDigest = Get-Digest $commitmentCanonical
    $mediumBindings = @($mediumSpecs | ForEach-Object {
        [pscustomobject][ordered]@{
            MediumId = $_.MediumId
            Projection = $_.Projection
            DataSource = $_.DataSource
            AnnotationHandling = $_.AnnotationHandling
            CoalescedSha256 = $coalescedDigest
        }
    })
    return [ordered]@{
        Name = $Name
        Mode = 'BlindThrough'
        Evaluation = 'Disabled'
        AnnotationHandling = 'DataOnly'
        PriorBinding = [string]$SeptemberAnnotation.PriorBinding
        GraphSha256 = $graphDigest
        SeptemberMatrixSha256 = $matrixDigest
        Mediums = $mediumBindings
        CoalescedSha256 = $coalescedDigest
        GraphGroups = [int]$AnnotationGraph.GroupCount
        GraphNodes = [int]$AnnotationGraph.NodeCount
        CategoryAnnotations = [int]$AnnotationGraph.CategoryAnnotations.Count
        RichtDivisions = [int]$AnnotationGraph.DivisionCount
        SeptemberMatrices = [int]$SeptemberAnnotation.MatrixCount
        SeptemberMatrixCells = [int]$SeptemberAnnotation.CellCount
        GavornRichts = [int]$SeptemberAnnotation.GavornRichtCount
    }
}

function Test-StoredScriptEnvironment(
    $Environment,
    $AnnotationGraph,
    $SeptemberAnnotation
) {
    if ($null -eq $Environment) {
        throw 'schema 6 record has no script environment annotation'
    }
    if ([string]$Environment.Mode -cne 'BlindThrough' -or
        [string]$Environment.Evaluation -cne 'Disabled' -or
        [string]$Environment.AnnotationHandling -cne 'DataOnly') {
        throw 'script environment is not operating through the blind data boundary'
    }
    $rebuilt = New-ScriptEnvironmentAnnotation `
        -Name ([string]$Environment.Name) `
        -AnnotationGraph $AnnotationGraph `
        -SeptemberAnnotation $SeptemberAnnotation
    $storedCanonical = $Environment | ConvertTo-Json -Depth 12 -Compress
    $rebuiltCanonical = $rebuilt | ConvertTo-Json -Depth 12 -Compress
    if ($storedCanonical -cne $rebuiltCanonical) {
        throw 'stored script environment does not match its coalesced reconstruction'
    }
}

function Test-StoredScriptEnvironmentV5(
    $Environment,
    $AnnotationGraph,
    $SeptemberAnnotation
) {
    if ($null -eq $Environment -or
        [string]$Environment.Mode -cne 'BlindThrough' -or
        [string]$Environment.Evaluation -cne 'Disabled' -or
        [string]$Environment.AnnotationHandling -cne 'DataOnly') {
        throw 'schema 5 script environment has invalid blind-through settings'
    }
    $graphCanonical = $AnnotationGraph | ConvertTo-Json -Depth 12 -Compress
    $matrixCanonical = $SeptemberAnnotation | ConvertTo-Json -Depth 12 -Compress
    $graphDigest = Get-Digest $graphCanonical
    $matrixDigest = Get-Digest $matrixCanonical
    $commitmentSource = [ordered]@{
        Name = [string]$Environment.Name
        Mode = 'BlindThrough'
        PriorBinding = [string]$SeptemberAnnotation.PriorBinding
        GraphSha256 = $graphDigest
        SeptemberMatrixSha256 = $matrixDigest
    }
    $commitmentCanonical = $commitmentSource |
        ConvertTo-Json -Depth 6 -Compress
    $expected = [ordered]@{
        Name = [string]$Environment.Name
        Mode = 'BlindThrough'
        Evaluation = 'Disabled'
        AnnotationHandling = 'DataOnly'
        PriorBinding = [string]$SeptemberAnnotation.PriorBinding
        GraphSha256 = $graphDigest
        SeptemberMatrixSha256 = $matrixDigest
        CoalescedSha256 = (Get-Digest $commitmentCanonical)
        GraphGroups = [int]$AnnotationGraph.GroupCount
        GraphNodes = [int]$AnnotationGraph.NodeCount
        CategoryAnnotations = [int]$AnnotationGraph.CategoryAnnotations.Count
        RichtDivisions = [int]$AnnotationGraph.DivisionCount
        SeptemberMatrices = [int]$SeptemberAnnotation.MatrixCount
        SeptemberMatrixCells = [int]$SeptemberAnnotation.CellCount
        GavornRichts = [int]$SeptemberAnnotation.GavornRichtCount
    }
    $storedCanonical = $Environment | ConvertTo-Json -Depth 12 -Compress
    $expectedCanonical = $expected | ConvertTo-Json -Depth 12 -Compress
    if ($storedCanonical -cne $expectedCanonical) {
        throw 'schema 5 script environment does not match its legacy commitment'
    }
}

function Read-Records([string]$Folder) {
    $files = @(Get-ChildItem -LiteralPath $Folder -Filter '*.json' -File | Sort-Object Name)
    $previousHash = $null
    foreach ($file in $files) {
        try {
            $item = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($null -ne $item.PSObject.Properties['PayloadJson']) {
                # Schema 2 and later hash this exact string without a JSON round trip.
                $canonical = [string]$item.PayloadJson
                $payload = $canonical | ConvertFrom-Json
            } else {
                # Accept schema 1 records produced by the original package.
                $payload = $item.Payload
                $canonical = $payload | ConvertTo-Json -Depth 12 -Compress
            }
            $hash = [string]$item.PayloadSha256
            if ($hash -notmatch '^[0-9a-f]{64}$' -or (Get-Digest $canonical) -ne $hash) {
                throw 'payload digest mismatch'
            }
            if ([string]$payload.PreviousRecordSha256 -ne [string]$previousHash) {
                throw 'link to previous record does not match'
            }
            if ([int]$payload.Schema -ge 3) {
                Test-StoredAnnotationGraph $payload.AnnotationGraph
            }
            if ([int]$payload.Schema -ge 4) {
                $expectedPrior = if ($null -eq $previousHash) {
                    'GENESIS'
                } else {
                    [string]$previousHash
                }
                Test-StoredSeptemberMatrix `
                    -Annotation $payload.SeptemberMatrixAnnotation `
                    -ExpectedPriorBinding $expectedPrior
            }
            if ([int]$payload.Schema -ge 6) {
                Test-StoredScriptEnvironment `
                    -Environment $payload.ScriptEnvironment `
                    -AnnotationGraph $payload.AnnotationGraph `
                    -SeptemberAnnotation $payload.SeptemberMatrixAnnotation
            } elseif ([int]$payload.Schema -ge 5) {
                Test-StoredScriptEnvironmentV5 `
                    -Environment $payload.ScriptEnvironment `
                    -AnnotationGraph $payload.AnnotationGraph `
                    -SeptemberAnnotation $payload.SeptemberMatrixAnnotation
            }
            [pscustomobject]@{ File = $file.FullName; Payload = $payload; PayloadSha256 = $hash }
            $previousHash = $hash
        } catch {
            throw "Record verification failed for $($file.FullName): $($_.Exception.Message)"
        }
    }
}

function Get-Observation {
    $os = Get-CimInstance -ClassName Win32_OperatingSystem
    $procs = @(Get-Process | Group-Object ProcessName | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{ Name = [string]$_.Name; Count = [int]$_.Count }
    })
    $services = @(Get-Service | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{ Name = [string]$_.Name; Status = [string]$_.Status }
    })
    return [ordered]@{
        Utc = [DateTime]::UtcNow.ToString('o')
        OsCaption = [string]$os.Caption
        TotalMemoryMiB = [int64][Math]::Round([double]$os.TotalVisibleMemorySize / 1024)
        FreeMemoryMiB = [int64][Math]::Round([double]$os.FreePhysicalMemory / 1024)
        ProcessCounts = $procs
        Services = $services
    }
}

function Find-ProcessCount($Observation, [string]$Name) {
    $match = @($Observation.ProcessCounts | Where-Object { $_.Name -ieq $Name })
    if ($match.Count -eq 0) { return 0 }
    return [int]$match[0].Count
}

function Find-ServiceStatus($Observation, [string]$Name) {
    $match = @($Observation.Services | Where-Object { $_.Name -ieq $Name })
    if ($match.Count -eq 0) { return 'Absent' }
    return [string]$match[0].Status
}

function Escape-Cell([object]$Value) {
    return ([string]$Value).Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
}

New-Item -ItemType Directory -Path $DataDirectory -Force | Out-Null
$records = @(Read-Records $DataDirectory)
if ($Verify) {
    [pscustomobject]@{
        Status = 'Verified'
        Records = $records.Count
        LastDigest = if ($records.Count -gt 0) { $records[-1].PayloadSha256 } else { $null }
    }
    return
}
$prior = if ($records.Count -gt 0) { $records[-1] } else { $null }
$now = Get-Observation
$before = if ($null -ne $prior) { $prior.Payload.Now } else { $null }
$changes = @()
if ($null -ne $before) {
    $changes += [pscustomobject]@{
        Item = 'Free memory MiB'
        Before = [string]$before.FreeMemoryMiB
        Now = [string]$now.FreeMemoryMiB
    }
    $names = @($before.ProcessCounts.Name) + @($now.ProcessCounts.Name) |
        Sort-Object -Unique
    foreach ($name in $names) {
        $old = Find-ProcessCount $before $name
        $new = Find-ProcessCount $now $name
        if ($old -ne $new) {
            $changes += [pscustomobject]@{ Item = "Process $name"; Before = "$old"; Now = "$new" }
        }
    }
    $serviceNames = @($before.Services.Name) + @($now.Services.Name) |
        Sort-Object -Unique
    foreach ($name in $serviceNames) {
        $old = Find-ServiceStatus $before $name
        $new = Find-ServiceStatus $now $name
        if ($old -ne $new) {
            $changes += [pscustomobject]@{ Item = "Service $name"; Before = $old; Now = $new }
        }
    }
}

$checks = @()
if ($null -ne $prior) {
    foreach ($name in @($prior.Payload.Forward.ExpectedProcess)) {
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        $count = Find-ProcessCount $now $name
        $checks += [pscustomobject]@{ Expectation = "Process $name running"; Observed = "Count $count"; Met = ($count -gt 0) }
    }
    foreach ($name in @($prior.Payload.Forward.ExpectedService)) {
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        $status = Find-ServiceStatus $now $name
        $checks += [pscustomobject]@{ Expectation = "Service $name running"; Observed = $status; Met = ($status -eq 'Running') }
    }
}

$annotationGraph = New-AnnotationGraph `
    -GroupSpecs @($GraphGroup) `
    -LinkSpecs @($GraphLink) `
    -AnnotationSpecs @($CategoryAnnotation)
$priorBinding = if ($null -ne $prior) {
    [string]$prior.PayloadSha256
} else {
    'GENESIS'
}
$septemberMatrixAnnotation = New-SeptemberMatrixAnnotation `
    -CellSpecs @($SeptemberMatrix) `
    -RichtSpecs @($GavornRicht) `
    -PriorBinding $priorBinding
$scriptEnvironmentAnnotation = New-ScriptEnvironmentAnnotation `
    -Name $ScriptEnvironment `
    -AnnotationGraph $annotationGraph `
    -SeptemberAnnotation $septemberMatrixAnnotation

$payload = [ordered]@{
    Schema = 6
    PreviousRecordSha256 = if ($null -ne $prior) { [string]$prior.PayloadSha256 } else { $null }
    OnceWas = if ($null -ne $before) { [ordered]@{
        Utc = $before.Utc
        FreeMemoryMiB = $before.FreeMemoryMiB
        ProcessCounts = $before.ProcessCounts
        Services = $before.Services
    } } else { $null }
    Now = $now
    Changes = $changes
    PreviousForwardChecks = $checks
    Dialogue = [ordered]@{
        HumanStatement = $HumanNote
        AssistantProposal = $AssistantProposal
        Provenance = 'Supplied as command-line notes; not machine-verified'
    }
    AnnotationGraph = $annotationGraph
    SeptemberMatrixAnnotation = $septemberMatrixAnnotation
    ScriptEnvironment = $scriptEnvironmentAnnotation
    Forward = [ordered]@{
        Status = 'Unverified until next capture'
        ExpectedProcess = @($ExpectedProcess | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
        ExpectedService = @($ExpectedService | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
    }
}

$canonical = $payload | ConvertTo-Json -Depth 12 -Compress
$record = [ordered]@{ PayloadJson = $canonical; PayloadSha256 = (Get-Digest $canonical) }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')
$base = Join-Path $DataDirectory $stamp
$jsonPath = "$base.json"
$markdownPath = "$base.md"
$tempPath = "$jsonPath.tmp"
$record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $tempPath -Encoding UTF8
Move-Item -LiteralPath $tempPath -Destination $jsonPath -ErrorAction Stop

$lines = @(
    '# PowerShell forward pass annotation', ''
    "Captured (UTC): $($now.Utc)", ''
    '## Once was', ''
)
if ($null -eq $before) { $lines += 'No previous capture exists.' }
else {
    $lines += "Previous capture: $($before.Utc). Free memory: $($before.FreeMemoryMiB) MiB."
    $lines += ''
    $lines += '| Observed item | Before | Now |'
    $lines += '| --- | ---: | ---: |'
    if ($changes.Count -eq 0) { $lines += '| Selected observations | No measured change | No measured change |' }
    foreach ($change in $changes) {
        $lines += '| {0} | {1} | {2} |' -f (Escape-Cell $change.Item), (Escape-Cell $change.Before), (Escape-Cell $change.Now)
    }
}
$lines += @('', '## Happening now', '',
    "OS: $($now.OsCaption). Free physical memory: $($now.FreeMemoryMiB) / $($now.TotalMemoryMiB) MiB.",
    "Distinct process names: $(@($now.ProcessCounts).Count). Services observed: $(@($now.Services).Count).", '',
    "Human statement (supplied): $(Escape-Cell $HumanNote)",
    "Assistant proposal (supplied): $(Escape-Cell $AssistantProposal)", '',
    '## Previous forward pass checked now', ''
)
if ($checks.Count -eq 0) { $lines += 'No earlier expectations to check.' }
else {
    foreach ($check in $checks) {
        $outcome = if ($check.Met) { 'met' } else { 'unmet' }
        $lines += "- $(Escape-Cell $check.Expectation): $outcome ($($check.Observed))."
    }
}
$lines += @('', '## Annotation graph groups', '')
if ($annotationGraph.GroupCount -eq 0) {
    $lines += 'No graph groups were supplied.'
} else {
    $lines += '| Group | Node | Label |'
    $lines += '| --- | --- | --- |'
    foreach ($group in $annotationGraph.Groups) {
        foreach ($node in $group.Nodes) {
            $lines += '| {0} | {1} | {2} |' -f
                (Escape-Cell $group.GroupId),
                (Escape-Cell $node.NodeId),
                (Escape-Cell $node.Label)
        }
    }
    $lines += @('', '### Graph links', '')
    if ($annotationGraph.LinkCount -eq 0) {
        $lines += 'The declared nodes have no links.'
    } else {
        $lines += '| Group | From | To | Relation |'
        $lines += '| --- | --- | --- | --- |'
        foreach ($group in $annotationGraph.Groups) {
            foreach ($link in $group.Links) {
                $lines += '| {0} | {1} | {2} | {3} |' -f
                    (Escape-Cell $group.GroupId),
                    (Escape-Cell $link.FromNode),
                    (Escape-Cell $link.ToNode),
                    (Escape-Cell $link.Relation)
            }
        }
    }
}
$lines += @('', '## Category annotations', '')
if ($annotationGraph.CategoryCount -eq 0) {
    $lines += 'No category annotations were supplied.'
} else {
    $lines += '| Richt division | Category | Group | Node | Annotation |'
    $lines += '| --- | --- | --- | --- | --- |'
    foreach ($annotation in $annotationGraph.CategoryAnnotations) {
        $lines += '| {0} | {1} | {2} | {3} | {4} |' -f
            (Escape-Cell $annotation.RichtDivision),
            (Escape-Cell $annotation.CategoryId),
            (Escape-Cell $annotation.GroupId),
            (Escape-Cell $annotation.NodeId),
            (Escape-Cell $annotation.Text)
    }
    $lines += @('', '### Richt divisions', '')
    $lines += '| Division | Annotations | Distinct nodes |'
    $lines += '| --- | ---: | ---: |'
    foreach ($division in $annotationGraph.RichtDivisions) {
        $lines += '| {0} | {1} | {2} |' -f
            (Escape-Cell $division.RichtDivision),
            $division.AnnotationCount,
            $division.NodeCount
    }
}
$lines += @('', '## September matrix annotation', '')
if ($septemberMatrixAnnotation.MatrixCount -eq 0) {
    $lines += 'No September matrices were supplied.'
    $lines += "Prior binding: $(Escape-Cell $septemberMatrixAnnotation.PriorBinding)."
} else {
    $lines += "Prior binding: $(Escape-Cell $septemberMatrixAnnotation.PriorBinding)."
    $lines += ''
    $lines += '| Matrix | Rows | Columns | Cells | Weight sum | Gavornrichts |'
    $lines += '| --- | --- | --- | ---: | ---: | ---: |'
    foreach ($matrix in $septemberMatrixAnnotation.Matrices) {
        $rows = @($matrix.RowCategories) -join ', '
        $columns = @($matrix.ColumnCategories) -join ', '
        $lines += '| {0} | {1} | {2} | {3} | {4:N6} | {5} |' -f
            (Escape-Cell $matrix.MatrixId),
            (Escape-Cell $rows),
            (Escape-Cell $columns),
            $matrix.CellCount,
            $matrix.WeightSum,
            (@($matrix.GavornRichts).Count)
    }
    $lines += @('', '### Coalesced matrix cells', '')
    $lines += '| Matrix | Row category | Column category | Weight | Annotation |'
    $lines += '| --- | --- | --- | ---: | --- |'
    foreach ($matrix in $septemberMatrixAnnotation.Matrices) {
        foreach ($cell in $matrix.Cells) {
            $lines += '| {0} | {1} | {2} | {3:N6} | {4} |' -f
                (Escape-Cell $matrix.MatrixId),
                (Escape-Cell $cell.RowId),
                (Escape-Cell $cell.ColumnId),
                $cell.Weight,
                (Escape-Cell $cell.Annotation)
        }
    }
    $lines += @('', '### Gavornricht governance', '')
    $lines += '| Matrix | Authority | Right | Rule |'
    $lines += '| --- | --- | --- | --- |'
    foreach ($matrix in $septemberMatrixAnnotation.Matrices) {
        foreach ($richt in $matrix.GavornRichts) {
            $lines += '| {0} | {1} | {2} | {3} |' -f
                (Escape-Cell $matrix.MatrixId),
                (Escape-Cell $richt.AuthorityId),
                (Escape-Cell $richt.RightId),
                (Escape-Cell $richt.Rule)
        }
    }
}
$lines += @('', '## Script environment', '')
$lines += '| Environment | Mode | Evaluation | Annotation handling | Prior binding |'
$lines += '| --- | --- | --- | --- | --- |'
$lines += '| {0} | {1} | {2} | {3} | {4} |' -f
    (Escape-Cell $scriptEnvironmentAnnotation.Name),
    (Escape-Cell $scriptEnvironmentAnnotation.Mode),
    (Escape-Cell $scriptEnvironmentAnnotation.Evaluation),
    (Escape-Cell $scriptEnvironmentAnnotation.AnnotationHandling),
    (Escape-Cell $scriptEnvironmentAnnotation.PriorBinding)
$lines += @('', '### Blind-through commitments', '')
$lines += '| Component | SHA-256 |'
$lines += '| --- | --- |'
$lines += '| Annotation graph | {0} |' -f
    (Escape-Cell $scriptEnvironmentAnnotation.GraphSha256)
$lines += '| September matrices | {0} |' -f
    (Escape-Cell $scriptEnvironmentAnnotation.SeptemberMatrixSha256)
$lines += '| Coalesced environment | {0} |' -f
    (Escape-Cell $scriptEnvironmentAnnotation.CoalescedSha256)
$lines += @('',
    'Annotation values cross this environment as inert data. The script does not invoke, dot-source, or evaluate them.')
$lines += @('', '### Coalesced output media', '')
$lines += '| Medium | Projection | Shared commitment | Handling |'
$lines += '| --- | --- | --- | --- |'
foreach ($medium in $scriptEnvironmentAnnotation.Mediums) {
    $lines += '| {0} | {1} | {2} | {3} |' -f
        (Escape-Cell $medium.MediumId),
        (Escape-Cell $medium.Projection),
        (Escape-Cell $medium.CoalescedSha256),
        (Escape-Cell $medium.AnnotationHandling)
}
$lines += @('', '## Forward', '', 'These expectations are proposed; the next capture will check them.')
foreach ($name in $payload.Forward.ExpectedProcess) { $lines += "- Expect process $(Escape-Cell $name) to be running." }
foreach ($name in $payload.Forward.ExpectedService) { $lines += "- Expect service $(Escape-Cell $name) to be running." }
if ($payload.Forward.ExpectedProcess.Count + $payload.Forward.ExpectedService.Count -eq 0) {
    $lines += '- No machine condition was specified.'
}
$lines += @('', "Record SHA-256: $($record.PayloadSha256)")
$lines | Set-Content -LiteralPath $markdownPath -Encoding UTF8

[pscustomobject]@{
    Record = $jsonPath
    Report = $markdownPath
    PreviousChecks = $checks.Count
    Changes = $changes.Count
    GraphGroups = $annotationGraph.GroupCount
    GraphNodes = $annotationGraph.NodeCount
    CategoryAnnotations = $annotationGraph.CategoryAnnotations.Count
    RichtDivisions = $annotationGraph.DivisionCount
    SeptemberMatrices = $septemberMatrixAnnotation.MatrixCount
    SeptemberMatrixCells = $septemberMatrixAnnotation.CellCount
    GavornRichts = $septemberMatrixAnnotation.GavornRichtCount
    ScriptEnvironment = $scriptEnvironmentAnnotation.Name
    EnvironmentMode = $scriptEnvironmentAnnotation.Mode
    MediumBindings = @($scriptEnvironmentAnnotation.Mediums)
    CoalescedSha256 = $scriptEnvironmentAnnotation.CoalescedSha256
}
