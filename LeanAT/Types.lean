import Std
namespace LeanAT
abbrev TypeId := Nat
structure SourceSpan where
  file : String := "<generated>"
  line : Nat := 1
  column : Nat := 1
  deriving Repr, BEq, Inhabited
inductive HandleKind where
  | transaction | hop | event | process | wait | result | consumer | scope | task | access | lease
  | resourceTicket | spawnTicket | drain | gateTicket
  deriving Repr, BEq, DecidableEq, Inhabited
structure HandleIdentity where
  kind : HandleKind
  domain : UInt32
  store : UInt32
  slot : UInt32
  generation : UInt64
  owner : UInt64
  deriving Repr, BEq, DecidableEq, Inhabited
inductive Value where
  | unit | bool (value : Bool) | bits (width value : Nat)
  | record (fields : List Value) | variant (tag : Nat) (fields : List Value)
  | vec (elements : List Value)
  | bytes (data : List UInt8)
  | handle (identity : HandleIdentity)
  deriving Repr, BEq, Inhabited
inductive TypeSchema where
  | unit | bool | bits (width : Nat) | fin (bound : Nat)
  | bytes (capacity : Nat) | handle (kind : HandleKind)
  | record (fields : List TypeId) | variant (constructors : List (List TypeId))
  | vec (element : TypeId) (length : Nat) | boundedVec (element : TypeId) (capacity : Nat)
  deriving Repr, BEq, Inhabited
abbrev TypeEnvironment := List TypeSchema
inductive DecodeError where
  | typeMismatch | invalidTag | lengthExceeded | valueOutOfRange | unsupportedLayout
  deriving Repr, BEq, DecidableEq
/-- Bounded traversal rejects malformed or recursive schemas rather than diverging. -/
def conforms (env : TypeEnvironment) : Nat → TypeSchema → Value → Bool
  | 0, _, _ => false
  | _ + 1, .bytes capacity, .bytes data => data.length ≤ capacity
  | _ + 1, .handle kind, .handle identity => kind == identity.kind
  | _ + 1, .unit, .unit => true
  | _ + 1, .bool, .bool _ => true
  | _ + 1, .bits w, .bits w' n => w > 0 && w ≤ 64 && w == w' && n < 2^w
  | _ + 1, .fin b, .bits 64 n => b ≤ 2^64 && n < b
  | fuel + 1, .record ts, .record vs => ts.length == vs.length && (ts.zip vs).all (fun (t,v) => (env[t]?).any (fun s => conforms env fuel s v))
  | fuel + 1, .variant cs, .variant tag vs => (cs[tag]?).any (fun ts => ts.length == vs.length && (ts.zip vs).all (fun (t,v) => (env[t]?).any (fun s => conforms env fuel s v)))
  | fuel + 1, .vec t n, .vec vs => vs.length == n && vs.all (fun v => (env[t]?).any (fun s => conforms env fuel s v))
  | fuel + 1, .boundedVec t n, .vec vs => vs.length ≤ n && vs.all (fun v => (env[t]?).any (fun s => conforms env fuel s v))
  | _, _, _ => false
class ATRepr (α : Type) where
  schema : TypeSchema
  environment : TypeEnvironment := []
  encode : α → Value
  decode : Value → Except DecodeError α
  decode_encode : ∀ x, decode (encode x) = .ok x
  encode_valid : ∀ x, conforms environment (environment.length+1) schema (encode x) = true
instance : ATRepr Unit where
  schema := .unit
  encode := fun _ => .unit
  decode := fun v => match v with | .unit => .ok () | _ => .error .typeMismatch
  decode_encode := by intro x; cases x; rfl
  encode_valid := by intro x; rfl
instance : ATRepr Bool where
  schema := .bool
  encode := .bool
  decode := fun v => match v with | .bool b => .ok b | _ => .error .typeMismatch
  decode_encode := by intro x; rfl
  encode_valid := by intro x; rfl
structure BoundedBytes (capacity : Nat) where
  bytes : List UInt8
  bounded : bytes.length ≤ capacity
  deriving Repr
structure Tick where
  value : UInt64
  deriving Repr, BEq, DecidableEq, Inhabited
structure Duration where
  value : UInt64
  deriving Repr, BEq, DecidableEq, Inhabited
def checkedAddTime (t : Tick) (d : Duration) : Except String Tick :=
  if t.value.toNat + d.value.toNat < 2^64 then .ok ⟨UInt64.ofNat (t.value.toNat + d.value.toNat)⟩ else .error "ArithmeticOverflow"
class FinBound (n : Nat) : Prop where
  bound : n ≤ 2^64
instance (n : Nat) [FinBound n] : ATRepr (Fin n) where
  schema := .fin n
  encode := fun x => .bits 64 x.val
  decode := fun v => match v with
    | .bits 64 k => if h : k < n then .ok ⟨k,h⟩ else .error .valueOutOfRange
    | _ => .error .typeMismatch
  decode_encode := by intro x; simp [x.isLt]
  encode_valid := by intro x; have h := FinBound.bound (n := n); simpa [conforms, x.isLt] using h

/-- Boundary conversion retains the actual length proof. No unbounded ByteArray instance exists. -/
def boundBytes (capacity : Nat) (bytes : List UInt8) : Except DecodeError (BoundedBytes capacity) :=
  if h : bytes.length ≤ capacity then .ok ⟨bytes,h⟩ else .error .lengthExceeded
@[instance_reducible] private def fixedCodec {α : Type} (width : Nat) (positive : 0 < width) (supported : width ≤ 64)
    (toNat : α → Nat) (ofNat : Nat → α) (bound : ∀ x, toNat x < 2^width)
    (inverse : ∀ x, ofNat (toNat x) = x) : ATRepr α where
  schema := .bits width
  encode := fun x => .bits width (toNat x)
  decode := fun v => match v with
    | .bits w k => if w = width ∧ k < 2^width then .ok (ofNat k) else .error .valueOutOfRange
    | _ => .error .typeMismatch
  decode_encode := by intro x; simp [bound x, inverse x]
  encode_valid := by intro x; simp [conforms, positive, supported, bound x]
instance : ATRepr UInt8 := fixedCodec 8 (by decide) (by decide) UInt8.toNat UInt8.ofNat UInt8.toNat_lt (fun _ => UInt8.ofNat_toNat)
instance : ATRepr UInt16 := fixedCodec 16 (by decide) (by decide) UInt16.toNat UInt16.ofNat UInt16.toNat_lt (fun _ => UInt16.ofNat_toNat)
instance : ATRepr UInt32 := fixedCodec 32 (by decide) (by decide) UInt32.toNat UInt32.ofNat UInt32.toNat_lt (fun _ => UInt32.ofNat_toNat)
instance : ATRepr UInt64 := fixedCodec 64 (by decide) (by decide) UInt64.toNat UInt64.ofNat UInt64.toNat_lt (fun _ => UInt64.ofNat_toNat)
instance : ATRepr Tick := fixedCodec 64 (by decide) (by decide) (fun t => t.value.toNat) (fun n => ⟨UInt64.ofNat n⟩)
  (fun x => UInt64.toNat_lt x.value) (by intro x; simp [UInt64.ofNat_toNat])
instance : ATRepr Duration := fixedCodec 64 (by decide) (by decide) (fun t => t.value.toNat) (fun n => ⟨UInt64.ofNat n⟩)
  (fun x => UInt64.toNat_lt x.value) (by intro x; simp [UInt64.ofNat_toNat])
private def encodeBytes (xs : List UInt8) : List Value := xs.map (fun x => .bits 8 x.toNat)
private def decodeBytes : List Value → Except DecodeError (List UInt8)
  | [] => .ok []
  | .bits 8 n :: rest => if n < 256 then do pure (UInt8.ofNat n :: (← decodeBytes rest)) else .error .valueOutOfRange
  | _ :: _ => .error .typeMismatch
private theorem bytes_roundtrip (xs : List UInt8) : decodeBytes (encodeBytes xs) = .ok xs := by
  induction xs with
  | nil => rfl
  | cons x xs ih =>
    have h : x.toNat < 256 := UInt8.toNat_lt x
    simp [encodeBytes, decodeBytes, h, UInt8.ofNat_toNat] at ih ⊢
    rw [ih]; rfl
private theorem bytes_valid (xs : List UInt8) :
    (encodeBytes xs).all (fun v => conforms [.bits 8] 1 (.bits 8) v) = true := by
  induction xs with
  | nil => rfl
  | cons x xs ih =>
    have h : x.toNat < 256 := UInt8.toNat_lt x
    simpa [encodeBytes, conforms, h] using ih
instance (capacity : Nat) : ATRepr (BoundedBytes capacity) where
  schema := .boundedVec 0 capacity
  environment := [.bits 8]
  encode := fun x => .vec (encodeBytes x.bytes)
  decode := fun v => match v with
    | .vec vs => do
      let bytes ← decodeBytes vs
      if h : bytes.length ≤ capacity then pure ⟨bytes,h⟩ else throw .lengthExceeded
    | _ => .error .typeMismatch
  decode_encode := by intro x; simp [bytes_roundtrip]; change (if h : x.bytes.length ≤ capacity then Except.ok ⟨x.bytes,h⟩ else Except.error DecodeError.lengthExceeded) = .ok x; simp [x.bounded]
  encode_valid := by
    intro x
    have h := bytes_valid x.bytes
    simpa [conforms, encodeBytes, x.bounded] using h
/-- Serializable identity only. Possession does not establish a live runtime capability. -/
structure HandleValue (kind : HandleKind) where
  identity : HandleIdentity
  kindCorrect : identity.kind = kind
  deriving Repr
instance (kind : HandleKind) : ATRepr (HandleValue kind) where
  schema := .handle kind
  encode := fun x => .handle x.identity
  decode := fun v => match v with
    | .handle identity => if h : identity.kind = kind then .ok ⟨identity,h⟩ else .error .typeMismatch
    | _ => .error .typeMismatch
  decode_encode := by intro x; simp [x.kindCorrect]
  encode_valid := by intro x; simp [conforms, x.kindCorrect]; cases kind <;> rfl
/-- The finite schema refinement used at generic record/variant/vector codec boundaries. -/
abbrev ValueConforms (environment : TypeEnvironment) (schema : TypeSchema) (value : Value) : Prop :=
  conforms environment (environment.length+1) schema value = true
structure FiniteValue (environment : TypeEnvironment) (schema : TypeSchema) where
  value : Value
  valid : ValueConforms environment schema value
  deriving Repr
private def classifyDecodeError (schema : TypeSchema) (value : Value) : DecodeError :=
  match schema, value with
  | .variant constructors, .variant tag _ => if tag ≥ constructors.length then .invalidTag else .typeMismatch
  | .boundedVec _ capacity, .vec values => if values.length > capacity then .lengthExceeded else .typeMismatch
  | .bytes capacity, .bytes bytes => if bytes.length > capacity then .lengthExceeded else .typeMismatch
  | .vec _ length, .vec values => if values.length != length then .lengthExceeded else .typeMismatch
  | .bits _, .bits _ _ | .fin _, .bits _ _ => .valueOutOfRange
  | _, _ => .typeMismatch
instance (environment : TypeEnvironment) (schema : TypeSchema) : ATRepr (FiniteValue environment schema) where
  schema := schema
  environment := environment
  encode := FiniteValue.value
  decode := fun value => if h : ValueConforms environment schema value then .ok ⟨value,h⟩ else .error (classifyDecodeError schema value)
  decode_encode := by intro x; rw [dif_pos x.valid]
  encode_valid := fun x => x.valid
end LeanAT















