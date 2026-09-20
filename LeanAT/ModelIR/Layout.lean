import LeanAT.ModelIR.Validation
namespace LeanAT
inductive Endian where
  | little | big
  deriving Repr, BEq
structure LayoutProfile where
  endian : Endian := .little
  maxBytes : Nat := 1048576
  maxWidth : Nat := 64
  deriving Repr
structure LayoutDesc where
  size : Nat
  alignment : Nat
  offsets : List Nat := []
  tagCount : Nat := 0
  maxElements : Nat := 0
  endian : Endian := .little
  deriving Repr, BEq
private def aligned (n a : Nat) : Nat := ((n + a - 1) / a) * a
private def recordLayout (fields : List LayoutDesc) : LayoutDesc := Id.run do
  let mut offset := 0
  let mut alignment := 1
  let mut offsets := []
  for field in fields do
    offset := aligned offset field.alignment
    offsets := offsets ++ [offset]
    offset := offset + field.size
    alignment := max alignment field.alignment
  return ⟨aligned offset alignment,alignment,offsets,0,0,.little⟩
private def layoutReferences : TypeSchema → List TypeId
  | .record fields => fields
  | .variant constructors => constructors.flatten
  | .vec element _ | .boundedVec element _ => [element]
  | _ => []
private def layoutNode (schema : TypeSchema) (cache : Array (Option LayoutDesc)) (profile : LayoutProfile) : Except String LayoutDesc := do
  let child := fun (id : Nat) => match (cache[id]?).getD none with | some layout => Except.ok layout | none => Except.error "UnresolvedLayout"
  let layout ← match schema with
  | .unit => pure {size := 0,alignment := 1}
  | .bool => pure {size := 1,alignment := 1}
  | .bits width =>
    if width == 0 || width > profile.maxWidth || width > 64 then throw "UnsupportedWidth"
    pure {size := (width+7)/8,alignment := 1}
  | .fin bound =>
    if bound > 2^64 then throw "UnsupportedFinBound"
    pure {size := 8,alignment := 1}
  | .bytes capacity =>
    if capacity ≥ 2^64 then throw "UnsupportedElementCount"
    pure {size := 8+capacity,alignment := 1,offsets := [8],maxElements := capacity}
  | .handle _ => pure {size := 29,alignment := 1}
  | .record fields => do pure (recordLayout (← fields.mapM child))
  | .variant constructors => do
    let layouts ← constructors.mapM (fun fields => do pure (recordLayout (← fields.mapM child)))
    pure {size := 4+(layouts.map LayoutDesc.size).foldl max 0,alignment := 1,offsets := [4],tagCount := constructors.length}
  | .vec element count => do
    if count ≥ 2^64 then throw "UnsupportedElementCount"
    let element ← child element
    pure {size := element.size*count,alignment := 1,maxElements := count}
  | .boundedVec element count => do
    if count ≥ 2^64 then throw "UnsupportedElementCount"
    let element ← child element
    pure {size := 8+element.size*count,alignment := 1,offsets := [8],maxElements := count}
  if layout.size > profile.maxBytes then throw "LayoutSizeExceeded"
  pure {layout with endian := profile.endian}
private def layoutWork (schemas : Array TypeSchema) (profile : LayoutProfile) : Nat → Array (Option LayoutDesc) → List (Nat × Bool) → Except String (Array (Option LayoutDesc))
  | _, cache, [] => .ok cache
  | 0, _, _::_ => .error "LayoutGraphBudget"
  | fuel+1, cache, (id,exitNode)::rest => do
    let some schema := schemas[id]? | throw "UnknownType"
    if ((cache[id]?).getD none).isSome then layoutWork schemas profile fuel cache rest
    else if exitNode then
      let layout ← layoutNode schema cache profile
      layoutWork schemas profile fuel (cache.set! id (some layout)) rest
    else layoutWork schemas profile fuel cache ((layoutReferences schema).map (fun child => (child,false)) ++ [(id,true)] ++ rest)
/-- Portable packed layout, with cached DAG traversal and a separate 1,000,000-work host ceiling. -/
def deriveLayout (type : TypeId) (schemas : TypeEnvironment) (profile : LayoutProfile := {}) : Except String LayoutDesc := do
  ModelIR.checkTypes schemas
  let work := schemas.foldl (fun count schema => count+(layoutReferences schema).length) (2*schemas.length)
  let cache ← layoutWork schemas.toArray profile work (Array.replicate schemas.length none) [(type,false)]
  match (cache[type]?).getD none with | some layout => pure layout | none => throw "UnknownType"structure ExtensionKey where
  qualifiedName : String
  version : Nat
  schemaIdentity : String
  deriving Repr, BEq
structure ExtensionSchema where
  key : ExtensionKey
  typeId : TypeId
  layout : LayoutDesc
  deriving Repr

def deriveExtension (type : TypeId) (name : String) (version : Nat) (schemas : TypeEnvironment)
    (profile : LayoutProfile := {}) : Except String ExtensionSchema := do
  if name.isEmpty then throw "EmptyExtensionName"
  let layout ← deriveLayout type schemas profile
  pure ⟨⟨name,version,reprStr (schemas,layout)⟩,type,layout⟩
end LeanAT





