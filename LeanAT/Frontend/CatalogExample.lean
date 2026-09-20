import LeanAT.Frontend.BlockSyntax

namespace LeanAT.Frontend.CatalogExample
set_option maxRecDepth 16384
set_option maxHeartbeats 4000000

@[leanat_pure] def negateFlag (value : Bool) : Bool := !value

-- 真实 UTF-8 原生例子：the source bundle includes this exact document.
at_component NativeScalar where
  state cell : UInt64 := 0
  on internal.evaluate event do
    let small : UInt8 := 42
    let wide : UInt64 := UInt8.toUInt64 small
    let matched : Bool := wide == 42
    let opposite : Bool := negateFlag matched
    let tick : UInt64 := now
    set cell := if opposite then 0 else wide + tick
    return

end LeanAT.Frontend.CatalogExample
