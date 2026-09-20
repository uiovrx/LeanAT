import LeanAT.Compiler.OpcodeCases.Pure
import LeanAT.Compiler.OpcodeCases.Storage
import LeanAT.Compiler.OpcodeCases.Core
import LeanAT.Compiler.OpcodeCases.Protocol
import LeanAT.Compiler.OpcodeCases.Objects
import LeanAT.Compiler.OpcodeCases.Optional
import LeanAT.Compiler.OpcodeCases.Structured
import LeanAT.Compiler.OpcodeCases.Sideband

namespace LeanAT.Compiler.OpcodeCases.All

def cases : Except String (List OpcodeCase) := do
  let pureCases ← Pure.cases
  let storage ← Storage.cases
  let core ← Core.cases
  let protocol ← Protocol.cases
  let objects ← Objects.cases
  let optional ← Optional.cases
  let structured ← Structured.cases
  let sideband ← Sideband.cases
  pure (pureCases ++ storage ++ core ++ protocol ++ objects ++ optional ++ structured ++ sideband)

end LeanAT.Compiler.OpcodeCases.All