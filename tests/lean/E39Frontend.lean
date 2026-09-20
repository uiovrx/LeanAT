import LeanAT.Frontend.BlockSyntax
import LeanAT.Compiler.Main

open LeanAT LeanAT.Compiler
set_option maxRecDepth 16384
set_option maxHeartbeats 4000000

-- 原生 source ranges include nested arithmetic and conversion operands.
@[leanat_pure] def negateFlag (value : Bool) : Bool := !value

at_component NativeExpressions where
  state cell : UInt8 := 0
  on internal.evaluate e do
    let small : UInt8 := 42
    let wide : UInt64 := UInt8.toUInt64 small
    let negative : UInt64 := -wide
    let restored : UInt64 := -negative
    let complemented : UInt64 := ~~~restored
    let equal : Bool := restored == 42
    let falseFlag : Bool := !equal
    let unequal : Bool := negateFlag equal
    let chosen : UInt64 := if unequal then 0 else restored
    let narrow : UInt8 := UInt64.toUInt8 chosen
    let tick : UInt64 := now
    set cell := narrow + UInt64.toUInt8 tick
    return

-- Aggregate operators use the registered typed Core declaration route. This does
-- not claim native Vec/match surface syntax or native source ranges for records.
at_component TypedCollections := {
  contexts := [.timed]
  program := {
    types := [.bits 64, .vec 0 2, .variant [[],[0]], .bool, .bits 8]
    states := [{id := 0, typeId := 0, initial := .bits 64 0}]
    handlers := [{id := 0, body := [
      .letVal 0 (.makeVec 1 [.literal 0 (.bits 64 3), .literal 0 (.bits 64 4)]),
      .letVal 1 (.vecSet 1 (.local 1 0) (.literal 0 (.bits 64 0)) (.literal 0 (.bits 64 41))),
      .letVal 2 (.makeVariant 2 1 [.index 0 (.local 1 1) (.literal 0 (.bits 64 0))]),
      .check (.compare 3 0 (.variantTag 0 (.local 2 2)) (.literal 0 (.bits 64 1))) "tag",
      .letVal 3 (.convert 4 2 (.binary 0 .addWrap (.variantGet 0 (.local 2 2) 1 0) (.literal 0 (.bits 64 1)))),
      .writeState 0 (.convert 0 0 (.local 4 3)), .ret []]}]
  }
}

def main : IO Unit := do
  let checked ← match compileWithProvenance NativeExpressions.model with
    | .ok value => pure value | .error e => throw (IO.userError e)
  let bundle := NativeExpressions.sourceBundle
  unless checked.nodes.length == bundle.origins.length && checked.nodes.all
      (fun node => bundle.modelNodes.lookup node.path == some node.text) do
    throw (IO.userError "NativeExpressionNodeCoverage")
  let _ ← match buildSourceMap checked bundle with
    | .ok value => pure value | .error e => throw (IO.userError e)
  let some handler := NativeExpressions.model.handlers.head? | throw (IO.userError "MissingHandler")
  let high ← match ModelIR.runSegment NativeExpressions.model {values := [(0,.bits 8 0)], lastEventTime := some 7} handler.body with
    | .ok value => pure value | .error e => throw (IO.userError e)
  let low ← match ExecIR.evalExecSegment checked.validated 0 [] {«state» := [.bits 8 0]} 1000 {now := 7} with
    | .ok value => pure value | .error e => throw (IO.userError e)
  unless high.values.lookup 0 == some (.bits 8 49) && ExecIR.commit low.txn == [.bits 8 49] do
    throw (IO.userError "NativeExpressionExecutionMismatch")
  IO.FS.createDirAll "build/e39-native"
  IO.FS.writeBinFile "build/e39-native/expressions.execir.bin" (serialize checked.validated)
  let collections ← match compileWithProvenance TypedCollections.model with
    | .ok value => pure value | .error e => throw (IO.userError e)
  let _ ← match buildSourceMap collections with
    | .ok value => pure value | .error e => throw (IO.userError e)
  let some collectionHandler := TypedCollections.model.handlers.head? | throw (IO.userError "MissingCollectionHandler")
  let collectionHigh ← match ModelIR.runSegment TypedCollections.model {values := [(0,.bits 64 0)]} collectionHandler.body with
    | .ok value => pure value | .error e => throw (IO.userError e)
  let collectionLow ← match ExecIR.evalExecSegment collections.validated 0 [] {«state» := [.bits 64 0]} 1000 with
    | .ok value => pure value | .error e => throw (IO.userError e)
  unless collectionHigh.values.lookup 0 == some (.bits 64 42) && ExecIR.commit collectionLow.txn == [.bits 64 42] do
    throw (IO.userError "CollectionExecutionMismatch")
  IO.FS.writeBinFile "build/e39-native/collections.execir.bin" (serialize collections.validated)
  IO.println "Native expression source, node bindings, both IR executions and descriptor passed"

