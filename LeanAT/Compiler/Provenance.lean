import LeanAT.Frontend.Source

namespace LeanAT.Compiler

/-- Locations are semantic descriptor addresses, never byte offsets into host structs. -/
structure LoweredOrigin where
  program : Nat := 0
  kind : String
  block : Option Nat := none
  instruction : Option Nat := none
  node : String
  deriving Repr, BEq, Inhabited

abbrev ModelNode := SourceMapping.ModelNode
abbrev expressionNodes := SourceMapping.expressionNodes
abbrev statementNodes := SourceMapping.statementNodes
abbrev handlerNodes := SourceMapping.handlerNodes

end LeanAT.Compiler