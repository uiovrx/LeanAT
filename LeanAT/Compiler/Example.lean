import LeanAT.Compiler.Main

def main (args : List String) : IO UInt32 := LeanAT.Compiler.runCLI LeanAT.Compiler.scalarExample args
