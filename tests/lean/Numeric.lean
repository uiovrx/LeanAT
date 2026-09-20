import LeanAT.Pure.Numeric
open LeanAT LeanAT.Numeric
set_option maxRecDepth 4096
set_option maxHeartbeats 8000000

private def isOk (r : Except String Value) (v : Value) : Bool :=
  match r with | .ok x => x == v | _ => false
private def isError (r : Except String Value) (e : String) : Bool :=
  match r with | .error x => x == e | _ => false

#guard isOk (evalUnary 0 (.bool false)) (.bool true)
#guard isOk (evalUnary 0 (.bool true)) (.bool false)
#guard isOk (evalCompare 0 (.bool true) (.bool true)) (.bool true)
#guard isOk (evalCompare 1 (.bool true) (.bool false)) (.bool true)
#guard isOk (evalCompare 0 (.record [.bits 8 1]) (.record [.bits 8 1])) (.bool true)

-- Same width/value/mode matrix as test_numeric.cpp, including 64-bit boundaries.
private def boundary (w : Nat) : Bool := Id.run do
  let modulus := 2^w
  let m := modulus-1
  let sign := 2^(w-1)
  for x in [0,1,sign,m] do
    if !(isOk (evalUnary 1 (.bits w x)) (.bits w (m-x))) then return false
    if !(isOk (evalUnary 2 (.bits w x)) (.bits w ((modulus-x)%modulus))) then return false
    for y in [0,1,sign,m] do
      let expected : List Bool := [x == y,x != y,x < y,x ≤ y,x > y,x ≥ y]
      for mode in List.range 6 do
        if !(isOk (evalCompare mode (.bits w x) (.bits w y)) (.bool expected[mode]!)) then return false
    for dest in (List.range 64).map (·+1) do
      if dest ≥ w then
        if !(isOk (evalConvert 0 dest (.bits w x)) (.bits dest x)) then return false
      else if !(isError (evalConvert 0 dest (.bits w x)) "NumericTypeMismatch") then return false
      if dest ≤ w then
        if !(isOk (evalConvert 1 dest (.bits w x)) (.bits dest (x%2^dest))) then return false
        if x < 2^dest then
          if !(isOk (evalConvert 2 dest (.bits w x)) (.bits dest x)) then return false
        else if !(isError (evalConvert 2 dest (.bits w x)) "ArithmeticOverflow") then return false
      else
        if !(isError (evalConvert 1 dest (.bits w x)) "NumericTypeMismatch") then return false
        if !(isError (evalConvert 2 dest (.bits w x)) "NumericTypeMismatch") then return false
    if !(isOk (checkedShift (.bits w x) 0 true) (.bits w x)) then return false
    if !(isOk (checkedShift (.bits w x) (w-1) false) (.bits w (x/2^(w-1)))) then return false
    if !(isOk (checkedShift (.bits w x) (w-1) true) (.bits w ((x*2^(w-1))%modulus))) then return false
    if !(isError (checkedShift (.bits w x) w true) "ShiftOutOfRange") then return false
    if !(isError (checkedShift (.bits w x) (2^64-1) false) "ShiftOutOfRange") then return false
    if !(isError (checkedDivide (.bits w x) (.bits w 0)) "DivisionByZero") then return false
    if !(isOk (checkedDivide (.bits w x) (.bits w 1)) (.bits w x)) then return false
  return isError (checkedDivide (.bits w sign) (.bits w m) true) "ArithmeticOverflow"

#guard ((List.range 64).map (·+1)).all boundary

private def signedPairs (w : Nat) : Bool := Id.run do
  let modulus := 2^w
  let sign := modulus/2
  for a in List.range modulus do
    for b in List.range modulus do
      let x : Int := if a ≥ sign then (a : Int)-modulus else a
      let y : Int := if b ≥ sign then (b : Int)-modulus else b
      let actual := checkedDivide (.bits w a) (.bits w b) true
      if y == 0 then
        if !(isError actual "DivisionByZero") then return false
      else if x == -(sign : Int) && y == -1 then
        if !(isError actual "ArithmeticOverflow") then return false
      else
        let q := Int.tdiv x y
        let encoded := if q < 0 then modulus-q.natAbs else q.natAbs
        if !(isOk actual (.bits w encoded)) then return false
  return true
#guard [1,2,3,4,5].all signedPairs

#guard isError (evalUnary 1 (.bits 0 0)) "NumericTypeMismatch"
#guard isError (evalUnary 1 (.bits 65 0)) "NumericTypeMismatch"
#guard isError (evalUnary 1 (.bits 8 256)) "NumericTypeMismatch"
#guard isError (evalUnary 3 (.bits 8 0)) "UnknownUnaryMode"
#guard isError (evalCompare 6 (.bits 8 0) (.bits 8 0)) "UnknownCompareMode"
#guard isError (evalConvert 3 8 (.bits 8 0)) "UnknownConvertMode"
#guard isError (evalConvert 0 65 (.bits 8 0)) "NumericTypeMismatch"
#guard isError (evalCompare 2 (.bits 8 1) (.bits 16 1)) "NumericTypeMismatch"

-- Actual source-operation checks: modular negation and complement are total.
#guard isOk (evalUnary 2 (.bits 8 1)) (.bits 8 ((- (1 : UInt8)).toNat))
#guard isOk (evalUnary 1 (.bits 64 0)) (.bits 64 ((~~~ (0 : UInt64)).toNat))
#guard isOk (evalUnary 2 (.bits 7 1)) (.bits 7 ((- (1 : BitVec 7)).toNat))
#guard isOk (evalUnary 1 (.bits 7 1)) (.bits 7 ((~~~ (1 : BitVec 7)).toNat))
#guard isOk (evalConvert 1 8 (.bits 16 256)) (.bits 8 ((256 : UInt16).toUInt8.toNat))
