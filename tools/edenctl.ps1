# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Windows JSON-lines client for Eden --control-socket named pipes.
#requires -Version 7.0
[CmdletBinding()]
param([string]$PipeName='eden-control',[string]$Method='ping',[string]$ParamsJson='{}',[ValidateRange(1,3600000)][int]$TimeoutMs=10000)
$ErrorActionPreference='Stop'
if(-not $IsWindows){throw 'Use tools/edenctl.py for Unix control sockets'}
$id=[Guid]::NewGuid().ToString('N')
$params=ConvertFrom-Json -AsHashtable $ParamsJson
if($params -isnot [System.Collections.IDictionary]){throw 'ParamsJson must be a JSON object'}
$request=@{id=$id;method=$Method;params=$params}|ConvertTo-Json -Compress -Depth 30
$bytes=[Text.Encoding]::UTF8.GetBytes($request+"`n")
if($bytes.Length -gt 1MB){throw 'Request exceeds 1 MiB'}
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.', $PipeName,[IO.Pipes.PipeDirection]::InOut,[IO.Pipes.PipeOptions]::Asynchronous)
$timer=[Diagnostics.Stopwatch]::StartNew()
try {
 $pipe.Connect($TimeoutMs)
 $write=$pipe.WriteAsync($bytes,0,$bytes.Length)
 if(-not $write.Wait([Math]::Max(1,$TimeoutMs-$timer.ElapsedMilliseconds))){throw 'Write timed out'}
 $data=[IO.MemoryStream]::new()
 $buffer=[byte[]]::new(65536)
 do {
  $read=$pipe.ReadAsync($buffer,0,$buffer.Length)
  if(-not $read.Wait([Math]::Max(1,$TimeoutMs-$timer.ElapsedMilliseconds))){throw 'Reply timed out'}
  $n=$read.Result
  if($n -eq 0){throw 'Disconnected before reply'}
  $data.Write($buffer,0,$n)
  if($data.Length -gt 16MB){throw 'Response exceeds 16 MiB'}
  $newline=[Array]::IndexOf($buffer,[byte]10,0,$n)
 } while($newline -lt 0)
 $line=([Text.Encoding]::UTF8.GetString($data.ToArray()) -split "`n",2)[0]
 $response=$line|ConvertFrom-Json
 if($response.id -ne $id){throw 'Reply ID mismatch'}
 $line
} finally { $pipe.Dispose() }