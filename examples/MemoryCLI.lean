import LeanAT.Frontend.BlockSyntax
import LeanAT.Compiler.Main
set_option maxRecDepth 8192
set_option maxHeartbeats 1000000

-- Run a finite memory-cell service segment; event delivery is supplied by the caller.
at_component ByteMemory where
  state cell : UInt8 := 0
  on internal.write cmd do
    set cell := 42
    return

def main (args : List String) : IO UInt32 := LeanAT.Compiler.runCLI ByteMemory.model args ByteMemory.sourceBundle
