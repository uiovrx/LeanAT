import LeanAT.Types

namespace LeanAT.Numeric

private def readBits (value : Value) : Except String (Nat × Nat) :=
  match value with
  | .bits width n =>
    if width == 0 || width > 64 || n ≥ 2^width then .error "NumericTypeMismatch"
    else .ok (width, n)
  | _ => .error "NumericTypeMismatch"

/-- ExecIR Unary immediate: logical not, complement, modular negation. -/
def evalUnary (mode : Nat) (value : Value) : Except String Value := do
  if mode > 2 then throw "UnknownUnaryMode"
  if mode == 0 then
    match value with
    | .bool b => return .bool (!b)
    | _ => throw "NumericTypeMismatch"
  let (width, n) ← readBits value
  return .bits width (if mode == 1 then 2^width - 1 - n else (2^width - n) % 2^width)

private def valueFamily : Value → Nat
  | .unit => 0 | .bool _ => 1 | .bits _ _ => 2 | .bytes _ => 4
  | .record _ | .variant _ _ | .vec _ => 5 | .handle _ => 6

/-- Types are validated by ExecIR before structural equality. Ordered modes are unsigned. -/
def evalCompare (mode : Nat) (a b : Value) : Except String Value := do
  if mode > 5 then throw "UnknownCompareMode"
  if mode ≤ 1 then
    if valueFamily a != valueFamily b then throw "NumericTypeMismatch"
    match a, b with
    | .bits _ _, .bits _ _ =>
      let (w, _) ← readBits a
      let (v, _) ← readBits b
      if w != v then throw "NumericTypeMismatch"
    | .bits _ _, _ | _, .bits _ _ => throw "NumericTypeMismatch"
    | _, _ => pure ()
    return .bool (if mode == 0 then a == b else a != b)
  let (w, x) ← readBits a
  let (v, y) ← readBits b
  if w != v then throw "NumericTypeMismatch"
  return .bool (match mode with | 2 => x < y | 3 => x ≤ y | 4 => x > y | _ => x ≥ y)

/-- Explicit zero extension, truncation, or checked narrowing. No implicit address/time cast. -/
def evalConvert (mode destWidth : Nat) (value : Value) : Except String Value := do
  if mode > 2 then throw "UnknownConvertMode"
  let (width, n) ← readBits value
  if destWidth == 0 || destWidth > 64 ||
      (mode == 0 && width > destWidth) || (mode != 0 && width < destWidth) then
    throw "NumericTypeMismatch"
  if mode == 2 && n ≥ 2^destWidth then throw "ArithmeticOverflow"
  return .bits destWidth (n % 2^destWidth)

/-- Explicit checked helper on two's-complement encodings, truncating toward zero.
This is not a replacement for ordinary Lean division and has a separate error channel. -/
def checkedDivide (a b : Value) (signedTwosComplement : Bool := false) : Except String Value := do
  let (width, x) ← readBits a
  let (otherWidth, y) ← readBits b
  if width != otherWidth then throw "NumericTypeMismatch"
  if y == 0 then throw "DivisionByZero"
  if !signedTwosComplement then return .bits width (x / y)
  let modulus := 2^width
  let sign := 2^(width-1)
  if x == sign && y == modulus-1 then throw "ArithmeticOverflow"
  let negativeX := x ≥ sign
  let negativeY := y ≥ sign
  let magnitudeX := if negativeX then modulus-x else x
  let magnitudeY := if negativeY then modulus-y else y
  let quotient := magnitudeX / magnitudeY
  return .bits width (if negativeX != negativeY then (modulus-quotient) % modulus else quotient)

/-- Checked count, logical right shift and modular left shift; oversized counts fail. -/
def checkedShift (value : Value) (count : Nat) (left : Bool) : Except String Value := do
  let (width, n) ← readBits value
  if count ≥ width then throw "ShiftOutOfRange"
  return .bits width (if left then (n * 2^count) % 2^width else n / 2^count)

end LeanAT.Numeric
