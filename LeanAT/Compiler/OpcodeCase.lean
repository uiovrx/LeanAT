import LeanAT.Reference.Json
import LeanAT.Frontend.Source

namespace LeanAT.Compiler

/-- Executable source fixture inputs, never canned backend outputs. -/
structure OpcodeCase where
  id : String
  variant : String := "success"
  providerFamily : String := "pure"
  model : ModelIR.Project
  handlerId : Nat := 0
  programId : Nat := 0
  input : Reference.JsonIO.Input := {}
  expectedOpcodeTags : List Nat
  expectedOutcome : String := "success"
  profile : String := "AT-Core-1.1-draft"
  sourceBundle : SourceMapping.Bundle := {}
  sourceModule : String
  sourceFiles : List String

end LeanAT.Compiler
