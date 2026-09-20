import LeanAT.Reference.E39Main

def main (args : List String) : IO Unit := do
  let [descriptor,profile,digest,program,input,output] := args
    | throw (IO.userError "expected descriptor profile digest program input output")
  let some program := program.toNat? | throw (IO.userError "invalid program ID")
  LeanAT.Reference.E39Main.runDescriptorFile descriptor profile digest program input output
