[CmdletBinding()]
param(
    [string]$RuntimeDirectory = (Join-Path $PSScriptRoot 'runtime'),
    [switch]$UseClang
)

$fixtureBytes = [byte[]]@(0xB8, 0x34, 0x12, 0xBB, 0x78, 0x56, 0xBE, 0x00, 0x02, 0xC6, 0x04, 0x41, 0xC7, 0x44, 0x01, 0x43, 0x42, 0x8A, 0x04, 0x8B, 0x54, 0x01, 0x90, 0xCD, 0x20)
$loopFixtureBytes = [byte[]]@(0xEB, 0xFE)
$conditionFixtureBytes = [byte[]]@(0xB9, 0x05, 0x00, 0x31, 0xC0, 0x40, 0x90, 0xE2, 0xFC, 0xCD, 0x20)
$semanticInterruptFixtureBytes = [byte[]]@(0xB8, 0x00, 0x30, 0xCD, 0x21, 0xB8, 0x07, 0x4C, 0xCD, 0x21)
$traceEffectsFixtureBytes = [byte[]]@(0xBE, 0x13, 0x01, 0xC7, 0x04, 0x34, 0x12, 0x8B, 0x04, 0xBA, 0x80, 0x00, 0xEE, 0xEC, 0xB8, 0x00, 0x4C, 0xCD, 0x21, 0x00, 0x00)
$deviceInputFixtureBytes = [byte[]]@(
    0xB8, 0x40, 0x00,                         # mov ax,0040h
    0x8E, 0xC0,                               # mov es,ax
    0x26, 0xF6, 0x06, 0x17, 0x00, 0x02,       # test byte es:[0017h],02h (left Shift)
    0x74, 0xF8,                               # jz wait-down
    0xC6, 0x06, 0x00, 0x02, 0xD1,             # mov byte [0200h],D1h
    0x26, 0xF6, 0x06, 0x17, 0x00, 0x02,       # test byte es:[0017h],02h
    0x75, 0xF8,                               # jnz wait-up
    0xC6, 0x06, 0x01, 0x02, 0xD0,             # mov byte [0201h],D0h
    0xBA, 0x01, 0x02,                         # mov dx,0201h
    0xEE,                                     # out dx,al (start joystick timing)
    0xEC,                                     # in al,dx
    0xA2, 0x02, 0x02,                         # mov [0202h],al
    0xEB, 0xFE                                # loop forever
)
$stepOverCallFixtureBytes = [byte[]]@(0xE8, 0x05, 0x00, 0xBB, 0x78, 0x56, 0xCD, 0x20, 0xB8, 0x34, 0x12, 0xC3)
$stepOverInterruptFixtureBytes = [byte[]]@(0xCD, 0x2F, 0xBB, 0x78, 0x56, 0xCD, 0x20)
$stepOverRepFixtureBytes = [byte[]]@(0xBE, 0x0E, 0x01, 0xBF, 0x10, 0x01, 0xB9, 0x02, 0x00, 0xF3, 0xA4, 0x90, 0xCD, 0x20, 0x41, 0x42, 0x00, 0x00)
$protectedModeFixtureBytes = [byte[]]@(0xFA, 0x31, 0xC0, 0x8E, 0xC0, 0x26, 0xC7, 0x06, 0x00, 0x40, 0x03, 0x50, 0x26, 0xC7, 0x06, 0x02, 0x40, 0x00, 0x00, 0x26, 0xC7, 0x06, 0x04, 0x40, 0x00, 0x00, 0xBF, 0x00, 0x50, 0xB9, 0x10, 0x00, 0x31, 0xDB, 0x89, 0xD8, 0x83, 0xC8, 0x03, 0x26, 0x89, 0x05, 0x26, 0xC7, 0x45, 0x02, 0x00, 0x00, 0x81, 0xC3, 0x00, 0x10, 0x83, 0xC7, 0x04, 0xE2, 0xE9, 0x66, 0x31, 0xC0, 0x8C, 0xC8, 0x66, 0xC1, 0xE0, 0x04, 0x66, 0x05, 0x98, 0x01, 0x00, 0x00, 0x2E, 0x66, 0xA3, 0xBA, 0x01, 0x66, 0x2D, 0x98, 0x01, 0x00, 0x00, 0x66, 0x05, 0x72, 0x01, 0x00, 0x00, 0x2E, 0xA3, 0xBE, 0x01, 0x2E, 0x0F, 0x01, 0x16, 0xB8, 0x01, 0x0F, 0x20, 0xC0, 0x66, 0x83, 0xC8, 0x01, 0x0F, 0x22, 0xC0, 0x2E, 0xFF, 0x2E, 0xBE, 0x01, 0xB8, 0x10, 0x00, 0x8E, 0xD8, 0x8E, 0xD0, 0xBC, 0xF0, 0x0F, 0x66, 0xB8, 0x00, 0x40, 0x00, 0x00, 0x0F, 0x22, 0xD8, 0x0F, 0x20, 0xC0, 0x66, 0x0D, 0x00, 0x00, 0x00, 0x80, 0x0F, 0x22, 0xC0, 0x87, 0xDB, 0xEB, 0xFC, 0x90, 0x90, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x92, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00)
$fixtureDirectory = Join-Path $PSScriptRoot 'fixtures'

New-Item -ItemType Directory -Force -Path $fixtureDirectory, $RuntimeDirectory | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_fixture.com'), $fixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGENTFIX.COM'), $fixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_loop.com'), $loopFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGENTRUN.COM'), $loopFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_condition.com'), $conditionFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGCOND.COM'), $conditionFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_semantic_interrupt.com'), $semanticInterruptFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGINT.COM'), $semanticInterruptFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_trace_effects.com'), $traceEffectsFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGFX.COM'), $traceEffectsFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_device_input.com'), $deviceInputFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGINPUT.COM'), $deviceInputFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_step_over_call.com'), $stepOverCallFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGCALL.COM'), $stepOverCallFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGENTCALL.COM'), $stepOverCallFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_step_over_interrupt.com'), $stepOverInterruptFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGENTINT.COM'), $stepOverInterruptFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_step_over_rep.com'), $stepOverRepFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGENTREP.COM'), $stepOverRepFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $fixtureDirectory 'agent_protected_mode.com'), $protectedModeFixtureBytes)
[System.IO.File]::WriteAllBytes((Join-Path $RuntimeDirectory 'AGPMODE.COM'), $protectedModeFixtureBytes)

# Keep the file-service fixture readable and reproducible. LLVM's Windows
# binaries assemble and link the 16-bit source directly; every intermediate
# stays under the requested runtime directory. For installations providing
# clang/lld-link but not llvm-mc/lld, -UseClang uses the LLVM assembler and ELF
# linker through those drivers.
$assembler = Get-Command $(if ($UseClang) { 'clang.exe' } else { 'llvm-mc.exe' }) -ErrorAction Stop
$lld = Get-Command $(if ($UseClang) { 'lld-link.exe' } else { 'lld.exe' }) -ErrorAction Stop
$fileTraceSource = Join-Path $fixtureDirectory 'agent_dos_file_trace.asm'
$fileTraceObject = Join-Path $RuntimeDirectory '_agent_dos_file_trace.o'
$fileTraceBinary = Join-Path $fixtureDirectory 'AGFILE.COM'
if ($UseClang) {
    & $assembler.Source --target=i386-pc-none-elf -x assembler -c `
        -o $fileTraceObject $fileTraceSource
} else {
    & $assembler.Source --triple=i386-pc-none-elf --filetype=obj `
        -o $fileTraceObject $fileTraceSource
}
if ($LASTEXITCODE -ne 0) { throw "LLVM assembler failed to assemble $fileTraceSource" }
& $lld.Source -flavor gnu -m elf_i386 --image-base=0 --oformat=binary `
    -Ttext=0x100 -o $fileTraceBinary $fileTraceObject
if ($LASTEXITCODE -ne 0) { throw "lld failed to link $fileTraceSource" }
Copy-Item -LiteralPath $fileTraceBinary -Destination (Join-Path $RuntimeDirectory 'AGFILE.COM') -Force
Remove-Item -LiteralPath $fileTraceObject -Force
