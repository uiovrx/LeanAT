import LeanAT.Compiler.Main

open LeanAT LeanAT.Compiler

def traced : ModelIR.Project := {
  types := [.bool,.bits 64]
  states := [⟨0,1,.bits 64 3⟩]
  handlers := [{id := 0, body := [
    .emit "before" [.state 1 0],
    .writeState 0 (.literal 1 (.bits 64 18446744073709551615)),
    .emit "after" [.state 1 0],
    .ret [.state 1 0]]}]
}

def rollback : ModelIR.Project := {
  types := [.bool,.bits 64]
  states := [⟨0,1,.bits 64 7⟩]
  handlers := [{id := 0, body := [
    .writeState 0 (.literal 1 (.bits 64 99)),
    .emit "discarded" [.state 1 0],
    .check (.literal 0 (.bool false)) "profile rollback"]}]
}

def main (args : List String) : IO Unit := do
  let [output] := args | throw (IO.userError "expected output directory")
  IO.FS.createDirAll output
  for (name,model) in [("lazy-state",scalarExample),("trace-state",traced),("rollback",rollback)] do
    for (suffix,profile) in [("core","AT-Core-1.1-draft"),("ext","AT-Ext-1.1-draft")] do
      let executable ← match compileForProfile model profile with
        | .ok p => pure p
        | .error e => throw (IO.userError s!"{name}/{suffix}: {e}")
      let bytes := serialize executable
      match deserialize bytes {expectedProfile := profile} with
      | .error e => throw (IO.userError e)
      | .ok _ => pure ()
      let wrong := if suffix == "core" then "AT-Ext-1.1-draft" else "AT-Core-1.1-draft"
      match deserialize bytes {expectedProfile := wrong} with
      | .error "ProfileMismatch" => pure ()
      | _ => throw (IO.userError "wrong profile policy was not rejected")
      IO.FS.writeBinFile (output ++ "/" ++ name ++ "." ++ suffix ++ ".bin") bytes
  match compileForProfile scalarExample "invalid-profile" with
  | .error _ => pure ()
  | .ok _ => throw (IO.userError "unknown profile accepted")
  IO.println "actual compileForProfile and Lean loader policy controls passed"
